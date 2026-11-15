#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "api/http.hpp"

using namespace houseguest::service::api;

// The parser reads bytes that arrive from a network. Whatever they are, it must finish, never read outside the
// buffer, and never accept a message whose framing is ambiguous.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view buffer(reinterpret_cast<const char*>(data), size);
    const auto result = parse_request(buffer);
    if (result.status == ParseStatus::Complete) {
        if (result.consumed > size) __debugbreak();
        if (result.request.headers.contains("transfer-encoding")) __debugbreak();
        if (result.request.target.empty() || result.request.target[0] != '/') __debugbreak();
        if (result.request.target.find("..") != std::string::npos) __debugbreak();
    }
    for (std::size_t cut = 0; cut < size && cut < 64; cut += 7) (void)parse_request(buffer.substr(0, cut));
    return 0;
}
