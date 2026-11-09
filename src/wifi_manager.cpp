#include "wifi_manager.h"
#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wlanapi.h>

#include <algorithm>
#include <map>
#include <sstream>

namespace wifi {

namespace {

struct ScopedWlanHandle {
    HANDLE handle = nullptr;
    ScopedWlanHandle() {
        DWORD negotiated = 0;
        if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS) {
            handle = nullptr;
        }
    }
    ~ScopedWlanHandle() {
        if (handle) WlanCloseHandle(handle, nullptr);
    }
    bool valid() const { return handle != nullptr; }
};

std::wstring SsidToWide(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) {
        return L"";
    }
    std::string raw(reinterpret_cast<const char*>(ssid.ucSSID), ssid.uSSIDLength);
    std::wstring w = utils::Utf8ToWide(raw);
    if (!w.empty()) return w;
    // Fallback to ANSI codepage if SSID is GBK encoded
    int len = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring res(len, 0);
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &res[0], len);
    return res;
}

DOT11_SSID WideToDot11Ssid(const std::wstring& ssid) {
    DOT11_SSID res = {0, {0}};
    std::string utf8 = utils::WideToUtf8(ssid);
    ULONG len = static_cast<ULONG>(std::min<size_t>(utf8.size(), DOT11_SSID_MAX_LENGTH));
    res.uSSIDLength = len;
    for (ULONG i = 0; i < len; ++i) {
        res.ucSSID[i] = static_cast<UCHAR>(utf8[i]);
    }
    return res;
}

std::wstring FormatReasonCode(DWORD reason) {
    wchar_t buf[512] = {0};
    if (WlanReasonCodeToString(reason, 512, buf, nullptr) == ERROR_SUCCESS && buf[0] != L'\0') {
        return buf;
    }
    return L"ReasonCode=" + std::to_wstring(reason);
}

