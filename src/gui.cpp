#include "gui.h"
#include "config.h"
#include "portal_auth.h"
#include "utils.h"
#include "wifi_manager.h"
#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

const wchar_t* kWindowClassName = L"AutoWiFiMainWindowClass";
const wchar_t* kWindowTitle = L"AutoWiFi - Windows 自动认证连接助手 (802.1X & Portal)";

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND hWnd = nullptr;
    HFONT hFont = nullptr;
    HFONT hBoldFont = nullptr;
    NOTIFYICONDATAW nid = {};
    bool tray_added = false;
    bool first_minimize_notified = false;

    // Controls
    HWND hComboSsid = nullptr;
    HWND hBtnScan = nullptr;
    HWND hComboAuthMode = nullptr;
    HWND hEditUsername = nullptr;
    HWND hEditPassword = nullptr;
    HWND hChkShowPwd = nullptr;
    HWND hLblDomainOrPsk = nullptr;
    HWND hEditDomainOrPsk = nullptr;

    HWND hEditPortalUrl = nullptr;
    HWND hComboPortalMethod = nullptr;
    HWND hEditUserField = nullptr;
    HWND hEditPassField = nullptr;
    HWND hEditExtraParams = nullptr;

    HWND hChkAutoConnect = nullptr;
    HWND hEditInterval = nullptr;
    HWND hChkAutoStart = nullptr;

    HWND hBtnConnect = nullptr;
    HWND hBtnDisconnect = nullptr;
    HWND hBtnTestNet = nullptr;
    HWND hBtnSave = nullptr;
    HWND hBtnClearLog = nullptr;

    HWND hEditLog = nullptr;
    HWND hLblStatus = nullptr;

    // Threading & synchronization
    std::mutex cfg_mutex;
    AppConfig config;

    std::atomic<bool> running{true};
    std::atomic<bool> busy_op{false};
    std::mutex cv_mutex;
    std::condition_variable cv;
    std::thread monitor_thread;
};

AppState g_app;

void PostLog(const std::wstring& msg) {
    if (!g_app.hWnd) return;
    std::wstring* heap_msg = new std::wstring(utils::CurrentTimeStr() + L" " + msg + L"\r\n");
    if (!PostMessageW(g_app.hWnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(heap_msg))) {
        delete heap_msg;
    }
}

void PostStatus(const std::wstring& status_text) {
    if (!g_app.hWnd) return;
    std::wstring* heap_status = new std::wstring(status_text);
    if (!PostMessageW(g_app.hWnd, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(heap_status))) {
        delete heap_status;
    }
}

std::wstring GetControlText(HWND hCtrl) {
    int len = GetWindowTextLengthW(hCtrl);
    if (len <= 0) return L"";
    std::wstring s(len, L'\0');
    GetWindowTextW(hCtrl, &s[0], len + 1);
    return utils::Trim(s);
}

