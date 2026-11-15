#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/https.hpp"

namespace houseguest::service::ai {

using StreamTransport = std::function<system::HttpsResponse(const system::HttpsRequest&, const system::ChunkSink&,
                                                            const std::atomic<bool>*)>;

struct FunctionCall {
    std::string name;
    core::Json args = core::Json::object();
};

struct Turn {
    core::Json content;  // the model message exactly as received, so signatures survive the next request
    std::string text;
    std::vector<FunctionCall> calls;
    std::string error;
    std::string error_code;  // invalid_key, rate_limited, unavailable, cancelled, blocked, error
    int retry_after = 0;
    [[nodiscard]] bool ok() const { return error.empty(); }
};

// Incremental reader for the server-sent events of streamGenerateContent. It copes with events split across
// network reads and ignores anything that is not a data line.
class SseParser {
public:
    void feed(std::string_view chunk, const std::function<void(const core::Json&)>& on_event);

private:
    std::string buffer_;
};

// Google Gemini REST client. The API key travels only in the x-goog-api-key header and never in an address, a log
// or an error message.
class Gemini {
public:
    Gemini(StreamTransport transport, std::string api_key, std::string model);

    // Lists the models that can generate content. Doubles as the key check.
    struct ModelList {
        std::vector<std::string> models;
        std::string error;
        std::string error_code;
    };
    [[nodiscard]] ModelList list_models();

    [[nodiscard]] Turn generate(const core::Json& contents, const core::Json& tools, const std::string& system_instruction,
                                const std::function<void(const std::string&)>& on_text,
                                const std::atomic<bool>* cancel);

private:
    StreamTransport transport_;
    std::string key_;
    std::string model_;
};

[[nodiscard]] bool valid_model_id(const std::string& id);

}  // namespace houseguest::service::ai
