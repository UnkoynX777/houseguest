#include "store.hpp"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

constexpr int kStateVersion = 1;
constexpr std::size_t kMaxBackups = 20;

std::optional<std::string> read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_durably(const std::filesystem::path& file, const std::string& content) {
    const auto temp = std::filesystem::path(file).concat(L".tmp");
    HANDLE handle =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create state file");
    DWORD written = 0;
    const bool ok =
        WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot write state file");
    }
}

system::Bytes load_or_create_key(const std::filesystem::path& file) {
    if (const auto sealed = read_text(file)) {
        const auto raw =
            system::dpapi_unprotect({reinterpret_cast<const std::uint8_t*>(sealed->data()), sealed->size()});
        if (raw && raw->size() == 32) return *raw;
        throw std::runtime_error("integrity key is unreadable");
    }
    const auto key = system::random_bytes(32);
    const auto sealed = system::dpapi_protect_machine(key);
    write_durably(file, std::string(sealed.begin(), sealed.end()));
    return key;
}

std::string mac_of(const system::Bytes& key, const std::string& payload) {
    return system::to_hex(
        system::hmac_sha256(key, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()}));
}

}  // namespace

core::Json state_to_json(const State& state) {
    core::Json profiles = core::Json::array();
    for (const auto& profile : state.profiles) profiles.push_back(core::to_json(profile));
    core::Json library = core::Json::array();
    for (const auto& app : state.library) library.push_back(core::to_json(app));
    core::Json json{
        {"version", kStateVersion},
        {"owner_sid", state.owner_sid},
        {"pin",
         {{"salt", state.pin.salt_hex}, {"hash", state.pin.hash_hex}, {"iterations", state.pin.iterations}}},
        {"failed_attempts", state.failed_attempts},
        {"locked_until", state.locked_until},
        {"language", state.language},
        {"default_on_expiry", state.default_on_expiry},
        {"profiles", std::move(profiles)},
        {"library", std::move(library)}};
    if (state.session) {
        core::Json exceptions = core::Json::array();
        for (const auto& rule : state.session->exceptions) exceptions.push_back(core::to_json(rule));
        json["session"] = {
            {"profile_id", state.session->profile_id}, {"guest_sids", state.session->guest_sids},
            {"started_at", state.session->started_at}, {"ends_at", state.session->ends_at},
            {"on_expiry", state.session->on_expiry},   {"audit_only", state.session->audit_only},
            {"exceptions", std::move(exceptions)},     {"phase", state.session->phase},
            {"end_reason", state.session->end_reason}, {"end_apply_action", state.session->end_apply_action}};
    }
    core::Json managed = core::Json::array();
    for (const auto& account : state.managed)
        managed.push_back({{"name", account.name}, {"sid", account.sid}});
    json["managed"] = std::move(managed);
    core::Json manual = core::Json::array();
    for (const auto& app : state.manual_apps)
        manual.push_back({{"path", app.path}, {"name", app.name}, {"added_at", app.added_at}});
    json["manual_apps"] = std::move(manual);
    json["portable_roots"] = state.portable_roots;
    json["resolutions"] = state.resolutions;
    json["include_removable"] = state.include_removable;
    return json;
}

State state_from_json(const core::Json& json) {
    if (json.value("version", 0) != kStateVersion) throw std::invalid_argument("unsupported state version");
    State state;
    state.owner_sid = json.value("owner_sid", "");
    const auto& pin = json.at("pin");
    state.pin = {pin.value("salt", ""), pin.value("hash", ""), pin.value("iterations", 0u)};
    state.failed_attempts = json.value("failed_attempts", 0);
    state.locked_until = json.value("locked_until", std::int64_t{0});
    state.language = json.value("language", "en");
    state.default_on_expiry = json.value("default_on_expiry", "disconnect");
    for (const auto& item : json.at("profiles")) state.profiles.push_back(core::profile_from_json(item));
    if (const auto it = json.find("library"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) state.library.push_back(core::application_from_json(item));
    }
    if (const auto it = json.find("manual_apps"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            state.manual_apps.push_back(
                {item.value("path", ""), item.value("name", ""), item.value("added_at", std::int64_t{0})});
        }
    }
    if (const auto it = json.find("portable_roots"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) state.portable_roots.push_back(item.get<std::string>());
        }
    }
    if (const auto it = json.find("resolutions"); it != json.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (value.is_string()) state.resolutions[key] = value.get<std::string>();
        }
    }
    state.include_removable = json.value("include_removable", false);
    if (const auto it = json.find("managed"); it != json.end()) {
        for (const auto& account : *it) {
            state.managed.push_back(
                {account.at("name").get<std::string>(), account.at("sid").get<std::string>()});
        }
    }
    if (const auto it = json.find("session"); it != json.end()) {
        Session session;
        session.profile_id = it->at("profile_id").get<std::string>();
        session.guest_sids = it->at("guest_sids").get<std::vector<std::string>>();
        session.started_at = it->at("started_at").get<std::int64_t>();
        session.ends_at = it->at("ends_at").get<std::int64_t>();
        session.on_expiry = it->value("on_expiry", "disconnect");
        session.audit_only = it->value("audit_only", false);
        for (const auto& rule : it->at("exceptions"))
            session.exceptions.push_back(core::rule_from_json(rule));
        session.phase = it->value("phase", "active");
        if (session.phase != "active" && session.phase != "ending") throw std::invalid_argument("bad phase");
        session.end_reason = it->value("end_reason", "");
        session.end_apply_action = it->value("end_apply_action", false);
        state.session = std::move(session);
    }
    return state;
}