void ReadConfigFromUi(AppConfig& cfg) {
    // Extract SSID (strip any signal/type suffix like "MyWiFi  (92% | 802.1X 企业认证)")
    std::wstring raw_ssid = GetControlText(g_app.hComboSsid);
    size_t suffix_pos = raw_ssid.rfind(L"  (");
    if (suffix_pos != std::wstring::npos && !raw_ssid.empty() && raw_ssid.back() == L')') {
        raw_ssid = raw_ssid.substr(0, suffix_pos);
    }
    cfg.ssid = raw_ssid;

    int mode_idx = (int)SendMessageW(g_app.hComboAuthMode, CB_GETCURSEL, 0, 0);
    if (mode_idx < 0 || mode_idx > 3) mode_idx = 0;
    cfg.auth_mode = static_cast<AuthMode>(mode_idx);

    cfg.username = GetControlText(g_app.hEditUsername);
    cfg.password = GetControlText(g_app.hEditPassword);
    cfg.domain_or_psk = GetControlText(g_app.hEditDomainOrPsk);

    cfg.portal_url = GetControlText(g_app.hEditPortalUrl);
    int method_idx = (int)SendMessageW(g_app.hComboPortalMethod, CB_GETCURSEL, 0, 0);
    if (method_idx < 0 || method_idx > 2) method_idx = 0;
    cfg.portal_method = static_cast<PortalMethod>(method_idx);

    cfg.portal_user_field = GetControlText(g_app.hEditUserField);
    if (cfg.portal_user_field.empty()) cfg.portal_user_field = L"username";
    cfg.portal_pass_field = GetControlText(g_app.hEditPassField);
    if (cfg.portal_pass_field.empty()) cfg.portal_pass_field = L"password";
    cfg.portal_extra_params = GetControlText(g_app.hEditExtraParams);

    cfg.auto_connect = (SendMessageW(g_app.hChkAutoConnect, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring interval_str = GetControlText(g_app.hEditInterval);
    int interval = _wtoi(interval_str.c_str());
    if (interval < 3) interval = 3;
    if (interval > 3600) interval = 3600;
    cfg.check_interval_sec = interval;

    cfg.auto_start = (SendMessageW(g_app.hChkAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

void UpdateControlStatesByAuthMode() {
    int mode_idx = (int)SendMessageW(g_app.hComboAuthMode, CB_GETCURSEL, 0, 0);
    AuthMode mode = static_cast<AuthMode>(mode_idx >= 0 ? mode_idx : 0);

    bool need_portal = (mode == AuthMode::PORTAL_OPEN ||
                        mode == AuthMode::PORTAL_PSK ||
                        mode == AuthMode::ENT_PLUS_PORTAL);

    if (mode == AuthMode::PORTAL_PSK) {
        SetWindowTextW(g_app.hLblDomainOrPsk, L"Wi-Fi连接密码:");
        EnableWindow(g_app.hEditDomainOrPsk, TRUE);
    } else if (mode == AuthMode::PORTAL_OPEN) {
        SetWindowTextW(g_app.hLblDomainOrPsk, L"域 / 附加项:");
        EnableWindow(g_app.hEditDomainOrPsk, FALSE);
    } else {
        SetWindowTextW(g_app.hLblDomainOrPsk, L"认证域(选填):");
        EnableWindow(g_app.hEditDomainOrPsk, TRUE);
    }

    EnableWindow(g_app.hEditPortalUrl, need_portal ? TRUE : FALSE);
    EnableWindow(g_app.hComboPortalMethod, need_portal ? TRUE : FALSE);
    EnableWindow(g_app.hEditUserField, need_portal ? TRUE : FALSE);
    EnableWindow(g_app.hEditPassField, need_portal ? TRUE : FALSE);
    EnableWindow(g_app.hEditExtraParams, need_portal ? TRUE : FALSE);
}

void ApplyConfigToUi(const AppConfig& cfg) {
    SetWindowTextW(g_app.hComboSsid, cfg.ssid.c_str());
    SendMessageW(g_app.hComboAuthMode, CB_SETCURSEL, static_cast<WPARAM>(cfg.auth_mode), 0);
    SetWindowTextW(g_app.hEditUsername, cfg.username.c_str());
    SetWindowTextW(g_app.hEditPassword, cfg.password.c_str());
    SetWindowTextW(g_app.hEditDomainOrPsk, cfg.domain_or_psk.c_str());

    SetWindowTextW(g_app.hEditPortalUrl, cfg.portal_url.c_str());
    SendMessageW(g_app.hComboPortalMethod, CB_SETCURSEL, static_cast<WPARAM>(cfg.portal_method), 0);
    SetWindowTextW(g_app.hEditUserField, cfg.portal_user_field.c_str());
    SetWindowTextW(g_app.hEditPassField, cfg.portal_pass_field.c_str());
    SetWindowTextW(g_app.hEditExtraParams, cfg.portal_extra_params.c_str());

    SendMessageW(g_app.hChkAutoConnect, BM_SETCHECK, cfg.auto_connect ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_app.hEditInterval, std::to_wstring(cfg.check_interval_sec).c_str());
    SendMessageW(g_app.hChkAutoStart, BM_SETCHECK, cfg.auto_start ? BST_CHECKED : BST_UNCHECKED, 0);

    UpdateControlStatesByAuthMode();
}

// Perform full connection & authentication flow given an AppConfig
bool ExecuteConnectAndAuth(const AppConfig& cfg, bool is_auto_check) {
    if (cfg.ssid.empty()) {
        if (!is_auto_check) PostLog(L"[错误] 请先选择或输入目标 Wi-Fi 名称 (SSID)");
        return false;
    }

    WifiConnectionState wifi_st = wifi::GetConnectionState();
    bool already_on_ssid = (wifi_st.is_connected && wifi_st.current_ssid == cfg.ssid);

    // Check internet state if already connected to target SSID
    std::wstring redirect_url;
    if (already_on_ssid) {
        NetStatus net = portal::CheckInternetState(redirect_url);
        if (net == NetStatus::ONLINE) {
            std::wstring status_msg = L"状态: 已连接 [" + cfg.ssid + L"] (信号 " +
                                      std::to_wstring(wifi_st.signal_quality) + L"%) | 互联网正常";
            PostStatus(status_msg);
            if (!is_auto_check) {
                PostLog(L"[正常] 当前已连接至 [" + cfg.ssid + L"] 且外网访问正常");
            }
            return true;
        }
        if (is_auto_check) {
            PostLog(L"[检测] 热点 [" + cfg.ssid + L"] 已连接，但外网未认证或掉线，开始自动认证...");
        }
    } else {
        // Step 1: Connect Wi-Fi layer
        PostStatus(L"状态: 正在连接 Wi-Fi [" + cfg.ssid + L"] ...");
        PostLog(L"[连接] 正在发起 Wi-Fi 连接: [" + cfg.ssid + L"]");

        std::wstring wifi_msg;
        bool wifi_ok = false;
        if (cfg.auth_mode == AuthMode::WPA2_ENT_PEAP || cfg.auth_mode == AuthMode::ENT_PLUS_PORTAL) {
            wifi_ok = wifi::ConnectEnterprise8021X(cfg.ssid, cfg.username, cfg.password,
                                                   cfg.domain_or_psk, wifi_msg);
        } else if (cfg.auth_mode == AuthMode::PORTAL_OPEN) {
            wifi_ok = wifi::ConnectOpenOrPsk(cfg.ssid, L"", true, wifi_msg);
        } else if (cfg.auth_mode == AuthMode::PORTAL_PSK) {
            wifi_ok = wifi::ConnectOpenOrPsk(cfg.ssid, cfg.domain_or_psk, false, wifi_msg);
        }

        PostLog(std::wstring(wifi_ok ? L"[成功] " : L"[失败] ") + wifi_msg);
        if (!wifi_ok) {
            PostStatus(L"状态: Wi-Fi 连接失败 - " + wifi_msg);
            return false;
        }
    }

    // Step 2: Check internet & perform Portal authentication if required
    bool need_portal = (cfg.auth_mode == AuthMode::PORTAL_OPEN ||
                        cfg.auth_mode == AuthMode::PORTAL_PSK ||
                        cfg.auth_mode == AuthMode::ENT_PLUS_PORTAL);

    NetStatus net_after = portal::CheckInternetState(redirect_url);
    if (net_after == NetStatus::ONLINE && (!need_portal || cfg.portal_url.empty())) {
        PostStatus(L"状态: 已连接 [" + cfg.ssid + L"] | 互联网已连通");
        PostLog(L"[完成] 网络已连通，无需额外网页认证");
        return true;
    }

    if (need_portal || net_after == NetStatus::CAPTIVE_PORTAL) {
        if (!redirect_url.empty()) {
            PostLog(L"[探测] 检测到 Captive Portal 重定向地址: " + redirect_url);
        }
        PostStatus(L"状态: 正在提交网页 Portal 账号密码认证...");
        PostLog(L"[认证] 正在以账号 [" + cfg.username + L"] 登录网页 Portal...");

        std::wstring portal_msg;
        bool portal_ok = portal::LoginCaptivePortal(cfg, redirect_url, portal_msg);
        PostLog(std::wstring(portal_ok ? L"[成功] " : L"[提示] ") + portal_msg);

        if (portal_ok) {
            PostStatus(L"状态: 已连接 [" + cfg.ssid + L"] | Portal 认证通过，外网正常");
            return true;
        } else {
            PostStatus(L"状态: 已关联 [" + cfg.ssid + L"] | Portal 认证未通过");
            return false;
        }
    }

    PostStatus(L"状态: 已连接 [" + cfg.ssid + L"]");
    return true;
}

void StartAsyncScan() {
    EnableWindow(g_app.hBtnScan, FALSE);
    PostLog(L"[扫描] 正在扫描周围无线网络...");
    std::thread([]() {
        auto list = wifi::ScanNetworks(true);
        auto* heap_list = new std::vector<WifiNetworkInfo>(std::move(list));
        if (!PostMessageW(g_app.hWnd, WM_APP_SCAN_DONE, 0, reinterpret_cast<LPARAM>(heap_list))) {
            delete heap_list;
        }
    }).detach();
}

void StartAsyncConnect() {
    if (g_app.busy_op.exchange(true)) {
        PostLog(L"[提示] 已有连接任务正在执行中，请稍候...");
        return;
    }
    EnableWindow(g_app.hBtnConnect, FALSE);

    AppConfig snap;
    {
        std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
        ReadConfigFromUi(g_app.config);
        snap = g_app.config;
    }
    config::SaveConfig(snap);

    std::thread([snap]() {
        ExecuteConnectAndAuth(snap, false);
        g_app.busy_op = false;
        PostMessageW(g_app.hWnd, WM_APP_OP_DONE, 0, 0);
    }).detach();
}

void StartAsyncDisconnect() {
    if (g_app.busy_op.exchange(true)) return;
    std::thread([]() {
        // Temporarily pause auto-connect so user manual disconnect isn't immediately overridden
        std::wstring msg;
        bool ok = wifi::Disconnect(msg);
        PostLog(std::wstring(ok ? L"[断开] " : L"[错误] ") + msg);
        PostStatus(L"状态: 已手动断开 Wi-Fi 连接");
        g_app.busy_op = false;
        PostMessageW(g_app.hWnd, WM_APP_OP_DONE, 0, 0);
    }).detach();
}

void StartAsyncTestNet() {
    std::thread([]() {
        WifiConnectionState st = wifi::GetConnectionState();
        if (!st.has_adapter) {
            PostLog(L"[检测] 未检测到无线网卡");
            PostStatus(L"状态: 未检测到无线网卡");
            return;
        }
        if (!st.is_connected) {
            PostLog(L"[检测] 网卡 [" + st.adapter_name + L"] 当前未连接任何 Wi-Fi");
            PostStatus(L"状态: 未连接 Wi-Fi");
            return;
        }
        std::wstring redirect_url;
        NetStatus net = portal::CheckInternetState(redirect_url);
        if (net == NetStatus::ONLINE) {
            PostLog(L"[检测] 当前连接 [" + st.current_ssid + L"] (信号 " +
                    std::to_wstring(st.signal_quality) + L"%) -> 外网访问正常");
            PostStatus(L"状态: 已连接 [" + st.current_ssid + L"] | 互联网正常");
        } else if (net == NetStatus::CAPTIVE_PORTAL) {
            PostLog(L"[检测] 当前连接 [" + st.current_ssid + L"] -> 需要网页 Portal 认证 (重定向: " +
                    (redirect_url.empty() ? L"未知" : redirect_url) + L")");
            PostStatus(L"状态: 已连接 [" + st.current_ssid + L"] | 等待网页认证");
        } else {
            PostLog(L"[检测] 当前连接 [" + st.current_ssid + L"] -> 无法访问外网");
            PostStatus(L"状态: 已连接 [" + st.current_ssid + L"] | 无互联网连接");
        }
    }).detach();
}

void MonitorLoop() {
    while (g_app.running) {
        int wait_sec = 15;
        bool auto_enabled = false;
        AppConfig snap;
        {
            std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
            snap = g_app.config;
            wait_sec = snap.check_interval_sec;
            auto_enabled = snap.auto_connect;
        }
        if (wait_sec < 3) wait_sec = 3;

        std::unique_lock<std::mutex> ulk(g_app.cv_mutex);
        g_app.cv.wait_for(ulk, std::chrono::seconds(wait_sec), []() { return !g_app.running.load(); });
        if (!g_app.running) break;

        if (auto_enabled && !snap.ssid.empty() && !g_app.busy_op.exchange(true)) {
            ExecuteConnectAndAuth(snap, true);
            g_app.busy_op = false;
        }
    }
}

void InitTrayIcon(HWND hWnd) {
    g_app.nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_app.nid.hWnd = hWnd;
    g_app.nid.uID = 1;
    g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_TRAYICON;
    g_app.nid.hIcon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_app.nid.hIcon) {
        g_app.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(g_app.nid.szTip, L"AutoWiFi - 自动认证守护中");
    g_app.tray_added = (Shell_NotifyIconW(NIM_ADD, &g_app.nid) != FALSE);
}

void ShowTrayBalloon(const wchar_t* title, const wchar_t* msg) {
    if (!g_app.tray_added) return;
    g_app.nid.uFlags = NIF_INFO;
    wcscpy_s(g_app.nid.szInfoTitle, title);
    wcscpy_s(g_app.nid.szInfo, msg);
    g_app.nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_app.nid);
}

void RemoveTrayIcon() {
    if (g_app.tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.nid);
        g_app.tray_added = false;
    }
}

void ShowTrayContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"打开主界面");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CONNECT, L"立即重连并认证");

    bool auto_conn = false;
    {
        std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
        auto_conn = g_app.config.auto_connect;
    }
    AppendMenuW(hMenu, MF_STRING | (auto_conn ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_AUTO, L"启用断线自动重连");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"退出程序");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

HWND CreateCtrl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, HWND parent, int id, DWORD ex_style = 0) {
    HWND ctrl = CreateWindowExW(ex_style, cls, text, style | WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                g_app.hInst, nullptr);
    if (ctrl && g_app.hFont) {
        SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hFont), TRUE);
    }
    return ctrl;
}

