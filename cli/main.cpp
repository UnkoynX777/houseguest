#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "houseguest/system/pipe.hpp"

namespace {

using houseguest::core::Json;

std::string read_secret(const char* prompt) {
    std::fputs(prompt, stderr);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(input, &mode) != 0;
    if (console) SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    std::string line;
    std::getline(std::cin, line);
    if (console) {
        SetConsoleMode(input, mode);
        std::fputc('\n', stderr);
    }
    return line;
}

std::optional<std::string> env(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
}

Json call(houseguest::system::PipeClient& client, const std::string& command, Json args = Json::object(),
          const std::string& token = {}) {
    Json request{{"id", 1}, {"cmd", command}, {"args", std::move(args)}};
    if (!token.empty()) request["token"] = token;
    auto reply = client.call(request);
    if (!reply) {
        std::fprintf(stderr, "Cannot reach the Houseguest service. Is it running?\n");
        std::exit(3);
    }
    return *reply;
}

template <class Fallback>
std::string pin_from_env_or(Fallback&& fallback) {
    if (auto value = env("HOUSEGUEST_PIN")) return *value;
    return fallback();
}

int print_result(const Json& reply) {
    if (reply.value("ok", false)) {
        std::puts(reply["data"].dump(2).c_str());
        return 0;
    }
    std::fprintf(stderr, "error: %s (%s)\n", reply["error"].value("message", "").c_str(),
                 reply["error"].value("code", "").c_str());
    return 1;
}

std::string unlock(houseguest::system::PipeClient& client) {
    const auto pin = pin_from_env_or([] { return read_secret("Owner PIN: "); });
    const auto reply = call(client, "unlock", {{"pin", pin}});
    if (!reply.value("ok", false)) {
        print_result(reply);
        std::exit(1);
    }
    return reply["data"]["token"].get<std::string>();
}

void usage() {
    std::puts(
        "usage: houseguest-cli <command>\n"
        "  status                      show service state\n"
        "  setup                       first-time owner setup (elevated prompt required)\n"
        "  recover [--reset-owner]     end the active session without the PIN (elevated)\n"
        "  call <cmd> [json-args]      run any owner command; PIN from HOUSEGUEST_PIN or prompt\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    houseguest::system::PipeClient client;
    const std::string command = argv[1];

    if (command == "status") return print_result(call(client, "status"));

    if (command == "setup") {
        const auto pin = pin_from_env_or([] {
            const auto first = read_secret("Choose an owner PIN (4-64 characters): ");
            if (first != read_secret("Repeat the PIN: ")) {
                std::fputs("PINs do not match.\n", stderr);
                std::exit(1);
            }
            return first;
        });
        auto reply = call(client, "setup", {{"pin", pin}});
        if (reply.value("ok", false)) reply["data"].erase("token");
        return print_result(reply);
    }

    if (command == "recover") {
        const bool reset = argc > 2 && std::string(argv[2]) == "--reset-owner";
        return print_result(call(client, "recover", {{"reset_owner", reset}}));
    }

    if (command == "call" && argc >= 3) {
        Json args = argc > 3 ? Json::parse(argv[3], nullptr, false) : Json::object();
        if (args.is_discarded()) {
            std::fputs("Arguments must be valid JSON.\n", stderr);
            return 2;
        }
        return print_result(call(client, argv[2], std::move(args), unlock(client)));
    }

    usage();
    return 2;
}
