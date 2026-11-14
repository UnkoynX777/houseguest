#include "icons.hpp"

#include <windows.h>

#include <shobjidl.h>
#include <wincodec.h>
#include <wrl.h>

#include <fstream>
#include <sstream>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::host {
namespace {

using Microsoft::WRL::ComPtr;

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

std::string png_of(HBITMAP bitmap) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return {};
    ComPtr<IWICBitmap> source;
    if (FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source))) return {};
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return {};
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return {};
    UINT width = 0, height = 0;
    source->GetSize(&width, &height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return {};
    }
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global))) return {};
    const auto size = GlobalSize(global);
    const auto* data = static_cast<const char*>(GlobalLock(global));
    if (!data) return {};
    std::string png(data, size);
    GlobalUnlock(global);
    return png;
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
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

IconService::~IconService() {
    worker_.request_stop();
    ready_.notify_all();
}

void IconService::request(const std::string& utf8_path, int pixels, Reply reply) {
    Reply dropped;
    {
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= kQueueLimit) {
            dropped = std::move(queue_.front().reply);
            queue_.pop_front();
        }
        queue_.push_back({utf8_path, pixels, std::move(reply)});
    }
    ready_.notify_one();
    if (dropped) dropped({});
}

std::string IconService::cached(const std::string& key) {
    std::scoped_lock lock(mutex_);
    const auto it = memory_.find(key);
    if (it == memory_.end()) return {};
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

std::string IconService::produce(const std::string& utf8_path, int pixels) {
    const auto file = system::path_from_utf8(utf8_path);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    const auto size = std::filesystem::file_size(file, ec);
    const auto key = utf8_path + "|" + std::to_string(pixels) + "|" + std::to_string(modified) + "|" +
                     std::to_string(size);
    if (auto hit = cached(key); !hit.empty()) return hit;

    const auto name =
        system::to_hex(system::sha256({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}));
    const auto on_disk = directory_ / system::path_from_utf8(name + ".png");
    if (std::filesystem::exists(on_disk, ec)) {
        if (auto png = read_file(on_disk); !png.empty()) {
            auto url = std::string(kPrefix) + base64(png);
            remember(key, url);
            return url;
        }
    }

    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return {};
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage({pixels, pixels}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) ||
        !bitmap)
        return {};
    const auto png = png_of(bitmap);
    DeleteObject(bitmap);
    if (png.empty()) return {};
    std::ofstream(on_disk, std::ios::binary | std::ios::trunc)
        .write(png.data(), static_cast<std::streamsize>(png.size()));
    auto url = std::string(kPrefix) + base64(png);
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
        job.reply(produce(job.path, job.pixels));
    }
    if (initialized) CoUninitialize();
}

}  // namespace houseguest::host
