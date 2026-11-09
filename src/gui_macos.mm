#ifdef __APPLE__

#include "gui.h"
#include "config.h"
#include "portal_auth.h"
#include "utils.h"
#include "wifi_manager.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

@interface AutoWiFiFlippedView : NSView
@end

@implementation AutoWiFiFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

@interface AutoWiFiAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) NSMenuItem* menuAutoToggle;

// Controls
@property(nonatomic, strong) NSComboBox* comboSsid;
@property(nonatomic, strong) NSButton* btnScan;
@property(nonatomic, strong) NSPopUpButton* popupAuthMode;
@property(nonatomic, strong) NSTextField* editUsername;
@property(nonatomic, strong) NSSecureTextField* editPasswordSecure;
@property(nonatomic, strong) NSTextField* editPasswordPlain;
@property(nonatomic, strong) NSButton* chkShowPwd;
@property(nonatomic, strong) NSTextField* lblDomainOrPsk;
@property(nonatomic, strong) NSTextField* editDomainOrPsk;

@property(nonatomic, strong) NSTextField* editPortalUrl;
@property(nonatomic, strong) NSPopUpButton* popupPortalMethod;
@property(nonatomic, strong) NSTextField* editUserField;
@property(nonatomic, strong) NSTextField* editPassField;
@property(nonatomic, strong) NSTextField* editExtraParams;

@property(nonatomic, strong) NSButton* chkAutoConnect;
@property(nonatomic, strong) NSTextField* editInterval;
@property(nonatomic, strong) NSButton* chkAutoStart;

@property(nonatomic, strong) NSButton* btnConnect;
@property(nonatomic, strong) NSButton* btnDisconnect;
@property(nonatomic, strong) NSButton* btnTestNet;
@property(nonatomic, strong) NSButton* btnSave;
@property(nonatomic, strong) NSButton* btnClearLog;

@property(nonatomic, strong) NSTextView* textLog;
@property(nonatomic, strong) NSTextField* lblStatus;

@property(nonatomic, assign) BOOL startMinimized;
@property(nonatomic, assign) BOOL forceTerminate;

- (void)appendLog:(const std::wstring&)msg;
- (void)updateStatus:(const std::wstring&)statusText;
@end

