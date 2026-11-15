#include "assessor.hpp"

#include <windows.h>

#include <algorithm>

#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

using std::chrono::milliseconds;

constexpr auto kSnapshotLifetime = milliseconds(100);

bool older_than(std::uint32_t pid, std::uint64_t child_created) {
    const auto parent = system::ProcessRef::open(pid);
    const auto created = parent ? parent->creation_ticks() : std::nullopt;
    return created && *created < child_created;
}

bool contains(const std::vector<std::string>& list, const char* value) {
    return std::ranges::find(list, value) != list.end();
}

}  // namespace

Assessor::Assessor(IdentityResolver& resolver, ContinuityGuard& continuity, Lineage& lineage,
                   const std::string& system_root, const std::string& program_files)
    : resolver_(resolver),
      continuity_(continuity),
      lineage_(lineage),
      protection_(core::Protection::windows_default(system_root, program_files)),
      snapshot_(system::ProcessSnapshot::take()) {}

std::optional<std::uint32_t> Assessor::parent_of(std::uint32_t pid, std::uint64_t created) {
    std::scoped_lock lock(snapshot_mutex_);
    const auto age = std::chrono::steady_clock::now() - snapshot_.taken();
    if (age > kSnapshotLifetime || snapshot_.taken_ticks() <= created)
        snapshot_ = system::ProcessSnapshot::take();
    return snapshot_.parent_of(pid);
}

Assessment Assessor::assess(const system::ProcessRef& process, std::uint64_t created, const std::string& sid,
                            const EnforcementPolicy* policy, bool final,
                            const ParentPending& parent_pending) {
    Assessment a;
    a.sid = sid;
    a.session = process.session_id();
    auto& f = a.facts;
    f.kernel_critical = process.kernel_critical();
    f.protected_process = process.protected_process();
    const bool essential = f.kernel_critical || f.protected_process;

    const auto family = process.package_family();
    const auto image = process.image_path();
    if (!image) {
        f.image_known = false;
        if (!final && !essential) {
            a.retry_stage = "image";
            a.error = GetLastError();
            return a;
        }
        a.degraded = !essential;
        a.degraded_stage = "image";
    } else {
        auto first = resolver_.resolve_checked(*image, false, false, family);
        f.app = std::move(first.identity);
        f.remote_image = first.remote;
        a.image = *image;
    }

    if (const auto parent = parent_of(process.pid(), created);
        parent && *parent != 0 && *parent != process.pid()) {
        a.parent_pid = *parent;
        const auto membership = lineage_.find(*parent, created);
        if (membership && membership->sid == sid && membership->session == a.session) {
            f.lineage = membership->members;
        } else if (!membership && !final && parent_pending && parent_pending(*parent) &&
                   older_than(*parent, created)) {
            a.retry_stage = "parent";
            return a;
        }
    }

    const auto fail_to_resolve = [&](const char* stage) {
        if (!final) {
            a.retry_stage = stage;
            a.error = GetLastError();
            return true;
        }
        a.degraded = true;
        a.degraded_stage = stage;
        return false;
    };

    if (f.image_known && !f.remote_image) {
        if (continuity_.is_houseguest_image(f.app.path)) {
            auto full = resolver_.resolve_checked(*image, true, false, family);
            if (!full.complete && fail_to_resolve("hash")) return a;
            f.app = std::move(full.identity);
            f.hash_resolved = true;
            if (continuity_.houseguest_verified(f.app.path, f.app.sha256))
                f.continuity = core::Continuity::Houseguest;
        } else if (ContinuityGuard::is_webview_image(f.app.path) && contains(f.lineage, kHouseguestMember)) {
            auto full = resolver_.resolve_checked(*image, false, true, family);
            f.app = std::move(full.identity);
            f.publisher_resolved = true;
            if (continuity_.webview_verified(f.app.path, f.app.publisher))
                f.continuity = core::Continuity::WebView2;
        }
    }

    static const core::Profile kEmpty{"", "", core::Action::Allow, {}, {}};
    const auto& profile = policy ? policy->profile : kEmpty;
    const auto& protection = policy ? policy->protection : protection_;
    const auto now = std::chrono::system_clock::now();
    gather_activation_facts(a, created, protection);

    for (int round = 0; round < 3; ++round) {
        a.decision = core::evaluate(profile, protection, f, now);
        const bool need_hash = a.decision.needs_hash && !f.hash_resolved;
        const bool need_publisher = a.decision.needs_publisher && !f.publisher_resolved;
        if ((!need_hash && !need_publisher) || !f.image_known) break;
        if (f.remote_image) {
            f.hash_resolved = f.hash_resolved || need_hash;
            f.publisher_resolved = f.publisher_resolved || need_publisher;
            a.degraded = true;
            a.degraded_stage = "remote_image";
            continue;
        }
        auto full = resolver_.resolve_checked(*image, need_hash || f.hash_resolved,
                                              need_publisher || f.publisher_resolved, family);
        if (need_hash && !full.complete && fail_to_resolve("hash")) return a;
        f.app = std::move(full.identity);
        f.hash_resolved = f.hash_resolved || need_hash;
        f.publisher_resolved = f.publisher_resolved || need_publisher;
    }

    if (a.decision.action == core::Action::Allow) {
        auto members = a.decision.member_of;
        if (f.continuity != core::Continuity::None) members.emplace_back(kHouseguestMember);
        if (!members.empty()) lineage_.remember(process, created, sid, std::move(members));
    }
    return a;
}

void Assessor::gather_activation_facts(Assessment& a, std::uint64_t created,
                                       const core::Protection& protection) {
    auto& f = a.facts;
    if (!f.image_known || f.remote_image || a.parent_pid == 0 || protection.covers(f.app.path) ||
        !protection.may_be_com_server(f.app.path))
        return;
    if (!com_servers_.registered(system::path_from_utf8(f.app.path)) || !older_than(a.parent_pid, created))
        return;
    const auto parent = system::ProcessRef::open(a.parent_pid);
    const auto path = parent ? parent->image_path() : std::nullopt;
    if (!path) return;
    f.com_server = true;
    f.parent_path = resolver_.resolve_checked(*path, false, false).identity.path;
}

void Assessor::resolve_fully(Assessment& assessment, const system::ProcessRef& process, bool with_publisher) {
    auto& f = assessment.facts;
    if (!f.image_known || f.remote_image || (f.hash_resolved && (f.publisher_resolved || !with_publisher)))
        return;
    auto full = resolver_.resolve_checked(assessment.image, true, with_publisher || f.publisher_resolved,
                                          process.package_family());
    f.app = std::move(full.identity);
    f.hash_resolved = true;
    f.publisher_resolved = f.publisher_resolved || with_publisher;
}

}  // namespace houseguest::service