void CreateGuiControls(HWND hWnd) {
    g_app.hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    g_app.hBoldFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

    // Group 1: Wi-Fi & Account Credentials
    CreateCtrl(L"BUTTON", L"1. Wi-Fi 与账号认证设置", BS_GROUPBOX, 14, 10, 552, 182, hWnd, 0);

    CreateCtrl(L"STATIC", L"目标 Wi-Fi (SSID):", 0, 28, 36, 115, 22, hWnd, 0);
    g_app.hComboSsid = CreateCtrl(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                                  148, 33, 315, 220, hWnd, IDC_COMBO_SSID);
    g_app.hBtnScan = CreateCtrl(L"BUTTON", L"刷新扫描", BS_PUSHBUTTON | WS_TABSTOP,
                                472, 32, 80, 26, hWnd, IDC_BTN_SCAN);

    CreateCtrl(L"STATIC", L"认证方式:", 0, 28, 68, 115, 22, hWnd, 0);
    g_app.hComboAuthMode = CreateCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                      148, 65, 404, 160, hWnd, IDC_COMBO_AUTH_MODE);
    SendMessageW(g_app.hComboAuthMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"802.1X 企业级认证 (PEAP-MSCHAPv2 账号密码连接)"));
    SendMessageW(g_app.hComboAuthMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"网页 Portal 认证 (开放热点 + 网页账号密码登录)"));
    SendMessageW(g_app.hComboAuthMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"网页 Portal 认证 (WPA2密码连接 + 网页账号密码登录)"));
    SendMessageW(g_app.hComboAuthMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"双重认证 (802.1X 企业认证 + 网页 Portal 登录)"));

    CreateCtrl(L"STATIC", L"登录账号:", 0, 28, 100, 115, 22, hWnd, 0);
    g_app.hEditUsername = CreateCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                                     148, 97, 404, 24, hWnd, IDC_EDIT_USERNAME, WS_EX_CLIENTEDGE);

    CreateCtrl(L"STATIC", L"登录密码:", 0, 28, 132, 115, 22, hWnd, 0);
    g_app.hEditPassword = CreateCtrl(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP,
                                     148, 129, 315, 24, hWnd, IDC_EDIT_PASSWORD, WS_EX_CLIENTEDGE);
    g_app.hChkShowPwd = CreateCtrl(L"BUTTON", L"显示密码", BS_AUTOCHECKBOX | WS_TABSTOP,
                                   472, 131, 80, 22, hWnd, IDC_CHK_SHOW_PWD);

    g_app.hLblDomainOrPsk = CreateCtrl(L"STATIC", L"认证域(选填):", 0, 28, 164, 115, 22, hWnd, IDC_LBL_DOMAIN_OR_PSK);
    g_app.hEditDomainOrPsk = CreateCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                                        148, 161, 404, 24, hWnd, IDC_EDIT_DOMAIN_OR_PSK, WS_EX_CLIENTEDGE);

    // Group 2: Captive Portal Settings
    CreateCtrl(L"BUTTON", L"2. 网页 Captive Portal 参数配置 (仅网页认证模式启用)", BS_GROUPBOX, 14, 200, 552, 146, hWnd, 0);

    CreateCtrl(L"STATIC", L"Portal 登录URL:", 0, 28, 226, 115, 22, hWnd, 0);
    g_app.hEditPortalUrl = CreateCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                                      148, 223, 404, 24, hWnd, IDC_EDIT_PORTAL_URL, WS_EX_CLIENTEDGE);
    SendMessageW(g_app.hEditPortalUrl, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"留空则自动探测重定向地址，或填如 http://10.10.10.1/login"));

    CreateCtrl(L"STATIC", L"HTTP 请求方式:", 0, 28, 258, 115, 22, hWnd, 0);
    g_app.hComboPortalMethod = CreateCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                          148, 255, 165, 120, hWnd, IDC_COMBO_PORTAL_METHOD);
    SendMessageW(g_app.hComboPortalMethod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"POST 表单 (urlencoded)"));
    SendMessageW(g_app.hComboPortalMethod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"POST JSON"));
    SendMessageW(g_app.hComboPortalMethod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GET 查询参数"));

    CreateCtrl(L"STATIC", L"账号字段:", 0, 325, 258, 62, 22, hWnd, 0);
    g_app.hEditUserField = CreateCtrl(L"EDIT", L"username", ES_AUTOHSCROLL | WS_TABSTOP,
                                      390, 255, 65, 24, hWnd, IDC_EDIT_USER_FIELD, WS_EX_CLIENTEDGE);
    CreateCtrl(L"STATIC", L"密码字段:", 0, 462, 258, 62, 22, hWnd, 0);
    g_app.hEditPassField = CreateCtrl(L"EDIT", L"password", ES_AUTOHSCROLL | WS_TABSTOP,
                                      524, 255, 28, 24, hWnd, IDC_EDIT_PASS_FIELD, WS_EX_CLIENTEDGE);
    // Adjust field widths cleanly
    MoveWindow(g_app.hComboPortalMethod, 148, 255, 150, 120, TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_EDIT_USER_FIELD), 365, 255, 68, 24, TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_EDIT_PASS_FIELD), 494, 255, 58, 24, TRUE);

    CreateCtrl(L"STATIC", L"附加请求参数:", 0, 28, 290, 115, 22, hWnd, 0);
    g_app.hEditExtraParams = CreateCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                                        148, 287, 404, 24, hWnd, IDC_EDIT_EXTRA_PARAMS, WS_EX_CLIENTEDGE);
    SendMessageW(g_app.hEditExtraParams, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"选填，例如: ac_id=1&0MKKey=123456"));

    CreateCtrl(L"STATIC", L"提示: URL与附加参数支持 {username}、{password} 占位符；密码使用 Windows DPAPI 加密保存。",
               0, 28, 318, 524, 20, hWnd, 0);

    // Group 3: Automation & Tray Settings
    CreateCtrl(L"BUTTON", L"3. 自动化与守护设置", BS_GROUPBOX, 14, 354, 552, 60, hWnd, 0);

    g_app.hChkAutoConnect = CreateCtrl(L"BUTTON", L"启用断线/掉线自动重连", BS_AUTOCHECKBOX | WS_TABSTOP,
                                       28, 378, 175, 24, hWnd, IDC_CHK_AUTO_CONNECT);
    CreateCtrl(L"STATIC", L"检测间隔(秒):", 0, 212, 381, 88, 22, hWnd, 0);
    g_app.hEditInterval = CreateCtrl(L"EDIT", L"15", ES_NUMBER | ES_CENTER | WS_TABSTOP,
                                     302, 378, 48, 24, hWnd, IDC_EDIT_INTERVAL, WS_EX_CLIENTEDGE);
    g_app.hChkAutoStart = CreateCtrl(L"BUTTON", L"开机自动启动 (驻留系统托盘)", BS_AUTOCHECKBOX | WS_TABSTOP,
                                     368, 378, 190, 24, hWnd, IDC_CHK_AUTO_START);

    // Action Buttons
    g_app.hBtnConnect = CreateCtrl(L"BUTTON", L"立即连接并认证", BS_DEFPUSHBUTTON | WS_TABSTOP,
                                   14, 424, 132, 34, hWnd, IDC_BTN_CONNECT);
    if (g_app.hBoldFont) {
        SendMessageW(g_app.hBtnConnect, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.hBoldFont), TRUE);
    }
    g_app.hBtnDisconnect = CreateCtrl(L"BUTTON", L"断开连接", BS_PUSHBUTTON | WS_TABSTOP,
                                      156, 424, 96, 34, hWnd, IDC_BTN_DISCONNECT);
    g_app.hBtnTestNet = CreateCtrl(L"BUTTON", L"检测网络状态", BS_PUSHBUTTON | WS_TABSTOP,
                                   262, 424, 108, 34, hWnd, IDC_BTN_TEST_NET);
    g_app.hBtnSave = CreateCtrl(L"BUTTON", L"保存配置", BS_PUSHBUTTON | WS_TABSTOP,
                                380, 424, 92, 34, hWnd, IDC_BTN_SAVE);
    g_app.hBtnClearLog = CreateCtrl(L"BUTTON", L"清空日志", BS_PUSHBUTTON | WS_TABSTOP,
                                    482, 424, 84, 34, hWnd, IDC_BTN_CLEAR_LOG);

    // Log Output
    CreateCtrl(L"BUTTON", L"运行日志", BS_GROUPBOX, 14, 466, 552, 156, hWnd, 0);
    g_app.hEditLog = CreateCtrl(L"EDIT", L"",
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                24, 488, 532, 124, hWnd, IDC_EDIT_LOG, WS_EX_CLIENTEDGE);

    // Status bar label
    g_app.hLblStatus = CreateCtrl(L"STATIC", L"状态: 就绪", SS_LEFTNOWORDWRAP,
                                  16, 630, 548, 22, hWnd, IDC_LBL_STATUS);
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_app.hWnd = hWnd;
            CreateGuiControls(hWnd);
            InitTrayIcon(hWnd);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_COMBO_AUTH_MODE && code == CBN_SELCHANGE) {
                UpdateControlStatesByAuthMode();
                return 0;
            }
            if (id == IDC_CHK_SHOW_PWD && code == BN_CLICKED) {
                bool show = (SendMessageW(g_app.hChkShowPwd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SendMessageW(g_app.hEditPassword, EM_SETPASSWORDCHAR, show ? 0 : L'*', 0);
                InvalidateRect(g_app.hEditPassword, nullptr, TRUE);
                return 0;
            }
            if (id == IDC_BTN_SCAN && code == BN_CLICKED) {
                StartAsyncScan();
                return 0;
            }
            if (id == IDC_BTN_CONNECT && code == BN_CLICKED) {
                StartAsyncConnect();
                return 0;
            }
            if (id == IDC_BTN_DISCONNECT && code == BN_CLICKED) {
                StartAsyncDisconnect();
                return 0;
            }
            if (id == IDC_BTN_TEST_NET && code == BN_CLICKED) {
                StartAsyncTestNet();
                return 0;
            }
            if (id == IDC_BTN_SAVE && code == BN_CLICKED) {
                AppConfig snap;
                {
                    std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
                    ReadConfigFromUi(g_app.config);
                    snap = g_app.config;
                }
                config::SaveConfig(snap);
                g_app.cv.notify_all();
                PostLog(L"[配置] 配置已加密保存至: " + config::GetConfigFilePath());
                return 0;
            }
            if (id == IDC_BTN_CLEAR_LOG && code == BN_CLICKED) {
                SetWindowTextW(g_app.hEditLog, L"");
                return 0;
            }

            // Tray Menu Commands
            if (id == IDM_TRAY_OPEN) {
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                return 0;
            }
            if (id == IDM_TRAY_CONNECT) {
                StartAsyncConnect();
                return 0;
            }
            if (id == IDM_TRAY_TOGGLE_AUTO) {
                bool new_state = false;
                {
                    std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
                    g_app.config.auto_connect = !g_app.config.auto_connect;
                    new_state = g_app.config.auto_connect;
                    SendMessageW(g_app.hChkAutoConnect, BM_SETCHECK,
                                 new_state ? BST_CHECKED : BST_UNCHECKED, 0);
                    config::SaveConfig(g_app.config);
                }
                g_app.cv.notify_all();
                PostLog(new_state ? L"[设置] 已开启断线自动重连" : L"[设置] 已暂停断线自动重连");
                return 0;
            }
            if (id == IDM_TRAY_EXIT) {
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }

        case WM_APP_LOG: {
            std::wstring* pMsg = reinterpret_cast<std::wstring*>(lParam);
            if (pMsg) {
                int len = GetWindowTextLengthW(g_app.hEditLog);
                if (len > 24000) {
                    SetWindowTextW(g_app.hEditLog, L"");
                    len = 0;
                }
                SendMessageW(g_app.hEditLog, EM_SETSEL, len, len);
                SendMessageW(g_app.hEditLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(pMsg->c_str()));
                delete pMsg;
            }
            return 0;
        }

        case WM_APP_STATUS: {
            std::wstring* pSt = reinterpret_cast<std::wstring*>(lParam);
            if (pSt) {
                SetWindowTextW(g_app.hLblStatus, pSt->c_str());
                if (g_app.tray_added) {
                    g_app.nid.uFlags = NIF_TIP;
                    std::wstring tip = L"AutoWiFi - " + *pSt;
                    if (tip.size() >= 127) tip.resize(126);
                    wcscpy_s(g_app.nid.szTip, tip.c_str());
                    Shell_NotifyIconW(NIM_MODIFY, &g_app.nid);
                }
                delete pSt;
            }
            return 0;
        }

        case WM_APP_SCAN_DONE: {
            EnableWindow(g_app.hBtnScan, TRUE);
            auto* pList = reinterpret_cast<std::vector<WifiNetworkInfo>*>(lParam);
            if (pList) {
                std::wstring current_text = GetControlText(g_app.hComboSsid);
                SendMessageW(g_app.hComboSsid, CB_RESETCONTENT, 0, 0);
                for (const auto& item : *pList) {
                    std::wstring label = item.ssid + L"  (" + std::to_wstring(item.signal_quality) +
                                         L"% | " + item.auth_desc + L")";
                    SendMessageW(g_app.hComboSsid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                }
                if (!current_text.empty()) {
                    SetWindowTextW(g_app.hComboSsid, current_text.c_str());
                } else if (!pList->empty()) {
                    SetWindowTextW(g_app.hComboSsid, (*pList)[0].ssid.c_str());
                }
                PostLog(L"[扫描] 扫描完成，共发现 " + std::to_wstring(pList->size()) + L" 个无线网络");
                delete pList;
            }
            return 0;
        }

        case WM_APP_OP_DONE: {
            EnableWindow(g_app.hBtnConnect, TRUE);
            return 0;
        }

        case WM_TRAYICON: {
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu(hWnd);
            }
            return 0;
        }

        case WM_CLOSE: {
            // Minimize to system tray instead of exiting
            ShowWindow(hWnd, SW_HIDE);
            if (!g_app.first_minimize_notified) {
                g_app.first_minimize_notified = true;
                ShowTrayBalloon(L"AutoWiFi 已最小化到托盘",
                                L"程序正在后台持续监控 Wi-Fi 状态，右键托盘图标可退出。");
            }
            return 0;
        }

        case WM_DESTROY: {
            g_app.running = false;
            g_app.cv.notify_all();
            RemoveTrayIcon();
            if (g_app.hFont) DeleteObject(g_app.hFont);
            if (g_app.hBoldFont) DeleteObject(g_app.hBoldFont);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}  // namespace

int RunGuiApp(HINSTANCE hInstance, bool start_minimized) {
    g_app.hInst = hInstance;

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClassName;
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"注册窗口类失败！", L"AutoWiFi 错误", MB_ICONERROR);
        return 1;
    }

    int win_w = 596;
    int win_h = 696;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - win_w) / 2;
    int pos_y = (screen_h - win_h) / 2;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND hWnd = CreateWindowExW(0, kWindowClassName, kWindowTitle, style,
                                pos_x, pos_y, win_w, win_h,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        MessageBoxW(nullptr, L"创建主窗口失败！", L"AutoWiFi 错误", MB_ICONERROR);
        return 1;
    }

    // Load saved configuration
    {
        std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
        g_app.config = config::LoadConfig();
        ApplyConfigToUi(g_app.config);
    }

    PostLog(L"[启动] AutoWiFi 已就绪，配置文件路径: " + config::GetConfigFilePath());
    StartAsyncScan();
    StartAsyncTestNet();

    // If auto-connect is enabled and SSID is configured, trigger initial check
    {
        std::lock_guard<std::mutex> lk(g_app.cfg_mutex);
        if (g_app.config.auto_connect && !g_app.config.ssid.empty()) {
            StartAsyncConnect();
        }
    }

    g_app.monitor_thread = std::thread(MonitorLoop);

    if (start_minimized) {
        ShowWindow(hWnd, SW_HIDE);
    } else {
        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_app.running = false;
    g_app.cv.notify_all();
    if (g_app.monitor_thread.joinable()) {
        g_app.monitor_thread.join();
    }

    return static_cast<int>(msg.wParam);
}

int RunConnectOnceCli() {
    AppConfig cfg = config::LoadConfig();
    if (cfg.ssid.empty()) {
        return 2;
    }
    bool ok = ExecuteConnectAndAuth(cfg, false);
    return ok ? 0 : 1;
}
