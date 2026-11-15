#include "icon_render.hpp"

#include <windows.h>

#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::host {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxImageBytes = 16u << 20;
constexpr int kMaxEdge = 1024;
constexpr int kMaxCanvas = 512;

struct Picture {
    int width = 0;
    int height = 0;
    int native = 0;
    std::vector<std::uint8_t> bgra;
};

enum class Tier { Image, Resource, Executable };

struct Candidate {
    fs::path file;
    int index = 0;
    std::string kind;
    Tier tier = Tier::Executable;
};

bool is_image_file(const fs::path& file) {
    auto extension = file.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".ico" || extension == L".png" || extension == L".jpg" || extension == L".jpeg" ||
           extension == L".bmp" || extension == L".gif";
}

bool readable_size(const fs::path& file, std::uintmax_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    const auto size = fs::file_size(file, ec);
    return !ec && size > 0 && size <= limit;
}

ComPtr<IWICImagingFactory> imaging() {
    ComPtr<IWICImagingFactory> factory;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

std::optional<Picture> from_source(IWICImagingFactory* factory, IWICBitmapSource* source, int native) {
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > kMaxEdge ||
        height > kMaxEdge)
        return std::nullopt;
    Picture picture;
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);
    picture.native = native;
    picture.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(picture.bgra.size()),
                                     picture.bgra.data())))
        return std::nullopt;
    return picture;
}

// Image files go through the imaging component, which understands .ico files holding several sizes (including
// PNG-compressed ones) and rejects damaged files instead of crashing. The frame closest to the wanted size is
// used: the smallest one that is large enough, else the largest.
std::optional<Picture> load_image(const fs::path& file, int pixels) {
    if (!readable_size(file, kMaxImageBytes)) return std::nullopt;
    const auto factory = imaging();
    if (!factory) return std::nullopt;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
        return std::nullopt;
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> best;
    int best_edge = 0;
    for (UINT i = 0; i < std::min<UINT>(frames, 32); ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0, height = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&width, &height)) || width == 0 ||
            height == 0 || width > kMaxEdge || height > kMaxEdge)
            continue;
        const int edge = static_cast<int>(std::max(width, height));
        const bool better = !best || (best_edge < pixels ? edge > best_edge : edge >= pixels && edge < best_edge);
        if (better) {
            best = frame;
            best_edge = edge;
        }
    }
    if (!best) return std::nullopt;
    return from_source(factory.Get(), best.Get(), best_edge);
}

struct GdiObject {
    HGDIOBJ handle = nullptr;
    ~GdiObject() {
        if (handle) DeleteObject(handle);
    }
};

std::optional<Picture> icon_pixels(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return std::nullopt;
    GdiObject color{info.hbmColor};
    GdiObject mask{info.hbmMask};
    if (!info.hbmColor) return std::nullopt;
    BITMAP bitmap{};
    if (!GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0 ||
        bitmap.bmWidth > kMaxEdge || bitmap.bmHeight > kMaxEdge)
        return std::nullopt;
    Picture picture;
    picture.width = bitmap.bmWidth;
    picture.height = bitmap.bmHeight;
    picture.bgra.resize(static_cast<std::size_t>(picture.width) * picture.height * 4);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(header.bmiHeader);
    header.bmiHeader.biWidth = picture.width;
    header.bmiHeader.biHeight = -picture.height;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return std::nullopt;
    const bool read = GetDIBits(dc, info.hbmColor, 0, picture.height, picture.bgra.data(), &header, DIB_RGB_COLORS) != 0;
    bool has_alpha = false;
    if (read) {
        for (std::size_t i = 3; i < picture.bgra.size(); i += 4) {
            if (picture.bgra[i] != 0) {
                has_alpha = true;
                break;
            }
        }
    }
    if (read && !has_alpha && info.hbmMask) {
        std::vector<std::uint8_t> cut(picture.bgra.size());
        if (GetDIBits(dc, info.hbmMask, 0, picture.height, cut.data(), &header, DIB_RGB_COLORS) != 0) {
            for (std::size_t i = 0; i < picture.bgra.size(); i += 4) picture.bgra[i + 3] = cut[i] == 0 ? 255 : 0;
            has_alpha = true;
        }
    }
    DeleteDC(dc);
    if (!read) return std::nullopt;
    if (!has_alpha) {
        for (std::size_t i = 3; i < picture.bgra.size(); i += 4) picture.bgra[i] = 255;
    }
    return picture;
}

// Reading resources out of damaged executables is done by system code that can fault on hostile input; the
// exception is contained here so one bad file cannot take the window down. No C++ objects live in these frames.
int icon_resource_count(const wchar_t* file) {
    __try {
        return static_cast<int>(ExtractIconExW(file, -1, nullptr, nullptr, 0));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

HICON extract_resource_icon(const wchar_t* file, int index, int edge) {
    HICON icon = nullptr;
    __try {
        UINT id = 0;
        if (PrivateExtractIconsW(file, index, edge, edge, &icon, &id, 1, LR_DEFAULTCOLOR) != 1) icon = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        icon = nullptr;
    }
    return icon;
}

std::optional<Picture> shell_picture(const fs::path& file, int edge) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return std::nullopt;
    HBITMAP bitmap = nullptr;
    if (FAILED(factory->GetImage({edge, edge}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap)) || !bitmap)
        return std::nullopt;
    GdiObject owner{bitmap};
    const auto wic = imaging();
    if (!wic) return std::nullopt;
    ComPtr<IWICBitmap> source;
    if (FAILED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source))) return std::nullopt;
    return from_source(wic.Get(), source.Get(), 0);
}