std::wstring BuildEnterpriseProfileXml(const std::wstring& ssid) {
    std::wstring escaped_ssid = utils::EscapeXml(ssid);
    std::wstring hex_ssid = utils::StringToHex(utils::WideToUtf8(ssid));

    std::wstringstream ss;
    ss << L"<?xml version=\"1.0\"?>\r\n"
       << L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n"
       << L"  <name>" << escaped_ssid << L"</name>\r\n"
       << L"  <SSIDConfig>\r\n"
       << L"    <SSID>\r\n"
       << L"      <hex>" << hex_ssid << L"</hex>\r\n"
       << L"      <name>" << escaped_ssid << L"</name>\r\n"
       << L"    </SSID>\r\n"
       << L"  </SSIDConfig>\r\n"
       << L"  <connectionType>ESS</connectionType>\r\n"
       << L"  <connectionMode>auto</connectionMode>\r\n"
       << L"  <autoSwitch>false</autoSwitch>\r\n"
       << L"  <MSM>\r\n"
       << L"    <security>\r\n"
       << L"      <authEncryption>\r\n"
       << L"        <authentication>WPA2</authentication>\r\n"
       << L"        <encryption>AES</encryption>\r\n"
       << L"        <useOneX>true</useOneX>\r\n"
       << L"      </authEncryption>\r\n"
       << L"      <PMKCacheMode>enabled</PMKCacheMode>\r\n"
       << L"      <PMKCacheTTL>720</PMKCacheTTL>\r\n"
       << L"      <PMKCacheSize>128</PMKCacheSize>\r\n"
       << L"      <preAuthMode>disabled</preAuthMode>\r\n"
       << L"      <OneX xmlns=\"http://www.microsoft.com/networking/OneX/v1\">\r\n"
       << L"        <cacheUserData>true</cacheUserData>\r\n"
       << L"        <authMode>user</authMode>\r\n"
       << L"        <EAPConfig>\r\n"
       << L"          <EapHostConfig xmlns=\"http://www.microsoft.com/provisioning/EapHostConfig\">\r\n"
       << L"            <EapMethod>\r\n"
       << L"              <Type xmlns=\"http://www.microsoft.com/provisioning/EapCommon\">25</Type>\r\n"
       << L"              <VendorId xmlns=\"http://www.microsoft.com/provisioning/EapCommon\">0</VendorId>\r\n"
       << L"              <VendorType xmlns=\"http://www.microsoft.com/provisioning/EapCommon\">0</VendorType>\r\n"
       << L"              <AuthorId xmlns=\"http://www.microsoft.com/provisioning/EapCommon\">0</AuthorId>\r\n"
       << L"            </EapMethod>\r\n"
       << L"            <Config xmlns=\"http://www.microsoft.com/provisioning/EapHostConfig\">\r\n"
       << L"              <Eap xmlns=\"http://www.microsoft.com/provisioning/BaseEapConnectionPropertiesV1\">\r\n"
       << L"                <Type>25</Type>\r\n"
       << L"                <EapType xmlns=\"http://www.microsoft.com/provisioning/MsPeapConnectionPropertiesV1\">\r\n"
       << L"                  <ServerValidation>\r\n"
       << L"                    <DisableUserPromptForServerValidation>true</DisableUserPromptForServerValidation>\r\n"
       << L"                    <ServerNames></ServerNames>\r\n"
       << L"                  </ServerValidation>\r\n"
       << L"                  <FastReconnect>true</FastReconnect>\r\n"
       << L"                  <InnerEapOptional>false</InnerEapOptional>\r\n"
       << L"                  <Eap xmlns=\"http://www.microsoft.com/provisioning/BaseEapConnectionPropertiesV1\">\r\n"
       << L"                    <Type>26</Type>\r\n"
       << L"                    <EapType xmlns=\"http://www.microsoft.com/provisioning/MsChapV2ConnectionPropertiesV1\">\r\n"
       << L"                      <UseWinLogonCredentials>false</UseWinLogonCredentials>\r\n"
       << L"                    </EapType>\r\n"
       << L"                  </Eap>\r\n"
       << L"                  <EnableQuarantineChecks>false</EnableQuarantineChecks>\r\n"
       << L"                  <RequireCryptoBinding>false</RequireCryptoBinding>\r\n"
       << L"                  <PeapExtensions>\r\n"
       << L"                    <PerformServerValidation xmlns=\"http://www.microsoft.com/provisioning/MsPeapConnectionPropertiesV2\">false</PerformServerValidation>\r\n"
       << L"                    <AcceptServerName xmlns=\"http://www.microsoft.com/provisioning/MsPeapConnectionPropertiesV2\">false</AcceptServerName>\r\n"
       << L"                  </PeapExtensions>\r\n"
       << L"                </EapType>\r\n"
       << L"              </Eap>\r\n"
       << L"            </Config>\r\n"
       << L"          </EapHostConfig>\r\n"
       << L"        </EAPConfig>\r\n"
       << L"      </OneX>\r\n"
       << L"    </security>\r\n"
       << L"  </MSM>\r\n"
       << L"</WLANProfile>";
    return ss.str();
}

