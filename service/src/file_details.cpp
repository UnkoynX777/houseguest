#include "file_details.hpp"

#include <chrono>

#include "houseguest/system/signature.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

using core::Json;

Json name_json(const system::CertificateName& name) {
    Json fields = Json::array();
    for (const auto& field : name.fields)
        fields.push_back({{"label", field.label}, {"oid", field.oid}, {"value", field.value}});
    return {{"common_name", name.common_name},
            {"organization", name.organization},
            {"country", name.country},
            {"fields", std::move(fields)}};
}

Json signature_json(const system::SignatureReport& report) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char code[16];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(report.status));
    return {{"state", system::state_name(report.state)},
            {"signer", report.signer},
            {"via_catalog", report.via_catalog},
            {"timestamped", report.timestamped},
            {"certificate_expired", report.not_after != 0 && report.not_after < now},
            {"subject", name_json(report.subject)},
            {"issuer", name_json(report.issuer)},
            {"thumbprint_sha1", report.thumbprint_sha1},
            {"thumbprint_sha256", report.thumbprint_sha256},
            {"serial", report.serial},
            {"algorithm", report.algorithm},
            {"not_before", report.not_before},
            {"not_after", report.not_after},
            {"status_code", code},
            {"revocation_checked", report.revocation_checked}};
}

}  // namespace

Json FileDetails::describe(const std::filesystem::path& file) {
    const auto key = system::path_to_utf8(file);
    const auto stamp = system::file_stamp(file);
    if (!stamp) return Json{{"path", key}, {"exists", false}};
    {
        std::scoped_lock lock(mutex_);
        if (const auto it = cache_.find(key); it != cache_.end() && it->second.stamp == *stamp) {
            order_.splice(order_.begin(), order_, it->second.position);
            return it->second.value;
        }
    }

    const auto declared = system::read_file_metadata(file);
    Json value{{"path", key},
               {"exists", true},
               {"size", stamp->size},
               {"declared",
                {{"company", declared.company},
                 {"product", declared.product},
                 {"description", declared.description},
                 {"file_version", declared.file_version},
                 {"product_version", declared.product_version},
                 {"copyright", declared.copyright},
                 {"original_name", declared.original_name},
                 {"internal_name", declared.internal_name}}},
               {"signature", signature_json(system::inspect_signature(file))}};

    std::scoped_lock lock(mutex_);
    if (const auto it = cache_.find(key); it != cache_.end()) {
        order_.erase(it->second.position);
        cache_.erase(it);
    }
    order_.push_front(key);
    cache_[key] = {*stamp, value, order_.begin()};
    while (cache_.size() > kEntries) {
        cache_.erase(order_.back());
        order_.pop_back();
    }
    return value;
}

std::size_t FileDetails::cached() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

}  // namespace houseguest::service
