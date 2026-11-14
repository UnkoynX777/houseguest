#pragma once

#include <memory>
#include <optional>
#include <string>

#include "houseguest/core/serialization.hpp"

namespace houseguest::system {

// Builds configured with HOUSEGUEST_INSTANCES read HOUSEGUEST_INSTANCE so a test service can run beside an
// installed one without touching its pipe or trace session. Release builds ignore the variable.
std::wstring instance_suffix();
std::wstring pipe_name();
inline constexpr std::size_t kMaxMessageBytes = 1 << 20;

class PipeClient {
public:
    PipeClient();
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    std::optional<core::Json> call(const core::Json& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace houseguest::system
