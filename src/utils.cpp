#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <iomanip>
#include <sstream>

namespace utils {

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

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

std::wstring DecryptPassword(const std::wstring& hex_cipher) {
    if (hex_cipher.empty() || (hex_cipher.size() % 2 != 0)) return L"";
    std::vector<BYTE> bytes;
    bytes.reserve(hex_cipher.size() / 2);
    for (size_t i = 0; i < hex_cipher.size(); i += 2) {
        int hi = HexVal(hex_cipher[i]);
        int lo = HexVal(hex_cipher[i + 1]);
        if (hi < 0 || lo < 0) return L"";
        bytes.push_back(static_cast<BYTE>((hi << 4) | lo));
    }

    DATA_BLOB in_blob;
    in_blob.pbData = bytes.data();
    in_blob.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB out_blob = {0, nullptr};
    if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return L"";
    }

    std::string utf8(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    LocalFree(out_blob.pbData);
    return Utf8ToWide(utf8);
}

std::wstring CurrentTimeStr() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"[%02d:%02d:%02d]", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

}  // namespace utils
