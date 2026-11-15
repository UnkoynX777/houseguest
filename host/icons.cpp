#include "icons.hpp"

#include <windows.h>

#include <objbase.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"

#include "icon_render.hpp"

namespace houseguest::host {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kPrefix = "data:image/png;base64,";

std::string base64(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += kAlphabet[a >> 2];
        out += kAlphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? kAlphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[c & 63] : '=';
    }
    return out;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

IconService::IconService(std::filesystem::path cache_directory) : directory_(std::move(cache_directory)) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    const auto limit = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 45);
    for (std::filesystem::directory_iterator it(directory_, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code stale;
        if (it->path().extension() == L".png" && it->last_write_time(stale) < limit && !stale)
            std::filesystem::remove(it->path(), stale);
    }
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

IconService::~IconService() {
    worker_.request_stop();
    ready_.notify_all();
}

void IconService::request(const std::string& utf8_path, std::vector<IconSource> sources, int pixels,
                          Reply reply) {
    Reply dropped;
    {
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= kQueueLimit) {
            dropped = std::move(queue_.front().reply);
            queue_.pop_front();
        }
        queue_.push_back({utf8_path, std::move(sources), pixels, std::move(reply)});
    }
    ready_.notify_one();
    if (dropped) dropped({});
}

std::optional<std::string> IconService::cached(const std::string& key) {
    std::scoped_lock lock(mutex_);
    const auto it = memory_.find(key);
    if (it == memory_.end()) return std::nullopt;
    order_.splice(order_.begin(), order_, it->second.second);
    return it->second.first;
}

void IconService::remember(const std::string& key, const std::string& value) {
    std::scoped_lock lock(mutex_);
    if (const auto it = memory_.find(key); it != memory_.end()) {
        order_.erase(it->second.second);
        memory_.erase(it);
    }
    order_.push_front(key);
    memory_[key] = {value, order_.begin()};
    while (memory_.size() > kMemoryEntries) {
        memory_.erase(order_.back());
        order_.pop_back();
    }
}

std::string IconService::produce(const std::string& utf8_path, const std::vector<IconSource>& sources,
                                 int pixels) {
    const auto key = icon_fingerprint(utf8_path, sources) + "#" + std::to_string(pixels);
    if (auto hit = cached(key)) return *hit;

    const auto name =
        system::to_hex(system::sha256({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}));
    const auto on_disk = directory_ / system::path_from_utf8(name + ".png");
    std::error_code ec;
    if (std::filesystem::exists(on_disk, ec)) {
        if (auto png = read_file(on_disk); !png.empty()) {
            auto url = std::string(kPrefix) + base64(png);
            remember(key, url);
            return url;
        }
    }

    const auto rendered = render_icon(utf8_path, sources, pixels);
    if (!rendered) {
        remember(key, {});
        return {};
    }
    std::ofstream(on_disk, std::ios::binary | std::ios::trunc)
        .write(rendered->png.data(), static_cast<std::streamsize>(rendered->png.size()));
    auto url = std::string(kPrefix) + base64(rendered->png);
    remember(key, url);
    return url;
}

void IconService::run(const std::stop_token& stop) {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stop, [this] { return !queue_.empty(); })) break;
            job = std::move(queue_.back());
            queue_.pop_back();
        }
        job.reply(produce(job.path, job.sources, job.pixels));
    }
    if (initialized) CoUninitialize();
}

}  // namespace houseguest::host