namespace {

struct MacGuiContext {
    std::mutex cfg_mutex;
    AppConfig config;
    std::atomic<bool> running{true};
    std::atomic<bool> busy_op{false};
    std::mutex cv_mutex;
    std::condition_variable cv;
    std::thread monitor_thread;
    __weak AutoWiFiAppDelegate* delegate = nil;
};

MacGuiContext g_mac;

NSString* ToNS(const std::wstring& w) {
    std::string u = utils::WideToUtf8(w);
    NSString* s = [NSString stringWithUTF8String:u.c_str()];
    return s ? s : @"";
}

std::wstring FromNS(NSString* s) {
    if (!s) return L"";
    const char* c = [s UTF8String];
    return c ? utils::Trim(utils::Utf8ToWide(c)) : L"";
}

void PostMacLog(const std::wstring& msg) {
    AutoWiFiAppDelegate* del = g_mac.delegate;
    if (del) {
        [del appendLog:msg];
    } else {
        std::cout << utils::WideToUtf8(utils::CurrentTimeStr()) << " "
                  << utils::WideToUtf8(msg) << std::endl;
    }
}

void PostMacStatus(const std::wstring& st) {
    AutoWiFiAppDelegate* del = g_mac.delegate;
    if (del) {
        [del updateStatus:st];
    }
}

bool ExecuteMacConnectAndAuth(const AppConfig& cfg, bool is_auto_check) {
    if (cfg.ssid.empty()) {
        if (!is_auto_check) PostMacLog(L"[错误] 请先选择或输入目标 Wi-Fi 名称 (SSID)");
        return false;
    }

    WifiConnectionState wifi_st = wifi::GetConnectionState();
    bool already_on_ssid = (wifi_st.is_connected && wifi_st.current_ssid == cfg.ssid);

    std::wstring redirect_url;
    if (already_on_ssid) {
        NetStatus net = portal::CheckInternetState(redirect_url);
        if (net == NetStatus::ONLINE) {
            std::wstring status_msg = L"状态: 已连接 [" + cfg.ssid + L"] (信号 " +
                                      std::to_wstring(wifi_st.signal_quality) + L"%) | 互联网正常";
            PostMacStatus(status_msg);
            if (!is_auto_check) {
                PostMacLog(L"[正常] 当前已连接至 [" + cfg.ssid + L"] 且外网访问正常");
            }
            return true;
        }
        if (is_auto_check) {
            PostMacLog(L"[检测] 热点 [" + cfg.ssid + L"] 已连接，但外网未认证或掉线，开始自动认证...");
        }
    } else {
        PostMacStatus(L"状态: 正在连接 Wi-Fi [" + cfg.ssid + L"] ...");
        PostMacLog(L"[连接] 正在发起 Wi-Fi 连接: [" + cfg.ssid + L"]");

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

        PostMacLog(std::wstring(wifi_ok ? L"[成功] " : L"[失败] ") + wifi_msg);
        if (!wifi_ok) {
            PostMacStatus(L"状态: Wi-Fi 连接失败 - " + wifi_msg);
            return false;
        }
    }

    bool need_portal = (cfg.auth_mode == AuthMode::PORTAL_OPEN ||
                        cfg.auth_mode == AuthMode::PORTAL_PSK ||
                        cfg.auth_mode == AuthMode::ENT_PLUS_PORTAL);

    NetStatus net_after = portal::CheckInternetState(redirect_url);
    if (net_after == NetStatus::ONLINE && (!need_portal || cfg.portal_url.empty())) {
        PostMacStatus(L"状态: 已连接 [" + cfg.ssid + L"] | 互联网已连通");
        PostMacLog(L"[完成] 网络已连通，无需额外网页认证");
        return true;
    }

    if (need_portal || net_after == NetStatus::CAPTIVE_PORTAL) {
        if (!redirect_url.empty()) {
            PostMacLog(L"[探测] 检测到 Captive Portal 重定向地址: " + redirect_url);
        }
        PostMacStatus(L"状态: 正在提交网页 Portal 账号密码认证...");
        PostMacLog(L"[认证] 正在以账号 [" + cfg.username + L"] 登录网页 Portal...");

        std::wstring portal_msg;
        bool portal_ok = portal::LoginCaptivePortal(cfg, redirect_url, portal_msg);
        PostMacLog(std::wstring(portal_ok ? L"[成功] " : L"[提示] ") + portal_msg);

        if (portal_ok) {
            PostMacStatus(L"状态: 已连接 [" + cfg.ssid + L"] | Portal 认证通过，外网正常");
            return true;
        } else {
            PostMacStatus(L"状态: 已关联 [" + cfg.ssid + L"] | Portal 认证未通过");
            return false;
        }
    }

    PostMacStatus(L"状态: 已连接 [" + cfg.ssid + L"]");
    return true;
}

void MacMonitorLoop() {
    while (g_mac.running) {
        int wait_sec = 15;
        bool auto_enabled = false;
        AppConfig snap;
        {
            std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
            snap = g_mac.config;
            wait_sec = snap.check_interval_sec;
            auto_enabled = snap.auto_connect;
        }
        if (wait_sec < 3) wait_sec = 3;

        std::unique_lock<std::mutex> ulk(g_mac.cv_mutex);
        g_mac.cv.wait_for(ulk, std::chrono::seconds(wait_sec), []() { return !g_mac.running.load(); });
        if (!g_mac.running) break;

        if (auto_enabled && !snap.ssid.empty() && !g_mac.busy_op.exchange(true)) {
            ExecuteMacConnectAndAuth(snap, true);
            g_mac.busy_op = false;
        }
    }
}

NSTextField* MakeLabel(NSString* text, NSRect frame, BOOL bold = NO) {
    NSTextField* lbl = [[NSTextField alloc] initWithFrame:frame];
    lbl.stringValue = text;
    lbl.bezeled = NO;
    lbl.drawsBackground = NO;
    lbl.editable = NO;
    lbl.selectable = NO;
    lbl.font = bold ? [NSFont boldSystemFontOfSize:13.0] : [NSFont systemFontOfSize:13.0];
    return lbl;
}

NSTextField* MakeInput(NSString* placeholder, NSRect frame) {
    NSTextField* tf = [[NSTextField alloc] initWithFrame:frame];
    tf.font = [NSFont systemFontOfSize:13.0];
    if (placeholder && placeholder.length > 0) {
        tf.placeholderString = placeholder;
    }
    return tf;
}

NSBox* MakeSeparator(CGFloat y, CGFloat width) {
    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(16, y, width, 1)];
    sep.boxType = NSBoxSeparator;
    return sep;
}

}  // namespace

@implementation AutoWiFiAppDelegate

