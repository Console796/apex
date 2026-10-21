#include "document/vmf.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace apex::vmf {
namespace {

Error invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

template <typename T> Result<T> number(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    T result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(invalid("invalid or out-of-range number"));
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(result)) {
            return std::unexpected(invalid("non-finite number"));
        }
    }
    return result;
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> result;
    text = trim(text);
    while (!text.empty()) {
        const auto end = text.find_first_of(" \t\r\n");
        result.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        text = trim(text.substr(end));
    }
    return result;
}

Result<Vec3> vec3(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && ((text.front() == '(' && text.back() == ')') ||
                             (text.front() == '[' && text.back() == ']'))) {
        text = trim(text.substr(1, text.size() - 2));
    }
    const auto parts = words(text);
    if (parts.size() != 3) {
        return std::unexpected(invalid("expected three vector components"));
    }
    Vec3 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto value = number<double>(parts[i]);
        if (!value) {
            return std::unexpected(value.error());
        }
        result[i] = *value;
    }
    return result;
}

Result<std::vector<Vec3>> blockPoints(const Block& block, std::string_view key,
                                      bool hasIndex) {
    std::vector<Vec3> result;
    for (const auto& value : block.values) {
        if (!apex::ciEquals(value.key(), key)) {
            continue;
        }
        auto text = value.text();
        if (!text) {
            return std::unexpected(text.error());
        }
        auto components = trim(std::string_view(*text));
        if (hasIndex) {
            const auto split = components.find_first_of(" \t");
            if (split == std::string_view::npos) {
                return std::unexpected(invalid("expected point index and three components"));
            }
            components = trim(components.substr(split));
        }
        auto point = vec3(components);
        if (!point) {
            return std::unexpected(point.error());
        }
        result.push_back(*point);
    }
    return result;
}

std::string real(double value) {
    char buffer[64]{};
    const auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 6);
    if (error != std::errc{}) {
        return "0";
    }
    return {buffer, end};
}

std::string geometryReal(double value) {
    const double integer = std::round(value);
    if (std::abs(value - integer) <= 1e-9) value = integer;
    // Suppress near-zero transform residue, including the D18 example 1e-7.
    if (std::abs(value) <= 1e-7) value = 0.0;
    char buffer[512]{};
    const auto [end, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    if (error != std::errc{}) return "0";
    return {buffer, end};
}

bool finite(const Vec3& value) {
    return std::ranges::all_of(value, [](double component) { return std::isfinite(component); });
}

std::string vectorText(const Vec3& value, char open = '\0', char close = '\0',
                       bool geometry = false) {
    std::string result;
    if (open != '\0') {
        result += open;
    }
    const auto format = geometry ? geometryReal : real;
    result += format(value[0]) + " " + format(value[1]) + " " + format(value[2]);
    if (close != '\0') {
        result += close;
    }
    return result;
}

Block block(kv1::Node& node) {
    Block result;
    result.node = &node;
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            result.values.push_back(Value(&child));
        }
    }
    return result;
}

template <typename T> T typedBlock(kv1::Node& node) {
    T result;
    static_cast<Block&>(result) = block(node);
    return result;
}

OpaqueBlock opaque(kv1::Node& node) {
    return typedBlock<OpaqueBlock>(node);
}

