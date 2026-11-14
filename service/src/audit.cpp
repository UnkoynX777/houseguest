#include "audit.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <sstream>

namespace houseguest::service {
namespace {

constexpr int kCurrentVersion = 2;
constexpr std::size_t kMaxEntryBytes = std::size_t{16} * 1024;
constexpr std::size_t kMaxLineBytes = std::size_t{64} * 1024;

std::string read_all(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool replace_atomically(const std::filesystem::path& file, const std::string& content) {
    const auto temp = std::filesystem::path(file).concat(L".tmp");
    HANDLE handle =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok =
        WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size();
    CloseHandle(handle);
    return ok && MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string anchor_mac(const system::Bytes& key, std::uint64_t sequence, const std::string& hash) {
    const auto body = "anchor|" + std::to_string(sequence) + "|" + hash;
    return system::to_hex(
        system::hmac_sha256(key, {reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}));
}

struct Position {
    std::uint64_t sequence = 0;
    std::string hash;
};

struct Scan {
    Position last;
    std::size_t good_end = 0;
    bool any = false;
};

Scan scan_entries(const std::string& text) {
    Scan scan;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        if (end == std::string::npos || end - offset > kMaxLineBytes) break;
        const auto entry = core::Json::parse(text.substr(offset, end - offset), nullptr, false);
        if (entry.is_discarded() || !entry.is_object() || !entry.contains("seq") ||
            !entry["seq"].is_number_unsigned() || !entry.contains("hash") || !entry["hash"].is_string()) {
            break;
        }
        scan.last = {entry["seq"].get<std::uint64_t>(), entry["hash"].get<std::string>()};
        scan.any = true;
        offset = end + 1;
        scan.good_end = offset;
    }
    return scan;
}

}  // namespace

AuditLog::AuditLog(std::filesystem::path file, system::Bytes key)
    : file_(std::move(file)), key_(std::move(key)) {
    recover();
    if (truncation_found_) {
        append("service", "audit.integrity_failure", {{"reason", "log is shorter than its sealed position"}});
    }
}

std::filesystem::path AuditLog::rotated(int index) const {
    return std::filesystem::path(file_).concat("." + std::to_string(index));
}

std::filesystem::path AuditLog::anchor_file() const {
    return std::filesystem::path(file_).concat(L".anchor");
}

std::vector<std::filesystem::path> AuditLog::files_oldest_first() const {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (int index = kRotatedFiles; index >= 1; --index) {
        if (std::filesystem::exists(rotated(index), ec)) files.push_back(rotated(index));
    }
    if (std::filesystem::exists(file_, ec)) files.push_back(file_);
    return files;
}

std::string AuditLog::seal(int version, const std::string& previous, const core::Json& entry) const {
    const auto body = previous + entry.dump();
    const system::ByteView view{reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
    return system::to_hex(version >= 2 ? system::hmac_sha256(key_, view) : system::sha256(view));
}

void AuditLog::recover() {
    std::scoped_lock lock(mutex_);
    auto text = read_all(file_);
    auto scan = scan_entries(text);
    if (scan.good_end < text.size()) {
        const auto quarantine =
            std::filesystem::path(file_).concat(L".torn-" + std::to_wstring(now_seconds()));
        std::ofstream(quarantine, std::ios::binary) << text.substr(scan.good_end);
        std::error_code ec;
        std::filesystem::resize_file(file_, scan.good_end, ec);
    }
    if (!scan.any) {
        for (int index = 1; index <= kRotatedFiles && !scan.any; ++index) {
            scan = scan_entries(read_all(rotated(index)));
        }
    }
    sequence_ = scan.last.sequence;
    last_hash_ = scan.last.hash;

    const auto anchor = core::Json::parse(read_all(anchor_file()), nullptr, false);
    if (anchor.is_discarded() || !anchor.is_object()) {
        truncation_found_ = sequence_ > 0 && std::filesystem::exists(anchor_file());
        return;
    }
    const auto seq = anchor.value("seq", std::uint64_t{0});
    const auto hash = anchor.value("hash", std::string());
    const auto expected = anchor_mac(key_, seq, hash);
    if (anchor.value("mac", std::string()) != expected) {
        truncation_found_ = true;
        return;
    }
    if (seq > sequence_ || (seq == sequence_ && hash != last_hash_)) truncation_found_ = true;
}

void AuditLog::write_anchor() {
    const core::Json anchor{
        {"seq", sequence_}, {"hash", last_hash_}, {"mac", anchor_mac(key_, sequence_, last_hash_)}};
    replace_atomically(anchor_file(), anchor.dump());
}

void AuditLog::rotate_if_needed() {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file_, ec);
    if (ec || size < kMaxFileBytes) return;
    std::filesystem::remove(rotated(kRotatedFiles), ec);
    for (int index = kRotatedFiles - 1; index >= 1; --index) {
        if (std::filesystem::exists(rotated(index), ec))
            std::filesystem::rename(rotated(index), rotated(index + 1), ec);
    }
    std::filesystem::rename(file_, rotated(1), ec);
}

bool AuditLog::append_line(const std::string& line) {
    HANDLE handle = CreateFileW(file_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
                    written == line.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok;
}

void AuditLog::append(const std::string& actor, const std::string& event, core::Json detail) {
    std::scoped_lock lock(mutex_);
    rotate_if_needed();
    if (!detail.is_object()) detail = core::Json::object();
    core::Json entry{{"v", kCurrentVersion}, {"seq", sequence_ + 1}, {"ts", now_seconds()},
                     {"actor", actor},       {"event", event},       {"detail", std::move(detail)},
                     {"prev", last_hash_}};
    if (entry.dump().size() > kMaxEntryBytes) entry["detail"] = {{"truncated", true}};
    const auto hash = seal(kCurrentVersion, last_hash_, entry);
    entry["hash"] = hash;
    if (!append_line(entry.dump() + '\n')) {
        ++write_failures_;
        return;
    }
    ++sequence_;
    last_hash_ = hash;
    write_anchor();
}

std::uint64_t AuditLog::write_failures() const {
    std::scoped_lock lock(mutex_);
    return write_failures_;
}

core::Json AuditLog::tail(std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    std::deque<core::Json> window;
    auto files = files_oldest_first();
    for (auto file = files.rbegin(); file != files.rend() && window.size() < limit; ++file) {
        std::ifstream in(*file);
        std::string line;
        std::deque<core::Json> local;
        while (std::getline(in, line)) {
            if (line.size() > kMaxLineBytes) continue;
            auto entry = core::Json::parse(line, nullptr, false);
            if (entry.is_discarded() || !entry.is_object()) continue;
            if (!entry.value("detail", core::Json()).is_object()) entry["detail"] = core::Json::object();
            local.push_back(std::move(entry));
            if (local.size() > limit) local.pop_front();
        }
        while (!local.empty() && window.size() < limit) {
            window.push_front(std::move(local.back()));
            local.pop_back();
        }
    }
    core::Json result = core::Json::array();
    for (auto it = window.rbegin(); it != window.rend(); ++it) result.push_back(std::move(*it));
    return result;
}

core::Json AuditLog::query(const Query& query) const {
    std::scoped_lock lock(mutex_);
    const auto lowered = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const auto needle = lowered(query.text);
    core::Json page = core::Json::array();
    bool more = false;
    auto files = files_oldest_first();
    for (auto file = files.rbegin(); file != files.rend() && !more; ++file) {
        std::ifstream in(*file);
        std::vector<core::Json> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (line.size() > kMaxLineBytes) continue;
            auto entry = core::Json::parse(line, nullptr, false);
            if (entry.is_discarded() || !entry.is_object()) continue;
            lines.push_back(std::move(entry));
        }
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            const auto sequence = it->value("seq", std::uint64_t{0});
            if (query.before != 0 && sequence >= query.before) continue;
            if (it->value("ts", std::int64_t{0}) < query.since) continue;
            if (!query.events.empty() &&
                std::ranges::find(query.events, it->value("event", std::string())) == query.events.end())
                continue;
            if (!it->value("detail", core::Json()).is_object()) (*it)["detail"] = core::Json::object();
            if (!needle.empty() && lowered(it->dump()).find(needle) == std::string::npos) continue;
            if (page.size() == query.limit) {
                more = true;
                break;
            }
            page.push_back(std::move(*it));
        }
    }
    return {{"entries", std::move(page)}, {"more", more}};
}

AuditReport AuditLog::verify() const {
    std::scoped_lock lock(mutex_);
    AuditReport report;
    const auto fail = [&](std::string problem) {
        report.valid = false;
        report.problem = std::move(problem);
        return report;
    };

    std::string previous;
    bool first = true;
    bool authenticated = false;
    for (const auto& file : files_oldest_first()) {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            if (line.size() > kMaxLineBytes) return fail("oversized record");
            auto entry = core::Json::parse(line, nullptr, false);
            if (entry.is_discarded() || !entry.is_object() || !entry.contains("hash") ||
                !entry["hash"].is_string() || !entry.contains("seq") || !entry["seq"].is_number_unsigned()) {
                return fail("unreadable record");
            }
            const int version = entry.value("v", 1);
            if (authenticated && version < 2) return fail("unauthenticated record after sealed records");
            if (version >= 2) authenticated = true;
            const auto sequence = entry["seq"].get<std::uint64_t>();
            if (!first &&
                (entry.value("prev", std::string()) != previous || sequence != report.last_sequence + 1))
                return fail("chain broken at sequence " + std::to_string(sequence));
            const auto claimed = entry["hash"].get<std::string>();
            entry.erase("hash");
            if (seal(version, entry.value("prev", std::string()), entry) != claimed)
                return fail("record altered at sequence " + std::to_string(sequence));
            previous = claimed;
            report.last_sequence = sequence;
            ++report.entries;
            first = false;
        }
    }

    const auto anchor = core::Json::parse(read_all(anchor_file()), nullptr, false);
    if (!anchor.is_discarded() && anchor.is_object() &&
        anchor.value("seq", std::uint64_t{0}) > report.last_sequence) {
        report.truncated = true;
        return fail("log is shorter than its sealed position");
    }
    if (truncation_found_) report.truncated = true;
    return report;
}

}  // namespace houseguest::service