- (void)setupMainMenu {
    NSMenu* mainBar = [[NSMenu alloc] init];
    [NSApp setMainMenu:mainBar];

    // Application Menu
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [mainBar addItem:appMenuItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"AutoWiFi"];
    [appMenu addItemWithTitle:@"打开主界面" action:@selector(onShowMainWindow:) keyEquivalent:@"o"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"退出 AutoWiFi" action:@selector(onQuitApp:) keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];

    // Edit Menu (enables Cmd+C, Cmd+V, Cmd+X, Cmd+A in NSTextField)
    NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
    [mainBar addItem:editMenuItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"编辑"];
    [editMenu addItemWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItemWithTitle:@"重做" action:@selector(redo:) keyEquivalent:@"Z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"拷贝" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editMenuItem setSubmenu:editMenu];
}

- (void)setupStatusBarItem {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    if (@available(macOS 11.0, *)) {
        NSImage* wifiImg = [NSImage imageWithSystemSymbolName:@"wifi" accessibilityDescription:@"AutoWiFi"];
        if (wifiImg) {
            [wifiImg setTemplate:YES];
            self.statusItem.button.image = wifiImg;
        } else {
            self.statusItem.button.title = @"AutoWiFi";
        }
    } else {
        self.statusItem.button.title = @"AutoWiFi";
    }
    self.statusItem.button.toolTip = @"AutoWiFi - 自动认证守护中";

    NSMenu* trayMenu = [[NSMenu alloc] initWithTitle:@"AutoWiFi"];
    NSMenuItem* itemOpen = [[NSMenuItem alloc] initWithTitle:@"打开主界面"
                                                      action:@selector(onShowMainWindow:)
                                               keyEquivalent:@""];
    itemOpen.target = self;
    [trayMenu addItem:itemOpen];

    NSMenuItem* itemConnect = [[NSMenuItem alloc] initWithTitle:@"立即重连并认证"
                                                         action:@selector(onConnectClicked:)
                                                  keyEquivalent:@""];
    itemConnect.target = self;
    [trayMenu addItem:itemConnect];

    self.menuAutoToggle = [[NSMenuItem alloc] initWithTitle:@"启用断线自动重连"
                                                     action:@selector(onTrayToggleAuto:)
                                              keyEquivalent:@""];
    self.menuAutoToggle.target = self;
    [trayMenu addItem:self.menuAutoToggle];

    [trayMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* itemQuit = [[NSMenuItem alloc] initWithTitle:@"退出程序"
                                                      action:@selector(onQuitApp:)
                                               keyEquivalent:@""];
    itemQuit.target = self;
    [trayMenu addItem:itemQuit];

    self.statusItem.menu = trayMenu;
}

- (void)setupWindowAndControls {
    NSRect frame = NSMakeRect(0, 0, 596, 668);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AutoWiFi - 自动认证连接助手 (802.1X & Portal)";
    self.window.delegate = self;
    [self.window center];

    AutoWiFiFlippedView* content = [[AutoWiFiFlippedView alloc] initWithFrame:frame];
    self.window.contentView = content;

    // Section 1: Wi-Fi & Account Credentials
    [content addSubview:MakeLabel(@"1. Wi-Fi 与账号认证设置", NSMakeRect(16, 12, 300, 20), YES)];
    [content addSubview:MakeSeparator(34, 564)];

    [content addSubview:MakeLabel(@"目标 Wi-Fi (SSID):", NSMakeRect(24, 46, 125, 22))];
    self.comboSsid = [[NSComboBox alloc] initWithFrame:NSMakeRect(152, 42, 326, 26)];
    self.comboSsid.usesDataSource = NO;
    self.comboSsid.editable = YES;
    self.comboSsid.completes = YES;
    [content addSubview:self.comboSsid];

    self.btnScan = [NSButton buttonWithTitle:@"刷新扫描" target:self action:@selector(onScanClicked:)];
    self.btnScan.frame = NSMakeRect(486, 40, 92, 28);
    [content addSubview:self.btnScan];

    [content addSubview:MakeLabel(@"认证方式:", NSMakeRect(24, 80, 125, 22))];
    self.popupAuthMode = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(152, 76, 424, 26) pullsDown:NO];
    [self.popupAuthMode addItemsWithTitles:@[
        @"802.1X 企业级认证 (PEAP-MSCHAPv2 账号密码连接)",
        @"网页 Portal 认证 (开放热点 + 网页账号密码登录)",
        @"网页 Portal 认证 (WPA2密码连接 + 网页账号密码登录)",
        @"双重认证 (802.1X 企业认证 + 网页 Portal 登录)"
    ]];
    self.popupAuthMode.target = self;
    self.popupAuthMode.action = @selector(onAuthModeChanged:);
    [content addSubview:self.popupAuthMode];

    [content addSubview:MakeLabel(@"登录账号:", NSMakeRect(24, 114, 125, 22))];
    self.editUsername = MakeInput(@"输入校园网/企业网账号", NSMakeRect(152, 110, 424, 24));
    [content addSubview:self.editUsername];

    [content addSubview:MakeLabel(@"登录密码:", NSMakeRect(24, 148, 125, 22))];
    self.editPasswordSecure = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(152, 144, 326, 24)];
    self.editPasswordSecure.font = [NSFont systemFontOfSize:13.0];
    [content addSubview:self.editPasswordSecure];

    self.editPasswordPlain = MakeInput(@"", NSMakeRect(152, 144, 326, 24));
    self.editPasswordPlain.hidden = YES;
    [content addSubview:self.editPasswordPlain];

    self.chkShowPwd = [NSButton checkboxWithTitle:@"显示密码" target:self action:@selector(onShowPwdToggled:)];
    self.chkShowPwd.frame = NSMakeRect(488, 145, 90, 22);
    [content addSubview:self.chkShowPwd];

    self.lblDomainOrPsk = MakeLabel(@"认证域(选填):", NSMakeRect(24, 182, 125, 22));
    [content addSubview:self.lblDomainOrPsk];
    self.editDomainOrPsk = MakeInput(@"选填", NSMakeRect(152, 178, 424, 24));
    [content addSubview:self.editDomainOrPsk];

    // Section 2: Captive Portal Settings
    [content addSubview:MakeLabel(@"2. 网页 Captive Portal 参数配置 (仅网页认证模式启用)", NSMakeRect(16, 216, 450, 20), YES)];
    [content addSubview:MakeSeparator(238, 564)];

    [content addSubview:MakeLabel(@"Portal 登录URL:", NSMakeRect(24, 250, 125, 22))];
    self.editPortalUrl = MakeInput(@"留空则自动探测重定向地址，或填如 http://10.10.10.1/login", NSMakeRect(152, 246, 424, 24));
    [content addSubview:self.editPortalUrl];

    [content addSubview:MakeLabel(@"HTTP 请求方式:", NSMakeRect(24, 284, 125, 22))];
    self.popupPortalMethod = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(152, 280, 165, 26) pullsDown:NO];
    [self.popupPortalMethod addItemsWithTitles:@[
        @"POST 表单 (urlencoded)",
        @"POST JSON",
        @"GET 查询参数"
    ]];
    [content addSubview:self.popupPortalMethod];

    [content addSubview:MakeLabel(@"账号字段:", NSMakeRect(326, 284, 64, 22))];
    self.editUserField = MakeInput(@"username", NSMakeRect(390, 280, 68, 24));
    [content addSubview:self.editUserField];

    [content addSubview:MakeLabel(@"密码字段:", NSMakeRect(466, 284, 64, 22))];
    self.editPassField = MakeInput(@"password", NSMakeRect(530, 280, 46, 24));
    [content addSubview:self.editPassField];

    [content addSubview:MakeLabel(@"附加请求参数:", NSMakeRect(24, 318, 125, 22))];
    self.editExtraParams = MakeInput(@"选填，例如: ac_id=1&0MKKey=123456 (支持 {username}/{password})", NSMakeRect(152, 314, 424, 24));
    [content addSubview:self.editExtraParams];

    // Section 3: Automation Settings
    [content addSubview:MakeLabel(@"3. 自动化与守护设置", NSMakeRect(16, 352, 300, 20), YES)];
    [content addSubview:MakeSeparator(374, 564)];

    self.chkAutoConnect = [NSButton checkboxWithTitle:@"启用断线/掉线自动重连" target:nil action:nil];
    self.chkAutoConnect.frame = NSMakeRect(24, 384, 185, 24);
    [content addSubview:self.chkAutoConnect];

    [content addSubview:MakeLabel(@"检测间隔(秒):", NSMakeRect(220, 386, 90, 22))];
    self.editInterval = MakeInput(@"15", NSMakeRect(310, 382, 52, 24));
    self.editInterval.alignment = NSTextAlignmentCenter;
    [content addSubview:self.editInterval];

    self.chkAutoStart = [NSButton checkboxWithTitle:@"开机自动启动 (常驻顶部菜单栏)" target:nil action:nil];
    self.chkAutoStart.frame = NSMakeRect(378, 384, 205, 24);
    [content addSubview:self.chkAutoStart];

    // Action Buttons
    self.btnConnect = [NSButton buttonWithTitle:@"立即连接并认证" target:self action:@selector(onConnectClicked:)];
    self.btnConnect.frame = NSMakeRect(16, 420, 138, 32);
    self.btnConnect.keyEquivalent = @"\r";
    [content addSubview:self.btnConnect];

    self.btnDisconnect = [NSButton buttonWithTitle:@"断开连接" target:self action:@selector(onDisconnectClicked:)];
    self.btnDisconnect.frame = NSMakeRect(162, 420, 96, 32);
    [content addSubview:self.btnDisconnect];

    self.btnTestNet = [NSButton buttonWithTitle:@"检测网络状态" target:self action:@selector(onTestNetClicked:)];
    self.btnTestNet.frame = NSMakeRect(266, 420, 112, 32);
    [content addSubview:self.btnTestNet];

    self.btnSave = [NSButton buttonWithTitle:@"保存配置" target:self action:@selector(onSaveClicked:)];
    self.btnSave.frame = NSMakeRect(386, 420, 96, 32);
    [content addSubview:self.btnSave];

    self.btnClearLog = [NSButton buttonWithTitle:@"清空日志" target:self action:@selector(onClearLogClicked:)];
    self.btnClearLog.frame = NSMakeRect(490, 420, 90, 32);
    [content addSubview:self.btnClearLog];

    // Section 4: Log Output
    [content addSubview:MakeLabel(@"运行日志", NSMakeRect(16, 462, 200, 20), YES)];
    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 486, 564, 142)];
    scrollView.hasVerticalScroller = YES;
    scrollView.borderType = NSBezelBorder;

    self.textLog = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 548, 142)];
    self.textLog.editable = NO;
    self.textLog.selectable = YES;
    self.textLog.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    scrollView.documentView = self.textLog;
    [content addSubview:scrollView];

    self.lblStatus = MakeLabel(@"状态: 就绪", NSMakeRect(16, 636, 564, 22));
    [content addSubview:self.lblStatus];
}

