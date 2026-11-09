# AutoWiFi - 跨平台自动认证 Wi-Fi 连接客户端 (Windows / macOS / Linux)

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

使用 C++17 编写的轻量级跨平台 Wi-Fi 自动连接与认证工具，支持 **Windows**、**macOS** 与 **Linux**。全面支持 **WPA2/WPA3-Enterprise (802.1X PEAP-MSCHAPv2)** 企业级无线认证与 **Captive Portal (校园网/企业网网页认证)**，提供图形界面/交互式终端菜单、后台常驻守护、断线自动重连与开机自启功能。

## 平台兼容性与底层实现

| 功能模块 | Windows (x64) | macOS (Apple Silicon / Intel) | Linux (x64 / ARM64) |
| :--- | :--- | :--- | :--- |
| **无线网卡控制 & 802.1X** | Native Wifi API (`wlanapi` + `EapHost`) | 原生 `CoreWLAN.framework` + `CoreLocation.framework`（自动弹窗申请定位/SSID 权限） | NetworkManager (`nmcli` 802-1x PEAP) |
| **Captive Portal 认证** | 原生 `WinHTTP` | `libcurl` (`curl` CLI 兜底) | `libcurl` (`curl` CLI 兜底) |
| **界面与交互模式** | Win32 原生 GUI + 系统托盘 + CLI | Cocoa 原生 GUI (`AutoWiFi.app`) + 顶部菜单栏托盘 + CLI | 交互式 TUI 终端菜单 + Daemon + CLI |
| **凭据安全存储** | Windows DPAPI (`CryptProtectData`) | 本地用户盐值流加密 (`0600` 权限) | 本地用户盐值流加密 (`0600` 权限) |
| **开机自动启动** | 注册表 `HKCU\...\Run` | LaunchAgent (`~/Library/LaunchAgents`) | XDG Autostart (`~/.config/autostart`) |

## 核心功能

1. **多模式账号密码认证支持**
   - **802.1X 企业级认证 (`PEAP-MSCHAPv2`)**：自动配置无线网卡 802.1X 凭据并发起关联，实现无人值守直接连接企业/校园 802.1X Wi-Fi。
   - **网页 Captive Portal 认证（开放热点 / WPA2-PSK 热点）**：自动关联热点并探测重定向地址（或使用自定义认证接口），支持 `POST 表单 (application/x-www-form-urlencoded)`、`POST JSON`、`GET 参数` 提交账号密码。
   - **双重认证模式（802.1X + 网页 Portal）**：先完成 802.1X 无线层接入，再自动完成网页 Portal 登录。
2. **图形界面 / 交互式终端与后台守护**
   - **Windows**：提供原生 Win32 图形界面与系统托盘常驻，支持关闭最小化到托盘、气泡通知与快捷菜单。
   - **macOS / Linux**：提供交互式终端控制菜单（TUI）与后台守护进程模式（`--daemon`），支持 `SIGINT`/`SIGTERM` 优雅退出。
3. **后台断线检测与自动重连**
   - 后台守护按设定间隔（默认 15 秒）检测无线网卡连接状态与外网连通性（`msftconnecttest`），发现掉线或被 Portal 拦截时自动重连并重新认证。
4. **安全存储与便携模式**
   - 默认配置路径：
     - Windows：`%APPDATA%\AutoWiFi\config.ini`
     - macOS / Linux：`~/.config/auto-wifi/config.ini`（或 `$XDG_CONFIG_HOME/auto-wifi/config.ini`）
   - 若可执行文件同级目录存在 `config.ini`，则优先启用便携模式。

## 命令行参数

### Windows (`auto-wifi.exe`)
- `auto-wifi.exe`：启动 Win32 图形主界面。
- `auto-wifi.exe --minimized`（或 `--tray` / `--daemon` / `-m` / `-d`）：启动后直接最小化到系统托盘后台运行。
- `auto-wifi.exe --connect-once`（或 `-c`）：读取配置执行一次连接与认证后立即退出。

### macOS (`AutoWiFi.app` / `auto-wifi`)
- **图形界面启动**：直接双击运行 `dist/AutoWiFi.app`（或在终端执行 `open dist/AutoWiFi.app`），启动原生 Cocoa 图形界面并常驻顶部菜单栏。
  - > **注意（浏览器下载后无法打开的解决方法）**：从 GitHub Releases 下载的未公证应用会被 macOS Gatekeeper 附加隔离属性（提示“未打开 AutoWiFi”或“已损坏”）。请在终端执行以下命令解除隔离后再双击打开：
  - > ```bash
  - > xattr -cr AutoWiFi.app
  - > open AutoWiFi.app
  - > ```
- `./auto-wifi --minimized`（或 `-m` / `-d`）：启动后仅驻留顶部菜单栏后台运行。
- `./auto-wifi --connect-once`（或 `-c`）：立即执行一次连接与认证后退出。
- `./auto-wifi --status`（或 `-s`）：检测当前无线连接与互联网/Portal 状态。
- `./auto-wifi --scan`：扫描并列出周围 Wi-Fi 网络。
- `./auto-wifi --help`（或 `-h`）：显示帮助信息。

### Linux (`auto-wifi`)
- `./auto-wifi`：启动交互式终端控制菜单（TUI）。
- `./auto-wifi --daemon`（或 `-d` / `-m`）：启动后台断线自动重连守护进程。
- `./auto-wifi --connect-once`（或 `-c`）：立即执行一次连接与认证后退出。
- `./auto-wifi --status`（或 `-s`）：检测当前无线连接与互联网/Portal 状态。
- `./auto-wifi --scan`：扫描并列出周围 Wi-Fi 网络。
- `./auto-wifi --config`：运行交互式账号密码与 Portal 配置向导。
- `./auto-wifi --disconnect`：断开当前 Wi-Fi 连接。

## 编译指南

### 一键编译（本机原生构建 + Windows 交叉编译）

```bash
# 同时构建当前平台可执行文件 (dist/auto-wifi) 与 Windows 可执行文件 (dist/auto-wifi.exe)
./scripts/build.sh

# 仅构建当前系统 (macOS / Linux) 原生产物
./scripts/build.sh native

# 仅交叉编译 Windows x64 产物 (需安装 mingw-w64)
./scripts/build.sh windows
```

## 开源许可 (License)

本项目基于 **GNU Affero General Public License v3.0 (AGPL-3.0)** 开源，详见 [LICENSE](LICENSE) 文件。

