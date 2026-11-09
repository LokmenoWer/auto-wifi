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
#include <functional>
#include <iostream>
#include <map>
#include <sstream>

@interface AutoWiFiLocationController : NSObject <CLLocationManagerDelegate>
@property(nonatomic, strong) CLLocationManager* manager;
@property(nonatomic, assign) BOOL authResolved;
@property(nonatomic, assign) CLAuthorizationStatus currentStatus;
@property(nonatomic, copy) void (^onAuthorizedCallback)(void);
+ (instancetype)shared;
- (void)requestPermissionInteractive:(BOOL)waitRunLoop;
@end

@implementation AutoWiFiLocationController

+ (instancetype)shared {
    static AutoWiFiLocationController* s_instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s_instance = [[AutoWiFiLocationController alloc] init];
    });
    return s_instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _manager = [[CLLocationManager alloc] init];
        _manager.delegate = self;
        _currentStatus = _manager.authorizationStatus;
        _authResolved = (_currentStatus != kCLAuthorizationStatusNotDetermined);
    }
    return self;
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
    self.currentStatus = manager.authorizationStatus;
    if (self.currentStatus != kCLAuthorizationStatusNotDetermined) {
        self.authResolved = YES;
        [self.manager stopUpdatingLocation];
        if ((self.currentStatus == kCLAuthorizationStatusAuthorizedAlways ||
             self.currentStatus == kCLAuthorizationStatusAuthorized) &&
            self.onAuthorizedCallback) {
            self.onAuthorizedCallback();
        }
    }
}

- (void)locationManager:(CLLocationManager*)manager
     didUpdateLocations:(NSArray<CLLocation*>*)locations {
    (void)manager;
    (void)locations;
    [self.manager stopUpdatingLocation];
}

- (void)locationManager:(CLLocationManager*)manager
       didFailWithError:(NSError*)error {
    (void)manager;
    (void)error;
}

- (void)requestPermissionInteractive:(BOOL)waitRunLoop {
    self.currentStatus = self.manager.authorizationStatus;
    if (self.currentStatus == kCLAuthorizationStatusAuthorizedAlways ||
        self.currentStatus == kCLAuthorizationStatusAuthorized) {
        return;
    }

    if (![CLLocationManager locationServicesEnabled]) {
        return;
    }

    if (self.currentStatus == kCLAuthorizationStatusNotDetermined) {
        if ([self.manager respondsToSelector:@selector(requestAlwaysAuthorization)]) {
            [self.manager requestAlwaysAuthorization];
        }
        if ([self.manager respondsToSelector:@selector(requestWhenInUseAuthorization)]) {
            [self.manager requestWhenInUseAuthorization];
        }
        [self.manager startUpdatingLocation];

        if (waitRunLoop) {
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:20.0];
            while (!self.authResolved && [deadline timeIntervalSinceNow] > 0) {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
            }
            [self.manager stopUpdatingLocation];
        }
    }
}

@end

namespace wifi {

namespace {

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

}  // namespace

void RequestMacLocationPermission(std::function<void()> on_authorized, bool wait_if_cli) {
    auto block = ^() {
        AutoWiFiLocationController* ctrl = [AutoWiFiLocationController shared];
        if (on_authorized) {
            ctrl.onAuthorizedCallback = ^{
                on_authorized();
            };
        }
        [ctrl requestPermissionInteractive:(wait_if_cli ? YES : NO)];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

bool IsMacLocationPermissionDenied() {
    __block CLAuthorizationStatus st = kCLAuthorizationStatusNotDetermined;
    if ([NSThread isMainThread]) {
        st = [AutoWiFiLocationController shared].manager.authorizationStatus;
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{
            st = [AutoWiFiLocationController shared].manager.authorizationStatus;
        });
    }
    return (st == kCLAuthorizationStatusDenied || st == kCLAuthorizationStatusRestricted);
}

void OpenMacLocationSettings() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSWorkspace sharedWorkspace]
            openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices"]];
    });
}

WifiConnectionState GetConnectionState() {
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

        // Connected to a Wi-Fi network, check if SSID is available via networksetup or redacted
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
            state.current_ssid = L"<需授予位置权限以显示SSID>";
        }
    }
    return state;
}

std::vector<WifiNetworkInfo> ScanNetworks(bool /*trigger_active_scan*/) {
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

    out_message = L"未在周围扫描到目标 802.1X 热点 [" + ssid + L"]（请确认已允许位置权限且热点在信号范围内）";
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
