#pragma once

#include <string>
#include <vector>

struct WifiNetworkInfo {
    std::wstring ssid;
    int signal_quality = 0;  // 0 - 100
    bool is_secure = false;
    bool is_8021x = false;
    std::wstring auth_desc;
};

struct WifiConnectionState {
    bool has_adapter = false;
    bool is_connected = false;
    std::wstring current_ssid;
    int signal_quality = 0;
    std::wstring adapter_name;
};

namespace wifi {

// Query current WLAN interface state
WifiConnectionState GetConnectionState();

// Scan and return available Wi-Fi networks sorted by signal strength
std::vector<WifiNetworkInfo> ScanNetworks(bool trigger_active_scan = true);

// Connect to 802.1X Enterprise (PEAP-MSCHAPv2) Wi-Fi using username & password
bool ConnectEnterprise8021X(const std::wstring& ssid,
                            const std::wstring& username,
                            const std::wstring& password,
                            const std::wstring& domain,
                            std::wstring& out_message);

// Connect to Open or WPA2-PSK Wi-Fi (for Captive Portal networks)
bool ConnectOpenOrPsk(const std::wstring& ssid,
                      const std::wstring& psk,
                      bool is_open,
                      std::wstring& out_message);

// Disconnect current Wi-Fi
bool Disconnect(std::wstring& out_message);

#ifdef __APPLE__
// Ensure macOS process runs within an .app bundle context so CoreLocation can prompt for permission
void EnsureMacAppBundleContext(int argc, char* argv[]);
#endif

}  // namespace wifi