Side side(kv1::Node& node) {
    auto result = typedBlock<Side>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "point_data")) {
            result.pointData = typedBlock<PointData>(child);
        } else if (apex::ciEquals(child.key(), "vertices_plus")) {
            result.verticesPlus = typedBlock<VerticesPlus>(child);
        } else if (apex::ciEquals(child.key(), "dispinfo")) {
            result.dispInfo = opaque(child);
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

Solid solid(kv1::Node& node) {
    auto result = typedBlock<Solid>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "side")) {
            result.sides.push_back(side(child));
        } else if (apex::ciEquals(child.key(), "editor") && !result.editor) {
            result.editor = opaque(child);
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

Group group(kv1::Node& node) {
    auto result = typedBlock<Group>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "solid")) {
            result.solids.push_back(solid(child));
        } else if (apex::ciEquals(child.key(), "group")) {
            result.groups.push_back(group(child));
        } else if (apex::ciEquals(child.key(), "editor") && !result.editor) {
            result.editor = opaque(child);
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

Entity entity(kv1::Node& node);

Hidden hidden(kv1::Node& node) {
    auto result = typedBlock<Hidden>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "solid")) {
            result.solids.push_back(solid(child));
        } else if (apex::ciEquals(child.key(), "group")) {
            result.groups.push_back(group(child));
        } else if (apex::ciEquals(child.key(), "hidden")) {
            result.hidden.push_back(hidden(child));
        } else if (apex::ciEquals(child.key(), "entity")) {
            result.entities.push_back(entity(child));
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

Entity entity(kv1::Node& node) {
    auto result = typedBlock<Entity>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "solid")) {
            result.solids.push_back(solid(child));
        } else if (apex::ciEquals(child.key(), "group")) {
            result.groups.push_back(group(child));
        } else if (apex::ciEquals(child.key(), "hidden")) {
            result.hidden.push_back(hidden(child));
        } else if (apex::ciEquals(child.key(), "connections") && !result.connections) {
            result.connections = opaque(child);
        } else if (apex::ciEquals(child.key(), "editor") && !result.editor) {
            result.editor = opaque(child);
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

World world(kv1::Node& node) {
    auto result = typedBlock<World>(node);
    for (auto& child : node.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(child.key(), "solid")) {
            result.solids.push_back(solid(child));
        } else if (apex::ciEquals(child.key(), "group")) {
            result.groups.push_back(group(child));
        } else if (apex::ciEquals(child.key(), "hidden")) {
            result.hidden.push_back(hidden(child));
        } else if (apex::ciEquals(child.key(), "editor") && !result.editor) {
            result.editor = opaque(child);
        } else {
            result.unknownBlocks.push_back(opaque(child));
        }
    }
    return result;
}

void addId(VmfFile& file, kv1::Node& node, bool isSide) {
    const auto* idNode = node.find("id");
    if (!idNode || idNode->kind() == kv1::NodeKind::Block) {
        return;
    }
    const auto id = idNode->asInt();
    const auto line = idNode->sourceSpan().line;
    auto& space = isSide ? file.sideIds : file.objectIds;
    if (!id) {
        file.problems.push_back({isSide ? ProblemCode::InvalidSideId
                                        : ProblemCode::InvalidObjectId,
                                 0, line, "invalid " + std::string(node.key()) + " id"});
        return;
    }
    const bool duplicate = space.contains(*id);
    space.record({*id, line, node.key()});
    if (duplicate) {
        file.problems.push_back({isSide ? ProblemCode::DuplicateSideId
                                        : ProblemCode::DuplicateObjectId,
                                 *id, line, "duplicate " + std::string(node.key()) + " id " +
                                                std::to_string(*id)});
    }
}

void scanIds(VmfFile& file, kv1::Node& parent) {
    for (auto& child : parent.children()) {
        if (child.kind() != kv1::NodeKind::Block) {
            continue;
        }
        const bool object = apex::ciEquals(child.key(), "solid") ||
                            apex::ciEquals(child.key(), "entity");
        const bool isSide = apex::ciEquals(child.key(), "side");
        if (object || isSide) {
            addId(file, child, isSide);
        }
        scanIds(file, child);
    }
}

} // namespace

std::string_view Value::key() const {
    return node_ ? node_->key() : std::string_view{};
}

std::string_view Value::raw() const {
    if (!node_) {
        return {};
    }
    return node_->rawValue().value_or(std::string_view{});
}

std::size_t Value::line() const {
    return node_ ? node_->sourceSpan().line : 0;
}

Result<std::string> Value::text() const {
    return node_ ? node_->decodedValue()
                 : Result<std::string>(std::unexpected(invalid("missing VMF value")));
}

Result<int> Value::asInt() const {
    return node_ ? node_->asInt() : Result<int>(std::unexpected(invalid("missing VMF value")));
}

Result<double> Value::asDouble() const {
    return node_ ? node_->asDouble()
                 : Result<double>(std::unexpected(invalid("missing VMF value")));
}

Result<Vec3> Value::asVec3() const {
    auto value = text();
    return value ? vec3(*value) : Result<Vec3>(std::unexpected(value.error()));
}

Result<PlanePoints> Value::asPlane() const {
    auto value = text();
    if (!value) {
        return std::unexpected(value.error());
    }
    std::string_view remaining = trim(*value);
    PlanePoints result;
    for (auto& point : result.points) {
        if (remaining.empty() || remaining.front() != '(') {
            return std::unexpected(invalid("expected three parenthesized plane points"));
        }
        const auto close = remaining.find(')');
        if (close == std::string_view::npos) {
            return std::unexpected(invalid("unterminated plane point"));
        }
        auto parsed = vec3(remaining.substr(0, close + 1));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        point = *parsed;
        remaining = trim(remaining.substr(close + 1));
    }
    if (!remaining.empty()) {
        return std::unexpected(invalid("extra text after plane"));
    }
    return result;
}