- (void)updateControlStatesByAuthMode {
    NSInteger idx = [self.popupAuthMode indexOfSelectedItem];
    AuthMode mode = static_cast<AuthMode>(idx >= 0 ? idx : 0);

    BOOL needPortal = (mode == AuthMode::PORTAL_OPEN ||
                       mode == AuthMode::PORTAL_PSK ||
                       mode == AuthMode::ENT_PLUS_PORTAL);

    if (mode == AuthMode::PORTAL_PSK) {
        self.lblDomainOrPsk.stringValue = @"Wi-Fi连接密码:";
        self.editDomainOrPsk.enabled = YES;
    } else if (mode == AuthMode::PORTAL_OPEN) {
        self.lblDomainOrPsk.stringValue = @"域 / 附加项:";
        self.editDomainOrPsk.enabled = NO;
    } else {
        self.lblDomainOrPsk.stringValue = @"认证域(选填):";
        self.editDomainOrPsk.enabled = YES;
    }

    self.editPortalUrl.enabled = needPortal;
    self.popupPortalMethod.enabled = needPortal;
    self.editUserField.enabled = needPortal;
    self.editPassField.enabled = needPortal;
    self.editExtraParams.enabled = needPortal;
}

- (void)applyConfigToUi:(const AppConfig&)cfg {
    self.comboSsid.stringValue = ToNS(cfg.ssid);
    [self.popupAuthMode selectItemAtIndex:static_cast<NSInteger>(cfg.auth_mode)];
    self.editUsername.stringValue = ToNS(cfg.username);
    self.editPasswordSecure.stringValue = ToNS(cfg.password);
    self.editPasswordPlain.stringValue = ToNS(cfg.password);
    self.editDomainOrPsk.stringValue = ToNS(cfg.domain_or_psk);

    self.editPortalUrl.stringValue = ToNS(cfg.portal_url);
    [self.popupPortalMethod selectItemAtIndex:static_cast<NSInteger>(cfg.portal_method)];
    self.editUserField.stringValue = ToNS(cfg.portal_user_field);
    self.editPassField.stringValue = ToNS(cfg.portal_pass_field);
    self.editExtraParams.stringValue = ToNS(cfg.portal_extra_params);

    self.chkAutoConnect.state = cfg.auto_connect ? NSControlStateValueOn : NSControlStateValueOff;
    self.menuAutoToggle.state = cfg.auto_connect ? NSControlStateValueOn : NSControlStateValueOff;
    self.editInterval.stringValue = [NSString stringWithFormat:@"%d", cfg.check_interval_sec];
    self.chkAutoStart.state = cfg.auto_start ? NSControlStateValueOn : NSControlStateValueOff;

    [self updateControlStatesByAuthMode];
}

