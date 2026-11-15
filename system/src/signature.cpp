// clang-format off
// mscat.h requires wincrypt.h to be included first.
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <wintrust.h>
// clang-format on

#include "houseguest/system/signature.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "win_util.hpp"

namespace houseguest::system {
namespace {

constexpr std::size_t kMaxFields = 24;
constexpr std::size_t kMaxValue = 256;

constexpr unsigned long kNoSignature = 0x800B0100, kFormUnknown = 0x800B0003, kProviderUnknown = 0x800B0001,
                        kExpired = 0x800B0101, kBadDigest = 0x80096010, kRevoked = 0x800B010C,
                        kExplicitDistrust = 0x800B0111, kCertSignature = 0x80096004;

struct Label {
    const char* oid;
    const char* label;
};

constexpr std::array<Label, 17> kLabels{{
    {"2.5.4.3", "Common name"},
    {"2.5.4.10", "Organization"},
    {"2.5.4.11", "Organizational unit"},
    {"2.5.4.6", "Country"},
    {"2.5.4.8", "State or province"},
    {"2.5.4.7", "Locality"},
    {"2.5.4.9", "Street"},
    {"2.5.4.17", "Postal code"},
    {"2.5.4.5", "Serial number"},
    {"2.5.4.15", "Business category"},
    {"2.5.4.97", "Organization identifier"},
    {"1.2.840.113549.1.9.1", "Email"},
    {"0.9.2342.19200300.100.1.25", "Domain component"},
    {"1.3.6.1.4.1.311.60.2.1.1", "Jurisdiction locality"},
    {"1.3.6.1.4.1.311.60.2.1.2", "Jurisdiction state"},
    {"1.3.6.1.4.1.311.60.2.1.3", "Jurisdiction country"},
    {"2.5.4.12", "Title"},
}};

std::string hex_of(const BYTE* data, DWORD size) {
    return to_hex({data, size});
}

std::int64_t unix_of(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t kEpoch = 116444736000000000ull;
    return value.QuadPart < kEpoch ? 0 : static_cast<std::int64_t>((value.QuadPart - kEpoch) / 10000000ull);
}

CertificateName name_of(const CERT_NAME_BLOB& blob) {
    return parse_distinguished_name({blob.pbData, blob.cbData});
}

}  // namespace

CertificateName parse_distinguished_name(std::span<const std::uint8_t> encoded) {
    CertificateName name;
    CERT_NAME_INFO* info = nullptr;
    DWORD size = 0;
    if (encoded.empty() ||
        !CryptDecodeObjectEx(X509_ASN_ENCODING, X509_NAME, encoded.data(), static_cast<DWORD>(encoded.size()),
                             CRYPT_DECODE_ALLOC_FLAG, nullptr, static_cast<void*>(&info), &size) ||
        info == nullptr)
        return name;
    for (DWORD i = info->cRDN; i > 0 && name.fields.size() < kMaxFields; --i) {
        const auto& rdn = info->rgRDN[i - 1];
        for (DWORD j = 0; j < rdn.cRDNAttr && name.fields.size() < kMaxFields; ++j) {
            const auto& attribute = rdn.rgRDNAttr[j];
            if (attribute.pszObjId == nullptr) continue;
            wchar_t value[kMaxValue + 1] = {};
            CertRDNValueToStrW(attribute.dwValueType, const_cast<CERT_RDN_VALUE_BLOB*>(&attribute.Value),
                               value, kMaxValue);
            NameField field;
            field.oid = attribute.pszObjId;
            field.value = to_utf8(value);
            const auto known =
                std::ranges::find_if(kLabels, [&](const Label& l) { return field.oid == l.oid; });
            field.label = known != kLabels.end() ? known->label : "OID " + field.oid;
            if (field.oid == "2.5.4.3" && name.common_name.empty()) name.common_name = field.value;
            if (field.oid == "2.5.4.10" && name.organization.empty()) name.organization = field.value;
            if (field.oid == "2.5.4.6" && name.country.empty()) name.country = field.value;
            name.fields.push_back(std::move(field));
        }
    }
    LocalFree(info);
    return name;
}

namespace {

void describe(const CERT_CONTEXT* certificate, SignatureReport& report) {
    const auto* info = certificate->pCertInfo;
    report.subject = name_of(info->Subject);
    report.issuer = name_of(info->Issuer);
    report.signer =
        !report.subject.common_name.empty() ? report.subject.common_name : report.subject.organization;
    report.not_before = unix_of(info->NotBefore);
    report.not_after = unix_of(info->NotAfter);
    BYTE sha1[20];
    DWORD size = sizeof(sha1);
    if (CertGetCertificateContextProperty(certificate, CERT_SHA1_HASH_PROP_ID, sha1, &size))
        report.thumbprint_sha1 = hex_of(sha1, size);
    report.thumbprint_sha256 = to_hex(sha256({certificate->pbCertEncoded, certificate->cbCertEncoded}));
    std::string serial(info->SerialNumber.cbData, '\0');
    for (DWORD i = 0; i < info->SerialNumber.cbData; ++i)
        serial[i] = static_cast<char>(info->SerialNumber.pbData[info->SerialNumber.cbData - 1 - i]);
    report.serial = to_hex({reinterpret_cast<const std::uint8_t*>(serial.data()), serial.size()});
    if (info->SignatureAlgorithm.pszObjId != nullptr) {
        if (const auto* oid = CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY,
                                               const_cast<char*>(info->SignatureAlgorithm.pszObjId), 0);
            oid != nullptr && oid->pwszName != nullptr)
            report.algorithm = to_utf8(oid->pwszName);
        else
            report.algorithm = info->SignatureAlgorithm.pszObjId;
    }
}

struct Outcome {
    unsigned long status = kNoSignature;
    bool certificate = false;
    SignatureReport report;
    std::string subject;
};

Outcome verify(WINTRUST_DATA& data) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto window = static_cast<HWND>(INVALID_HANDLE_VALUE);
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    Outcome outcome;
    outcome.status = static_cast<unsigned long>(WinVerifyTrust(window, &action, &data));
    if (data.hWVTStateData != nullptr) {
        if (auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (auto* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
                outcome.report.timestamped = signer->csCounterSigners > 0;
                if (auto* cert = WTHelperGetProvCertFromChain(signer, 0); cert && cert->pCert) {
                    outcome.certificate = true;
                    describe(cert->pCert, outcome.report);
                    wchar_t text[512] = {};
                    CertNameToStrW(X509_ASN_ENCODING, &cert->pCert->pCertInfo->Subject, CERT_X500_NAME_STR,
                                   text, 512);
                    outcome.subject = to_utf8(text);
                }
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(window, &action, &data);
    return outcome;
}

SignatureState classify(unsigned long status) {
    switch (status) {
        case 0:
            return SignatureState::Valid;
        case kNoSignature:
        case kFormUnknown:
        case kProviderUnknown:
            return SignatureState::Unsigned;
        case kExpired:
            return SignatureState::Expired;
        case kBadDigest:
        case kRevoked:
        case kExplicitDistrust:
        case kCertSignature:
            return SignatureState::Invalid;
        case 0x800B0109:  // CERT_E_UNTRUSTEDROOT
        case 0x800B010A:  // CERT_E_CHAINING
        case 0x800B0004:  // TRUST_E_SUBJECT_NOT_TRUSTED
        case 0x800B0112:  // CERT_E_UNTRUSTEDCA
        case 0x800B0106:  // CERT_E_WRONG_USAGE
        case 0x80092026:  // CRYPT_E_SECURITY_SETTINGS
            return SignatureState::Untrusted;
        default:
            return SignatureState::Unverified;
    }
}

Outcome embedded(const std::filesystem::path& file) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{};
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    return verify(data);
}

// Files that carry no signature of their own, as most of Windows does, are vouched for by a security catalog.
std::optional<Outcome> cataloged(const std::filesystem::path& file) {
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return std::nullopt;

    for (const wchar_t* algorithm : {BCRYPT_SHA256_ALGORITHM, static_cast<const wchar_t*>(nullptr)}) {
        HCATADMIN admin = nullptr;
        if (!CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0)) continue;
        std::optional<Outcome> result;

        DWORD hash_size = 0;
        CryptCATAdminCalcHashFromFileHandle2(admin, handle.get(), &hash_size, nullptr, 0);
        std::vector<BYTE> hash(hash_size);
        if (hash_size != 0 &&
            CryptCATAdminCalcHashFromFileHandle2(admin, handle.get(), &hash_size, hash.data(), 0)) {
            if (HCATINFO catalog =
                    CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hash_size, 0, nullptr)) {
                CATALOG_INFO info{sizeof(info)};
                if (CryptCATCatalogInfoFromContext(catalog, &info, 0)) {
                    std::wstring tag;
                    for (const BYTE byte : hash) {
                        wchar_t pair[3];
                        swprintf_s(pair, L"%02X", byte);
                        tag += pair;
                    }
                    WINTRUST_CATALOG_INFO catalog_info{};
                    catalog_info.cbStruct = sizeof(catalog_info);
                    catalog_info.pcwszCatalogFilePath = info.wszCatalogFile;
                    catalog_info.pcwszMemberTag = tag.c_str();
                    catalog_info.pcwszMemberFilePath = file.c_str();
                    catalog_info.hMemberFile = handle.get();
                    catalog_info.pbCalculatedFileHash = hash.data();
                    catalog_info.cbCalculatedFileHash = hash_size;
                    catalog_info.hCatAdmin = admin;
                    WINTRUST_DATA data{};
                    data.dwUnionChoice = WTD_CHOICE_CATALOG;
                    data.pCatalog = &catalog_info;
                    result = verify(data);
                }
                CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
            }
        }
        CryptCATAdminReleaseContext(admin, 0);
        if (result) return result;
    }
    return std::nullopt;
}

}  // namespace

