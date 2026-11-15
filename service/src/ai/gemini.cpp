#include "gemini.hpp"

#include <regex>

namespace houseguest::service::ai {
namespace {

using core::Json;
constexpr const char* kBase = "https://generativelanguage.googleapis.com/v1beta";

void classify(const system::HttpsResponse& response, std::string& code, std::string& message, int& retry) {
    Json doc;
    if (!response.body.empty()) doc = Json::parse(response.body, nullptr, false);
    std::string api_message;
    std::string api_status;
    if (doc.is_object() && doc.contains("error") && doc["error"].is_object()) {
        api_message = doc["error"].value("message", "");
        api_status = doc["error"].value("status", "");
    }
    if (response.error == "Cancelled.") {
        code = "cancelled";
        message = "Cancelled.";
    } else if (response.status == 0) {
        code = "unavailable";
        message = response.error.empty() ? "Gemini could not be reached." : response.error;
    } else if (response.status == 429) {
        code = "rate_limited";
        message = "The Gemini usage limit was reached. Wait a moment or choose another model.";
        retry = 30;
    } else if (response.status == 400 &&
               (api_message.find("API key") != std::string::npos ||
                (api_status == "INVALID_ARGUMENT" && api_message.find("key") != std::string::npos))) {
        code = "invalid_key";
        message = "Gemini does not accept this API key.";
    } else if (response.status == 401 || response.status == 403) {
        code = "invalid_key";
        message = "Gemini refused this API key or it may not use this model.";
    } else if (response.status >= 500) {
        code = "unavailable";
        message = "Gemini is not available right now.";
    } else {
        code = "error";
        message = "Gemini rejected the request (" + std::to_string(response.status) + ").";
    }
}

}  // namespace

bool valid_model_id(const std::string& id) {
    static const std::regex pattern(R"(^[a-z0-9][a-z0-9._-]{2,63}$)");
    return std::regex_match(id, pattern);
}

void SseParser::feed(std::string_view chunk, const std::function<void(const Json&)>& on_event) {
    buffer_.append(chunk);
    if (buffer_.size() > std::size_t{4} * 1024 * 1024) buffer_.clear();
    std::size_t newline = 0;
    while ((newline = buffer_.find('\n')) != std::string::npos) {
        auto line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.starts_with("data:")) continue;
        auto payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
        const Json event = Json::parse(payload, nullptr, false);
        if (event.is_object()) on_event(event);
    }
}

Gemini::Gemini(StreamTransport transport, std::string api_key, std::string model)
    : transport_(std::move(transport)), key_(std::move(api_key)), model_(std::move(model)) {}

Gemini::ModelList Gemini::list_models() {
    system::HttpsRequest request;
    request.url = std::string(kBase) + "/models?pageSize=200";
    request.headers = {{"x-goog-api-key", key_}};
    request.max_response = std::size_t{2} * 1024 * 1024;
    const auto response = transport_(request, nullptr, nullptr);
    ModelList list;
    if (!response.ok()) {
        int retry = 0;
        classify(response, list.error_code, list.error, retry);
        return list;
    }
    const Json doc = Json::parse(response.body, nullptr, false);
    if (doc.is_object() && doc.contains("models") && doc["models"].is_array()) {
        for (const auto& model : doc["models"]) {
            if (!model.is_object() || !model.contains("name") || !model["name"].is_string()) continue;
            bool generates = false;
            if (model.contains("supportedGenerationMethods") &&
                model["supportedGenerationMethods"].is_array())
                for (const auto& method : model["supportedGenerationMethods"])
                    if (method == "generateContent") generates = true;
            auto name = model["name"].get<std::string>();
            if (name.starts_with("models/")) name.erase(0, 7);
            if (generates && valid_model_id(name) && name.starts_with("gemini")) list.models.push_back(name);
        }
    }
    if (list.models.empty()) {
        list.error_code = "error";
        list.error = "Gemini listed no usable models for this key.";
    }
    return list;
}

