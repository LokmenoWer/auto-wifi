#include "utils.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#endif

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace utils {

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (size_needed <= 0) return L"";
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &result[0], size_needed);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &result[0], size_needed, nullptr, nullptr);
    return result;
}

#else  // POSIX UTF-8 <-> UTF-32 (wchar_t)

std::wstring Utf8ToWide(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        uint32_t cp = 0;
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = (c & 0x1F) << 6;
            cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = (c & 0x0F) << 12;
            cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6;
            cp |= (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = (c & 0x07) << 18;
            cp |= (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12;
            cp |= (static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6;
            cp |= (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = c;
            i += 1;
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string WideToUtf8(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size() * 3);
    for (wchar_t wc : wide) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

#endif

std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string TrimUtf8(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring EscapeXml(const std::wstring& input) {
    std::wstring out;
    out.reserve(input.size() * 2);
    for (wchar_t c : input) {
        switch (c) {
            case L'&':  out += L"&amp;"; break;
            case L'<':  out += L"&lt;"; break;
            case L'>':  out += L"&gt;"; break;
            case L'\"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:    out += c; break;
        }
    }
    return out;
}

std::wstring StringToHex(const std::string& input) {
    static const wchar_t kHexDigits[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(input.size() * 2);
    for (unsigned char b : input) {
        out.push_back(kHexDigits[(b >> 4) & 0x0F]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

std::string HexToBytes(const std::wstring& hex) {
    if (hex.empty() || (hex.size() % 2 != 0)) return "";
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = HexVal(hex[i]);
        int lo = HexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) return "";
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }
    return bytes;
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << (char)c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int(c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string EscapeJson(const std::string& value) {
    std::ostringstream o;
    for (unsigned char c : value) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    o << (char)c;
                }
        }
    }
    return o.str();
}

#ifdef _WIN32

std::wstring EncryptPassword(const std::wstring& plain_text) {
    if (plain_text.empty()) return L"";
    std::string utf8 = WideToUtf8(plain_text);

    DATA_BLOB in_blob;
    in_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.data()));
    in_blob.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB out_blob = {0, nullptr};
    if (!CryptProtectData(&in_blob, L"AutoWiFiCredential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return L"";
    }

    std::string raw(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    LocalFree(out_blob.pbData);
    return StringToHex(raw);
}

std::wstring DecryptPassword(const std::wstring& hex_cipher) {
    std::string raw = HexToBytes(hex_cipher);
    if (raw.empty()) return L"";

    DATA_BLOB in_blob;
    in_blob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw.data()));
    in_blob.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB out_blob = {0, nullptr};
    if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return L"";
    }

    std::string utf8(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    LocalFree(out_blob.pbData);
    return Utf8ToWide(utf8);
}

#else  // POSIX local user-salted stream encryption

static uint64_t DerivePosixKeyStream(uint64_t iv, size_t index) {
    const char* user = std::getenv("USER");
    std::string seed = std::string("AutoWiFi_POSIX_v1:") + (user ? user : "default");
    uint64_t h = 0xCBF29CE484222325ULL ^ iv ^ (static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ULL);
    for (unsigned char c : seed) {
        h ^= c;
        h *= 0x100000001B3ULL;
    }
    // SplitMix64 finalizer
    h ^= (h >> 30);
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= (h >> 27);
    h *= 0x94D049BB133111EBULL;
    h ^= (h >> 31);
    return h;
}

std::wstring EncryptPassword(const std::wstring& plain_text) {
    if (plain_text.empty()) return L"";
    std::string utf8 = WideToUtf8(plain_text);

    std::random_device rd;
    uint64_t iv = (static_cast<uint64_t>(rd()) << 32) | static_cast<uint64_t>(rd());

    std::string out;
    out.resize(8 + utf8.size());
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<char>((iv >> (i * 8)) & 0xFF);
    }
    for (size_t i = 0; i < utf8.size(); ++i) {
        uint64_t ks = DerivePosixKeyStream(iv, i / 8);
        uint8_t kbyte = static_cast<uint8_t>((ks >> ((i % 8) * 8)) & 0xFF);
        out[8 + i] = static_cast<char>(static_cast<uint8_t>(utf8[i]) ^ kbyte);
    }
    return StringToHex(out);
}

std::wstring DecryptPassword(const std::wstring& hex_cipher) {
    std::string raw = HexToBytes(hex_cipher);
    if (raw.size() <= 8) return L"";

    uint64_t iv = 0;
    for (int i = 0; i < 8; ++i) {
        iv |= (static_cast<uint64_t>(static_cast<uint8_t>(raw[i])) << (i * 8));
    }

    std::string utf8;
    utf8.resize(raw.size() - 8);
    for (size_t i = 0; i < utf8.size(); ++i) {
        uint64_t ks = DerivePosixKeyStream(iv, i / 8);
        uint8_t kbyte = static_cast<uint8_t>((ks >> ((i % 8) * 8)) & 0xFF);
        utf8[i] = static_cast<char>(static_cast<uint8_t>(raw[8 + i]) ^ kbyte);
    }
    return Utf8ToWide(utf8);
}

#endif

std::wstring CurrentTimeStr() {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"[%02d:%02d:%02d]", st.wHour, st.wMinute, st.wSecond);
    return buf;
#else
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    wchar_t buf[32];
    swprintf(buf, 32, L"[%02d:%02d:%02d]", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return buf;
#endif
}

void SleepMs(int milliseconds) {
    if (milliseconds <= 0) return;
#ifdef _WIN32
    Sleep(static_cast<DWORD>(milliseconds));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}

#ifndef _WIN32

std::string ShellEscape(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string RunCommand(const std::string& cmd, int* out_exit_code) {
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        if (out_exit_code) *out_exit_code = -1;
        return "";
    }
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int status = pclose(pipe);
    if (out_exit_code) {
        if (WIFEXITED(status)) {
            *out_exit_code = WEXITSTATUS(status);
        } else {
            *out_exit_code = status;
        }
    }
    return output;
}

#endif

}  // namespace utils
