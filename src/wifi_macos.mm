#ifdef __APPLE__

#include "wifi_manager.h"
#include "utils.h"

#import <AppKit/AppKit.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreWLAN/CoreWLAN.h>
#import <Foundation/Foundation.h>

#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

// Delegate to observe CoreLocation authorization changes
@interface AutoWiFiLocationDelegate : NSObject <CLLocationManagerDelegate>
@property(nonatomic, assign) BOOL authResolved;
@property(nonatomic, assign) CLAuthorizationStatus currentStatus;
@end

@implementation AutoWiFiLocationDelegate
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
    self.currentStatus = manager.authorizationStatus;
    if (self.currentStatus != kCLAuthorizationStatusNotDetermined) {
        self.authResolved = YES;
    }
}

- (void)locationManager:(CLLocationManager*)manager
    didChangeAuthorizationStatus:(CLAuthorizationStatus)status {
    (void)manager;
    self.currentStatus = status;
    if (status != kCLAuthorizationStatusNotDetermined) {
        self.authResolved = YES;
    }
}

- (void)locationManager:(CLLocationManager*)manager
     didUpdateLocations:(NSArray<CLLocation*>*)locations {
    (void)manager;
    (void)locations;
}

- (void)locationManager:(CLLocationManager*)manager
       didFailWithError:(NSError*)error {
    (void)manager;
    (void)error;
}
@end

namespace wifi {

namespace {

static const char* kEmbeddedInfoPlistXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "    <key>CFBundleDevelopmentRegion</key>\n"
    "    <string>zh_CN</string>\n"
    "    <key>CFBundleExecutable</key>\n"
    "    <string>auto-wifi</string>\n"
    "    <key>CFBundleIdentifier</key>\n"
    "    <string>io.github.lokmenower.autowifi</string>\n"
    "    <key>CFBundleInfoDictionaryVersion</key>\n"
    "    <string>6.0</string>\n"
    "    <key>CFBundleName</key>\n"
    "    <string>AutoWiFi</string>\n"
    "    <key>CFBundleDisplayName</key>\n"
    "    <string>AutoWiFi</string>\n"
    "    <key>CFBundlePackageType</key>\n"
    "    <string>APPL</string>\n"
    "    <key>CFBundleShortVersionString</key>\n"
    "    <string>1.2.0</string>\n"
    "    <key>CFBundleVersion</key>\n"
    "    <string>120</string>\n"
    "    <key>LSMinimumSystemVersion</key>\n"
    "    <string>11.0</string>\n"
    "    <key>NSLocationUsageDescription</key>\n"
    "    <string>AutoWiFi 需要访问位置权限以读取当前连接的 Wi-Fi 名称 (SSID) 及扫描周围无线网络（macOS 系统隐私要求）。</string>\n"
    "    <key>NSLocationWhenInUseUsageDescription</key>\n"
    "    <string>AutoWiFi 需要访问位置权限以读取当前连接的 Wi-Fi 名称 (SSID) 及扫描周围无线网络（macOS 系统隐私要求）。</string>\n"
    "    <key>NSLocationAlwaysAndWhenInUseUsageDescription</key>\n"
    "    <string>AutoWiFi 需要访问位置权限以在后台守护时检测当前连接的 Wi-Fi 名称 (SSID) 并自动重连。</string>\n"
    "</dict>\n"
    "</plist>\n";

void MkdirRecursive(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    mkdir(dir.c_str(), 0755);
}

int RssiToQuality(NSInteger rssi) {
    if (rssi == 0) return 0;
    return std::clamp(static_cast<int>(2 * (rssi + 100)), 5, 100);
}

std::wstring DescribeSecurity(CWNetwork* net, bool& out_secure, bool& out_8021x) {
    out_secure = true;
    out_8021x = false;

    if ([net supportsSecurity:kCWSecurityNone]) {
        out_secure = false;
        return L"开放网络 (Portal)";
    }
    if ([net supportsSecurity:kCWSecurityEnterprise] ||
        [net supportsSecurity:kCWSecurityWPAEnterprise] ||
        [net supportsSecurity:kCWSecurityWPA2Enterprise] ||
        [net supportsSecurity:kCWSecurityWPA3Enterprise]) {
        out_8021x = true;
        return L"802.1X 企业认证";
    }
    if ([net supportsSecurity:kCWSecurityWPA3Personal] ||
        [net supportsSecurity:kCWSecurityWPA3Transition]) {
        return L"WPA3-个人";
    }
    if ([net supportsSecurity:kCWSecurityWPA2Personal] ||
        [net supportsSecurity:kCWSecurityWPAPersonal] ||
        [net supportsSecurity:kCWSecurityWPAPersonalMixed]) {
        return L"WPA2-个人";
    }
    return L"加密网络";
}

// Request macOS Location Services authorization via CoreLocation so CoreWLAN can read real SSIDs
void RequestLocationAuthorizationOnMainThread() {
    static bool s_checked_once = false;
    static bool s_denied_prompted = false;

    @autoreleasepool {
        [NSApplication sharedApplication];

        CLLocationManager* manager = [[CLLocationManager alloc] init];
        AutoWiFiLocationDelegate* delegate = [[AutoWiFiLocationDelegate alloc] init];
        manager.delegate = delegate;

        CLAuthorizationStatus status = manager.authorizationStatus;

        if (status == kCLAuthorizationStatusAuthorizedAlways || status == kCLAuthorizationStatusAuthorized) {
            return;
        }

        if (![CLLocationManager locationServicesEnabled]) {
            if (!s_checked_once) {
                s_checked_once = true;
                std::cout << utils::WideToUtf8(utils::CurrentTimeStr())
                          << " [权限] macOS 系统全局「定位服务」处于关闭状态，无法读取 Wi-Fi SSID。\n";
            }
            return;
        }

        if (status == kCLAuthorizationStatusNotDetermined) {
            // Switch to Regular activation policy briefly so macOS WindowServer brings the authorization dialog to front
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];

            std::cout << utils::WideToUtf8(utils::CurrentTimeStr())
                      << " [权限] 正在向 macOS 申请「定位服务」权限（用于读取 Wi-Fi SSID 与扫描热点），请在弹出的系统对话框中点击「允许」..."
                      << std::endl;

            if ([manager respondsToSelector:@selector(requestAlwaysAuthorization)]) {
                [manager requestAlwaysAuthorization];
            }
            if ([manager respondsToSelector:@selector(requestWhenInUseAuthorization)]) {
                [manager requestWhenInUseAuthorization];
            }
            [manager startUpdatingLocation];

            // Pump NSRunLoop for up to 25 seconds while waiting for the user to click Allow/Don't Allow
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:25.0];
            while (!delegate.authResolved && [deadline timeIntervalSinceNow] > 0) {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
            }

            [manager stopUpdatingLocation];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

            status = manager.authorizationStatus;

            if (status == kCLAuthorizationStatusAuthorizedAlways || status == kCLAuthorizationStatusAuthorized) {
                std::cout << utils::WideToUtf8(utils::CurrentTimeStr())
                          << " [权限] 已获得 macOS 定位与 Wi-Fi 信息访问授权！" << std::endl;
                return;
            }
        }