std::wstring BuildPeapUserCredentialsXml(const std::wstring& username,
                                         const std::wstring& password,
                                         const std::wstring& domain) {
    std::wstring esc_user = utils::EscapeXml(username);
    std::wstring esc_pass = utils::EscapeXml(password);
    std::wstring esc_domain = utils::EscapeXml(domain);

    std::wstringstream ss;
    ss << L"<?xml version=\"1.0\"?>\r\n"
       << L"<EapHostUserCredentials xmlns=\"http://www.microsoft.com/provisioning/EapHostUserCredentials\" "
       << L"xmlns:eapCommon=\"http://www.microsoft.com/provisioning/EapCommon\" "
       << L"xmlns:baseEap=\"http://www.microsoft.com/provisioning/BaseEapMethodUserPropertiesV1\">\r\n"
       << L"  <EapMethod>\r\n"
       << L"    <eapCommon:Type>25</eapCommon:Type>\r\n"
       << L"    <eapCommon:AuthorId>0</eapCommon:AuthorId>\r\n"
       << L"  </EapMethod>\r\n"
       << L"  <Credentials xmlns:eapUser=\"http://www.microsoft.com/provisioning/EapUserPropertiesV1\" "
       << L"xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
       << L"xmlns:baseEap=\"http://www.microsoft.com/provisioning/BaseEapUserPropertiesV1\" "
       << L"xmlns:MsPeap=\"http://www.microsoft.com/provisioning/MsPeapUserPropertiesV1\" "
       << L"xmlns:MsChapV2=\"http://www.microsoft.com/provisioning/MsChapV2UserPropertiesV1\">\r\n"
       << L"    <baseEap:Eap>\r\n"
       << L"      <baseEap:Type>25</baseEap:Type>\r\n"
       << L"      <MsPeap:EapType>\r\n"
       << L"        <MsPeap:RoutingIdentity>" << esc_user << L"</MsPeap:RoutingIdentity>\r\n"
       << L"        <baseEap:Eap>\r\n"
       << L"          <baseEap:Type>26</baseEap:Type>\r\n"
       << L"          <MsChapV2:EapType>\r\n"
       << L"            <MsChapV2:Username>" << esc_user << L"</MsChapV2:Username>\r\n"
       << L"            <MsChapV2:Password>" << esc_pass << L"</MsChapV2:Password>\r\n"
       << L"            <MsChapV2:LogonDomain>" << esc_domain << L"</MsChapV2:LogonDomain>\r\n"
       << L"          </MsChapV2:EapType>\r\n"
       << L"        </baseEap:Eap>\r\n"
       << L"      </MsPeap:EapType>\r\n"
       << L"    </baseEap:Eap>\r\n"
       << L"  </Credentials>\r\n"
       << L"</EapHostUserCredentials>";
    return ss.str();
}

std::wstring BuildOpenOrPskProfileXml(const std::wstring& ssid, const std::wstring& psk, bool is_open) {
    std::wstring escaped_ssid = utils::EscapeXml(ssid);
    std::wstring hex_ssid = utils::StringToHex(utils::WideToUtf8(ssid));
    std::wstring escaped_psk = utils::EscapeXml(psk);

    std::wstringstream ss;
    ss << L"<?xml version=\"1.0\"?>\r\n"
       << L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\r\n"
       << L"  <name>" << escaped_ssid << L"</name>\r\n"
       << L"  <SSIDConfig>\r\n"
       << L"    <SSID>\r\n"
       << L"      <hex>" << hex_ssid << L"</hex>\r\n"
       << L"      <name>" << escaped_ssid << L"</name>\r\n"
       << L"    </SSID>\r\n"
       << L"  </SSIDConfig>\r\n"
       << L"  <connectionType>ESS</connectionType>\r\n"
       << L"  <connectionMode>auto</connectionMode>\r\n"
       << L"  <MSM>\r\n"
       << L"    <security>\r\n"
       << L"      <authEncryption>\r\n";
    if (is_open) {
        ss << L"        <authentication>open</authentication>\r\n"
           << L"        <encryption>none</encryption>\r\n"
           << L"        <useOneX>false</useOneX>\r\n"
           << L"      </authEncryption>\r\n";
    } else {
        ss << L"        <authentication>WPA2PSK</authentication>\r\n"
           << L"        <encryption>AES</encryption>\r\n"
           << L"        <useOneX>false</useOneX>\r\n"
           << L"      </authEncryption>\r\n"
           << L"      <sharedKey>\r\n"
           << L"        <keyType>passPhrase</keyType>\r\n"
           << L"        <protected>false</protected>\r\n"
           << L"        <keyMaterial>" << escaped_psk << L"</keyMaterial>\r\n"
           << L"      </sharedKey>\r\n";
    }
    ss << L"    </security>\r\n"
       << L"  </MSM>\r\n"
       << L"</WLANProfile>";
    return ss.str();
}

