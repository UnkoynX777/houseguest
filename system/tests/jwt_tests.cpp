#include <windows.h>

#include <bcrypt.h>

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/jwt.hpp"

using namespace houseguest::system;
using houseguest::core::Json;

namespace {

struct Pair {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    JwtKey pub;
    explicit Pair(const std::string& kid) {
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
        BCryptGenerateKeyPair(algorithm, &key, 2048, 0);
        BCryptFinalizeKeyPair(key, 0);
        ULONG size = 0;
        BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0);
        Bytes blob(size);
        BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0);
        const auto* h = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob.data());
        const auto* base = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
        pub.kid = kid;
        pub.exponent.assign(base, base + h->cbPublicExp);
        pub.modulus.assign(base + h->cbPublicExp, base + h->cbPublicExp + h->cbModulus);
    }
    ~Pair() {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    std::string sign(const std::string& header, const std::string& payload) const {
        auto enc = [](const std::string& t) {
            return base64url_encode({reinterpret_cast<const std::uint8_t*>(t.data()), t.size()});
        };
        const auto signing = enc(header) + "." + enc(payload);
        const auto digest = sha256({reinterpret_cast<const std::uint8_t*>(signing.data()), signing.size()});
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ULONG length = 0;
        BCryptSignHash(key, &padding, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                       nullptr, 0, &length, BCRYPT_PAD_PKCS1);
        Bytes signature(length);
        BCryptSignHash(key, &padding, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                       signature.data(), length, &length, BCRYPT_PAD_PKCS1);
        return signing + "." + base64url_encode(signature);
    }
};

const std::int64_t kNow = 1'800'000'000;
JwtPolicy policy() {
    return {"https://team.cloudflareaccess.com", "aud-tag", kNow, 60};
}

std::string claims(std::int64_t exp = kNow + 600, const std::string& aud = "\"aud-tag\"",
                   const std::string& iss = "https://team.cloudflareaccess.com") {
    return R"({"iss":")" + iss + R"(","aud":[)" + aud + R"(],"exp":)" + std::to_string(exp) + R"(,"iat":)" +
           std::to_string(kNow - 5) + R"(,"email":"owner@example.com","sub":"u1"})";
}

const std::string kHeader = R"({"alg":"RS256","kid":"k1","typ":"JWT"})";

}  // namespace

TEST_CASE("base64url round-trips and rejects other alphabets", "[jwt]") {
    const Bytes data{0, 1, 2, 250, 251, 252, 253};
    CHECK(base64url_decode(base64url_encode(data)) == data);
    CHECK_FALSE(base64url_decode("ab+/"));
    CHECK_FALSE(base64url_decode("a"));
}

TEST_CASE("a correctly signed token is accepted", "[jwt][security]") {
    Pair pair("k1");
    const auto result = verify_jwt(pair.sign(kHeader, claims()), {pair.pub}, policy());
    CHECK(result.valid);
    CHECK(result.email == "owner@example.com");
}

TEST_CASE("tampering, wrong keys and unknown kids are refused", "[jwt][security]") {
    Pair good("k1");
    Pair other("k1");
    auto token = good.sign(kHeader, claims());
    CHECK_FALSE(verify_jwt(token, {other.pub}, policy()).valid);
    CHECK(verify_jwt(token, {Pair("k2").pub}, policy()).reason == "unknown_key");
    auto forged = good.sign(kHeader, claims(kNow + 600, "\"aud-tag\""));
    const auto dots = forged.find('.');
    const auto second = forged.find('.', dots + 1);
    const std::string evil =
        R"({"iss":"https://team.cloudflareaccess.com","aud":["aud-tag"],"exp":9999999999})";
    const auto payload = base64url_encode({reinterpret_cast<const std::uint8_t*>(evil.data()), evil.size()});
    CHECK_FALSE(
        verify_jwt(forged.substr(0, dots + 1) + payload + forged.substr(second), {good.pub}, policy()).valid);
}

TEST_CASE("algorithm confusion is refused", "[jwt][security]") {
    Pair pair("k1");
    CHECK_FALSE(verify_jwt(pair.sign(R"({"alg":"none","kid":"k1"})", claims()), {pair.pub}, policy()).valid);
    CHECK_FALSE(verify_jwt(pair.sign(R"({"alg":"HS256","kid":"k1"})", claims()), {pair.pub}, policy()).valid);
    CHECK_FALSE(verify_jwt(pair.sign(R"({"kid":"k1"})", claims()), {pair.pub}, policy()).valid);
    CHECK_FALSE(verify_jwt("a.b", {pair.pub}, policy()).valid);
    CHECK_FALSE(verify_jwt("", {pair.pub}, policy()).valid);
}

TEST_CASE("issuer, audience and time claims are enforced", "[jwt][security]") {
    Pair pair("k1");
    CHECK_FALSE(verify_jwt(pair.sign(kHeader, claims(kNow + 600, "\"other\"")), {pair.pub}, policy()).valid);
    CHECK_FALSE(
        verify_jwt(pair.sign(kHeader, claims(kNow + 600, "\"aud-tag\"", "https://evil.cloudflareaccess.com")),
                   {pair.pub}, policy())
            .valid);
    CHECK_FALSE(verify_jwt(pair.sign(kHeader, claims(kNow - 600)), {pair.pub}, policy()).valid);
    CHECK(verify_jwt(pair.sign(kHeader, claims(kNow - 30)), {pair.pub}, policy()).valid);  // within skew
    auto empty = policy();
    empty.audience.clear();
    CHECK_FALSE(verify_jwt(pair.sign(kHeader, claims()), {pair.pub}, empty).valid);
    CHECK_FALSE(verify_jwt(pair.sign(kHeader, R"({"iss":5,"aud":1,"exp":"x"})"), {pair.pub}, policy()).valid);
}

TEST_CASE("a JWKS document yields only usable RSA signing keys", "[jwt]") {
    Pair pair("k1");
    const Json jwks{{"keys",
                     {{{"kty", "RSA"},
                       {"kid", "k1"},
                       {"alg", "RS256"},
                       {"use", "sig"},
                       {"n", base64url_encode(pair.pub.modulus)},
                       {"e", base64url_encode(pair.pub.exponent)}},
                      {{"kty", "EC"}, {"kid", "k2"}},
                      {{"kty", "RSA"}, {"kid", "short"}, {"n", "AQAB"}, {"e", "AQAB"}}}}};
    const auto keys = parse_jwks(jwks.dump());
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].kid == "k1");
    CHECK(verify_jwt(pair.sign(kHeader, claims()), keys, policy()).valid);
    CHECK(parse_jwks("not json").empty());
}