        if ((status == kCLAuthorizationStatusDenied || status == kCLAuthorizationStatusRestricted) && !s_denied_prompted) {
            s_denied_prompted = true;
            std::cout << utils::WideToUtf8(utils::CurrentTimeStr())
                      << " [权限] 尚未授予 AutoWiFi 定位权限（macOS 会将 SSID 隐藏为 <redacted>）。\n"
                      << "       请在「系统设置 -> 隐私与安全性 -> 定位服务」中开启 AutoWiFi（或当前终端）权限。" << std::endl;
            // Open Location Services settings pane once to help user grant permission easily
            [[NSWorkspace sharedWorkspace]
                openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices"]];
        }
        s_checked_once = true;
    }
}

void EnsureLocationAuthorization() {
    if ([NSThread isMainThread]) {
        RequestLocationAuthorizationOnMainThread();
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{
            RequestLocationAuthorizationOnMainThread();
        });
    }
}

}  // namespace

// Ensure the process runs from inside an .app bundle on macOS so locationd shows the authorization dialog
void EnsureMacAppBundleContext(int argc, char* argv[]) {
    if (std::getenv("AUTOWIFI_NO_REEXEC") != nullptr) {
        return;
    }

    char exe_buf[PATH_MAX] = {0};
    uint32_t sz = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &sz) != 0) {
        return;
    }
    char real_buf[PATH_MAX] = {0};
    std::string exe_path = realpath(exe_buf, real_buf) ? real_buf : exe_buf;

    // Already running inside *.app/Contents/MacOS/*
    if (exe_path.find(".app/Contents/MacOS/") != std::string::npos) {
        return;
    }

    // Preserve original executable path for config.cpp portable config lookup and LaunchAgent registration
    setenv("AUTOWIFI_ORIGINAL_EXE", exe_path.c_str(), 1);
    setenv("AUTOWIFI_NO_REEXEC", "1", 1);

    // Check if AutoWiFi.app exists alongside the executable first
    size_t slash = exe_path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string sibling_app_bin = exe_path.substr(0, slash + 1) + "AutoWiFi.app/Contents/MacOS/auto-wifi";
        struct stat st_sib{};
        if (stat(sibling_app_bin.c_str(), &st_sib) == 0 && (st_sib.st_mode & S_IXUSR)) {
            execv(sibling_app_bin.c_str(), argv);
        }
    }

    // Otherwise create or update ~/.cache/auto-wifi/AutoWiFi.app bundle wrapper on the fly
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return;

    std::string app_dir = std::string(home) + "/.cache/auto-wifi/AutoWiFi.app";
    std::string contents_dir = app_dir + "/Contents";
    std::string macos_dir = contents_dir + "/MacOS";
    std::string plist_path = contents_dir + "/Info.plist";
    std::string bundled_bin = macos_dir + "/auto-wifi";

    MkdirRecursive(macos_dir);

    bool need_update = false;
    struct stat st_src{}, st_dst{};
    if (stat(exe_path.c_str(), &st_src) == 0) {
        if (stat(bundled_bin.c_str(), &st_dst) != 0 ||
            st_src.st_mtime != st_dst.st_mtime ||
            st_src.st_size != st_dst.st_size) {
            need_update = true;
        }
    }

    if (need_update) {
        std::ofstream plist_ofs(plist_path, std::ios::trunc);
        if (plist_ofs.is_open()) {
            plist_ofs << kEmbeddedInfoPlistXml;
            plist_ofs.close();
        }

        std::ifstream src(exe_path, std::ios::binary);
        std::ofstream dst(bundled_bin, std::ios::binary | std::ios::trunc);
        if (src.is_open() && dst.is_open()) {
            dst << src.rdbuf();
            dst.close();
            chmod(bundled_bin.c_str(), 0755);
            // Ad-hoc sign the app bundle so macOS locationd accepts it
            utils::RunCommand("codesign --force --deep --sign - " + utils::ShellEscape(app_dir));
        }
    }

    struct stat st_check{};
    if (stat(bundled_bin.c_str(), &st_check) == 0 && (st_check.st_mode & S_IXUSR)) {
        (void)argc;
        execv(bundled_bin.c_str(), argv);
    }
}

