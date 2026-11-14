#include "houseguest/system/crypto.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>

#include <fstream>
#include <memory>
#include <stdexcept>

namespace houseguest::system {
namespace {

constexpr std::size_t kDigestSize = 32;

void check(NTSTATUS status, const char* what) {
    if (status < 0) throw std::runtime_error(std::string("BCrypt failure: ") + what);
}

struct AlgorithmCloser {
    void operator()(void* handle) const { BCryptCloseAlgorithmProvider(handle, 0); }
};
using Algorithm = std::unique_ptr<void, AlgorithmCloser>;

Algorithm open_sha256(bool hmac) {
    BCRYPT_ALG_HANDLE handle = nullptr;
    check(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                      hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0),
          "open");
    return Algorithm(handle);
}

}  // namespace

Bytes random_bytes(std::size_t count) {
    Bytes out(count);
    check(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
          "random");
    return out;
}

Bytes sha256(ByteView data) {
    const auto alg = open_sha256(false);
    Bytes out(kDigestSize);
    check(BCryptHash(alg.get(), nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), kDigestSize),
          "hash");
    return out;
}

std::optional<std::string> sha256_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;

    const auto alg = open_sha256(false);
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptCreateHash(alg.get(), &hash, nullptr, 0, nullptr, 0, 0), "create hash");
    std::unique_ptr<void, decltype(&BCryptDestroyHash)> guard(hash, &BCryptDestroyHash);

    std::vector<char> buffer(1 << 16);
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        check(
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(in.gcount()), 0),
            "hash data");
    }
    if (in.bad()) return std::nullopt;

    Bytes out(kDigestSize);
    check(BCryptFinishHash(hash, out.data(), kDigestSize, 0), "finish");
    return to_hex(out);
}

Bytes hmac_sha256(ByteView key, ByteView data) {
    const auto alg = open_sha256(true);
    Bytes out(kDigestSize);
    check(
        BCryptHash(alg.get(), const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                   const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), out.data(), kDigestSize),
        "hmac");
    return out;
}

Bytes pbkdf2_sha256(std::string_view password, ByteView salt, std::uint32_t iterations, std::size_t length) {
    const auto alg = open_sha256(true);
    Bytes out(length);
    check(BCryptDeriveKeyPBKDF2(alg.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
                                static_cast<ULONG>(salt.size()), iterations, out.data(),
                                static_cast<ULONG>(length), 0),
          "pbkdf2");
    return out;
}

std::string to_hex(ByteView data) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const auto byte : data) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

std::optional<Bytes> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
    }
    return out;
}

bool constant_time_equal(ByteView a, ByteView b) {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

Bytes dpapi_protect_machine(ByteView data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Houseguest", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &out)) {
        throw std::runtime_error("CryptProtectData failed");
    }
    Bytes result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::optional<Bytes> dpapi_unprotect(ByteView data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return std::nullopt;
    Bytes result(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

}  // namespace houseguest::system
