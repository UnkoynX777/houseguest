#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/crypto.hpp"

namespace houseguest::system {

struct JwtKey {
    std::string kid;
    Bytes modulus;   // big-endian
    Bytes exponent;  // big-endian
};

struct JwtPolicy {
    std::string issuer;    // e.g. https://team.cloudflareaccess.com
    std::string audience;  // Access application AUD tag
    std::int64_t now = 0;
    std::int64_t skew = 60;
};

struct JwtResult {
    bool valid = false;
    std::string reason;
    std::string email;
    std::string subject;
    std::int64_t expires = 0;
};

[[nodiscard]] std::optional<Bytes> base64url_decode(std::string_view text);
[[nodiscard]] std::string base64url_encode(ByteView data);

// Reads the RSA keys of a JWKS document. Keys that are not RS256-capable RSA keys are skipped.
[[nodiscard]] std::vector<JwtKey> parse_jwks(std::string_view jwks);

// Verifies an RS256 token: algorithm pinned to RS256 (never taken from the header on trust, "none" and HMAC are
// refused), signature checked against the key named by kid, then issuer, audience, expiry and not-before.
[[nodiscard]] JwtResult verify_jwt(std::string_view token, const std::vector<JwtKey>& keys, const JwtPolicy& policy);

// Test and tooling support: build the CNG public key blob for a key.
[[nodiscard]] bool rsa_verify_pkcs1_sha256(const JwtKey& key, ByteView message, ByteView signature);

}  // namespace houseguest::system
