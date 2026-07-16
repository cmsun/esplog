#!/usr/bin/env python3
"""
esplog WiFi 日志接收工具 (TCP 客户端模式)

连接 ESP32-AP 热点后，作为 TCP 客户端连接到 ESP32 的 TCP 服务器（默认 192.168.3.1:9000），
接收并显示 esplog 发送的日志。日志自带 ANSI 颜色转义序列和时间戳，直接在终端中彩色显示。

使用方法:
    python3 esplog_wifi_rec.py                      # 默认连接 192.168.3.1:9000
    python3 esplog_wifi_rec.py -s 192.168.3.1       # 指定服务器 IP
    python3 esplog_wifi_rec.py -p 9000              # 指定端口
    python3 esplog_wifi_rec.py --retry              # 连接断开后自动重连
    python3 esplog_wifi_rec.py -o mylog.txt         # 同时保存日志到文件
    python3 esplog_wifi_rec.py --no-color           # 纯文本模式
"""

import argparse
import socket
import sys
import time
from datetime import datetime

# 默认配置
DEFAULT_HOST = "192.168.3.1"
DEFAULT_PORT = 9000
RECV_BUFFER = 4096
RECONNECT_INTERVAL = 3  # 重连间隔（秒）


class EsplogTcpClient:
    """TCP 日志客户端，连接 ESP32 TCP 服务器并接收日志。"""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 auto_reconnect: bool = False, save_to_file: bool = False,
                 output_file: str | None = None):
        self.host = host
        self.port = port
        self.auto_reconnect = auto_reconnect
        self.save_to_file = save_to_file
        self.output_file = output_file or f"esplog_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
        self._sock: socket.socket | None = None
        self._running = False
        self._file_handle = None
        self._buf = b""  # 接收缓冲区

    def start(self):
        """启动客户端，连接并接收日志。"""
        self._running = True

        if self.save_to_file:
            self._file_handle = open(self.output_file, "w", encoding="utf-8")
            print(f"[esplog_receiver] 日志保存至: {self.output_file}")

        print(f"[esplog_receiver] 目标: {self.host}:{self.port} (TCP)")
        print(f"[esplog_receiver] 请确保已连接 WiFi: ESP32-AP (密码: 12345678)")
        print(f"[esplog_receiver] 按 Ctrl+C 停止...\n")

        try:
            self._receive_loop()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def stop(self):
        """停止客户端并清理资源。"""
        self._running = False
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        if self._file_handle:
            self._file_handle.close()
            print(f"\n[esplog_receiver] 日志已保存至: {self.output_file}")
        print("[esplog_receiver] 已停止。")

    def _connect(self) -> bool:
        """连接到 ESP32 TCP 服务器。返回是否连接成功。"""
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass

        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(5)  # 连接超时 5 秒
            self._sock.connect((self.host, self.port))
            self._sock.settimeout(1.0)  # 接收超时 1 秒，方便响应 Ctrl+C
            print(f"[esplog_receiver] 已连接到 {self.host}:{self.port}")
            return True
        except (OSError, socket.timeout) as e:
            print(f"[esplog_receiver] 连接失败: {e}", file=sys.stderr)
            self._sock = None
            return False

    def _receive_loop(self):
        """主接收循环。"""
        while self._running:
            if self._sock is None:
                if not self._connect():
                    if self.auto_reconnect:
                        print(f"[esplog_receiver] {RECONNECT_INTERVAL} 秒后重试...")
                        for _ in range(RECONNECT_INTERVAL):
                            if not self._running:
                                return
                            time.sleep(1)
                        continue
                    else:
                        print("[esplog_receiver] 无法连接，退出。", file=sys.stderr)
                        return

            try:
                data = self._sock.recv(RECV_BUFFER)
                if not data:
                    # 连接被对端关闭
                    print(f"[esplog_receiver] 服务器断开连接")
                    self._sock.close()
                    self._sock = None
                    if not self.auto_reconnect:
                        return
                    print(f"[esplog_receiver] {RECONNECT_INTERVAL} 秒后重试...")
                    for _ in range(RECONNECT_INTERVAL):
                        if not self._running:
                            return
                        time.sleep(1)
                    continue

                self._buf += data
                self._process_buffer()

            except socket.timeout:
                # 超时，处理缓冲区中可能残留的数据
                if self._buf:
                    self._process_buffer()
                continue
            except OSError as e:
                if self._running:
                    print(f"[esplog_receiver] 接收错误: {e}", file=sys.stderr)
                self._sock = None
                if not self.auto_reconnect:
                    return
                time.sleep(RECONNECT_INTERVAL)
                continue

    def _process_buffer(self):
        """处理接收缓冲区，按行分割并显示。"""
        while True:
            pos = self._buf.find(b"\n")
            if pos == -1:
                break
            line = self._buf[:pos].rstrip(b"\r\n")
            self._buf = self._buf[pos + 1:]
            if line:
                self._display(line)

    def _display(self, data: bytes):
        """显示并可选地保存收到的日志行。"""
        try:
            text = data.decode("utf-8", errors="replace")
        except UnicodeDecodeError:
            text = data.decode("latin-1", errors="replace")

        if not text:
            return

        sys.stdout.write(text + "\n")
        sys.stdout.flush()

        if self._file_handle:
            self._file_handle.write(text + "\n")
            self._file_handle.flush()