WifiConnectionState GetConnectionState() {
    EnsureLocationAuthorization();

    WifiConnectionState state;
    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];
        if (!iface) {
            return state;
        }

        state.has_adapter = true;
        if (iface.interfaceName) {
            state.adapter_name = utils::Utf8ToWide([iface.interfaceName UTF8String]);
        } else {
            state.adapter_name = L"en0";
        }

        NSString* ssid = iface.ssid;
        if (ssid && ssid.length > 0) {
            state.is_connected = true;
            state.current_ssid = utils::Utf8ToWide([ssid UTF8String]);
            state.signal_quality = RssiToQuality(iface.rssiValue);
            return state;
        }

        // If connected to a network but location permission was denied (ssid is nil while rssiValue != 0)
        if (iface.rssiValue != 0 || iface.transmitRate > 0) {
            state.is_connected = true;
            state.signal_quality = RssiToQuality(iface.rssiValue);

            std::string ifname = utils::WideToUtf8(state.adapter_name);
            std::string out = utils::RunCommand("networksetup -getairportnetwork " + utils::ShellEscape(ifname));
            const std::string prefix = "Current Wi-Fi Network:";
            size_t pos = out.find(prefix);
            if (pos != std::string::npos) {
                std::string net_ssid = utils::TrimUtf8(out.substr(pos + prefix.size()));
                if (!net_ssid.empty()) {
                    state.current_ssid = utils::Utf8ToWide(net_ssid);
                    return state;
                }
            }
            state.current_ssid = L"<未授权读取SSID>";
        }
    }
    return state;
}

std::vector<WifiNetworkInfo> ScanNetworks(bool /*trigger_active_scan*/) {
    EnsureLocationAuthorization();

    std::vector<WifiNetworkInfo> results;
    std::map<std::wstring, WifiNetworkInfo> best_by_ssid;

    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];
        if (iface) {
            NSError* err = nil;
            NSSet<CWNetwork*>* networks = [iface scanForNetworksWithName:nil error:&err];
            if (networks && networks.count > 0) {
                for (CWNetwork* net in networks) {
                    NSString* nsSsid = net.ssid;
                    if (!nsSsid || nsSsid.length == 0) continue;

                    std::wstring ssid = utils::Utf8ToWide([nsSsid UTF8String]);
                    if (ssid.empty()) continue;

                    WifiNetworkInfo info;
                    info.ssid = ssid;
                    info.signal_quality = RssiToQuality(net.rssiValue);
                    info.auth_desc = DescribeSecurity(net, info.is_secure, info.is_8021x);

                    auto it = best_by_ssid.find(ssid);
                    if (it == best_by_ssid.end() || info.signal_quality > it->second.signal_quality) {
                        best_by_ssid[ssid] = info;
                    }
                }
            }
        }
    }

    for (auto& kv : best_by_ssid) {
        results.push_back(kv.second);
    }
    std::sort(results.begin(), results.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
        return a.signal_quality > b.signal_quality;
    });
    return results;
}

