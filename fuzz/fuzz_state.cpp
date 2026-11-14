#include <cstddef>
#include <cstdint>
#include <exception>

#include "store.hpp"

using namespace houseguest;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto json = core::Json::parse(data, data + size, nullptr, false);
    if (json.is_discarded()) return 0;
    try {
        const service::State state = service::state_from_json(json);
        const auto written = service::state_to_json(state);
        service::state_from_json(written);
    } catch (const std::exception&) {
    }
    return 0;
}
