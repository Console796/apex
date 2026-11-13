#include "document/displacement.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace apex::document {
namespace {

using geometry::Displacement;
using Rows = std::vector<std::vector<double>>;

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, "dispinfo: " + std::move(message)}; }

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const auto start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
        if (i > start) result.push_back(text.substr(start, i - start));
    }
    return result;
}

Result<double> real(std::string_view word) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (error != std::errc{} || end != word.data() + word.size() || !std::isfinite(value))
        return std::unexpected(invalid("bad number '" + std::string(word) + "'"));
    return value;
}

// Same rule as VMF geometry numbers: near-integers and near-zero transform residue are snapped.
std::string text(double value) {
    const double integer = std::round(value);
    if (std::abs(value - integer) <= 1e-9) value = integer;
    if (std::abs(value) <= 1e-7) value = 0.0;
    char buffer[512]{};
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    if (error != std::errc{}) return "0";
    return {buffer, end};
}

std::string rowText(const std::vector<double>& values) {
    std::string result;
    for (const auto value : values) {
        if (!result.empty()) result += ' ';
        result += text(value);
    }
    return result;
}

struct RowBlock {
    const char* name;
    int width;    // Numbers per vertex (or per triangle).
    bool triangles;
};
constexpr RowBlock kBlocks[] = {{"normals", 3, false},   {"distances", 1, false}, {"offsets", 3, false},
                                {"offset_normals", 3, false}, {"alphas", 1, false}, {"triangle_tags", 1, true}};

int rowCount(const Displacement& d, const RowBlock& b) { return b.triangles ? d.size() - 1 : d.size(); }
std::size_t rowLength(const Displacement& d, const RowBlock& b) {
    return static_cast<std::size_t>(b.triangles ? 2 * (d.size() - 1) : d.size() * b.width);
}

// Flatten one block of a value into its rows of numbers.
Rows rows(const Displacement& d, const RowBlock& b) {
    std::vector<double> flat;
    const auto vectors = [&](const std::vector<DVec3>& values) {
        for (const auto& v : values) flat.insert(flat.end(), {v.x, v.y, v.z});
    };
    const std::string_view name = b.name;
    if (name == "normals") vectors(d.normals);
    else if (name == "offsets") vectors(d.offsets);
    else if (name == "offset_normals") vectors(d.offsetNormals);
    else if (name == "distances") flat = d.distances;
    else if (name == "alphas") flat = d.alphas;
    else for (const auto tag : d.triangleTags) flat.push_back(tag);
    Rows result;
    const auto length = rowLength(d, b);
    for (std::size_t at = 0; at + length <= flat.size() && length > 0; at += length)
        result.emplace_back(flat.begin() + static_cast<std::ptrdiff_t>(at), flat.begin() + static_cast<std::ptrdiff_t>(at + length));
    return result;
}

void assign(Displacement& d, const RowBlock& b, const std::vector<double>& flat) {
    const auto vectors = [&](std::vector<DVec3>& out) {
        out.clear();
        for (std::size_t i = 0; i + 2 < flat.size(); i += 3) out.push_back({flat[i], flat[i + 1], flat[i + 2]});
    };
    const std::string_view name = b.name;
    if (name == "normals") vectors(d.normals);
    else if (name == "offsets") vectors(d.offsets);
    else if (name == "offset_normals") vectors(d.offsetNormals);
    else if (name == "distances") d.distances = flat;
    else if (name == "alphas") d.alphas = flat;
    else {
        d.triangleTags.clear();
        for (const auto value : flat) d.triangleTags.push_back(static_cast<int>(value));
    }
}

// allowed_verts is one key naming its word count: Hammer writes "10" signed 32-bit words,
// Strata Hammer "5" unsigned 64-bit words. Both hold one bit per vertex index.
int wordBits(std::size_t count) { return count >= 10 ? 32 : 64; }

Result<std::vector<bool>> allowedBits(const kv1::Node& block) {
    std::vector<bool> bits;
    for (const auto& entry : block.children()) {
        if (entry.kind() != kv1::NodeKind::Value) continue;
        const auto value = entry.decodedValue();
        if (!value) return std::unexpected(value.error());
        const auto list = words(*value);
        const int width = wordBits(list.size());
        for (const auto word : list) {
            std::uint64_t bitsOfWord = 0;
            if (!word.empty() && word.front() == '-') {
                std::int64_t signedValue = 0;
                const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), signedValue);
                if (error != std::errc{} || end != word.data() + word.size())
                    return std::unexpected(invalid("bad allowed_verts word"));
                bitsOfWord = static_cast<std::uint64_t>(signedValue);
            } else {
                const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), bitsOfWord);
                if (error != std::errc{} || end != word.data() + word.size())
                    return std::unexpected(invalid("bad allowed_verts word"));
            }
            for (int bit = 0; bit < width; ++bit) bits.push_back(((bitsOfWord >> bit) & 1U) != 0);
        }
        break;
    }
    return bits;
}