bool WaitUntilConnected(const std::wstring& target_ssid, int timeout_ms) {
    int elapsed = 0;
    const int step = 400;
    while (elapsed < timeout_ms) {
        Sleep(step);
        elapsed += step;
        WifiConnectionState st = GetConnectionState();
        if (st.is_connected && st.current_ssid == target_ssid) {
            return true;
        }
    }
    return false;
}

}  // namespace

WifiConnectionState GetConnectionState() {
    WifiConnectionState state;
    ScopedWlanHandle wlan;
    if (!wlan.valid()) return state;

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList) {
        return state;
    }

    if (pIfList->dwNumberOfItems > 0) {
        state.has_adapter = true;
        const WLAN_INTERFACE_INFO& ifInfo = pIfList->InterfaceInfo[0];
        state.adapter_name = ifInfo.strInterfaceDescription;

        if (ifInfo.isState == wlan_interface_state_connected) {
            PWLAN_CONNECTION_ATTRIBUTES pConnAttr = nullptr;
            DWORD dataSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcodeType;
            if (WlanQueryInterface(wlan.handle, &ifInfo.InterfaceGuid,
                                   wlan_intf_opcode_current_connection, nullptr,
                                   &dataSize, reinterpret_cast<PVOID*>(&pConnAttr),
                                   &opcodeType) == ERROR_SUCCESS && pConnAttr) {
                if (pConnAttr->isState == wlan_interface_state_connected) {
                    state.is_connected = true;
                    state.current_ssid = SsidToWide(pConnAttr->wlanAssociationAttributes.dot11Ssid);
                    state.signal_quality = static_cast<int>(pConnAttr->wlanAssociationAttributes.wlanSignalQuality);
                }
                WlanFreeMemory(pConnAttr);
            }
        }
    }

    WlanFreeMemory(pIfList);
    return state;
}

std::vector<WifiNetworkInfo> ScanNetworks(bool trigger_active_scan) {
    std::vector<WifiNetworkInfo> results;
    ScopedWlanHandle wlan;
    if (!wlan.valid()) return results;

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList) {
        return results;
    }

    if (pIfList->dwNumberOfItems == 0) {
        WlanFreeMemory(pIfList);
        return results;
    }

    GUID ifGuid = pIfList->InterfaceInfo[0].InterfaceGuid;
    if (trigger_active_scan) {
        WlanScan(wlan.handle, &ifGuid, nullptr, nullptr, nullptr);
        Sleep(650);
    }

    PWLAN_AVAILABLE_NETWORK_LIST pNetList = nullptr;
    if (WlanGetAvailableNetworkList(wlan.handle, &ifGuid, 0, nullptr, &pNetList) == ERROR_SUCCESS && pNetList) {
        std::map<std::wstring, WifiNetworkInfo> best_by_ssid;
        for (DWORD i = 0; i < pNetList->dwNumberOfItems; ++i) {
            const WLAN_AVAILABLE_NETWORK& net = pNetList->Network[i];
            std::wstring ssid = SsidToWide(net.dot11Ssid);
            if (ssid.empty()) continue;

            WifiNetworkInfo info;
            info.ssid = ssid;
            info.signal_quality = static_cast<int>(net.wlanSignalQuality);
            info.is_secure = (net.bSecurityEnabled != FALSE);

            switch (net.dot11DefaultAuthAlgorithm) {
                case DOT11_AUTH_ALGO_80211_OPEN:
                    info.auth_desc = L"开放网络 (Portal)";
                    info.is_8021x = false;
                    break;
                case DOT11_AUTH_ALGO_WPA:
                case DOT11_AUTH_ALGO_RSNA:
                    info.auth_desc = L"802.1X 企业认证";
                    info.is_8021x = true;
                    break;
                case DOT11_AUTH_ALGO_WPA_PSK:
                case DOT11_AUTH_ALGO_RSNA_PSK:
                    info.auth_desc = L"WPA2-个人";
                    info.is_8021x = false;
                    break;
                default:
                    if (net.dot11DefaultAuthAlgorithm >= 8) {
                        info.auth_desc = L"WPA3/高级加密";
                    } else {
                        info.auth_desc = L"其他加密";
                    }
                    break;
            }

            auto it = best_by_ssid.find(ssid);
            if (it == best_by_ssid.end() || info.signal_quality > it->second.signal_quality) {
                best_by_ssid[ssid] = info;
            }
        }
        for (auto& kv : best_by_ssid) {
            results.push_back(kv.second);
        }
        std::sort(results.begin(), results.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
            return a.signal_quality > b.signal_quality;
        });
        WlanFreeMemory(pNetList);
    }

    WlanFreeMemory(pIfList);
    return results;
}