def parse_args():
    parser = argparse.ArgumentParser(
        description="esplog WiFi 日志接收工具 (TCP 客户端)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python3 esplog_wifi_rec.py                     # 默认连接 192.168.3.1:9000
  python3 esplog_wifi_rec.py -s 192.168.3.1      # 指定服务器 IP
  python3 esplog_wifi_rec.py -p 9000             # 指定端口
  python3 esplog_wifi_rec.py --retry             # 断线自动重连
  python3 esplog_wifi_rec.py -o my.log           # 保存日志到文件
  python3 esplog_wifi_rec.py --no-color          # 去除 ANSI 颜色
        """,
    )
    parser.add_argument(
        "-s", "--server", type=str, default=DEFAULT_HOST,
        help=f"ESP32 TCP 服务器地址 (默认: {DEFAULT_HOST})",
    )
    parser.add_argument(
        "-p", "--port", type=int, default=DEFAULT_PORT,
        help=f"TCP 端口 (默认: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "-r", "--retry", action="store_true",
        help="连接断开后自动重连",
    )
    parser.add_argument(
        "-o", "--output", type=str, default=None,
        help="日志保存路径 (默认: esplog_YYYYMMDD_HHMMSS.log)",
    )
    parser.add_argument(
        "--no-color", action="store_true",
        help="去除 ANSI 颜色转义码，以纯文本显示",
    )
    return parser.parse_args()


def strip_ansi(text: str) -> str:
    """去除 ANSI 转义序列。"""
    import re
    ansi_pattern = re.compile(r"\x1b\[[0-9;]*m")
    return ansi_pattern.sub("", text)


def main():
    args = parse_args()

    # 如果要求去除颜色，monkey-patch _display 方法
    if args.no_color:
        original_display = EsplogTcpClient._display

        def no_color_display(self, data):
            try:
                text = data.decode("utf-8", errors="replace")
            except UnicodeDecodeError:
                text = data.decode("latin-1", errors="replace")
            if not text:
                return
            text = strip_ansi(text)
            sys.stdout.write(text + "\n")
            sys.stdout.flush()
            if self._file_handle:
                self._file_handle.write(text + "\n")
                self._file_handle.flush()

        EsplogTcpClient._display = no_color_display

    client = EsplogTcpClient(
        host=args.server,
        port=args.port,
        auto_reconnect=args.retry,
        save_to_file=args.output is not None,
        output_file=args.output,
    )
    client.start()


if __name__ == "__main__":
    main()
