#include "config.h"
#include "utils.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace config {

#ifdef _WIN32

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

static int ReadIniInt(const std::wstring& path, const wchar_t* key, int def_val) {
    return GetPrivateProfileIntW(kSectionGeneral, key, def_val, path.c_str());
}

#else  // POSIX (macOS / Linux)

static std::string GetExecutablePathPosix() {
    const char* orig_exe = std::getenv("AUTOWIFI_ORIGINAL_EXE");
    if (orig_exe && orig_exe[0] != '\0') {
        return orig_exe;
    }
#ifdef __APPLE__
    char buf[PATH_MAX] = {0};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real_buf[PATH_MAX] = {0};
        if (realpath(buf, real_buf)) return real_buf;
        return buf;
    }
#else
    char buf[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return buf;
    }
#endif
    return "";
}

static bool FileExistsPosix(const std::string& path) {
    struct stat st{};
    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

static void MkdirRecursivePosix(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0700);
        }
    }
    mkdir(dir.c_str(), 0700);
}

std::wstring GetConfigFilePath() {
    std::string exe_path = GetExecutablePathPosix();
    if (!exe_path.empty()) {
        size_t pos = exe_path.find_last_of('/');
        if (pos != std::string::npos) {
            std::string portable_ini = exe_path.substr(0, pos + 1) + "config.ini";
            if (FileExistsPosix(portable_ini)) {
                return utils::Utf8ToWide(portable_ini);
            }
        }
    }

    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base_dir;
    if (xdg && xdg[0] != '\0') {
        base_dir = std::string(xdg) + "/auto-wifi";
    } else {
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            base_dir = std::string(home) + "/.config/auto-wifi";
        } else {
            return L"config.ini";
        }
    }
    MkdirRecursivePosix(base_dir);
    return utils::Utf8ToWide(base_dir + "/config.ini");
}

static std::map<std::string, std::string> LoadPosixIniMap(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream ifs(path);
    if (!ifs.is_open()) return kv;

    std::string line;
    bool in_section = true;  // Accept top-level or [AutoWiFi]
    while (std::getline(ifs, line)) {
        std::string trimmed = utils::TrimUtf8(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string sec = trimmed.substr(1, trimmed.size() - 2);
            in_section = (sec == "AutoWiFi");
            continue;
        }
        if (!in_section) continue;
        size_t eq = trimmed.find('=');
        if (eq != std::string::npos) {
            std::string key = utils::TrimUtf8(trimmed.substr(0, eq));
            std::string val = utils::TrimUtf8(trimmed.substr(eq + 1));
            kv[key] = val;
        }
    }
    return kv;
}

static std::wstring ReadIniString(const std::wstring& path, const wchar_t* key, const wchar_t* def_val) {
    auto kv = LoadPosixIniMap(utils::WideToUtf8(path));
    std::string k = utils::WideToUtf8(key);
    auto it = kv.find(k);
    if (it != kv.end()) {
        return utils::Utf8ToWide(it->second);
    }
    return def_val ? def_val : L"";
}

static int ReadIniInt(const std::wstring& path, const wchar_t* key, int def_val) {
    std::wstring s = ReadIniString(path, key, L"");
    if (s.empty()) return def_val;
    try {
        return std::stoi(utils::WideToUtf8(s));
    } catch (...) {
        return def_val;
    }
}

#endif

AppConfig LoadConfig() {
    AppConfig cfg;
    std::wstring path = GetConfigFilePath();

    cfg.ssid = ReadIniString(path, L"SSID", L"");
    int mode = ReadIniInt(path, L"AuthMode", 0);
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

    int pmethod = ReadIniInt(path, L"PortalMethod", 0);
    if (pmethod < 0 || pmethod > 2) pmethod = 0;
    cfg.portal_method = static_cast<PortalMethod>(pmethod);

    cfg.portal_user_field = ReadIniString(path, L"PortalUserField", L"username");
    cfg.portal_pass_field = ReadIniString(path, L"PortalPassField", L"password");
    cfg.portal_extra_params = ReadIniString(path, L"PortalExtraParams", L"");

    cfg.auto_connect = ReadIniInt(path, L"AutoConnect", 1) != 0;
    cfg.check_interval_sec = ReadIniInt(path, L"CheckIntervalSec", 15);
    if (cfg.check_interval_sec < 3) cfg.check_interval_sec = 3;
    if (cfg.check_interval_sec > 3600) cfg.check_interval_sec = 3600;

    cfg.auto_start = IsAutoStartEnabled();
    return cfg;
}