bool ConnectEnterprise8021X(const std::wstring& ssid,
                            const std::wstring& username,
                            const std::wstring& password,
                            const std::wstring& domain,
                            std::wstring& out_message) {
    if (ssid.empty()) {
        out_message = L"SSID 不能为空";
        return false;
    }
    if (username.empty()) {
        out_message = L"802.1X 认证账号不能为空";
        return false;
    }

    ScopedWlanHandle wlan;
    if (!wlan.valid()) {
        out_message = L"无法打开 Windows WLAN 服务句柄";
        return false;
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList || pIfList->dwNumberOfItems == 0) {
        if (pIfList) WlanFreeMemory(pIfList);
        out_message = L"未检测到可用的无线网卡接口";
        return false;
    }

    GUID ifGuid = pIfList->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(pIfList);

    std::wstring profile_xml = BuildEnterpriseProfileXml(ssid);
    DWORD reason = 0;
    DWORD profile_flags = 0;  // All-user profile
    DWORD err = WlanSetProfile(wlan.handle, &ifGuid, profile_flags, profile_xml.c_str(),
                               nullptr, TRUE, nullptr, &reason);
    if (err == ERROR_ACCESS_DENIED) {
        profile_flags = WLAN_PROFILE_USER;
        err = WlanSetProfile(wlan.handle, &ifGuid, profile_flags, profile_xml.c_str(),
                             nullptr, TRUE, nullptr, &reason);
    }
    if (err != ERROR_SUCCESS) {
        out_message = L"写入 802.1X 配置文件失败 (错误码 " + std::to_wstring(err) + L", " + FormatReasonCode(reason) + L")";
        return false;
    }

    std::wstring user_xml = BuildPeapUserCredentialsXml(username, password, domain);
    DWORD eap_err = WlanSetProfileEapXmlUserData(wlan.handle, &ifGuid, ssid.c_str(),
                                                 0, user_xml.c_str(), nullptr);
    if (eap_err != ERROR_SUCCESS) {
        // Also try setting for all users if per-user fails
        eap_err = WlanSetProfileEapXmlUserData(wlan.handle, &ifGuid, ssid.c_str(),
                                               WLAN_SET_EAPHOST_DATA_ALL_USERS, user_xml.c_str(), nullptr);
    }
    if (eap_err != ERROR_SUCCESS) {
        out_message = L"设置 802.1X 账号密码凭据失败 (错误码 " + std::to_wstring(eap_err) + L")";
        return false;
    }

    DOT11_SSID dot11_ssid = WideToDot11Ssid(ssid);
    WLAN_CONNECTION_PARAMETERS conn_params = {};
    conn_params.wlanConnectionMode = wlan_connection_mode_profile;
    conn_params.strProfile = ssid.c_str();
    conn_params.pDot11Ssid = &dot11_ssid;
    conn_params.pDesiredBssidList = nullptr;
    conn_params.dot11BssType = dot11_BSS_type_infrastructure;
    conn_params.dwFlags = 0;

    err = WlanConnect(wlan.handle, &ifGuid, &conn_params, nullptr);
    if (err != ERROR_SUCCESS) {
        out_message = L"发起 802.1X 连接请求失败 (错误码 " + std::to_wstring(err) + L")";
        return false;
    }

    if (WaitUntilConnected(ssid, 12000)) {
        out_message = L"已成功通过 802.1X 认证并连接至 [" + ssid + L"]";
        return true;
    }

    out_message = L"已发送 802.1X 连接请求，但等待关联超时（请检查信号或账号密码）";
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
    if (!is_open && psk.size() < 8) {
        out_message = L"WPA2 预共享密钥长度至少需要 8 位";
        return false;
    }

    // If already connected to this SSID, skip re-associating
    WifiConnectionState cur = GetConnectionState();
    if (cur.is_connected && cur.current_ssid == ssid) {
        out_message = L"当前已连接至 Wi-Fi [" + ssid + L"]";
        return true;
    }

    ScopedWlanHandle wlan;
    if (!wlan.valid()) {
        out_message = L"无法打开 Windows WLAN 服务句柄";
        return false;
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList || pIfList->dwNumberOfItems == 0) {
        if (pIfList) WlanFreeMemory(pIfList);
        out_message = L"未检测到可用的无线网卡接口";
        return false;
    }

    GUID ifGuid = pIfList->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(pIfList);

    std::wstring profile_xml = BuildOpenOrPskProfileXml(ssid, psk, is_open);
    DWORD reason = 0;
    DWORD err = WlanSetProfile(wlan.handle, &ifGuid, 0, profile_xml.c_str(),
                               nullptr, TRUE, nullptr, &reason);
    if (err == ERROR_ACCESS_DENIED) {
        err = WlanSetProfile(wlan.handle, &ifGuid, WLAN_PROFILE_USER, profile_xml.c_str(),
                             nullptr, TRUE, nullptr, &reason);
    }
    if (err != ERROR_SUCCESS) {
        out_message = L"写入 Wi-Fi 配置文件失败 (错误码 " + std::to_wstring(err) + L", " + FormatReasonCode(reason) + L")";
        return false;
    }

    DOT11_SSID dot11_ssid = WideToDot11Ssid(ssid);
    WLAN_CONNECTION_PARAMETERS conn_params = {};
    conn_params.wlanConnectionMode = wlan_connection_mode_profile;
    conn_params.strProfile = ssid.c_str();
    conn_params.pDot11Ssid = &dot11_ssid;
    conn_params.pDesiredBssidList = nullptr;
    conn_params.dot11BssType = dot11_BSS_type_infrastructure;
    conn_params.dwFlags = 0;

    err = WlanConnect(wlan.handle, &ifGuid, &conn_params, nullptr);
    if (err != ERROR_SUCCESS) {
        out_message = L"发起 Wi-Fi 连接失败 (错误码 " + std::to_wstring(err) + L")";
        return false;
    }

    if (WaitUntilConnected(ssid, 10000)) {
        out_message = L"已成功关联至热点 [" + ssid + L"]";
        return true;
    }

    out_message = L"关联热点 [" + ssid + L"] 超时";
    return false;
}

bool Disconnect(std::wstring& out_message) {
    ScopedWlanHandle wlan;
    if (!wlan.valid()) {
        out_message = L"无法打开 Windows WLAN 服务句柄";
        return false;
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList || pIfList->dwNumberOfItems == 0) {
        if (pIfList) WlanFreeMemory(pIfList);
        out_message = L"未检测到可用的无线网卡接口";
        return false;
    }

    GUID ifGuid = pIfList->InterfaceInfo[0].InterfaceGuid;
    WlanFreeMemory(pIfList);

    DWORD err = WlanDisconnect(wlan.handle, &ifGuid, nullptr);
    if (err == ERROR_SUCCESS) {
        out_message = L"已断开当前 Wi-Fi 连接";
        return true;
    }
    out_message = L"断开 Wi-Fi 失败 (错误码 " + std::to_wstring(err) + L")";
    return false;
}

}  // namespace wifi
