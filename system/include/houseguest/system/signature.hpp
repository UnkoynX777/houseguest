#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace houseguest::system {

// What Authenticode verification concluded about a file. The states are kept apart on purpose: a certificate
// that is present is not a signature that is valid, and a signature that could not be checked is not a bad
// one.
enum class SignatureState {
    Unsigned,   // no embedded signature and no catalog vouches for the file
    Valid,      // the signature verifies and the certificate chains to a trusted root
    Untrusted,  // a signature is present, but its certificate does not chain to a trusted root
    Expired,    // a signature is present, but its certificate has expired and nothing proves it was signed in
                // time
    Invalid,    // the file was modified after signing, or the certificate was revoked or explicitly blocked
    Unverified,  // verification could not be completed (unreadable file, unexpected error)
};

const char* state_name(SignatureState state);

struct NameField {
    std::string label;
    std::string oid;
    std::string value;
};

// One side of a certificate (subject or issuer) in readable form. `fields` keeps every attribute with its OID
// for the advanced view; `common_name` and `organization` are the parts a person recognises.
struct CertificateName {
    std::string common_name;
    std::string organization;
    std::string country;
    std::vector<NameField> fields;
};

struct SignatureReport {
    SignatureState state = SignatureState::Unsigned;
    bool via_catalog = false;
    bool timestamped = false;
    std::string signer;
    CertificateName subject;
    CertificateName issuer;
    std::string thumbprint_sha1;
    std::string thumbprint_sha256;
    std::string serial;
    std::string algorithm;
    std::int64_t not_before = 0;
    std::int64_t not_after = 0;
    long status = 0;
    // Revocation lists are never fetched from the network: checking is limited to what Windows already has
    // cached, so a valid result does not prove the certificate has not been revoked since.
    bool revocation_checked = false;
};

// Decodes a DER-encoded X.500 name into labelled attributes. Attributes that have no friendly label keep
// their OID in the label so nothing is lost, but they never become the name shown to people.
CertificateName parse_distinguished_name(std::span<const std::uint8_t> encoded);

// Reads and verifies the Authenticode signature of one file, trying its embedded signature first and the
// security catalogs of the system second. The file is opened for reading only and nothing in it is executed.
SignatureReport inspect_signature(const std::filesystem::path& file);

}  // namespace houseguest::system
