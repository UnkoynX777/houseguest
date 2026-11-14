#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>

#include "houseguest/core/analysis.hpp"
#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/serialization.hpp"

using namespace houseguest::core;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto json = Json::parse(data, data + size, nullptr, false);
    if (json.is_discarded()) return 0;
    try {
        const Profile profile = profile_from_json(json);
        const auto baseline = Baseline::windows_default("c:/windows", "c:/program files/houseguest");
        const auto now = std::chrono::system_clock::now();
        static_cast<void>(analyze(profile, baseline, now));
        const AppIdentity app{"c:/users/guest/app.exe", std::string(64, 'a'), "O=Example", ""};
        static_cast<void>(evaluate(profile, baseline, app, now));
        const Profile again = profile_from_json(to_json(profile));
        if (again.rules.size() != profile.rules.size()) std::abort();
    } catch (const std::exception&) {
    }
    return 0;
}
