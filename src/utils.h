#pragma once

#include <string>
#include <vector>

namespace utils {

// UTF-8 <-> UTF-16 (Wide string) conversions
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// String trimming
std::wstring Trim(const std::wstring& s);
std::string TrimUtf8(const std::string& s);

// XML escaping for WLAN Profile and EAP credentials XML
std::wstring EscapeXml(const std::wstring& input);

// Hex encoding for SSID in WLANProfile
std::wstring StringToHex(const std::string& input);

// URL encoding for HTTP form/query parameters
std::string UrlEncode(const std::string& value);

// JSON string escaping
std::string EscapeJson(const std::string& value);

// Windows DPAPI encryption/decryption (returns/accepts hex-encoded ciphertext)
std::wstring EncryptPassword(const std::wstring& plain_text);
std::wstring DecryptPassword(const std::wstring& hex_cipher);

// Current time string [HH:MM:SS]
std::wstring CurrentTimeStr();

}  // namespace utils
