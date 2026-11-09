#include "config.h"
#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

namespace config {

static const wchar_t* kRegRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRegValueName = L"AutoWiFiClient";
static const wchar_t* kSectionGeneral = L"AutoWiFi";

std::wstring GetConfigFilePath() {
    // Check if portable config.ini exists next to executable first
    wchar_t exe_path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        std::wstring dir(exe_path);
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring portable_ini = dir.substr(0, pos + 1) + L"config.ini";
            DWORD attr = GetFileAttributesW(portable_ini.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return portable_ini;
            }
        }
    }

    // Default to %APPDATA%\AutoWiFi\config.ini
    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\AutoWiFi";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.ini";
    }
    return L"config.ini";
}

static std::wstring ReadIniString(const std::wstring& path, const wchar_t* key, const wchar_t* def_val) {
    wchar_t buf[2048] = {0};
    GetPrivateProfileStringW(kSectionGeneral, key, def_val, buf, 2048, path.c_str());
    return buf;
}

AppConfig LoadConfig() {
    AppConfig cfg;
    std::wstring path = GetConfigFilePath();

    cfg.ssid = ReadIniString(path, L"SSID", L"");
    int mode = GetPrivateProfileIntW(kSectionGeneral, L"AuthMode", 0, path.c_str());
    if (mode < 0 || mode > 3) mode = 0;
    cfg.auth_mode = static_cast<AuthMode>(mode);

    cfg.username = ReadIniString(path, L"Username", L"");
    std::wstring enc_pwd = ReadIniString(path, L"PasswordEnc", L"");
    if (!enc_pwd.empty()) {
        cfg.password = utils::DecryptPassword(enc_pwd);
    } else {
        // Fallback if user manually edited PlainPassword in config.ini
        cfg.password = ReadIniString(path, L"PlainPassword", L"");
    }

    cfg.domain_or_psk = ReadIniString(path, L"DomainOrPsk", L"");
    cfg.portal_url = ReadIniString(path, L"PortalUrl", L"");

    int pmethod = GetPrivateProfileIntW(kSectionGeneral, L"PortalMethod", 0, path.c_str());
    if (pmethod < 0 || pmethod > 2) pmethod = 0;
    cfg.portal_method = static_cast<PortalMethod>(pmethod);

    cfg.portal_user_field = ReadIniString(path, L"PortalUserField", L"username");
    cfg.portal_pass_field = ReadIniString(path, L"PortalPassField", L"password");
    cfg.portal_extra_params = ReadIniString(path, L"PortalExtraParams", L"");

    cfg.auto_connect = GetPrivateProfileIntW(kSectionGeneral, L"AutoConnect", 1, path.c_str()) != 0;
    cfg.check_interval_sec = GetPrivateProfileIntW(kSectionGeneral, L"CheckIntervalSec", 15, path.c_str());
    if (cfg.check_interval_sec < 3) cfg.check_interval_sec = 3;
    if (cfg.check_interval_sec > 3600) cfg.check_interval_sec = 3600;

    cfg.auto_start = IsAutoStartEnabled();
    return cfg;
}

bool SaveConfig(const AppConfig& cfg) {
    std::wstring path = GetConfigFilePath();

    WritePrivateProfileStringW(kSectionGeneral, L"SSID", cfg.ssid.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"AuthMode",
                               std::to_wstring(static_cast<int>(cfg.auth_mode)).c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"Username", cfg.username.c_str(), path.c_str());

    std::wstring enc_pwd = utils::EncryptPassword(cfg.password);
    WritePrivateProfileStringW(kSectionGeneral, L"PasswordEnc", enc_pwd.c_str(), path.c_str());
    // Remove any plain password key for security
    WritePrivateProfileStringW(kSectionGeneral, L"PlainPassword", nullptr, path.c_str());

    WritePrivateProfileStringW(kSectionGeneral, L"DomainOrPsk", cfg.domain_or_psk.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"PortalUrl", cfg.portal_url.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"PortalMethod",
                               std::to_wstring(static_cast<int>(cfg.portal_method)).c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"PortalUserField", cfg.portal_user_field.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"PortalPassField", cfg.portal_pass_field.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"PortalExtraParams", cfg.portal_extra_params.c_str(), path.c_str());

    WritePrivateProfileStringW(kSectionGeneral, L"AutoConnect", cfg.auto_connect ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"CheckIntervalSec",
                               std::to_wstring(cfg.check_interval_sec).c_str(), path.c_str());

    SetAutoStartRegistry(cfg.auto_start);
    return true;
}

bool SetAutoStartRegistry(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    bool ok = false;
    if (enable) {
        wchar_t exe_path[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
            std::wstring cmd = L"\"" + std::wstring(exe_path) + L"\" --minimized";
            DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
            ok = (RegSetValueExW(hKey, kRegValueName, 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(cmd.c_str()), bytes) == ERROR_SUCCESS);
        }
    } else {
        LSTATUS st = RegDeleteValueW(hKey, kRegValueName);
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(hKey);
    return ok;
}

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    bool exists = (RegQueryValueExW(hKey, kRegValueName, nullptr, &type,
                                    reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
                   type == REG_SZ && buf[0] != L'\0');
    RegCloseKey(hKey);
    return exists;
}

}  // namespace config
