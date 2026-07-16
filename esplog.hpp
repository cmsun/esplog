#pragma once

#include <algorithm>
#include <stdio.h>
#include <string_view>
#include <vector>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h" // socket / accept / send / sockaddr_in 等 POSIX socket API

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fmt/chrono.h"

class esplog {

public:
  esplog() = delete;
  ~esplog() = delete;

  enum class level : uint8_t { info, debug, warn, error, none };

  // 如果 wifi_print 参数为 true ，则通过 WIFI 的 TCP 服务器发送日志，否则通过
  // idf.py menuconfig 配置的 Console 打印日志。TCP 服务器绑定端口
  // 9000，客户端连接后即可接收日志。
  static void init(level l, bool wifi_print = false) {
    // 设置日志等级
    _level = l;
    // 重定向标准输出到WIFI打印函数
    if (wifi_print) {
      stdout = fwopen(NULL, &stdout_redirection);
      setvbuf(stdout, NULL, _IONBF, 0);
      // 初始化 WIFI，创建热点SSID为"ESP32-AP"，密码为"12345678"
      wifi_init("ESP32-AP", "12345678");
      // 设置主机 IP 地址和 DHCP 的分配范围
      dhcp_init();
      // 创建TCP服务器，在端口号9000上监听客户端连接
      tcp_server_init(9000);
    }
  }

  // 信息日志（绿色）
  template <typename... Args>
  static inline void info(fmt::format_string<Args...> fmt, Args &&...args) {
    if (_level <= level::info) {
      esplog_impl(GREEN, fmt, fmt::make_format_args(args...));
    }
  }

  // 调试日志（蓝色）
  template <typename... Args>
  static inline void debug(fmt::format_string<Args...> fmt, Args &&...args) {
    if (_level <= level::debug) {
      esplog_impl(BLUE, fmt, fmt::make_format_args(args...));
    }
  }

  // 警告日志（黄色）
  template <typename... Args>
  static inline void warn(fmt::format_string<Args...> fmt, Args &&...args) {
    if (_level <= level::warn) {
      esplog_impl(YELLOW, fmt, fmt::make_format_args(args...));
    }
  }

  // 错误日志（红色）
  template <typename... Args>
  static inline void error(fmt::format_string<Args...> fmt, Args &&...args) {
    if (_level <= level::error) {
      esplog_impl(RED, fmt, fmt::make_format_args(args...));
    }
  }

private:
  static inline level _level = level::none;
  static inline int _server_sock = -1; // TCP 服务器监听 socket，-1 表示未初始化
  static inline SemaphoreHandle_t _clients_mutex =
      nullptr; // 保护 _client_socks / _client_addrs
  static inline std::vector<std::pair<int, sockaddr_in>>
      _client_info; // 客户端信息
  static constexpr std::string_view GREEN = "\033[32m";
  static constexpr std::string_view BLUE = "\033[34m";
  static constexpr std::string_view YELLOW = "\033[33m";
  static constexpr std::string_view RED = "\033[31m";
  static constexpr std::string_view RESET = "\033[0m\r\n";

  // 类型擦除的底层实现：使用 fmt::string_view + fmt::format_args，
  // 避免空参数包导致的 format_string<> 推导失败
  static inline void esplog_impl(std::string_view color,
                                 fmt::string_view fmt,
                                 fmt::format_args args) {
    char buff[512];
    auto now = std::chrono::system_clock::now();
    auto result_time = fmt::format_to_n(buff, sizeof(buff), "[{}]: ", now);
    auto result_fmt =
        fmt::vformat_to_n(buff + result_time.size,
                          sizeof(buff) - result_time.size, fmt, args);
    fwrite(color.data(), 1, color.size(), stdout);
    fwrite(buff, 1, result_time.size + result_fmt.size, stdout);
    fwrite(RESET.data(), 1, RESET.size(), stdout);
  }

  // 输出重定向函数：将日志发送给所有已连接的 TCP 客户端
  static int stdout_redirection(void *cookie, const char *data, int size) {
    if (_server_sock != -1 && _clients_mutex != nullptr) {
      if (xSemaphoreTake(_clients_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < _client_info.size();) {
          int ret = send(_client_info[i].first, data, size, 0);
          if (ret <= 0) {
            // 客户端已断开，关闭 socket 并从 vector 中移除
            close(_client_info[i].first);
            _client_info.erase(_client_info.begin() + i);
          } else {
            ++i;
          }
        }
        xSemaphoreGive(_clients_mutex);
      }
    }
    return size;
  }