Result<UvAxis> Value::asUvAxis() const {
    auto value = text();
    if (!value) {
        return std::unexpected(value.error());
    }
    const std::string_view input = trim(*value);
    if (input.empty() || input.front() != '[') {
        return std::unexpected(invalid("expected [x y z shift] scale"));
    }
    const auto close = input.find(']');
    if (close == std::string_view::npos) {
        return std::unexpected(invalid("unterminated UV axis"));
    }
    const auto components = words(input.substr(1, close - 1));
    if (components.size() != 4) {
        return std::unexpected(invalid("expected four UV axis components"));
    }
    UvAxis result;
    for (std::size_t i = 0; i < 3; ++i) {
        auto component = number<double>(components[i]);
        if (!component) {
            return std::unexpected(component.error());
        }
        result.axis[i] = *component;
    }
    auto shift = number<double>(components[3]);
    auto scale = number<double>(trim(input.substr(close + 1)));
    if (!shift || !scale) {
        return std::unexpected(invalid("invalid UV shift or scale"));
    }
    result.shift = *shift;
    result.scale = *scale;
    return result;
}

Result<void> Value::set(std::string value) {
    return node_ ? node_->setValue(std::move(value))
                 : Result<void>(std::unexpected(invalid("missing VMF value")));
}

Result<void> Value::set(int value) {
    return set(std::to_string(value));
}

Result<void> Value::set(double value) {
    return set(real(value));
}

Result<void> Value::set(Vec3 value) {
    return set(vectorText(value));
}

Result<void> Value::set(PlanePoints value) {
    std::string result;
    for (std::size_t i = 0; i < value.points.size(); ++i) {
        if (!finite(value.points[i])) return std::unexpected(invalid("non-finite geometry"));
        if (i != 0) {
            result += ' ';
        }
        result += vectorText(value.points[i], '(', ')', true);
    }
    return set(std::move(result));
}

Result<void> Value::set(UvAxis value) {
    return set("[" + vectorText(value.axis) + " " + real(value.shift) + "] " +
               real(value.scale));
}

Result<void> Value::setGeometry(Vec3 value) {
    if (!finite(value)) return std::unexpected(invalid("non-finite geometry"));
    return set(vectorText(value, '\0', '\0', true));
}

Result<void> Value::setGeometry(UvAxis value) {
    if (!finite(value.axis) || !std::isfinite(value.shift) || !std::isfinite(value.scale))
        return std::unexpected(invalid("non-finite geometry"));
    return set("[" + vectorText(value.axis, '\0', '\0', true) + " " + real(value.shift) + "] " +
               real(value.scale));
}

std::string_view Block::name() const {
    return node ? node->key() : std::string_view{};
}

std::size_t Block::line() const {
    return node ? node->sourceSpan().line : 0;
}

Value* Block::find(std::string_view key) {
    const auto found = std::find_if(values.begin(), values.end(), [&](const Value& value) {
        return apex::ciEquals(value.key(), key);
    });
    return found == values.end() ? nullptr : &*found;
}

const Value* Block::find(std::string_view key) const {
    const auto found = std::find_if(values.begin(), values.end(), [&](const Value& value) {
        return apex::ciEquals(value.key(), key);
    });
    return found == values.end() ? nullptr : &*found;
}

std::vector<Value*> Block::findAll(std::string_view key) {
    std::vector<Value*> result;
    for (auto& value : values) {
        if (apex::ciEquals(value.key(), key)) {
            result.push_back(&value);
        }
    }
    return result;
}

std::vector<const Value*> Block::findAll(std::string_view key) const {
    std::vector<const Value*> result;
    for (const auto& value : values) {
        if (apex::ciEquals(value.key(), key)) {
            result.push_back(&value);
        }
    }
    return result;
}

Result<std::vector<Vec3>> PointData::points() const {
    return blockPoints(*this, "point", true);
}

Result<std::vector<Vec3>> VerticesPlus::points() const {
    auto result = blockPoints(*this, "v", false);
    if (result && result->empty()) {
        result = blockPoints(*this, "point", false);
    }
    return result;
}

std::size_t IdSpace::count(int id) const {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
                                                  [id](const IdOccurrence& entry) {
                                                      return entry.id == id;
                                                  }));
}

