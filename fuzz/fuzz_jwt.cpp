#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "houseguest/system/jwt.hpp"

using namespace houseguest::system;

// Tokens come from the internet. No input may crash the verifier or make it accept an unsigned token.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const std::vector<JwtKey> none;
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto result = verify_jwt(text, none, {"https://team.cloudflareaccess.com", "aud", 1'800'000'000, 60});
    if (result.valid) __debugbreak();
    (void)parse_jwks(text);
    (void)base64url_decode(text);
    return 0;
}
