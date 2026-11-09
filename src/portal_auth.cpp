#include "portal_auth.h"
#include "utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace portal {

namespace {

struct ScopedWinHttp {
    HINTERNET h = nullptr;
    explicit ScopedWinHttp(HINTERNET handle = nullptr) : h(handle) {}
    ~ScopedWinHttp() {
        if (h) WinHttpCloseHandle(h);
    }
    operator HINTERNET() const { return h; }
    bool valid() const { return h != nullptr; }
};

std::string ReadResponseBody(HINTERNET hRequest, size_t max_bytes = 65536) {
    std::string body;
    DWORD dwSize = 0;
    while (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
        if (body.size() + dwSize > max_bytes) {
            dwSize = static_cast<DWORD>(max_bytes - body.size());
        }
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize);
        DWORD dwRead = 0;
        if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwRead) && dwRead > 0) {
            body.append(buf.data(), dwRead);
        } else {
            break;
        }
        if (body.size() >= max_bytes) break;
    }
    return body;
}

std::wstring ExtractRedirectFromHtml(const std::string& html) {
    // Check common patterns in captive portal HTML stubs:
    // 1. url=http... or URL='http...'
    // 2. location.href="http..." or location.replace("http...")
    const char* patterns[] = {"url=", "URL=", "location.href=", "location.replace("};
    for (const char* pat : patterns) {
        size_t pos = html.find(pat);
        if (pos != std::string::npos) {
            pos += strlen(pat);
            while (pos < html.size() && (html[pos] == ' ' || html[pos] == '\'' || html[pos] == '"')) {
                ++pos;
            }
            size_t end = pos;
            while (end < html.size() && html[end] != '\'' && html[end] != '"' &&
                   html[end] != '>' && html[end] != ' ' && html[end] != ';' && html[end] != ')') {
                ++end;
            }
            if (end > pos) {
                std::string candidate = html.substr(pos, end - pos);
                if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0) {
                    return utils::Utf8ToWide(candidate);
                }
            }
        }
    }
    return L"";
}

void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

}  // namespace

NetStatus CheckInternetState(std::wstring& out_redirect_url) {
    out_redirect_url.clear();

    ScopedWinHttp hSession(WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AutoWiFi/1.0",
                                       WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession.valid()) return NetStatus::OFFLINE;

    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

    ScopedWinHttp hConnect(WinHttpConnect(hSession, L"www.msftconnecttest.com", INTERNET_DEFAULT_HTTP_PORT, 0));
    if (!hConnect.valid()) return NetStatus::OFFLINE;

    ScopedWinHttp hRequest(WinHttpOpenRequest(hConnect, L"GET", L"/connecttest.txt",
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_REFRESH));
    if (!hRequest.valid()) return NetStatus::OFFLINE;

    // Disable automatic redirects so we can catch 302 Captive Portal redirects
    DWORD disable_redirect = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirect, sizeof(disable_redirect));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        return NetStatus::OFFLINE;
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX);

    if (status_code >= 300 && status_code < 400) {
        DWORD loc_len = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &loc_len, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && loc_len > 0) {
            std::wstring loc(loc_len / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &loc[0], &loc_len, WINHTTP_NO_HEADER_INDEX)) {
                if (!loc.empty() && loc.back() == L'\0') loc.pop_back();
                out_redirect_url = loc;
            }
        }
        return NetStatus::CAPTIVE_PORTAL;
    }

    std::string body = ReadResponseBody(hRequest, 8192);
    if (status_code == 200) {
        if (body.find("Microsoft Connect Test") != std::string::npos) {
            return NetStatus::ONLINE;
        }
        // Gateway returned HTTP 200 with HTML redirect page
        out_redirect_url = ExtractRedirectFromHtml(body);
        return NetStatus::CAPTIVE_PORTAL;
    }

    return NetStatus::OFFLINE;
}