- (void)readConfigFromUi:(AppConfig&)cfg {
    std::wstring raw_ssid = FromNS(self.comboSsid.stringValue);
    size_t suffix_pos = raw_ssid.rfind(L"  (");
    if (suffix_pos != std::wstring::npos && !raw_ssid.empty() && raw_ssid.back() == L')') {
        raw_ssid = raw_ssid.substr(0, suffix_pos);
    }
    cfg.ssid = raw_ssid;

    NSInteger mode_idx = [self.popupAuthMode indexOfSelectedItem];
    if (mode_idx < 0 || mode_idx > 3) mode_idx = 0;
    cfg.auth_mode = static_cast<AuthMode>(mode_idx);

    cfg.username = FromNS(self.editUsername.stringValue);
    if (self.chkShowPwd.state == NSControlStateValueOn) {
        cfg.password = FromNS(self.editPasswordPlain.stringValue);
        self.editPasswordSecure.stringValue = self.editPasswordPlain.stringValue;
    } else {
        cfg.password = FromNS(self.editPasswordSecure.stringValue);
        self.editPasswordPlain.stringValue = self.editPasswordSecure.stringValue;
    }
    cfg.domain_or_psk = FromNS(self.editDomainOrPsk.stringValue);

    cfg.portal_url = FromNS(self.editPortalUrl.stringValue);
    NSInteger pm_idx = [self.popupPortalMethod indexOfSelectedItem];
    if (pm_idx < 0 || pm_idx > 2) pm_idx = 0;
    cfg.portal_method = static_cast<PortalMethod>(pm_idx);

    cfg.portal_user_field = FromNS(self.editUserField.stringValue);
    if (cfg.portal_user_field.empty()) cfg.portal_user_field = L"username";
    cfg.portal_pass_field = FromNS(self.editPassField.stringValue);
    if (cfg.portal_pass_field.empty()) cfg.portal_pass_field = L"password";
    cfg.portal_extra_params = FromNS(self.editExtraParams.stringValue);

    cfg.auto_connect = (self.chkAutoConnect.state == NSControlStateValueOn);
    self.menuAutoToggle.state = cfg.auto_connect ? NSControlStateValueOn : NSControlStateValueOff;

    int interval = [self.editInterval.stringValue intValue];
    if (interval < 3) interval = 3;
    if (interval > 3600) interval = 3600;
    cfg.check_interval_sec = interval;

    cfg.auto_start = (self.chkAutoStart.state == NSControlStateValueOn);
}