std::string allowedText(const std::vector<bool>& bits, std::size_t count) {
    const int width = wordBits(count);
    std::string result;
    for (std::size_t w = 0; w < count; ++w) {
        std::uint64_t word = 0;
        for (int bit = 0; bit < width; ++bit) {
            const auto index = w * static_cast<std::size_t>(width) + static_cast<std::size_t>(bit);
            if (index < bits.size() && bits[index]) word |= std::uint64_t{1} << bit;
        }
        if (!result.empty()) result += ' ';
        result += width == 32 ? std::to_string(static_cast<std::int32_t>(static_cast<std::uint32_t>(word)))
                              : std::to_string(word);
    }
    return result;
}

Result<double> scalar(const kv1::Node& block, std::string_view key, double fallback) {
    const auto* node = block.find(key);
    if (!node) return fallback;
    const auto value = node->decodedValue();
    if (!value) return std::unexpected(value.error());
    const auto list = words(*value);
    if (list.size() != 1) return std::unexpected(invalid(std::string(key) + " is not a number"));
    return real(list.front());
}

void setScalar(kv1::Node& block, std::string_view key, std::string value) {
    if (auto* node = block.find(key)) {
        (void)node->setValue(std::move(value));
        return;
    }
    // New scalars go before the first sub-block, where Hammer writes them.
    std::size_t at = 0;
    while (at < block.children().size() && block.children()[at].kind() == kv1::NodeKind::Value) ++at;
    block.insert(at, kv1::Node::value(std::string(key), std::move(value))).value();
}

std::string vectorText(DVec3 v, bool brackets) {
    const auto inner = text(v.x) + " " + text(v.y) + " " + text(v.z);
    return brackets ? "[" + inner + "]" : inner;
}

} // namespace

Result<Displacement> parseDisplacement(const kv1::Node& dispinfo) {
    Displacement d;
    if (!dispinfo.find("power")) return std::unexpected(invalid("missing power"));
    const auto power = scalar(dispinfo, "power", 0);
    if (!power) return std::unexpected(power.error());
    if (*power != std::floor(*power) || *power < Displacement::kMinPower || *power > Displacement::kMaxPower)
        return std::unexpected(invalid("power must be 2, 3 or 4"));
    d.power = static_cast<int>(*power);
    if (const auto* start = dispinfo.find("startposition")) {
        auto copy = *start;
        const auto value = vmf::Value(&copy).asVec3();
        if (!value) return std::unexpected(invalid("bad startposition"));
        d.startPosition = {(*value)[0], (*value)[1], (*value)[2]};
    }
    const auto flags = scalar(dispinfo, "flags", 0), elevation = scalar(dispinfo, "elevation", 0),
               subdiv = scalar(dispinfo, "subdiv", 0);
    if (!flags || !elevation || !subdiv) return std::unexpected(invalid("bad flags, elevation or subdiv"));
    d.flags = static_cast<int>(*flags);
    d.elevation = *elevation;
    d.subdiv = static_cast<int>(*subdiv);
    for (const auto& b : kBlocks) {
        const auto* block = dispinfo.find(b.name);
        const auto count = rowCount(d, b);
        const auto length = rowLength(d, b);
        std::vector<double> flat(static_cast<std::size_t>(count) * length, 0.0);
        if (block) {
            for (int r = 0; r < count; ++r) {
                const auto* row = block->find("row" + std::to_string(r));
                if (!row) continue;
                const auto value = row->decodedValue();
                if (!value) return std::unexpected(value.error());
                const auto list = words(*value);
                if (list.size() != length)
                    return std::unexpected(invalid(std::string(b.name) + " row" + std::to_string(r) + " has " +
                                                   std::to_string(list.size()) + " numbers, expected " +
                                                   std::to_string(length)));
                for (std::size_t i = 0; i < length; ++i) {
                    const auto number = real(list[i]);
                    if (!number) return std::unexpected(number.error());
                    flat[static_cast<std::size_t>(r) * length + i] = *number;
                }
            }
        }
        assign(d, b, flat);
    }
    if (const auto* allowed = dispinfo.find("allowed_verts")) {
        auto bits = allowedBits(*allowed);
        if (!bits) return std::unexpected(bits.error());
        d.allowedVerts = std::move(*bits);
    }
    if (const auto problem = d.problem()) return std::unexpected(invalid(*problem));
    return d;
}

