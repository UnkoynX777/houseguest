#include <windows.h>

#include <bcrypt.h>

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "api/access.hpp"

using namespace houseguest::service::api;
using namespace houseguest::system;
using houseguest::core::Json;

namespace {

struct Signer {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::string jwks;
    Signer() {
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
        BCryptGenerateKeyPair(algorithm, &key, 2048, 0);
        BCryptFinalizeKeyPair(key, 0);
        ULONG size = 0;
        BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0);
        Bytes blob(size);
        BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0);
        const auto* h = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob.data());
        const auto* base = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
        Bytes e(base, base + h->cbPublicExp), n(base + h->cbPublicExp, base + h->cbPublicExp + h->cbModulus);
        jwks = Json{{"keys",
                     {{{"kty", "RSA"},
                       {"kid", "k1"},
                       {"alg", "RS256"},
                       {"use", "sig"},
                       {"n", base64url_encode(n)},
                       {"e", base64url_encode(e)}}}}}
                   .dump();
    }
    ~Signer() {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    std::string token(std::int64_t exp, const std::string& email = "owner@example.com",
                      const std::string& aud = "aud-tag") const {
        auto enc = [](const std::string& t) {
            return base64url_encode({reinterpret_cast<const std::uint8_t*>(t.data()), t.size()});
        };
        const auto payload = Json{
            {"iss", "https://team.cloudflareaccess.com"},
            {"aud", {aud}},
            {"exp", exp},
            {"email", email}}.dump();
        const auto signing = enc(R"({"alg":"RS256","kid":"k1"})") + "." + enc(payload);
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

Request remote(const std::string& token = {}) {
    Request r;
    r.method = "GET";
    r.target = "/v1/status";
    r.headers["host"] = "hg.example.com";
    r.headers["cf-ray"] = "abc";
    if (!token.empty()) r.headers["cf-access-jwt-assertion"] = token;
    return r;
}

struct Fixture {
    Signer signer;
    std::int64_t now = 1'800'000'000;
    int fetches = 0;
    AccessGate gate{[this](const std::string& url) -> std::optional<std::string> {
                        ++fetches;
                        if (url != "https://team.cloudflareaccess.com/cdn-cgi/access/certs")
                            return std::nullopt;
                        return signer.jwks;
                    },
                    [this] { return now; }};
    Fixture() { gate.configure({true, "Team.CloudflareAccess.com", "aud-tag", "hg.example.com", {}}); }
};

}  // namespace

TEST_CASE("local requests are left to device authentication", "[access]") {
    Fixture f;
    Request local;
    local.headers["host"] = "127.0.0.1:5000";
    CHECK_FALSE(AccessGate::is_remote(local));
    CHECK_FALSE(f.gate.check(local));
}

TEST_CASE("any proxy header marks a request as remote", "[access][security]") {
    for (const char* name :
         {"cf-connecting-ip", "x-forwarded-for", "forwarded", "via", "x-real-ip", "cf-ray"}) {
        Request r;
        r.headers[name] = "1";
        CHECK(AccessGate::is_remote(r));
    }
}

TEST_CASE("remote requests need a valid Access identity", "[access][security]") {
    Fixture f;
    CHECK(f.gate.check(remote()));
    CHECK(f.gate.check(remote("garbage")));
    CHECK_FALSE(f.gate.check(remote(f.signer.token(f.now + 300))));
    CHECK(f.gate.check(remote(f.signer.token(f.now - 3600))));
    CHECK(f.gate.check(remote(f.signer.token(f.now + 300, "owner@example.com", "other-app"))));
}

TEST_CASE("remote requests are refused while remote access is off", "[access][security]") {
    Fixture f;
    f.gate.configure({false, "team.cloudflareaccess.com", "aud-tag", "hg.example.com", {}});
    CHECK(f.gate.check(remote(f.signer.token(f.now + 300))));
    f.gate.configure({true, "", "", "", {}});
    CHECK(f.gate.check(remote(f.signer.token(f.now + 300))));
}

TEST_CASE("the identity can be limited to listed e-mail addresses", "[access][security]") {
    Fixture f;
    f.gate.configure({true, "team.cloudflareaccess.com", "aud-tag", "hg.example.com", {"Owner@Example.com"}});
    CHECK_FALSE(f.gate.check(remote(f.signer.token(f.now + 300, "owner@example.com"))));
    CHECK(f.gate.check(remote(f.signer.token(f.now + 300, "stranger@example.com"))));
}

TEST_CASE("keys are cached and refreshed at a bounded rate", "[access]") {
    Fixture f;
    for (int i = 0; i < 5; ++i) CHECK_FALSE(f.gate.check(remote(f.signer.token(f.now + 300))));
    CHECK(f.fetches == 1);
    Signer rotated;
    for (int i = 0; i < 20; ++i) CHECK(f.gate.check(remote(rotated.token(f.now + 300))));
    CHECK(f.fetches <= 2);  // unknown keys cannot make us fetch on every request
    f.now += AccessGate::kKeyLifetime + 1;
    CHECK_FALSE(f.gate.check(remote(f.signer.token(f.now + 300))));
    CHECK(f.fetches == 2);
}

TEST_CASE("an unreachable key endpoint fails closed", "[access][security]") {
    Signer signer;
    AccessGate gate([](const std::string&) { return std::nullopt; },
                    [] { return std::int64_t{1'800'000'000}; });
    gate.configure({true, "team.cloudflareaccess.com", "aud-tag", "hg.example.com", {}});
    CHECK(gate.check(remote(signer.token(1'800'000'300))));
}