- (void)appendLog:(const std::wstring&)msg {
    NSString* line = ToNS(utils::CurrentTimeStr() + L" " + msg + L"\n");
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.textLog) return;
        NSTextStorage* storage = self.textLog.textStorage;
        if (storage.length > 24000) {
            [storage setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
        }
        NSDictionary* attrs = @{
            NSFontAttributeName : [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular],
            NSForegroundColorAttributeName : [NSColor labelColor]
        };
        NSAttributedString* attrLine = [[NSAttributedString alloc] initWithString:line attributes:attrs];
        [storage appendAttributedString:attrLine];
        [self.textLog scrollRangeToVisible:NSMakeRange(storage.length, 0)];
    });
}

- (void)updateStatus:(const std::wstring&)statusText {
    NSString* nsSt = ToNS(statusText);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.lblStatus) {
            self.lblStatus.stringValue = nsSt;
        }
        if (self.statusItem && self.statusItem.button) {
            self.statusItem.button.toolTip = [NSString stringWithFormat:@"AutoWiFi - %@", nsSt];
        }
    });
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    g_mac.delegate = self;

    [self setupMainMenu];
    [self setupStatusBarItem];
    [self setupWindowAndControls];

    {
        std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
        g_mac.config = config::LoadConfig();
        [self applyConfigToUi:g_mac.config];
    }

    if (!self.startMinimized) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [self.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    } else {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }

    [self appendLog:(L"[启动] AutoWiFi 已就绪，配置文件路径: " + config::GetConfigFilePath())];

    // Request macOS Location Services permission so CoreWLAN can read Wi-Fi SSID and scan networks
    wifi::RequestMacLocationPermission([self]() {
        [self appendLog:L"[权限] 已获得 macOS 定位服务授权，正在刷新 Wi-Fi 列表..."];
        [self onScanClicked:nil];
        [self onTestNetClicked:nil];
    }, false);

    if (wifi::IsMacLocationPermissionDenied()) {
        [self appendLog:L"[提示] 尚未授予 AutoWiFi 定位服务权限（macOS 需要定位权限才能读取 Wi-Fi 名称），请在「系统设置 -> 隐私与安全性 -> 定位服务」中开启。"];
    }

    [self onScanClicked:nil];
    [self onTestNetClicked:nil];

    {
        std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
        if (g_mac.config.auto_connect && !g_mac.config.ssid.empty()) {
            [self onConnectClicked:nil];
        }
    }

    g_mac.monitor_thread = std::thread(MacMonitorLoop);
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.forceTerminate) {
        return YES;
    }
    // Hide window to menu bar instead of quitting when clicking close button
    [self.window orderOut:nil];
    [self appendLog:L"[托盘] 窗口已隐藏至顶部菜单栏，程序将在后台继续守护 Wi-Fi 连接"];
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    if (!flag) {
        [self onShowMainWindow:nil];
    }
    return YES;
}

