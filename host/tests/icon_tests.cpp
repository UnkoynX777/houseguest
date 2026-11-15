#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../icon_render.hpp"

using namespace houseguest::host;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

struct Apartment {
    Apartment() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~Apartment() { CoUninitialize(); }
};

fs::path scratch(const wchar_t* name) {
    const auto folder = fs::temp_directory_path() / L"houseguest-icon-tests";
    fs::create_directories(folder);
    return folder / name;
}

// A flat 32-bit picture of the given colour (BGRA) encoded as a PNG.
std::string png_bytes(int width, int height, std::uint32_t colour) {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, colour);
    ComPtr<IWICBitmap> bitmap;
    factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                    static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()),
                                    &bitmap);
    ComPtr<IStream> stream;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(width, height);
    frame->SetPixelFormat(&format);
    frame->WriteSource(bitmap.Get(), nullptr);
    frame->Commit();
    encoder->Commit();
    HGLOBAL global = nullptr;
    GetHGlobalFromStream(stream.Get(), &global);
    const auto size = GlobalSize(global);
    const auto* data = static_cast<const char*>(GlobalLock(global));
    std::string bytes(data, size);
    GlobalUnlock(global);
    return bytes;
}

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream(file, std::ios::binary | std::ios::trunc)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// An .ico container holding one PNG-compressed image.
void write_ico(const fs::path& file, int edge, std::uint32_t colour) {
    const auto png = png_bytes(edge, edge, colour);
    std::string out(22, '\0');
    out[2] = 1;
    out[4] = 1;
    out[6] = static_cast<char>(edge >= 256 ? 0 : edge);
    out[7] = static_cast<char>(edge >= 256 ? 0 : edge);
    out[10] = 1;
    out[12] = 32;
    const auto length = static_cast<std::uint32_t>(png.size());
    std::memcpy(&out[14], &length, 4);
    out[18] = 22;
    write(file, out + png);
}

struct Decoded {
    UINT width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};

Decoded decode(const std::string& png) {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IStream> stream;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ULONG written = 0;
    stream->Write(png.data(), static_cast<ULONG>(png.size()), &written);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ComPtr<IWICBitmapDecoder> decoder;
    factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    ComPtr<IWICBitmapFrameDecode> frame;
    decoder->GetFrame(0, &frame);
    Decoded out;
    frame->GetSize(&out.width, &out.height);
    ComPtr<IWICFormatConverter> converter;
    factory->CreateFormatConverter(&converter);
    converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom);
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
    converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.pixels.size() * 4),
                          reinterpret_cast<BYTE*>(out.pixels.data()));
    return out;
}

std::string system_file(const wchar_t* name) {
    wchar_t folder[MAX_PATH] = {};
    GetSystemDirectoryW(folder, MAX_PATH);
    return (fs::path(folder) / name).string();
}

std::string without_icon() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return fs::path(self).string();
}

}  // namespace

TEST_CASE("a declared icon file is preferred to the executable and keeps its colour") {
    Apartment apartment;
    const auto ico = scratch(L"declared.ico");
    write_ico(ico, 256, 0xFF2040C0);
    const auto result = render_icon(system_file(L"cmd.exe"), {{"registry", ico.string(), 0}}, 64);
    REQUIRE(result);
    CHECK(result->source == "registry");
    const auto picture = decode(result->png);
    CHECK(picture.width == 64);
    CHECK(picture.height == 64);
    CHECK(picture.pixels[32 * 64 + 32] == 0xFF2040C0);
}

TEST_CASE("a picture that is not square is centred, never stretched") {
    Apartment apartment;
    const auto wide = scratch(L"wide.png");
    write(wide, png_bytes(200, 100, 0xFF00AA00));
    const auto result = render_icon("", {{"package", wide.string(), 0}}, 64);
    REQUIRE(result);
    const auto picture = decode(result->png);
    CHECK((picture.pixels[2 * 64 + 32] >> 24) == 0);
    CHECK(picture.pixels[32 * 64 + 32] == 0xFF00AA00);
    CHECK((picture.pixels[61 * 64 + 32] >> 24) == 0);
}