bool SaveConfig(const AppConfig& cfg) {
    std::wstring path = GetConfigFilePath();
    std::wstring enc_pwd = utils::EncryptPassword(cfg.password);

#ifdef _WIN32
    WritePrivateProfileStringW(kSectionGeneral, L"SSID", cfg.ssid.c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"AuthMode",
                               std::to_wstring(static_cast<int>(cfg.auth_mode)).c_str(), path.c_str());
    WritePrivateProfileStringW(kSectionGeneral, L"Username", cfg.username.c_str(), path.c_str());
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
#else
    std::string path_utf8 = utils::WideToUtf8(path);
    std::ofstream ofs(path_utf8, std::ios::trunc);
    if (!ofs.is_open()) return false;

    ofs << "[AutoWiFi]\n"
        << "SSID=" << utils::WideToUtf8(cfg.ssid) << "\n"
        << "AuthMode=" << static_cast<int>(cfg.auth_mode) << "\n"
        << "Username=" << utils::WideToUtf8(cfg.username) << "\n"
        << "PasswordEnc=" << utils::WideToUtf8(enc_pwd) << "\n"
        << "DomainOrPsk=" << utils::WideToUtf8(cfg.domain_or_psk) << "\n"
        << "PortalUrl=" << utils::WideToUtf8(cfg.portal_url) << "\n"
        << "PortalMethod=" << static_cast<int>(cfg.portal_method) << "\n"
        << "PortalUserField=" << utils::WideToUtf8(cfg.portal_user_field) << "\n"
        << "PortalPassField=" << utils::WideToUtf8(cfg.portal_pass_field) << "\n"
        << "PortalExtraParams=" << utils::WideToUtf8(cfg.portal_extra_params) << "\n"
        << "AutoConnect=" << (cfg.auto_connect ? 1 : 0) << "\n"
        << "CheckIntervalSec=" << cfg.check_interval_sec << "\n";
    ofs.close();
    chmod(path_utf8.c_str(), 0600);
#endif

    SetAutoStartRegistry(cfg.auto_start);
    return true;
}

#ifdef _WIN32

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

#elif defined(__APPLE__)

static std::string GetMacLaunchAgentPath() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return "";
    std::string dir = std::string(home) + "/Library/LaunchAgents";
    MkdirRecursivePosix(dir);
    return dir + "/io.github.autowifi.plist";
}

bool SetAutoStartRegistry(bool enable) {
    std::string plist_path = GetMacLaunchAgentPath();
    if (plist_path.empty()) return false;

    if (!enable) {
        unlink(plist_path.c_str());
        return true;
    }

    std::string exe_path = GetExecutablePathPosix();
    if (exe_path.empty()) return false;

    std::ofstream ofs(plist_path, std::ios::trunc);
    if (!ofs.is_open()) return false;

    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n"
        << "<dict>\n"
        << "  <key>Label</key>\n"
        << "  <string>io.github.autowifi</string>\n"
        << "  <key>ProgramArguments</key>\n"
        << "  <array>\n"
        << "    <string>" << exe_path << "</string>\n"
        << "    <string>--daemon</string>\n"
        << "  </array>\n"
        << "  <key>RunAtLoad</key>\n"
        << "  <true/>\n"
        << "  <key>KeepAlive</key>\n"
        << "  <true/>\n"
        << "</dict>\n"
        << "</plist>\n";
    return true;
}

bool IsAutoStartEnabled() {
    std::string plist_path = GetMacLaunchAgentPath();
    return !plist_path.empty() && FileExistsPosix(plist_path);
}

#else  // Linux XDG Autostart

static std::string GetLinuxAutostartPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string dir;
    if (xdg && xdg[0]) {
        dir = std::string(xdg) + "/autostart";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !home[0]) return "";
        dir = std::string(home) + "/.config/autostart";
    }
    MkdirRecursivePosix(dir);
    return dir + "/auto-wifi.desktop";
}

bool SetAutoStartRegistry(bool enable) {
    std::string desktop_path = GetLinuxAutostartPath();
    if (desktop_path.empty()) return false;

    if (!enable) {
        unlink(desktop_path.c_str());
        return true;
    }

    std::string exe_path = GetExecutablePathPosix();
    if (exe_path.empty()) return false;

    std::ofstream ofs(desktop_path, std::ios::trunc);
    if (!ofs.is_open()) return false;

    ofs << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=AutoWiFi\n"
        << "Comment=Automatic Wi-Fi 802.1X & Captive Portal Client\n"
        << "Exec=\"" << exe_path << "\" --daemon\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
    return true;
}

bool IsAutoStartEnabled() {
    std::string desktop_path = GetLinuxAutostartPath();
    return !desktop_path.empty() && FileExistsPosix(desktop_path);
}

#endif

}  // namespace config
