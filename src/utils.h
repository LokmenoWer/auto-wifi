#pragma once

#include <string>
#include <vector>

namespace utils {

// UTF-8 <-> Wide string conversions (cross-platform)
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// String trimming
std::wstring Trim(const std::wstring& s);
std::string TrimUtf8(const std::string& s);

// XML escaping for WLAN Profile and EAP credentials XML
std::wstring EscapeXml(const std::wstring& input);

// Hex encoding for SSID in WLANProfile / ciphertexts
std::wstring StringToHex(const std::string& input);
std::string HexToBytes(const std::wstring& hex);

// URL encoding for HTTP form/query parameters
std::string UrlEncode(const std::string& value);

// JSON string escaping
std::string EscapeJson(const std::string& value);

// Password encryption/decryption (Windows DPAPI on Win32, salted stream cipher on POSIX)
std::wstring EncryptPassword(const std::wstring& plain_text);
std::wstring DecryptPassword(const std::wstring& hex_cipher);

// Current time string [HH:MM:SS]
std::wstring CurrentTimeStr();

// Cross-platform sleep in milliseconds
void SleepMs(int milliseconds);

#ifndef _WIN32
// POSIX shell single-quote escaping
std::string ShellEscape(const std::string& arg);

// Execute a command on POSIX and capture stdout+stderr
std::string RunCommand(const std::string& cmd, int* out_exit_code = nullptr);
#endif

}  // namespace utils
