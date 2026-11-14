#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "controller.hpp"

using namespace houseguest;

namespace {

struct Harness {
    std::filesystem::path directory;
    std::unique_ptr<service::Controller> controller;
    std::string token;
    service::Caller owner{"S-1-5-21-1-2-3-1001", true};

    Harness() {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        directory =
            std::filesystem::path(temp) / (L"houseguest-fuzz-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        controller = std::make_unique<service::Controller>(directory, false);
        const auto reply =
            controller->handle(owner, {{"id", 1}, {"cmd", "setup"}, {"args", {{"pin", "482913"}}}});
        if (reply.value("ok", false)) token = reply["data"]["token"].get<std::string>();
    }

    ~Harness() {
        controller.reset();
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static Harness harness;
    if (size == 0) return 0;
    auto request = core::Json::parse(data + 1, data + size, nullptr, false);
    if (!request.is_object()) return 0;
    if (!request.contains("token") && (data[0] & 1)) request["token"] = harness.token;
    const service::Caller caller{harness.owner.sid, (data[0] & 2) != 0};
    harness.controller->handle(caller, request);
    return 0;
}
