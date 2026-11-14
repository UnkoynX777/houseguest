#include "audit.hpp"

#include <chrono>
#include <deque>
#include <fstream>

#include "houseguest/system/crypto.hpp"

namespace houseguest::service {
namespace {

std::string chain_hash(const std::string& previous, const core::Json& entry) {
    const auto body = previous + entry.dump();
    return system::to_hex(system::sha256({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}));
}

}  // namespace

AuditLog::AuditLog(std::filesystem::path file) : file_(std::move(file)) {
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        const auto entry = core::Json::parse(line, nullptr, false);
        if (entry.is_discarded()) continue;
        sequence_ = entry.value("seq", sequence_);
        last_hash_ = entry.value("hash", last_hash_);
    }
}

void AuditLog::append(const std::string& actor, const std::string& event, core::Json detail) {
    std::scoped_lock lock(mutex_);
    core::Json entry{{"seq", ++sequence_},
                     {"ts", std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()},
                     {"actor", actor},
                     {"event", event},
                     {"detail", std::move(detail)},
                     {"prev", last_hash_}};
    last_hash_ = chain_hash(last_hash_, entry);
    entry["hash"] = last_hash_;
    std::ofstream out(file_, std::ios::app);
    out << entry.dump() << '\n';
}

core::Json AuditLog::tail(std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    std::deque<core::Json> window;
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        auto entry = core::Json::parse(line, nullptr, false);
        if (entry.is_discarded()) continue;
        window.push_back(std::move(entry));
        if (window.size() > limit) window.pop_front();
    }
    core::Json result = core::Json::array();
    for (auto it = window.rbegin(); it != window.rend(); ++it) result.push_back(std::move(*it));
    return result;
}

bool AuditLog::verify_chain() const {
    std::scoped_lock lock(mutex_);
    std::ifstream in(file_);
    std::string line, previous;
    while (std::getline(in, line)) {
        auto entry = core::Json::parse(line, nullptr, false);
        if (entry.is_discarded() || !entry.contains("hash") || entry.value("prev", "") != previous)
            return false;
        const auto claimed = entry["hash"].get<std::string>();
        entry.erase("hash");
        if (chain_hash(previous, entry) != claimed) return false;
        previous = claimed;
    }
    return true;
}

}  // namespace houseguest::service
