#include "assets/hotspot.hpp"

#include "assets/vmt.hpp"
#include "core/kv1.hpp"
#include "core/strings.hpp"

#include <charconv>
#include <cstring>

namespace apex::assets {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + i])) << (8 * i);
    return value;
}
std::uint16_t u16(std::span<const std::byte> bytes, std::size_t at) {
    return std::uint16_t(std::to_integer<unsigned char>(bytes[at]) | (std::to_integer<unsigned char>(bytes[at + 1]) << 8));
}

// VTF 7.3's header: the resource count at 68, the 8-byte entries (3-byte tag, flags, offset) at 80.
constexpr std::size_t kResourceCount = 68, kResources = 80;
constexpr std::uint8_t kResourceNoData = 0x2;

std::optional<std::pair<int, int>> pair(const kv1::Node* node) {
    if (!node) return {};
    const auto text = node->decodedValue().value_or("");
    int a = 0, b = 0;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end && *p == ' ') ++p;
    auto r = std::from_chars(p, end, a);
    if (r.ec != std::errc{}) return {};
    p = r.ptr;
    while (p < end && *p == ' ') ++p;
    r = std::from_chars(p, end, b);
    if (r.ec != std::errc{}) return {};
    return std::pair{a, b};
}

} // namespace

Result<std::optional<std::vector<HotspotRegion>>> readVtfHotspots(std::span<const std::byte> bytes) {
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "VTF\0", 4) != 0) return std::unexpected(invalid("not a VTF"));
    if (u32(bytes, 4) != 7 || u32(bytes, 8) < 3) return std::optional<std::vector<HotspotRegion>>{}; // No resources.
    if (bytes.size() < kResources) return std::unexpected(invalid("truncated VTF header"));
    const auto count = u32(bytes, kResourceCount);
    if (count > 32) return std::unexpected(invalid("implausible VTF resource count"));
    if (bytes.size() < kResources + 8 * std::size_t(count)) return std::unexpected(invalid("truncated VTF resources"));
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = kResources + 8 * i;
        if (bytes[at] != std::byte{0x2b} || bytes[at + 1] != std::byte{0} || bytes[at + 2] != std::byte{0}) continue;
        if ((std::to_integer<std::uint8_t>(bytes[at + 3]) & kResourceNoData) != 0)
            return std::unexpected(invalid("hotspot resource has no data"));
        // A resource with data starts with its size (VTF 7.3).
        const std::size_t offset = u32(bytes, at + 4);
        if (offset + 4 > bytes.size()) return std::unexpected(invalid("hotspot resource past the end of the data read"));
        const std::size_t size = u32(bytes, offset);
        const auto body = offset + 4;
        if (size < 4 || body + size > bytes.size()) return std::unexpected(invalid("hotspot resource past the end of the data read"));
        if (std::to_integer<std::uint8_t>(bytes[body]) != 1) return std::unexpected(invalid("unknown hotspot resource version"));
        const std::size_t rects = u16(bytes, body + 2);
        if (4 + 9 * rects > size) return std::unexpected(invalid("hotspot resource shorter than its rect count"));
        std::vector<HotspotRegion> result;
        for (std::size_t r = 0; r < rects; ++r) {
            const auto p = body + 4 + 9 * r;
            result.push_back({u16(bytes, p + 1), u16(bytes, p + 3), u16(bytes, p + 5), u16(bytes, p + 7),
                              std::to_integer<std::uint8_t>(bytes[p])});
        }
        return std::optional{std::move(result)};
    }
    return std::optional<std::vector<HotspotRegion>>{};
}

Result<std::vector<HotspotRegion>> parseRectFile(std::string_view text) {
    auto document = kv1::parse(std::string(text), kv1::EscapeMode::Legacy);
    if (!document) return std::unexpected(document.error());
    const kv1::Node* set = nullptr;
    for (const auto& node : document->root().children())
        if (node.kind() == kv1::NodeKind::Block && ciEquals(node.key(), "Rectangles")) { set = &node; break; }
    if (!set) return std::unexpected(invalid("a .rect file needs a Rectangles block"));
    std::vector<HotspotRegion> result;
    for (const auto& rect : set->children()) {
        if (rect.kind() != kv1::NodeKind::Block || !ciEquals(rect.key(), "rectangle")) continue;
        const auto min = pair(rect.find("min")), max = pair(rect.find("max"));
        if (!min || !max) return std::unexpected(invalid("a rectangle needs min and max as two integers"));
        HotspotRegion region{min->first, min->second, max->first, max->second};
        const auto flag = [&](std::string_view key, std::uint8_t bit) {
            if (const auto* node = rect.find(key); node && node->asInt().value_or(0) != 0) region.flags |= bit;
        };
        flag("rotate", HotspotRegion::kRotate);
        flag("reflect", HotspotRegion::kReflect);
        flag("alt", HotspotRegion::kAlt);
        result.push_back(region);
    }
    return result;
}

Result<std::optional<HotspotSet>> loadHotspots(const Vfs& vfs, std::string_view material) {
    auto parsed = loadMaterial(vfs, material);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->baseTexture.empty()) {
        const auto path = texturePath(parsed->baseTexture);
        // The resource sits before the image data, so the header's first 64 KiB nearly always hold it.
        if (auto head = vfs.openPrefix(path, 64 * 1024)) {
            auto rects = readVtfHotspots(*head);
            if (!rects && head->size() == 64 * 1024)
                if (auto whole = vfs.open(path)) rects = readVtfHotspots(*whole);
            if (rects && *rects && !(*rects)->empty())
                return std::optional{HotspotSet{HotspotSource::VtfResource, path, std::move(**rects)}};
        }
    }
    for (const auto& name : {parsed->rectangleMap, std::string(material)}) {
        if (name.empty()) continue;
        const auto path = rectMapPath(name);
        const auto blob = vfs.open(path);
        if (!blob) continue;
        auto rects = parseRectFile(std::string_view(reinterpret_cast<const char*>(blob->data()), blob->size()));
        if (!rects) return std::unexpected(rects.error().withContext(path));
        if (!rects->empty()) return std::optional{HotspotSet{HotspotSource::RectFile, path, std::move(*rects)}};
    }
    return std::optional<HotspotSet>{};
}

} // namespace apex::assets