void restrict_to_administrators(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                                                              SDDL_REVISION_1, &descriptor, nullptr)) {
        throw std::runtime_error("invalid security descriptor");
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    if (result != ERROR_SUCCESS) throw std::runtime_error("cannot restrict data directory permissions");
}

Store::Store(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::optional<State> Store::read_verified(const std::filesystem::path& file) const {
    const auto text = read_text(file);
    if (!text) return std::nullopt;
    const auto envelope = core::Json::parse(*text, nullptr, false);
    if (envelope.is_discarded() || !envelope.contains("state") || !envelope.contains("mac"))
        return std::nullopt;
    const auto payload = envelope["state"].dump();
    const auto expected = system::from_hex(mac_of(mac_key_, payload));
    const auto actual = system::from_hex(envelope["mac"].get<std::string>());
    if (!expected || !actual || !system::constant_time_equal(*expected, *actual)) return std::nullopt;
    try {
        return state_from_json(envelope["state"]);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

system::Bytes Store::derive_key(const std::string& label) const {
    return system::hmac_sha256(mac_key_, {reinterpret_cast<const std::uint8_t*>(label.data()), label.size()});
}

LoadResult Store::load() {
    restrict_to_administrators(directory_);
    mac_key_ = load_or_create_key(directory_ / L"integrity.key");

    const auto primary = directory_ / L"state.json";
    const auto previous = directory_ / L"state.previous.json";
    if (!std::filesystem::exists(primary) && !std::filesystem::exists(previous)) return LoadResult::Fresh;

    if (auto loaded = read_verified(primary)) {
        state = std::move(*loaded);
        return LoadResult::Loaded;
    }
    std::error_code ec;
    if (std::filesystem::exists(primary, ec)) {
        std::filesystem::rename(primary, directory_ / L"state.rejected.json", ec);
    }
    if (auto loaded = read_verified(previous)) {
        state = std::move(*loaded);
        return LoadResult::Recovered;
    }
    return LoadResult::Tampered;
}

void Store::save() {
    const auto primary = directory_ / L"state.json";
    std::error_code ec;
    if (std::filesystem::exists(primary, ec)) {
        std::filesystem::copy_file(primary, directory_ / L"state.previous.json",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    const auto payload = state_to_json(state);
    const core::Json envelope{{"state", payload}, {"mac", mac_of(mac_key_, payload.dump())}};
    write_durably(primary, envelope.dump(2));
}

std::filesystem::path Store::create_backup() {
    const auto folder = directory_ / L"backups";
    std::filesystem::create_directories(folder);
    const auto stamp =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto target = folder / (L"state-" + std::to_wstring(stamp) + L".json");
    std::filesystem::copy_file(directory_ / L"state.json", target,
                               std::filesystem::copy_options::overwrite_existing);

    auto names = list_backups();
    while (names.size() > kMaxBackups) {
        std::filesystem::remove(folder / system::path_from_utf8(names.front()));
        names.erase(names.begin());
    }
    return target;
}

std::vector<std::string> Store::list_backups() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_ / L"backups", ec)) {
        if (entry.path().extension() == L".json")
            names.push_back(system::path_to_utf8(entry.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

bool Store::restore_backup(const std::string& name) {
    if (name.find_first_of("/\\") != std::string::npos) return false;
    const auto file = directory_ / L"backups" / system::path_from_utf8(name);
    auto restored = read_verified(file);
    if (!restored) return false;
    state = std::move(*restored);
    return true;
}

}  // namespace houseguest::service
