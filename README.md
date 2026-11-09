# AutoWiFi - Windows 自动认证 Wi-Fi 连接客户端

使用 C++ (Win32 API / Native Wifi API / WinHTTP / DPAPI) 编写的轻量级 Windows 自动连接与认证工具，支持 **WPA2/WPA3-Enterprise (802.1X PEAP-MSCHAPv2)** 企业级无线认证与 **Captive Portal (校园网/企业网网页认证)**，具备图形界面、系统托盘常驻、断线自动重连与开机自启功能。

## 核心功能

1. **多模式账号密码认证支持**
   - **802.1X 企业级认证 (`PEAP-MSCHAPv2`)**：通过 Windows 原生 `wlanapi` 自动下发 `WLANProfile` 与 `EapHostUserCredentials` 凭据（自动禁用首次连接证书弹窗拦截），实现无人值守直接连接企业/校园 802.1X Wi-Fi。
   - **网页 Captive Portal 认证（开放热点 / WPA2-PSK 热点）**：自动关联热点并通过 `WinHTTP` 探测重定向地址（或使用自定义认证接口），支持 `POST 表单 (application/x-www-form-urlencoded)`、`POST JSON`、`GET 参数` 提交账号密码。
   - **双重认证模式（802.1X + 网页 Portal）**：先完成 802.1X 无线层接入，再自动完成网页 Portal 登录。
2. **Win32 图形界面与系统托盘常驻**
   - 一键扫描周围可用 Wi-Fi 信号强度与加密类型。
   - 关闭窗口自动最小化至系统托盘，右键托盘菜单支持快速重连、开关自动重连、退出程序。
3. **后台断线检测与自动重连**
   - 后台守护线程按设定间隔（默认 15 秒）检测无线网卡连接状态与外网连通性（`msftconnecttest`），发现掉线或被 Portal 拦截时自动重连并重新认证。
4. **安全存储与便携模式**
   - 密码采用 Windows **DPAPI (`CryptProtectData`)** 加密存储在 `%APPDATA%\AutoWiFi\config.ini`（若程序同级目录存在 `config.ini` 则优先使用便携模式）。
   - 支持写入当前用户注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 实现开机自动最小化启动。

## 编译产物

直接运行位于 `dist/auto-wifi.exe` 的独立可执行文件（已静态链接 C/C++ 运行库，无需安装额外依赖，支持 Windows 10 / 11 x64）。

### 命令行参数

- `auto-wifi.exe`：启动图形主界面。
- `auto-wifi.exe --minimized`（或 `--tray` / `-m`）：启动后直接最小化到系统托盘后台运行。
- `auto-wifi.exe --connect-once`（或 `-c`）：读取配置执行一次连接与认证后立即退出（适合配合计划任务或脚本调用）。

## 自行编译指南

### 在 macOS / Linux 下交叉编译 Windows 产物

需安装 `mingw-w64` 与 `cmake`（macOS 可通过 `brew install mingw-w64 cmake` 安装）：

```bash
./scripts/build.sh
```

编译完成后，产物输出至 `dist/auto-wifi.exe`。
