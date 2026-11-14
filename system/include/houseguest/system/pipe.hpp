#pragma once

#include <memory>
#include <optional>
#include <string>

#include "houseguest/core/serialization.hpp"

namespace houseguest::system {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\houseguest";
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
