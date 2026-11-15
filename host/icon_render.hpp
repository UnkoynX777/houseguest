#pragma once

#include <optional>
#include <string>
#include <vector>

namespace houseguest::host {

// A place a picture may be read from. The kind records who declared it ("registry", "shortcut", "package"),
// the index selects a resource inside an executable or library. Nothing is ever executed to read it.
struct IconSource {
    std::string kind;
    std::string path;
    int index = 0;
};

struct RenderedIcon {
    std::string png;
    std::string source;
    int native = 0;
};

// Chooses and draws the best picture for an application: an image file the installer declared, then an icon
// resource it declared, then the executable's own resources. The first source that is genuine and at least as
// large as requested wins; otherwise the largest genuine one is used. A file without a real icon never yields
// the system's generic program icon, so callers can show their own fallback. The result is a square PNG that
// keeps the proportions of the original and its transparency.
std::optional<RenderedIcon> render_icon(const std::string& executable, const std::vector<IconSource>& sources,
                                        int pixels);

// Text that changes whenever any file that could supply the picture changes.
std::string icon_fingerprint(const std::string& executable, const std::vector<IconSource>& sources);

}  // namespace houseguest::host
