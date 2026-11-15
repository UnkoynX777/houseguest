#pragma once

#include <span>
#include <string_view>

namespace houseguest::service::api {

struct Asset {
    const char* route;
    const char* content_type;
    std::string_view body;
};

// The remote panel: a small web application served from memory, never from disk.
std::span<const Asset> panel_assets();

}  // namespace houseguest::service::api
