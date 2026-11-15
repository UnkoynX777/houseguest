#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "gemini.hpp"

namespace houseguest::service::ai {

class ToolHost {
public:
    virtual ~ToolHost() = default;
    // Function declarations in the shape Gemini expects.
    [[nodiscard]] virtual core::Json declarations() const = 0;
    // Runs one tool. It validates its own arguments and throws or returns {"error": ...} for anything it
    // refuses.
    virtual core::Json call(const std::string& name, const core::Json& args) = 0;
};

struct ChatEvent {
    std::string type;  // text, tool, error
    core::Json data;
};
using EventSink = std::function<void(const ChatEvent&)>;

struct ChatOutcome {
    bool ok = false;
    std::string error;
    std::string error_code;
    std::string text;
    int rounds = 0;
};

inline constexpr int kMaxToolRounds = 6;
inline constexpr std::size_t kMaxToolResult = 16 * 1024;

[[nodiscard]] std::string system_instruction();

// Drives the conversation: sends the history, runs the tools the model asks for, feeds the results back and
// stops when the model answers in text. Tool output is wrapped as untrusted data on every round.
class ChatRunner {
public:
    ChatRunner(Gemini& gemini, ToolHost& tools) : gemini_(gemini), tools_(tools) {}
    ChatOutcome run(core::Json& history, const std::string& user_text, const EventSink& sink,
                    const std::atomic<bool>* cancel);

private:
    Gemini& gemini_;
    ToolHost& tools_;
};

}  // namespace houseguest::service::ai