- (void)onShowMainWindow:(id)sender {
    (void)sender;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)onQuitApp:(id)sender {
    (void)sender;
    self.forceTerminate = YES;
    g_mac.running = false;
    g_mac.cv.notify_all();
    if (g_mac.monitor_thread.joinable()) {
        g_mac.monitor_thread.join();
    }
    [NSApp terminate:nil];
}

- (void)onAuthModeChanged:(id)sender {
    (void)sender;
    [self updateControlStatesByAuthMode];
}

- (void)onShowPwdToggled:(id)sender {
    (void)sender;
    BOOL show = (self.chkShowPwd.state == NSControlStateValueOn);
    if (show) {
        self.editPasswordPlain.stringValue = self.editPasswordSecure.stringValue;
        self.editPasswordSecure.hidden = YES;
        self.editPasswordPlain.hidden = NO;
    } else {
        self.editPasswordSecure.stringValue = self.editPasswordPlain.stringValue;
        self.editPasswordPlain.hidden = YES;
        self.editPasswordSecure.hidden = NO;
    }
}

- (void)onScanClicked:(id)sender {
    (void)sender;
    self.btnScan.enabled = NO;
    [self appendLog:L"[扫描] 正在扫描周围无线网络..."];
    std::thread([self]() {
        auto list = wifi::ScanNetworks(true);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.btnScan.enabled = YES;
            NSString* currentText = self.comboSsid.stringValue;
            [self.comboSsid removeAllItems];
            for (const auto& item : list) {
                std::wstring label = item.ssid + L"  (" + std::to_wstring(item.signal_quality) +
                                     L"% | " + item.auth_desc + L")";
                [self.comboSsid addItemWithObjectValue:ToNS(label)];
            }
            if (currentText && currentText.length > 0) {
                self.comboSsid.stringValue = currentText;
            } else if (!list.empty()) {
                self.comboSsid.stringValue = ToNS(list[0].ssid);
            }
            [self appendLog:(L"[扫描] 扫描完成，共发现 " + std::to_wstring(list.size()) + L" 个无线网络")];
        });
    }).detach();
}

- (void)onConnectClicked:(id)sender {
    (void)sender;
    if (g_mac.busy_op.exchange(true)) {
        [self appendLog:L"[提示] 已有连接任务正在执行中，请稍候..."];
        return;
    }
    self.btnConnect.enabled = NO;

    AppConfig snap;
    {
        std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
        [self readConfigFromUi:g_mac.config];
        snap = g_mac.config;
    }
    config::SaveConfig(snap);

    std::thread([self, snap]() {
        ExecuteMacConnectAndAuth(snap, false);
        g_mac.busy_op = false;
        dispatch_async(dispatch_get_main_queue(), ^{
            self.btnConnect.enabled = YES;
        });
    }).detach();
}

- (void)onDisconnectClicked:(id)sender {
    (void)sender;
    if (g_mac.busy_op.exchange(true)) return;
    std::thread([self]() {
        std::wstring msg;
        bool ok = wifi::Disconnect(msg);
        [self appendLog:(std::wstring(ok ? L"[断开] " : L"[错误] ") + msg)];
        [self updateStatus:L"状态: 已手动断开 Wi-Fi 连接"];
        g_mac.busy_op = false;
    }).detach();
}

- (void)onTestNetClicked:(id)sender {
    (void)sender;
    std::thread([self]() {
        WifiConnectionState st = wifi::GetConnectionState();
        if (!st.has_adapter) {
            [self appendLog:L"[检测] 未检测到无线网卡"];
            [self updateStatus:L"状态: 未检测到无线网卡"];
            return;
        }
        if (!st.is_connected) {
            [self appendLog:(L"[检测] 网卡 [" + st.adapter_name + L"] 当前未连接任何 Wi-Fi")];
            [self updateStatus:L"状态: 未连接 Wi-Fi"];
            return;
        }
        std::wstring redirect_url;
        NetStatus net = portal::CheckInternetState(redirect_url);
        if (net == NetStatus::ONLINE) {
            [self appendLog:(L"[检测] 当前连接 [" + st.current_ssid + L"] (信号 " +
                             std::to_wstring(st.signal_quality) + L"%) -> 外网访问正常")];
            [self updateStatus:(L"状态: 已连接 [" + st.current_ssid + L"] | 互联网正常")];
        } else if (net == NetStatus::CAPTIVE_PORTAL) {
            [self appendLog:(L"[检测] 当前连接 [" + st.current_ssid + L"] -> 需要网页 Portal 认证 (重定向: " +
                             (redirect_url.empty() ? L"未知" : redirect_url) + L")")];
            [self updateStatus:(L"状态: 已连接 [" + st.current_ssid + L"] | 等待网页认证")];
        } else {
            [self appendLog:(L"[检测] 当前连接 [" + st.current_ssid + L"] -> 无法访问外网")];
            [self updateStatus:(L"状态: 已连接 [" + st.current_ssid + L"] | 无互联网连接")];
        }
    }).detach();
}