std::optional<int> IdSpace::maximum() const {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return std::max_element(entries_.begin(), entries_.end(),
                            [](const IdOccurrence& left, const IdOccurrence& right) {
                                return left.id < right.id;
                            })
        ->id;
}

Result<VmfFile> parse(std::string_view bytes, ParseOptions options) {
    auto document = kv1::parse(std::string(bytes), options.escapeMode);
    if (!document) {
        return std::unexpected(document.error().withContext("VMF"));
    }
    VmfFile file(std::move(*document));
    for (auto& node : file.document_.root().children()) {
        if (node.kind() != kv1::NodeKind::Block) {
            continue;
        }
        if (apex::ciEquals(node.key(), "versioninfo") && !file.versionInfo) {
            file.versionInfo = typedBlock<VersionInfo>(node);
        } else if (apex::ciEquals(node.key(), "visgroups") && !file.visgroups) {
            auto result = typedBlock<Visgroups>(node);
            for (auto& child : node.children()) {
                if (child.kind() != kv1::NodeKind::Block) {
                    continue;
                }
                if (apex::ciEquals(child.key(), "visgroup")) {
                    const auto build = [&](const auto& self, kv1::Node& source) -> Visgroup {
                        auto value = typedBlock<Visgroup>(source);
                        for (auto& nested : source.children()) {
                            if (nested.kind() != kv1::NodeKind::Block) {
                                continue;
                            }
                            if (apex::ciEquals(nested.key(), "visgroup")) {
                                value.children.push_back(self(self, nested));
                            } else {
                                value.unknownBlocks.push_back(opaque(nested));
                            }
                        }
                        return value;
                    };
                    result.children.push_back(build(build, child));
                } else {
                    result.unknownBlocks.push_back(opaque(child));
                }
            }
            file.visgroups = std::move(result);
        } else if (apex::ciEquals(node.key(), "viewsettings") && !file.viewSettings) {
            auto result = typedBlock<ViewSettings>(node);
            for (auto& child : node.children()) {
                if (child.kind() != kv1::NodeKind::Block) {
                    continue;
                }
                if (apex::ciEquals(child.key(), "views") && !result.views) {
                    auto views = typedBlock<Views>(child);
                    for (auto& view : child.children()) {
                        if (view.kind() == kv1::NodeKind::Block && view.key().size() >= 2 &&
                            (view.key().front() == 'v' || view.key().front() == 'V')) {
                            views.children.push_back(typedBlock<View>(view));
                        } else if (view.kind() == kv1::NodeKind::Block) {
                            views.unknownBlocks.push_back(opaque(view));
                        }
                    }
                    result.views = std::move(views);
                } else {
                    result.unknownBlocks.push_back(opaque(child));
                }
            }
            file.viewSettings = std::move(result);
        } else if (apex::ciEquals(node.key(), "world") && !file.world) {
            file.world = world(node);
        } else if (apex::ciEquals(node.key(), "entity")) {
            file.entities.push_back(entity(node));
        } else if (apex::ciEquals(node.key(), "group")) {
            file.groups.push_back(group(node));
        } else if (apex::ciEquals(node.key(), "hidden")) {
            file.hidden.push_back(hidden(node));
        } else if (apex::ciEquals(node.key(), "cameras") && !file.cameras) {
            auto result = typedBlock<Cameras>(node);
            for (auto& child : node.children()) {
                if (child.kind() != kv1::NodeKind::Block) {
                    continue;
                }
                if (apex::ciEquals(child.key(), "camera")) {
                    result.cameras.push_back(typedBlock<Camera>(child));
                } else {
                    result.unknownBlocks.push_back(opaque(child));
                }
            }
            file.cameras = std::move(result);
        } else if (apex::ciEquals(node.key(), "cordons") && !file.cordons) {
            auto result = typedBlock<Cordons>(node);
            for (auto& child : node.children()) {
                if (child.kind() != kv1::NodeKind::Block) {
                    continue;
                }
                if (apex::ciEquals(child.key(), "cordon")) {
                    result.cordons.push_back(typedBlock<Cordon>(child));
                } else {
                    result.unknownBlocks.push_back(opaque(child));
                }
            }
            file.cordons = std::move(result);
        } else {
            file.unknownBlocks.push_back(opaque(node));
        }
    }
    scanIds(file, file.document_.root());
    return file;
}

std::string write(const VmfFile& file) {
    auto result = kv1::write(file.document_);
    return result ? std::move(*result) : std::string{};
}

} // namespace apex::vmf