const char* state_name(SignatureState state) {
    switch (state) {
        case SignatureState::Unsigned:
            return "unsigned";
        case SignatureState::Valid:
            return "valid";
        case SignatureState::Untrusted:
            return "untrusted";
        case SignatureState::Expired:
            return "expired";
        case SignatureState::Invalid:
            return "invalid";
        case SignatureState::Unverified:
            return "unverified";
    }
    return "unverified";
}

SignatureReport inspect_signature(const std::filesystem::path& file) {
    auto outcome = embedded(file);
    bool catalog = false;
    if (classify(outcome.status) == SignatureState::Unsigned && !outcome.certificate) {
        if (auto vouched = cataloged(file); vouched && vouched->certificate) {
            outcome = std::move(*vouched);
            catalog = true;
        }
    }
    auto report = std::move(outcome.report);
    report.state = classify(outcome.status);
    report.status = static_cast<long>(outcome.status);
    report.via_catalog = catalog;
    if (report.state == SignatureState::Unsigned || report.state == SignatureState::Unverified) {
        if (!outcome.certificate) report = SignatureReport{.state = report.state, .status = report.status};
    }
    return report;
}

std::string trusted_publisher(const std::filesystem::path& file) {
    auto outcome = embedded(file);
    if (outcome.status == 0 && !outcome.subject.empty()) return outcome.subject;
    const auto vouched = cataloged(file);
    return vouched && vouched->status == 0 ? vouched->subject : std::string{};
}

}  // namespace houseguest::system