Result<void> writeDisplacement(kv1::Node& dispinfo, const Displacement& value) {
    if (const auto problem = value.problem()) return std::unexpected(invalid(*problem));
    if (dispinfo.kind() != kv1::NodeKind::Block) return std::unexpected(invalid("not a block"));
    const auto parsed = parseDisplacement(dispinfo);
    const Displacement* old = parsed ? &*parsed : nullptr;
    auto block = dispinfo;
    const auto differs = [&](auto member) { return !old || old->*member != value.*member; };
    if (differs(&Displacement::power)) setScalar(block, "power", std::to_string(value.power));
    if (differs(&Displacement::startPosition)) setScalar(block, "startposition", vectorText(value.startPosition, true));
    if (differs(&Displacement::flags)) setScalar(block, "flags", std::to_string(value.flags));
    if (differs(&Displacement::elevation)) setScalar(block, "elevation", text(value.elevation));
    if (differs(&Displacement::subdiv)) setScalar(block, "subdiv", std::to_string(value.subdiv));
    for (const auto& b : kBlocks) {
        const auto next = rows(value, b);
        const auto before = old ? rows(*old, b) : Rows{};
        auto* rowsBlock = block.find(b.name);
        for (std::size_t r = 0; r < next.size(); ++r) {
            const auto key = "row" + std::to_string(r);
            if (r < before.size() && before[r] == next[r] && rowsBlock && rowsBlock->find(key)) continue;
            // A missing block or row read as zeros; it is only written once it differs.
            if (r < before.size() && before[r] == next[r] && std::ranges::all_of(next[r], [](double v) { return v == 0.0; }))
                continue;
            if (!rowsBlock) {
                block.append(kv1::Node::block(b.name)).value();
                rowsBlock = block.find(b.name);
            }
            if (auto* row = rowsBlock->find(key)) (void)row->setValue(rowText(next[r]));
            else rowsBlock->append(kv1::Node::value(key, rowText(next[r]))).value();
        }
        // Rows past a smaller power are dropped.
        if (!rowsBlock) continue;
        for (std::size_t i = rowsBlock->children().size(); i-- > 0;) {
            const auto key = rowsBlock->children()[i].key();
            if (key.size() < 4 || !ciEquals(key.substr(0, 3), "row")) continue;
            std::size_t index = 0;
            const auto digits = key.substr(3);
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (error == std::errc{} && end == digits.data() + digits.size() && index >= next.size())
                rowsBlock->remove(i).value();
        }
    }
    if (!value.allowedVerts.empty() && (!old || old->allowedVerts != value.allowedVerts)) {
        auto* allowed = block.find("allowed_verts");
        if (!allowed) {
            block.append(kv1::Node::block("allowed_verts")).value();
            allowed = block.find("allowed_verts");
        }
        kv1::Node* entry = nullptr;
        for (auto& child : allowed->children())
            if (child.kind() == kv1::NodeKind::Value) { entry = &child; break; }
        if (entry) {
            const auto count = words(entry->decodedValue().value_or("")).size();
            (void)entry->setValue(allowedText(value.allowedVerts, count));
        } else {
            allowed->append(kv1::Node::value("10", allowedText(value.allowedVerts, 10))).value();
        }
    }
    dispinfo = std::move(block);
    return {};
}

Result<std::optional<Displacement>> displacement(const Document& doc, SideId side) {
    const auto location = doc.side(side);
    if (!location) return std::optional<Displacement>{};
    const auto* solid = doc.get(location->solid);
    if (!solid) return std::optional<Displacement>{};
    std::size_t face = 0;
    for (const auto& child : solid->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (face++ != location->faceIndex) continue;
        const auto* dispinfo = child.find("dispinfo");
        if (!dispinfo) return std::optional<Displacement>{};
        auto parsed = parseDisplacement(*dispinfo);
        if (!parsed) return std::unexpected(parsed.error().withContext("side " + std::to_string(side)));
        return std::optional<Displacement>{std::move(*parsed)};
    }
    return std::optional<Displacement>{};
}

void Transaction::setDisplacement(SideId sideId, const geometry::Displacement& value) {
    if (!doc_) throw std::logic_error("transaction already finished");
    const auto location = doc_->side(sideId);
    if (!location) throw std::invalid_argument("unknown side ID");
    const auto id = checked(location->solid);
    auto block = *doc_->node(id)->block;
    std::size_t face = 0;
    for (auto& child : block.children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (face++ != location->faceIndex) continue;
        auto* dispinfo = child.find("dispinfo");
        if (!dispinfo) {
            child.append(kv1::Node::block("dispinfo")).value();
            dispinfo = child.find("dispinfo");
        }
        // An equal value is not an edit: no undo entry, no dirty flag.
        if (const auto current = parseDisplacement(*dispinfo); current && *current == value) return;
        if (const auto written = writeDisplacement(*dispinfo, value); !written)
            throw std::invalid_argument(written.error().what());
        touch(id, Dirty::Geometry);
        doc_->node(id)->block = std::make_shared<const kv1::Node>(std::move(block));
        key_ = {id, "side:" + std::to_string(location->faceIndex) + ":dispinfo"};
        ++operations_;
        return;
    }
    throw std::invalid_argument("side index out of range");
}

} // namespace apex::document
