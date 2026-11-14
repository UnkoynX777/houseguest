#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "houseguest/system/app_catalog.hpp"

namespace houseguest::system::catalog {

namespace fs = std::filesystem;

std::string lower(std::string value);
std::string alnum(const std::string& text);
std::vector<std::string> tokens(const std::string& text);
double token_overlap(const std::vector<std::string>& wanted, const std::vector<std::string>& offered);

fs::path expand(std::wstring text);
fs::path environment_path(const wchar_t* name);

std::wstring registry_text(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0);
DWORD registry_number(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0);
std::vector<std::wstring> subkeys(HKEY root, const std::wstring& key, REGSAM view = 0);

fs::path resolve_shortcut(const fs::path& link);

struct VersionInfo {
    std::string company;
    std::string product;
    std::string description;
    std::string version;
    std::string original_name;
    std::string internal_name;
};
VersionInfo read_version(const fs::path& file);

enum class Subsystem { Unknown, Gui, Console };
Subsystem pe_subsystem(const fs::path& file);

std::string canonical_key(const fs::path& path);
bool key_inside(const std::string& root_key, const std::string& key);
bool under_windows(const std::string& lowered_path);
bool usable_executable(const fs::path& file);
std::string volume_of(const fs::path& path);

struct FileEntry {
    fs::path path;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

struct WalkBudget {
    std::size_t max_entries = 6000;
    std::chrono::milliseconds max_time{2000};
    const std::atomic<bool>* cancel = nullptr;
};

struct WalkResult {
    std::vector<FileEntry> executables;
    std::size_t visited = 0;
    std::size_t skipped_links = 0;
    bool truncated = false;
};

WalkResult find_executables(const fs::path& root, const WalkBudget& budget,
                            const std::function<bool(const std::wstring& name)>& skip_directory = {});

std::vector<std::string> running_images();

std::string hash_text(const std::string& text);
std::int64_t epoch_now();

}  // namespace houseguest::system::catalog
