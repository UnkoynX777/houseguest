#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace houseguest::system {

class ProcessRef {
public:
    static std::optional<ProcessRef> open(std::uint32_t pid);
    static std::uint64_t now_ticks();

    [[nodiscard]] std::uint32_t pid() const { return pid_; }
    [[nodiscard]] std::optional<std::string> user_sid() const;
    [[nodiscard]] std::optional<std::filesystem::path> image_path() const;
    [[nodiscard]] std::optional<std::uint64_t> creation_ticks() const;
    [[nodiscard]] std::uint32_t session_id() const;
    bool terminate() const;

private:
    ProcessRef(std::uint32_t pid, std::shared_ptr<void> handle) : pid_(pid), handle_(std::move(handle)) {}

    std::uint32_t pid_;
    std::shared_ptr<void> handle_;
};

std::vector<std::uint32_t> running_process_ids();

}  // namespace houseguest::system