- (void)onSaveClicked:(id)sender {
    (void)sender;
    AppConfig snap;
    {
        std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
        [self readConfigFromUi:g_mac.config];
        snap = g_mac.config;
    }
    config::SaveConfig(snap);
    g_mac.cv.notify_all();
    [self appendLog:(L"[配置] 配置已加密保存至: " + config::GetConfigFilePath())];
}

- (void)onClearLogClicked:(id)sender {
    (void)sender;
    if (self.textLog && self.textLog.textStorage) {
        [self.textLog.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
    }
}

- (void)onTrayToggleAuto:(id)sender {
    (void)sender;
    bool new_state = false;
    {
        std::lock_guard<std::mutex> lk(g_mac.cfg_mutex);
        g_mac.config.auto_connect = !g_mac.config.auto_connect;
        new_state = g_mac.config.auto_connect;
        self.chkAutoConnect.state = new_state ? NSControlStateValueOn : NSControlStateValueOff;
        self.menuAutoToggle.state = new_state ? NSControlStateValueOn : NSControlStateValueOff;
        config::SaveConfig(g_mac.config);
    }
    g_mac.cv.notify_all();
    [self appendLog:(new_state ? L"[设置] 已开启断线自动重连" : L"[设置] 已暂停断线自动重连")];
}

@end

int RunConnectOnceCli() {
    AppConfig cfg = config::LoadConfig();
    if (cfg.ssid.empty()) {
        return 2;
    }
    bool ok = ExecuteMacConnectAndAuth(cfg, false);
    return ok ? 0 : 1;
}

int RunPosixApp(int argc, char* argv[]) {
    bool start_minimized = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "AutoWiFi - macOS 自动认证 Wi-Fi 客户端 (GUI + 菜单栏托盘 + CLI)\n\n"
                      << "用法:\n"
                      << "  auto-wifi                  启动 macOS 原生图形界面与顶部菜单栏图标\n"
                      << "  auto-wifi --minimized (-m) 启动后仅驻留顶部菜单栏后台运行\n"
                      << "  auto-wifi --connect-once (-c) 执行一次连接与认证后退出\n"
                      << "  auto-wifi --status (-s)    命令行检测当前无线与互联网状态\n"
                      << "  auto-wifi --scan           命令行扫描周围 Wi-Fi 列表\n";
            return 0;
        }
        if (arg == "--connect-once" || arg == "-c") {
            wifi::RequestMacLocationPermission(nullptr, true);
            return RunConnectOnceCli();
        }
        if (arg == "--minimized" || arg == "--tray" || arg == "--daemon" || arg == "-m" || arg == "-d") {
            start_minimized = true;
        }
        if (arg == "--status" || arg == "-s") {
            wifi::RequestMacLocationPermission(nullptr, true);
            WifiConnectionState st = wifi::GetConnectionState();
            std::wstring redirect;
            NetStatus net = portal::CheckInternetState(redirect);
            std::cout << "Wi-Fi: " << (st.is_connected ? utils::WideToUtf8(st.current_ssid) : "未连接")
                      << " | 网络状态: " << (net == NetStatus::ONLINE ? "互联网正常" :
                                            (net == NetStatus::CAPTIVE_PORTAL ? "需要Portal认证" : "离线"))
                      << std::endl;
            return 0;
        }
        if (arg == "--scan") {
            wifi::RequestMacLocationPermission(nullptr, true);
            auto list = wifi::ScanNetworks(true);
            for (const auto& item : list) {
                std::cout << item.signal_quality << "%\t"
                          << utils::WideToUtf8(item.auth_desc) << "\t"
                          << utils::WideToUtf8(item.ssid) << std::endl;
            }
            return 0;
        }
    }

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AutoWiFiAppDelegate* delegate = [[AutoWiFiAppDelegate alloc] init];
        delegate.startMinimized = start_minimized ? YES : NO;
        app.delegate = delegate;
        [app run];
    }
    return 0;
}

#endif  // __APPLE__
