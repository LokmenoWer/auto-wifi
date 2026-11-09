#pragma once

#include "config.h"
#include <string>

enum class NetStatus {
    ONLINE = 0,          // Direct internet access confirmed
    CAPTIVE_PORTAL = 1,  // Intercepted by captive portal login page
    OFFLINE = 2          // No network / request failed
};

namespace portal {

// Probe internet connectivity and detect captive portal redirect URL if intercepted
NetStatus CheckInternetState(std::wstring& out_redirect_url);

// Perform captive portal authentication using configured or auto-detected URL
bool LoginCaptivePortal(const AppConfig& cfg,
                        const std::wstring& detected_redirect_url,
                        std::wstring& out_message);

}  // namespace portal