bool LoginCaptivePortal(const AppConfig& cfg,
                        const std::wstring& detected_redirect_url,
                        std::wstring& out_message) {
    std::wstring target_url = utils::Trim(cfg.portal_url);
    if (target_url.empty()) {
        target_url = utils::Trim(detected_redirect_url);
    }
    if (target_url.empty()) {
        out_message = L"未配置 Portal 登录 URL，且未能自动探测到重定向地址";
        return false;
    }

    std::string user_utf8 = utils::WideToUtf8(cfg.username);
    std::string pass_utf8 = utils::WideToUtf8(cfg.password);
    std::string user_field = utils::WideToUtf8(cfg.portal_user_field.empty() ? L"username" : cfg.portal_user_field);
    std::string pass_field = utils::WideToUtf8(cfg.portal_pass_field.empty() ? L"password" : cfg.portal_pass_field);
    std::string extra_utf8 = utils::WideToUtf8(cfg.portal_extra_params);

    // Replace placeholders if present in URL or extra params
    std::string url_utf8 = utils::WideToUtf8(target_url);
    ReplaceAll(url_utf8, "{username}", utils::UrlEncode(user_utf8));
    ReplaceAll(url_utf8, "{password}", utils::UrlEncode(pass_utf8));
    ReplaceAll(extra_utf8, "{username}", utils::UrlEncode(user_utf8));
    ReplaceAll(extra_utf8, "{password}", utils::UrlEncode(pass_utf8));

    std::string form_query = utils::UrlEncode(user_field) + "=" + utils::UrlEncode(user_utf8) +
                             "&" + utils::UrlEncode(pass_field) + "=" + utils::UrlEncode(pass_utf8);
    if (!extra_utf8.empty()) {
        if (extra_utf8[0] != '&') form_query += "&";
        form_query += extra_utf8;
    }

    if (cfg.portal_method == PortalMethod::GET_QUERY) {
        if (url_utf8.find('?') == std::string::npos) {
            url_utf8 += "?" + form_query;
        } else {
            url_utf8 += "&" + form_query;
        }
    }

    std::wstring final_url = utils::Utf8ToWide(url_utf8);

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(final_url.c_str(), static_cast<DWORD>(final_url.size()), 0, &urlComp)) {
        out_message = L"Portal URL 格式无效: " + final_url;
        return false;
    }

    ScopedWinHttp hSession(WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AutoWiFi/1.0",
                                       WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession.valid()) {
        out_message = L"初始化 WinHTTP 会话失败";
        return false;
    }

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    ScopedWinHttp hConnect(WinHttpConnect(hSession, host, urlComp.nPort, 0));
    if (!hConnect.valid()) {
        out_message = std::wstring(L"无法连接到认证服务器: ") + host;
        return false;
    }

    const wchar_t* verb = (cfg.portal_method == PortalMethod::GET_QUERY) ? L"GET" : L"POST";
    DWORD req_flags = WINHTTP_FLAG_REFRESH;
    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
        req_flags |= WINHTTP_FLAG_SECURE;
    }

    std::wstring full_path = (path[0] != L'\0') ? path : L"/";
    ScopedWinHttp hRequest(WinHttpOpenRequest(hConnect, verb, full_path.c_str(),
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags));
    if (!hRequest.valid()) {
        out_message = L"创建 HTTP 认证请求失败";
        return false;
    }

    if (urlComp.nScheme == INTERNET_SCHEME_HTTPS) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
    }

    std::string post_body;
    std::wstring headers;
    if (cfg.portal_method == PortalMethod::POST_FORM) {
        headers = L"Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n";
        post_body = form_query;
    } else if (cfg.portal_method == PortalMethod::POST_JSON) {
        headers = L"Content-Type: application/json; charset=UTF-8\r\n";
        std::ostringstream js;
        js << "{\"" << utils::EscapeJson(user_field) << "\":\"" << utils::EscapeJson(user_utf8) << "\","
           << "\"" << utils::EscapeJson(pass_field) << "\":\"" << utils::EscapeJson(pass_utf8) << "\"";
        if (!extra_utf8.empty()) {
            // Support key=val&key2=val2 appended as JSON fields
            std::istringstream iss(extra_utf8);
            std::string kv;
            while (std::getline(iss, kv, '&')) {
                size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    std::string k = kv.substr(0, eq);
                    std::string v = kv.substr(eq + 1);
                    js << ",\"" << utils::EscapeJson(k) << "\":\"" << utils::EscapeJson(v) << "\"";
                }
            }
        }
        js << "}";
        post_body = js.str();
    }

    LPCWSTR hdr_ptr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
    LPVOID body_ptr = post_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(post_body.data());
    DWORD body_len = static_cast<DWORD>(post_body.size());

    if (!WinHttpSendRequest(hRequest, hdr_ptr, static_cast<DWORD>(headers.size()),
                            body_ptr, body_len, body_len, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        out_message = L"发送 Portal 认证请求失败 (错误码 " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    DWORD status_code = 0;
    DWORD sz = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz, WINHTTP_NO_HEADER_INDEX);

    ReadResponseBody(hRequest, 16384);

    // Wait briefly and check if internet is now accessible
    Sleep(800);
    std::wstring dummy_redirect;
    NetStatus net = CheckInternetState(dummy_redirect);
    if (net == NetStatus::ONLINE) {
        out_message = L"网页 Portal 认证成功，外网已连通 (HTTP " + std::to_wstring(status_code) + L")";
        return true;
    }

    if (status_code >= 200 && status_code < 400) {
        out_message = L"Portal 请求已提交 (HTTP " + std::to_wstring(status_code) + L")，但外网探测仍被拦截，请检查字段配置或账号密码";
        return false;
    }

    out_message = L"Portal 认证服务器返回错误状态码: HTTP " + std::to_wstring(status_code);
    return false;
}

}  // namespace portal
