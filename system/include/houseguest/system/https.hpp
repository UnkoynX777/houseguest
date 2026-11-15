#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace houseguest::system {

struct HttpsRequest {
    std::string method = "GET";
    std::string url;  // https only
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 15000;
    std::size_t max_response = 4 * 1024 * 1024;
};

struct HttpsResponse {
    int status = 0;  // 0 when no response arrived
    std::string body;
    std::string error;
    [[nodiscard]] bool ok() const { return status >= 200 && status < 300; }
};

// Called with each piece of the response as it arrives; returning false abandons the request.
using ChunkSink = std::function<bool(std::string_view)>;

// HTTPS through WinHTTP with the system trust store. Certificate errors are never ignored, plain http is refused,
// TLS 1.2 is the floor (1.3 where Windows offers it) and redirects never leave https.
HttpsResponse https_request(const HttpsRequest& request, const ChunkSink& sink = nullptr,
                            const std::atomic<bool>* cancel = nullptr);

}  // namespace houseguest::system
