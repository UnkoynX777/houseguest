#include "houseguest/system/https.hpp"

#include <windows.h>

#include <winhttp.h>

#include <memory>

namespace houseguest::system {
namespace {

struct HandleCloser {
    void operator()(void* h) const {
        if (h) WinHttpCloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

HttpsResponse fail(std::string message) {
    HttpsResponse r;
    r.error = std::move(message);
    return r;
}

}  // namespace

HttpsResponse https_request(const HttpsRequest& request, const ChunkSink& sink,
                            const std::atomic<bool>* cancel) {
    const auto url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        return fail("The address is not valid.");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) return fail("Only https addresses are allowed.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Handle session(WinHttpOpen(L"Houseguest", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail("Could not start the network session.");
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000;  // TLS 1.2 and 1.3
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session.get(), request.timeout_ms, request.timeout_ms, request.timeout_ms,
                       request.timeout_ms);

    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) return fail("Could not reach the server.");
    Handle handle(WinHttpOpenRequest(connection.get(), widen(request.method).c_str(), path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!handle) return fail("Could not create the request.");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(handle.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

    std::wstring headers;
    for (const auto& [name, value] : request.headers) headers += widen(name) + L": " + widen(value) + L"\r\n";
    if (!request.body.empty() && request.method != "GET") {
        headers += L"Content-Length: " + std::to_wstring(request.body.size()) + L"\r\n";
    }
    const void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : request.body.data();
    const auto length = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(handle.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(headers.size()), const_cast<void*>(body), length, length, 0)) {
        const auto code = GetLastError();
        if (code == ERROR_WINHTTP_SECURE_FAILURE || code == ERROR_WINHTTP_SECURE_INVALID_CERT ||
            code == ERROR_WINHTTP_SECURE_CERT_REV_FAILED)
            return fail("The server certificate could not be verified.");
        return fail("The request could not be sent (" + std::to_string(code) + ").");
    }
    if (!WinHttpReceiveResponse(handle.get(), nullptr)) return fail("No response arrived.");

    HttpsResponse response;
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    for (;;) {
        if (cancel && cancel->load()) {
            response.error = "Cancelled.";
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handle.get(), &available)) {
            response.error = "The connection was interrupted.";
            break;
        }
        if (available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(handle.get(), chunk.data(), available, &read)) {
            response.error = "The connection was interrupted.";
            break;
        }
        chunk.resize(read);
        if (sink) {
            if (!sink(chunk)) {
                response.error = "Cancelled.";
                break;
            }
        } else {
            if (response.body.size() + chunk.size() > request.max_response) {
                response.error = "The response is too large.";
                break;
            }
            response.body += chunk;
        }
    }
    return response;
}

}  // namespace houseguest::system
