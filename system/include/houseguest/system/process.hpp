#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace houseguest::system {

class ProcessRef {
public:
    static std::optional<ProcessRef> open(std::uint32_t pid, std::uint32_t* error = nullptr);
    static std::uint64_t now_ticks();

    [[nodiscard]] std::uint32_t pid() const { return pid_; }
    [[nodiscard]] std::optional<std::string> user_sid() const;
    [[nodiscard]] std::optional<std::filesystem::path> image_path() const;
    [[nodiscard]] std::optional<std::uint64_t> creation_ticks() const;
    [[nodiscard]] std::uint32_t session_id() const;
    [[nodiscard]] std::string package_family() const;
    [[nodiscard]] bool exited() const;
    [[nodiscard]] std::optional<std::uint64_t> exit_ticks() const;
    [[nodiscard]] bool kernel_critical() const;
    [[nodiscard]] bool protected_process() const;

    enum class Termination { Done, Refused, Failed };
    // Re-reads the critical and protection state immediately before ending the process, so a decision made
    // earlier can never take down something the kernel has since flagged as essential.
    [[nodiscard]] Termination terminate_checked() const;

private:
    ProcessRef(std::uint32_t pid, std::shared_ptr<void> handle) : pid_(pid), handle_(std::move(handle)) {}

    std::uint32_t pid_;
    std::shared_ptr<void> handle_;
};

std::vector<std::uint32_t> running_process_ids();

// One toolhelp snapshot: process ids and their recorded parents. A parent id only names the parent while the
// parent's creation time is earlier than the child's; callers verify that.
class ProcessSnapshot {
public:
    static ProcessSnapshot take();
    [[nodiscard]] std::optional<std::uint32_t> parent_of(std::uint32_t pid) const;
    [[nodiscard]] std::chrono::steady_clock::time_point taken() const { return taken_; }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> parents_;
    std::chrono::steady_clock::time_point taken_;
};

}  // namespace houseguest::system