std::optional<Picture> load_resource(const fs::path& file, int index, int pixels) {
    if (!readable_size(file, std::uintmax_t{4} << 30)) return std::nullopt;
    if (icon_resource_count(file.c_str()) <= 0) return std::nullopt;
    const int edge = std::clamp(pixels, 48, 256);
    if (HICON icon = extract_resource_icon(file.c_str(), index, edge)) {
        auto picture = icon_pixels(icon);
        DestroyIcon(icon);
        if (picture) return picture;
    }
    if (index == 0) return shell_picture(file, edge);
    return std::nullopt;
}

std::optional<Picture> load(const Candidate& candidate, int pixels) {
    return candidate.tier == Tier::Image ? load_image(candidate.file, pixels)
                                          : load_resource(candidate.file, candidate.index, pixels);
}

std::vector<Candidate> candidates_of(const std::string& executable, const std::vector<IconSource>& sources) {
    std::vector<Candidate> list;
    const auto add = [&](const std::string& kind, const std::string& text, int index, Tier forced, bool use_forced) {
        if (text.empty() || text.size() > 1024) return;
        auto file = system::path_from_utf8(text);
        if (!file.is_absolute() || !system::is_local_disk_path(file)) return;
        const auto tier = use_forced ? forced : (is_image_file(file) ? Tier::Image : Tier::Resource);
        list.push_back({std::move(file), index, kind, tier});
    };
    for (const auto& source : sources) add(source.kind, source.path, source.index, Tier::Resource, false);
    add("executable", executable, 0, Tier::Executable, true);
    std::ranges::stable_sort(list, [](const Candidate& a, const Candidate& b) { return a.tier < b.tier; });
    return list;
}

std::string encode_png(const std::vector<std::uint8_t>& bgra, int edge) {
    const auto factory = imaging();
    if (!factory) return {};
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(edge, edge, GUID_WICPixelFormat32bppBGRA, edge * 4,
                                               static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()),
                                               &bitmap)))
        return {};
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(edge, edge)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global))) return {};
    const auto size = GlobalSize(global);
    const auto* data = static_cast<const char*>(GlobalLock(global));
    if (!data) return {};
    std::string png(data, size);
    GlobalUnlock(global);
    return png;
}

// Draws the picture centred on a transparent square, scaled to fit without changing its proportions.
std::string fit_square(const Picture& picture, int edge) {
    const double scale = std::min(static_cast<double>(edge) / picture.width, static_cast<double>(edge) / picture.height);
    const int width = std::clamp(static_cast<int>(picture.width * scale + 0.5), 1, edge);
    const int height = std::clamp(static_cast<int>(picture.height * scale + 0.5), 1, edge);
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(edge) * edge * 4, 0);
    const auto factory = imaging();
    if (!factory) return {};
    ComPtr<IWICBitmap> source;
    if (FAILED(factory->CreateBitmapFromMemory(picture.width, picture.height, GUID_WICPixelFormat32bppBGRA,
                                               picture.width * 4, static_cast<UINT>(picture.bgra.size()),
                                               const_cast<BYTE*>(picture.bgra.data()), &source)))
        return {};
    ComPtr<IWICBitmapSource> scaled = source;
    if (width != picture.width || height != picture.height) {
        ComPtr<IWICBitmapScaler> scaler;
        const auto mode = scale < 1.0 ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeHighQualityCubic;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(source.Get(), width, height, mode)))
            return {};
        scaled = scaler;
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(scaled->CopyPixels(nullptr, width * 4, static_cast<UINT>(row.size()), row.data()))) return {};
    const int left = (edge - width) / 2;
    const int top = (edge - height) / 2;
    for (int y = 0; y < height; ++y) {
        std::copy_n(row.data() + static_cast<std::size_t>(y) * width * 4, static_cast<std::size_t>(width) * 4,
                    canvas.data() + (static_cast<std::size_t>(top + y) * edge + left) * 4);
    }
    return encode_png(canvas, edge);
}

}  // namespace

std::optional<RenderedIcon> render_icon(const std::string& executable, const std::vector<IconSource>& sources,
                                        int pixels) {
    pixels = std::clamp(pixels, 16, kMaxCanvas);
    std::optional<Picture> best;
    std::string best_source;
    for (const auto& candidate : candidates_of(executable, sources)) {
        auto picture = load(candidate, pixels);
        if (!picture) continue;
        const bool sufficient = picture->native == 0 || picture->native >= pixels;
        const bool larger = !best || (best->native != 0 && picture->native > best->native);
        if (sufficient) {
            best = std::move(picture);
            best_source = candidate.kind;
            break;
        }
        if (larger) {
            best = std::move(picture);
            best_source = candidate.kind;
        }
    }
    if (!best) return std::nullopt;
    auto png = fit_square(*best, pixels);
    if (png.empty()) return std::nullopt;
    return RenderedIcon{std::move(png), std::move(best_source), best->native};
}

std::string icon_fingerprint(const std::string& executable, const std::vector<IconSource>& sources) {
    std::string text;
    const auto note = [&](const std::string& path, int index) {
        text += path;
        text += '|' + std::to_string(index) + '|';
        if (const auto stamp = system::file_stamp(system::path_from_utf8(path))) {
            text += std::to_string(stamp->size) + ':' + std::to_string(stamp->written) + ':' + std::to_string(stamp->changed);
        } else {
            text += "missing";
        }
        text += ';';
    };
    note(executable, 0);
    for (const auto& source : sources) note(source.path, source.index);
    return text;
}

}  // namespace houseguest::host