  /**
   * @brief 初始化 WiFi，并将设备配置为 SoftAP（热点）模式
   *
   * @param ssid     热点名称，最长 32 字节
   * @param password 连接密码，WPA/WPA2 下长度需在 [8, 63] 字节之间
   *
   * 说明：
   *  - SSID 允许包含嵌入的 '\0'，因此必须显式设置 ap.ssid_len；
   *    若 ssid_len 为 0，driver 才会退化为用 strlen 推断长度。
   *  - Password 是纯 ASCII 字符串（不允许含 '\0'），driver 内部用
   *    strlen 自动推断长度，结构体里没有 password_len 字段，故无需设置。
   */
  static void wifi_init(std::string_view ssid, std::string_view password) {
    // 1. 使用默认参数初始化 WiFi 底层（分配资源、注册事件等）
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 2. 设置为 AP（热点）模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // 3. 构造 AP 配置，{} 将整块内存零初始化，
    //    从而保证 ssid/password 缓冲区末尾自带 '\0' 终止符
    wifi_config_t ap_config = {};

    // 3.1 拷贝 SSID，并显式记录长度（SSID 可含嵌入空字符）
    size_t ssid_len = std::min<size_t>(ssid.size(), sizeof(ap_config.ap.ssid));
    memcpy(ap_config.ap.ssid, ssid.data(), ssid_len);
    ap_config.ap.ssid_len = ssid_len;

    // 3.2 拷贝密码；限制长度 < 64，确保末尾保留 '\0' 供 driver 推断长度
    size_t pwd_len =
        std::min<size_t>(password.size(), sizeof(ap_config.ap.password) - 1);
    memcpy(ap_config.ap.password, password.data(), pwd_len);

    // 3.3 其余 AP 参数
    ap_config.ap.max_connection = 4;                // 最大允许 4 个 STA 接入
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK; // 加密方式
    ap_config.ap.channel = 6;                       // 工作信道
    ap_config.ap.ssid_hidden = 0;                   // 不隐藏 SSID
    ap_config.ap.beacon_interval = 100;             // 信标间隔(ms)

    // 4. 应用配置并启动 WiFi
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
  }

  /**
   * @brief 初始化 SoftAP 的 DHCP 服务器
   *
   * 完成两件事：
   *  1. 为 AP 主机设置静态 IP（192.168.3.1/24）；
   *  2. 设置 DHCP 服务器下发给接入设备分配同一个网段的 IP 地址
   * ESP-IDF 已内置 DHCP 服务器，直接调用 esp-netif 接口即可，
   * 无需自行实现 67 端口的 socket 服务。
   */
  static void dhcp_init() {
    // 1. 初始化 esp-netif 组件（幂等，重复调用安全）
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. 获取/创建默认 AP 网络接口（创建时默认已带 DHCP 服务器）
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr) {
      ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (ap_netif == nullptr) {
      return; // 接口创建失败，放弃 DHCP 配置
    }

    // 3. 修改 AP 配置前必须先停止 DHCP 服务器
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));

    // 3.1 设置 AP 主机静态 IP（192.168.3.1/24），则 DHCP 会自动分配同一个网段的
    // IP 地址
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 3, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 3, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    // 4. 重新启动 DHCP 服务器，使上述配置生效
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
  }

  /**
   * @brief 创建 TCP 服务器 socket，绑定端口、启动监听并创建 accept 线程
   *
   * @param port 服务器监听端口（如 9000）
   *
   * 流程：
   *  1. 创建互斥锁保护客户端列表；
   *  2. 创建 SOCK_STREAM(TCP) socket；
   *  3. 设置 SO_REUSEADDR 允许端口复用；
   *  4. bind 到 0.0.0.0:port + listen；
   *  5. 启动 FreeRTOS 线程 _accept_task 阻塞等待客户端连接。
   * 客户端地址和端口存入 _client_addrs，日志通过 stdout_redirection 发送。
   */
  static void tcp_server_init(uint16_t port) {
    // 1. 创建互斥锁，保护客户端列表的并发访问
    _clients_mutex = xSemaphoreCreateMutex();
    if (_clients_mutex == nullptr) {
      return;
    }

    // 2. 创建 TCP socket（阻塞模式，由 accept 线程专职处理）
    _server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_sock < 0) {
      return;
    }

    // 3. 设置 SO_REUSEADDR，允许快速重启后重新绑定端口
    int reuse = 1;
    setsockopt(_server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 4. 绑定到 0.0.0.0:port
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(_server_sock, reinterpret_cast<sockaddr *>(&server_addr),
             sizeof(server_addr)) < 0) {
      close(_server_sock);
      _server_sock = -1;
      return;
    }

    // 5. 开始监听，最大等待队列长度 4
    if (listen(_server_sock, 4) < 0) {
      close(_server_sock);
      _server_sock = -1;
      return;
    }

    // 6. 启动 accept 线程，阻塞等待客户端连接
    xTaskCreate(&accept_task, "esplog_accept", 3072, nullptr, 5, nullptr);
  }

  /**
   * @brief accept 线程：阻塞等待客户端连接，将客户端 fd / 地址存入列表
   */
  static void accept_task(void *arg) {
    while (true) {
      if (_server_sock < 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      sockaddr_in client_addr;
      socklen_t addr_len = sizeof(client_addr);
      int client_sock = accept(
          _server_sock, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);

      if (client_sock >= 0) {
        xSemaphoreTake(_clients_mutex, portMAX_DELAY);
        _client_info.emplace_back(client_sock, client_addr);
        xSemaphoreGive(_clients_mutex);
      } else {
        // accept 失败（如 socket 被关闭），短暂延迟后重试
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }

}; // class esplog
