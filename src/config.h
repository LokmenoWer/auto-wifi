#pragma once

#include <string>

enum class AuthMode : int {
    WPA2_ENT_PEAP = 0,   // 802.1X Enterprise (PEAP-MSCHAPv2)
    PORTAL_OPEN = 1,     // Open Wi-Fi + Web Captive Portal
    PORTAL_PSK = 2,      // WPA2-PSK Wi-Fi + Web Captive Portal
    ENT_PLUS_PORTAL = 3  // 802.1X Enterprise + Web Captive Portal
};

enum class PortalMethod : int {
    POST_FORM = 0,  // application/x-www-form-urlencoded
    POST_JSON = 1,  // application/json
    GET_QUERY = 2   // GET with query string
};

struct AppConfig {
    std::wstring ssid;
    AuthMode auth_mode = AuthMode::WPA2_ENT_PEAP;
    std::wstring username;
    std::wstring password;
    std::wstring domain_or_psk;  // Domain for 802.1X or Wi-Fi PSK for PORTAL_PSK

    // Captive Portal settings
    std::wstring portal_url;  // Empty = auto-detect redirect URL
    PortalMethod portal_method = PortalMethod::POST_FORM;
    std::wstring portal_user_field = L"username";
    std::wstring portal_pass_field = L"password";
    std::wstring portal_extra_params;

    // Automation settings
    bool auto_connect = true;
    int check_interval_sec = 15;
    bool auto_start = false;
};

namespace config {

std::wstring GetConfigFilePath();
AppConfig LoadConfig();
bool SaveConfig(const AppConfig& cfg);

bool SetAutoStartRegistry(bool enable);
bool IsAutoStartEnabled();

}  // namespace config
