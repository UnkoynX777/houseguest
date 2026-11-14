#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "audit.hpp"
#include "ipc_limits.hpp"

using namespace houseguest;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto directory = [] {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        auto path = std::filesystem::path(temp) /
                    (L"houseguest-fuzz-audit-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(path);
        return path;
    }();

    static_cast<void>(
        service::nesting_within({reinterpret_cast<const char*>(data), size}, service::kMaxJsonDepth));

    const auto file = directory / "audit.log";
    std::filesystem::remove(file);
    std::filesystem::remove(directory / "audit.log.anchor");
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

    service::AuditLog log(file, system::Bytes(32, 1));
    static_cast<void>(log.verify());
    static_cast<void>(log.tail(8));
    log.append("fuzz", "event", {{"size", size}});
    static_cast<void>(log.verify());
    return 0;
}