TEST_CASE("the executable's own resources are used when nothing else is declared") {
    Apartment apartment;
    const auto result = render_icon(system_file(L"cmd.exe"), {}, 96);
    REQUIRE(result);
    CHECK(result->source == "executable");
    CHECK(decode(result->png).width == 96);
}

TEST_CASE("a program without an icon yields nothing instead of the generic program icon") {
    Apartment apartment;
    CHECK_FALSE(render_icon(without_icon(), {}, 64));
}

TEST_CASE("damaged and hostile files are rejected without taking the process down") {
    Apartment apartment;
    const auto broken = scratch(L"broken.ico");
    write(broken, std::string("\0\0\1\0\5\0garbage that is not an icon directory", 42));
    const auto empty = scratch(L"empty.png");
    write(empty, "");
    const auto text = scratch(L"text.dll");
    write(text, "MZ this is not a portable executable");
    CHECK_FALSE(render_icon(without_icon(),
                            {{"registry", broken.string(), 0},
                             {"shortcut", empty.string(), 0},
                             {"registry", text.string(), 3},
                             {"registry", "relative\\path.ico", 0},
                             {"registry", "\\\\server\\share\\x.ico", 0}},
                            64));
}

TEST_CASE("a changed file changes the fingerprint and a missing one is distinguishable") {
    Apartment apartment;
    const auto ico = scratch(L"fingerprint.ico");
    write_ico(ico, 32, 0xFF112233);
    const auto before = icon_fingerprint("", {{"registry", ico.string(), 0}});
    write_ico(ico, 48, 0xFF112233);
    CHECK(icon_fingerprint("", {{"registry", ico.string(), 0}}) != before);
    CHECK(icon_fingerprint("", {{"registry", ico.string() + ".gone", 0}}).find("missing") != std::string::npos);
}

// Not part of the normal run. `houseguest_host_tests "[.survey]"` reads the library saved in HG_SURVEY (the output of
// `apps.library`), renders every entry the way the window does, and reports where each picture came from compared
// with asking the shell for the executable alone, which is what the window did before.
#include <shobjidl.h>

#include <iostream>

#include "houseguest/core/serialization.hpp"

namespace {
std::string environment(const char* name) {
    char buffer[1024] = {};
    const auto length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return length == 0 || length >= sizeof(buffer) ? std::string() : std::string(buffer, length);
}
}  // namespace

TEST_CASE("survey of the machine's library", "[.survey]") {
    Apartment apartment;
    const auto input = environment("HG_SURVEY");
    REQUIRE_FALSE(input.empty());
    std::ifstream in(input);
    const auto library = houseguest::core::Json::parse(in);
    const auto output = environment("HG_SURVEY_OUT");
    if (!output.empty()) fs::create_directories(output);
    int total = 0, before_ok = 0, after_ok = 0, declared = 0;
    for (const auto& app : library["apps"]) {
        const std::string path = app.value("path", std::string());
        if (path.empty()) continue;
        ++total;
        std::vector<IconSource> sources;
        for (const auto& icon : app.value("icons", houseguest::core::Json::array()))
            sources.push_back({icon.value("kind", std::string()), icon.value("path", std::string()), icon.value("index", 0)});
        ComPtr<IShellItemImageFactory> factory;
        bool before = false;
        if (SUCCEEDED(SHCreateItemFromParsingName(fs::path(path).c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
            HBITMAP bitmap = nullptr;
            before = SUCCEEDED(factory->GetImage({64, 64}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) && bitmap;
            if (bitmap) DeleteObject(bitmap);
        }
        const auto after = render_icon(path, sources, 64);
        before_ok += before;
        after_ok += after.has_value();
        declared += after && after->source != "executable";
        std::cout << (before ? "old:ok  " : "old:none") << " new:" << (after ? after->source : std::string("none")) << "  "
                  << app.value("name", std::string()) << "  [" << sources.size() << " declared]\n";
        if (!output.empty() && after) {
            std::string name = app.value("id", std::string("x")) + ".png";
            write(fs::path(output) / name, after->png);
        }
    }
    std::cout << "entries=" << total << " old_with_picture=" << before_ok << " new_with_picture=" << after_ok
              << " from_declared_sources=" << declared << "\n";
}
