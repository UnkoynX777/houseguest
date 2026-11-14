#include "lineage.hpp"

#include <algorithm>

namespace houseguest::service {

void Lineage::remember(const system::ProcessRef& process, std::uint64_t created, const std::string& sid,
                       std::vector<std::string> members) {
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(process.pid());
    if (it == records_.end() && records_.size() >= kMaxRecords) {
        ++saturated_;
        return;
    }
    if (it != records_.end() && it->second.created == created) {
        auto& known = it->second.members;
        for (auto& member : members) {
            if (std::ranges::find(known, member) == known.end()) known.push_back(std::move(member));
        }
        return;
    }
    records_.insert_or_assign(process.pid(), Record{process, created, sid, std::move(members), std::nullopt});
}

std::optional<ParentMembership> Lineage::find(std::uint32_t parent_pid, std::uint64_t child_created) const {
    std::scoped_lock lock(mutex_);
    const auto it = records_.find(parent_pid);
    if (it == records_.end() || it->second.created >= child_created) return std::nullopt;
    if (const auto exit = it->second.process.exit_ticks(); exit && child_created > *exit) return std::nullopt;
    return ParentMembership{it->second.members, it->second.sid, it->second.process.session_id()};
}

void Lineage::sweep() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    std::erase_if(records_, [&](auto& entry) {
        auto& record = entry.second;
        if (!record.gone && record.process.exited()) record.gone = now;
        return record.gone && now - *record.gone > kGrace;
    });
}

void Lineage::clear() {
    std::scoped_lock lock(mutex_);
    records_.clear();
}

std::size_t Lineage::size() const {
    std::scoped_lock lock(mutex_);
    return records_.size();
}

std::uint64_t Lineage::saturated() const {
    std::scoped_lock lock(mutex_);
    return saturated_;
}

}  // namespace houseguest::service