bool ConnectEnterprise8021X(const std::wstring& ssid,
                            const std::wstring& username,
                            const std::wstring& password,
                            const std::wstring& /*domain*/,
                            std::wstring& out_message) {
    if (ssid.empty() || username.empty()) {
        out_message = L"SSID 与 802.1X 认证账号不能为空";
        return false;
    }

    EnsureLocationAuthorization();

    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];
        if (!iface) {
            out_message = L"未检测到可用的 macOS 无线网卡接口";
            return false;
        }

        NSString* nsSsid = [NSString stringWithUTF8String:utils::WideToUtf8(ssid).c_str()];
        NSString* nsUser = [NSString stringWithUTF8String:utils::WideToUtf8(username).c_str()];
        NSString* nsPass = [NSString stringWithUTF8String:utils::WideToUtf8(password).c_str()];

        NSError* err = nil;
        NSSet<CWNetwork*>* nets = [iface scanForNetworksWithName:nsSsid error:&err];
        CWNetwork* targetNet = nil;
        for (CWNetwork* n in nets) {
            if (!targetNet || n.rssiValue > targetNet.rssiValue) {
                targetNet = n;
            }
        }

        if (targetNet) {
            NSError* assocErr = nil;
            BOOL ok = [iface associateToEnterpriseNetwork:targetNet
                                                 identity:nil
                                                 username:nsUser
                                                 password:nsPass
                                                    error:&assocErr];
            if (ok && !assocErr) {
                out_message = L"已通过 CoreWLAN 802.1X 认证并连接至 [" + ssid + L"]";
                return true;
            }
            if (assocErr) {
                out_message = L"CoreWLAN 802.1X 关联失败: " +
                              utils::Utf8ToWide([[assocErr localizedDescription] UTF8String]);
                return false;
            }
        }
    }

    out_message = L"未在周围扫描到目标 802.1X 热点 [" + ssid + L"]（请确认定位权限已允许且热点在信号范围内）";
    return false;
}

bool ConnectOpenOrPsk(const std::wstring& ssid,
                      const std::wstring& psk,
                      bool is_open,
                      std::wstring& out_message) {
    if (ssid.empty()) {
        out_message = L"SSID 不能为空";
        return false;
    }

    EnsureLocationAuthorization();

    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];
        if (!iface) {
            out_message = L"未检测到可用的 macOS 无线网卡接口";
            return false;
        }

        NSString* nsSsid = [NSString stringWithUTF8String:utils::WideToUtf8(ssid).c_str()];
        NSString* nsPsk = (!is_open && !psk.empty())
                              ? [NSString stringWithUTF8String:utils::WideToUtf8(psk).c_str()]
                              : nil;

        if (iface.ssid && [iface.ssid isEqualToString:nsSsid]) {
            out_message = L"当前已连接至 Wi-Fi [" + ssid + L"]";
            return true;
        }

        NSError* scanErr = nil;
        NSSet<CWNetwork*>* nets = [iface scanForNetworksWithName:nsSsid error:&scanErr];
        CWNetwork* targetNet = nil;
        for (CWNetwork* n in nets) {
            if (!targetNet || n.rssiValue > targetNet.rssiValue) {
                targetNet = n;
            }
        }

        if (targetNet) {
            NSError* assocErr = nil;
            BOOL ok = [iface associateToNetwork:targetNet password:nsPsk error:&assocErr];
            if (ok && !assocErr) {
                out_message = L"已通过 CoreWLAN 成功关联至热点 [" + ssid + L"]";
                return true;
            }
        }

        // Fallback to networksetup if scan could not see SSID
        std::string ifname = iface.interfaceName ? [iface.interfaceName UTF8String] : "en0";
        std::string cmd = "networksetup -setairportnetwork " + utils::ShellEscape(ifname) + " " +
                          utils::ShellEscape(utils::WideToUtf8(ssid));
        if (!is_open && !psk.empty()) {
            cmd += " " + utils::ShellEscape(utils::WideToUtf8(psk));
        }
        int code = 0;
        std::string out = utils::RunCommand(cmd, &code);
        if (code == 0 && out.find("Failed") == std::string::npos && out.find("Could not") == std::string::npos) {
            out_message = L"已成功关联至热点 [" + ssid + L"]";
            return true;
        }
        out_message = L"关联热点失败: " + utils::Trim(utils::Utf8ToWide(out));
        return false;
    }
}

bool Disconnect(std::wstring& out_message) {
    @autoreleasepool {
        CWWiFiClient* client = [CWWiFiClient sharedWiFiClient];
        CWInterface* iface = [client interface];
        if (!iface) {
            out_message = L"未检测到可用的 macOS 无线网卡接口";
            return false;
        }
        [iface disassociate];
        out_message = L"已断开当前 Wi-Fi 连接";
        return true;
    }
}

}  // namespace wifi

#endif  // __APPLE__
