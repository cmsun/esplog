#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include <driver/gpio.h>
#include <driver/uart.h>

#include "esplog.hpp"

/**
 * @brief 配置并初始化 UART0
 *
 * 设置波特率 115200、8 数据位、无校验、1 停止位、无流控，
 * 安装 UART 驱动，使其可用于收发。
 *
 * @return esp_err_t ESP_OK 表示成功，其他值表示失败
 */
esp_err_t configure_uart0(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_NUM_0, &uart_config);
    if (err != ESP_OK)
    {
        // printf("Failed to configure UART parameters: %d\n", err);
        return err;
    }

    err = uart_set_pin(UART_NUM_0, GPIO_NUM_43, GPIO_NUM_44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        // printf("Failed to set uart pin: %d\n", err);
        return err;
    }

    err = uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0);
    if (err != ESP_OK)
    {
        // printf("Failed to install UART driver: %d\n", err);
        return err;
    }

    return err;
}

extern "C" void app_main(void)
{
    // 初始化 NVS 分区
    nvs_flash_init();
    // 初始化事件循环
    esp_event_loop_create_default();
    // 初始化网络接口
    esp_netif_init();
    // 启用 DHCP 服务器‌
    esp_netif_create_default_wifi_ap();
    // 启用 DHCP 客户端以支持 IP 地址获取及上层 TCP/IP 通信‌
    esp_netif_create_default_wifi_sta();

    // configure_uart0();
    esplog::init(esplog::level::info, "ESP32-LOG");

    while (true)
    {
        esplog::debug("Hello World debug.");
        esplog::info("Hello World info.");
        esplog::warn("Hello World warn.");
        esplog::error("Hello World error.");

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}