Turn Gemini::generate(const Json& contents, const Json& tools, const std::string& system_instruction,
                      const std::function<void(const std::string&)>& on_text,
                      const std::atomic<bool>* cancel) {
    Turn turn;
    if (!valid_model_id(model_)) {
        turn.error = "The model name is not valid.";
        turn.error_code = "error";
        return turn;
    }
    Json body{{"contents", contents},
              {"systemInstruction", {{"parts", {{{"text", system_instruction}}}}}},
              {"generationConfig", {{"temperature", 0.3}, {"maxOutputTokens", 2048}}}};
    if (tools.is_array() && !tools.empty()) {
        body["tools"] = Json::array({{{"functionDeclarations", tools}}});
        body["toolConfig"] = {{"functionCallingConfig", {{"mode", "AUTO"}}}};
    }
    system::HttpsRequest request;
    request.method = "POST";
    request.url = std::string(kBase) + "/models/" + model_ + ":streamGenerateContent?alt=sse";
    request.headers = {{"x-goog-api-key", key_}, {"Content-Type", "application/json"}};
    request.body = body.dump();
    request.timeout_ms = 60000;

    Json parts = Json::array();
    std::string block_reason;
    SseParser parser;
    std::string raw;
    const auto response = transport_(
        request,
        [&](std::string_view chunk) {
            if (raw.size() < 8192) raw.append(chunk.substr(0, 8192 - raw.size()));
            parser.feed(chunk, [&](const Json& event) {
                if (event.contains("promptFeedback") && event["promptFeedback"].is_object())
                    block_reason = event["promptFeedback"].value("blockReason", block_reason);
                if (!event.contains("candidates") || !event["candidates"].is_array() ||
                    event["candidates"].empty())
                    return;
                const auto& candidate = event["candidates"][0];
                if (!candidate.is_object() || !candidate.contains("content") ||
                    !candidate["content"].is_object() || !candidate["content"].contains("parts") ||
                    !candidate["content"]["parts"].is_array())
                    return;
                for (const auto& part : candidate["content"]["parts"]) {
                    if (!part.is_object()) continue;
                    if (part.contains("functionCall") && part["functionCall"].is_object()) {
                        const auto& call = part["functionCall"];
                        if (call.contains("name") && call["name"].is_string()) {
                            FunctionCall fc;
                            fc.name = call["name"].get<std::string>();
                            if (call.contains("args") && call["args"].is_object()) fc.args = call["args"];
                            turn.calls.push_back(std::move(fc));
                        }
                        parts.push_back(part);
                    } else if (part.contains("text") && part["text"].is_string() &&
                               part.value("thought", false) != true) {
                        const auto text = part["text"].get<std::string>();
                        turn.text += text;
                        if (on_text) on_text(text);
                        if (!parts.empty() && parts.back().contains("text") &&
                            !parts.back().contains("thoughtSignature") && !part.contains("thoughtSignature"))
                            parts.back()["text"] = parts.back()["text"].get<std::string>() + text;
                        else
                            parts.push_back(part);
                    } else {
                        parts.push_back(
                            part);  // thought signatures and other parts must be returned unchanged
                    }
                }
            });
            return !(cancel && cancel->load());
        },
        cancel);

    if (!response.ok() || !response.error.empty()) {
        // For a non-2xx answer the body arrived through the sink, so rebuild a response to classify.
        system::HttpsResponse failed = response;
        if (failed.body.empty()) failed.body = raw;
        classify(failed, turn.error_code, turn.error, turn.retry_after);
        return turn;
    }
    if (!block_reason.empty() && turn.text.empty() && turn.calls.empty()) {
        turn.error_code = "blocked";
        turn.error = "Gemini declined to answer this request.";
        return turn;
    }
    turn.content = {{"role", "model"}, {"parts", parts}};
    return turn;
}

}  // namespace houseguest::service::ai
