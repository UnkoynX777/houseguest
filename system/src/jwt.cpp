#include "houseguest/system/jwt.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <memory>

namespace houseguest::system {
namespace {

using core::Json;

constexpr std::size_t kMaxToken = 8192;

std::optional<std::string_view> part(std::string_view text, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const auto dot = text.find('.', start);
        if (dot == std::string_view::npos) return std::nullopt;
        start = dot + 1;
    }
    const auto end = text.find('.', start);
    return text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

bool audience_matches(const Json& aud, const std::string& expected) {
    if (aud.is_string()) return aud.get<std::string>() == expected;
    if (aud.is_array())
        return std::ranges::any_of(aud, [&](const Json& v) { return v.is_string() && v.get<std::string>() == expected; });
    return false;
}

}  // namespace

std::optional<Bytes> base64url_decode(std::string_view text) {
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '-') value = 62;
        else if (c == '_') value = 63;
        else return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
        }
    }
    if (bits >= 6) return std::nullopt;
    return out;
}

std::string base64url_encode(ByteView data) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 0x3f];
        }
    }
    if (bits > 0) out += alphabet[(buffer << (6 - bits)) & 0x3f];
    return out;
}

std::vector<JwtKey> parse_jwks(std::string_view jwks) {
    std::vector<JwtKey> keys;
    const Json doc = Json::parse(jwks, nullptr, false);
    if (!doc.is_object() || !doc.contains("keys") || !doc["keys"].is_array()) return keys;
    for (const auto& item : doc["keys"]) {
        if (!item.is_object() || item.value("kty", "") != "RSA") continue;
        if (item.contains("alg") && item["alg"] != "RS256") continue;
        if (item.contains("use") && item["use"] != "sig") continue;
        if (!item.contains("kid") || !item["kid"].is_string() || !item.contains("n") || !item["n"].is_string() ||
            !item.contains("e") || !item["e"].is_string())
            continue;
        auto n = base64url_decode(item["n"].get<std::string>());
        auto e = base64url_decode(item["e"].get<std::string>());
        if (!n || !e || n->size() < 256 || n->size() > 1024 || e->empty() || e->size() > 8) continue;
        keys.push_back({item["kid"].get<std::string>(), std::move(*n), std::move(*e)});
    }
    return keys;
}

bool rsa_verify_pkcs1_sha256(const JwtKey& key, ByteView message, ByteView signature) {
    if (signature.size() != key.modulus.size()) return false;
    Bytes blob(sizeof(BCRYPT_RSAKEY_BLOB) + key.exponent.size() + key.modulus.size());
    auto* header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header->BitLength = static_cast<ULONG>(key.modulus.size() * 8);
    header->cbPublicExp = static_cast<ULONG>(key.exponent.size());
    header->cbModulus = static_cast<ULONG>(key.modulus.size());
    header->cbPrime1 = 0;
    header->cbPrime2 = 0;
    std::copy(key.exponent.begin(), key.exponent.end(), blob.begin() + sizeof(BCRYPT_RSAKEY_BLOB));
    std::copy(key.modulus.begin(), key.modulus.end(), blob.begin() + sizeof(BCRYPT_RSAKEY_BLOB) + key.exponent.size());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0) return false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = false;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_RSAPUBLIC_BLOB, &handle, blob.data(),
                            static_cast<ULONG>(blob.size()), 0) >= 0) {
        const auto digest = sha256(message);
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ok = BCryptVerifySignature(handle, &padding, const_cast<PUCHAR>(digest.data()),
                                   static_cast<ULONG>(digest.size()), const_cast<PUCHAR>(signature.data()),
                                   static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
        BCryptDestroyKey(handle);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

namespace {
JwtResult verify_impl(std::string_view token, const std::vector<JwtKey>& keys, const JwtPolicy& policy) {
    JwtResult result;
    auto refuse = [&](const char* reason) {
        result.valid = false;
        result.reason = reason;
        return result;
    };
    if (token.empty() || token.size() > kMaxToken) return refuse("The token is missing or too large.");
    if (std::ranges::count(token, '.') != 2) return refuse("The token is malformed.");
    const auto header_part = *part(token, 0);
    const auto payload_part = *part(token, 1);
    const auto signature_part = *part(token, 2);
    const auto header_bytes = base64url_decode(header_part);
    const auto payload_bytes = base64url_decode(payload_part);
    const auto signature = base64url_decode(signature_part);
    if (!header_bytes || !payload_bytes || !signature) return refuse("The token is malformed.");

    const Json header = Json::parse(header_bytes->begin(), header_bytes->end(), nullptr, false);
    if (!header.is_object() || header.value("alg", "") != "RS256") return refuse("Only RS256 tokens are accepted.");
    if (!header.contains("kid") || !header["kid"].is_string()) return refuse("The token names no key.");
    const auto kid = header["kid"].get<std::string>();
    const auto key = std::ranges::find(keys, kid, &JwtKey::kid);
    if (key == keys.end()) return refuse("unknown_key");

    const auto signed_part = token.substr(0, header_part.size() + 1 + payload_part.size());
    if (!rsa_verify_pkcs1_sha256(*key, {reinterpret_cast<const std::uint8_t*>(signed_part.data()), signed_part.size()},
                                 *signature))
        return refuse("The signature is not valid.");

    const Json claims = Json::parse(payload_bytes->begin(), payload_bytes->end(), nullptr, false);
    if (!claims.is_object()) return refuse("The claims are malformed.");
    if (claims.value("iss", "") != policy.issuer || policy.issuer.empty()) return refuse("Unexpected issuer.");
    if (policy.audience.empty() || !claims.contains("aud") || !audience_matches(claims["aud"], policy.audience))
        return refuse("Unexpected audience.");
    if (!claims.contains("exp") || !claims["exp"].is_number_integer()) return refuse("The token has no expiry.");
    const auto expires = claims["exp"].get<std::int64_t>();
    if (expires + policy.skew < policy.now) return refuse("The token has expired.");
    if (claims.contains("nbf") && claims["nbf"].is_number_integer() &&
        claims["nbf"].get<std::int64_t>() > policy.now + policy.skew)
        return refuse("The token is not valid yet.");
    if (claims.contains("iat") && claims["iat"].is_number_integer() &&
        claims["iat"].get<std::int64_t>() > policy.now + policy.skew)
        return refuse("The token was issued in the future.");

    result.valid = true;
    result.expires = expires;
    result.email = claims.value("email", "");
    result.subject = claims.value("sub", "");
    return result;
}
}  // namespace

JwtResult verify_jwt(std::string_view token, const std::vector<JwtKey>& keys, const JwtPolicy& policy) {
    try {
        return verify_impl(token, keys, policy);
    } catch (const std::exception&) {
        return {false, "The token claims are malformed.", {}, {}, 0};
    }
}

}  // namespace houseguest::system
