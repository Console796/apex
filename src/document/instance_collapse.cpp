#include "document/instance_collapse.hpp"

#include "core/strings.hpp"
#include "document/connection_codec.hpp"
#include "document/entity_transform.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace apex::document {
namespace {
using Kv = kv1::Node;
std::string text(const Kv& block, std::string_view key) {
    const auto* value = block.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
bool block(const Kv& node, std::string_view key) { return node.kind() == kv1::NodeKind::Block && ciEquals(node.key(), key); }
Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }
std::string number(double value) {
    auto node = Kv::value("", "");
    (void)vmf::Value(&node).set(value);
    return node.decodedValue().value_or("");
}
std::string vector(DVec3 value) {
    auto node = Kv::value("", "");
    (void)vmf::Value(&node).setGeometry(vmf::Vec3{value.x, value.y, value.z});
    return node.decodedValue().value_or("");
}
DVec3 vec(std::string value) {
    const auto v = Kv::value("", std::move(value)).asVec3().value_or(kv1::Vec3{});
    return {v[0], v[1], v[2]};
}
std::pair<std::string, std::string> instanceCommand(std::string_view value) {
    const auto semi = value.find(';');
    if (value.size() >= 9 && ciEquals(value.substr(0, 9), "instance:") && semi != std::string_view::npos)
        return {toLowerAscii(value.substr(9, semi - 9)), toLowerAscii(value.substr(semi + 1))};
    return {};
}
Connection combine(const Connection& first, Connection second) {
    if (second.parameter.empty()) second.parameter = first.parameter;
    const auto delay = [](const std::string& s) { return Kv::value("", s).asDouble().value_or(0.0); };
    second.delay = number(delay(first.delay) + delay(second.delay));
    const int a = Kv::value("", first.times).asInt().value_or(-1);
    const int b = Kv::value("", second.times).asInt().value_or(-1);
    second.times = std::to_string(a < 0 ? b : b < 0 ? a : std::min(a, b));
    if (first.separator == '\x1b') second.separator = '\x1b';
    return second;
}
void appendConnection(Kv& entity, std::string output, const Connection& connection) {
    if (!entity.find("connections")) (void)entity.append(Kv::block("connections"));
    (void)entity.find("connections")->append(Kv::value(std::move(output), serialize(connection)));
}
void transformSolid(Kv& solid, const EntityPose& pose) {
    for (auto& side : solid.children()) {
        if (!block(side, "side")) continue;
        if (auto* value = side.find("plane")) {
            if (auto points = vmf::Value(value).asPlane()) {
                for (auto& p : points->points) {
                    const auto v = pose.basis * DVec3{p[0], p[1], p[2]} + pose.origin;
                    p = {v.x, v.y, v.z};
                }
                (void)vmf::Value(value).set(*points);
            }
        }
        for (const auto* key : {"point_data", "vertices_plus"}) {
            if (auto* points = side.find(key)) {
                for (auto& point : points->children()) {
                    if (!ciEquals(point.key(), "point") && !ciEquals(key, "vertices_plus")) continue;
                    const auto raw = point.decodedValue().value_or("");
                    const auto space = raw.find(' ');
                    const bool indexed = ciEquals(key, "point_data");
                    if (indexed && space == std::string::npos) continue;
                    const auto value = vec(indexed ? raw.substr(space + 1) : raw);
                    (void)point.setValue((indexed ? raw.substr(0, space + 1) : "") + vector(pose.basis * value + pose.origin));
                }
            }
        }
        for (const auto* key : {"uaxis", "vaxis"}) {
            if (auto* value = side.find(key)) {
                if (auto axis = vmf::Value(value).asUvAxis()) {
                    const auto v = pose.basis * DVec3{axis->axis[0], axis->axis[1], axis->axis[2]};
                    axis->axis = {v.x, v.y, v.z};
                    if (axis->scale != 0.0) axis->shift -= glm::dot(v, pose.origin) / axis->scale;
                    (void)vmf::Value(value).setGeometry(*axis);
                }
            }
        }
        // Displacements carry world-space start positions and direction vectors.
        if (auto* disp = side.find("dispinfo")) {
            if (auto* start = disp->find("startposition")) {
                auto s = start->decodedValue().value_or("");
                if (s.size() >= 2) s = s.substr(1, s.size() - 2);
                (void)start->setValue("[" + vector(pose.basis * vec(s) + pose.origin) + "]");
            }
            for (auto& field : disp->children()) {
                if (!block(field, "normals") && !block(field, "offsets") && !block(field, "offset_normals")) continue;
                for (auto& row : field.children()) {
                    const auto raw = row.decodedValue().value_or("");
                    const auto words = split(raw, ' ');
                    std::string out;
                    for (std::size_t i = 0; i + 2 < words.size(); i += 3) {
                        if (!out.empty()) out += ' ';
                        out += vector(pose.basis * vec(std::string(words[i]) + ' ' + std::string(words[i+1]) + ' ' + std::string(words[i+2])));
                    }
                    (void)row.setValue(out);
                }
            }
        }
    }
}
void transformEntity(Kv& entity, const EntityPose& pose, const InstanceCollapseOptions& options) {
    const auto classname = text(entity, "classname");
    auto local = entityPose(entity);
    if (entity.find("pitch")) local.anglesDeg.x = vec(text(entity, "pitch") + " 0 0").x;
    if (entity.find("yaw")) local.anglesDeg.y = vec("0 " + text(entity, "yaw") + " 0").y;
    const auto typeOf = [&](std::string_view key) {
        return options.keyTypes ? options.keyTypes(classname, key) : defaultInstanceKeyType(classname, key);
    };
    if (typeOf("pitch") == InstanceKeyType::NegativePitch) local.anglesDeg.x = -local.anglesDeg.x;
    const auto axes = angleVectors(local.anglesDeg);
    const auto angles = anglesFromForward(pose.basis * axes.forward, pose.basis * axes.up);
    for (auto& child : entity.children()) {
        if (block(child, "solid")) { transformSolid(child, pose); continue; }
        if (child.kind() != kv1::NodeKind::Value) continue;
        const auto value = child.decodedValue().value_or("");
        switch (typeOf(child.key())) {
        case InstanceKeyType::Position: (void)child.setValue(vector(pose.basis * vec(value) + pose.origin)); break;
        case InstanceKeyType::Direction: (void)child.setValue(vector(pose.basis * vec(value))); break;
        case InstanceKeyType::Angles: {
            const auto own = angleVectors(vec(value));
            (void)child.setValue(vector(ciEquals(child.key(), "angles") ? angles :
                anglesFromForward(pose.basis * own.forward, pose.basis * own.up)));
            break;
        }
        case InstanceKeyType::Pitch: (void)child.setValue(number(angles.x)); break;
        case InstanceKeyType::NegativePitch: (void)child.setValue(number(-angles.x)); break;
        case InstanceKeyType::Yaw: (void)child.setValue(number(angles.y)); break;
        case InstanceKeyType::Axis: {
            const auto comma = value.find(',');
            if (comma != std::string::npos)
                (void)child.setValue(vector(pose.basis * vec(value.substr(0, comma)) + pose.origin) + ", " +
                                     vector(pose.basis * vec(value.substr(comma + 1)) + pose.origin));
            break;
        }
        default: break;
        }
    }
}
struct Content { std::vector<Kv> objects; };
void collect(const Kv& node, Content& content) {
    for (const auto& child : node.children()) {
        if (block(child, "entity") || block(child, "solid")) {
            const auto* editor = child.find("editor");
            if (!editor || text(*editor, "visgroupshown") != "0") content.objects.push_back(child);
        } else if (block(child, "group") || block(child, "visgroups")) content.objects.push_back(child);
        else if (block(child, "world")) collect(child, content);
    }
}
struct Collapse {
    const InstanceTemplateLookup& lookup;
    const InstanceCollapseOptions& options;
    unsigned autoName;
    int membershipId = 1;
    int objectId = 1;
    int sideId = 1;
    std::vector<const Document*> ancestors;

    Result<Content> expand(Kv instance, const Document& templ, std::vector<Kv>& siblings) {
        if (ancestors.size() >= 8) return std::unexpected(invalid("instance depth limit (8) exceeded"));
        if (std::ranges::find(ancestors, &templ) != ancestors.end()) return std::unexpected(invalid("instance template cycle: " + text(instance, "file")));
        ancestors.push_back(&templ);
        auto fixup = InstanceFixup::parse(instance);
        if (fixup.name.empty()) fixup.name = "InstanceAuto" + std::to_string(autoName++);
        const auto pose = entityPose(instance);
        auto root = Kv::block("");
        for (auto id : templ.roots()) (void)root.append(templ.exportSubtree(id));
        Content content;
        collect(root, content);

        // Remap fragment-local memberships so two nested templates can reuse their IDs.
        std::map<std::string, int> groups, visgroups;
        const auto remap = [&](auto&& self, Kv& node) -> void {
            for (auto& child : node.children()) {
                if (child.kind() == kv1::NodeKind::Block) self(self, child);
                else if ((ciEquals(node.key(), "group") && ciEquals(child.key(), "id")) || ciEquals(child.key(), "groupid") || ciEquals(child.key(), "visgroupid")) {
                    auto& map = ciEquals(child.key(), "visgroupid") ? visgroups : groups;
                    const auto key = child.decodedValue().value_or("");
                    if (!map.contains(key)) map[key] = membershipId++;
                    (void)child.setValue(std::to_string(map[key]));
                }
            }
        };
        for (auto& obj : content.objects) remap(remap, obj);
        std::map<int, int> sides;
        const auto remapIds = [&](auto&& self, Kv& node) -> void {
            if (block(node, "entity") || block(node, "solid") || block(node, "side")) {
                if (auto* id = node.find("id")) {
                    const int old = id->asInt().value_or(0);
                    const int fresh = block(node, "side") ? sideId++ : objectId++;
                    if (block(node, "side")) sides[old] = fresh;
                    (void)id->setValue(std::to_string(fresh));
                }
            }
            for (auto& child : node.children()) if (child.kind() == kv1::NodeKind::Block) self(self, child);
        };
        for (auto& obj : content.objects) remapIds(remapIds, obj);

        using Command = std::pair<std::string, std::string>;
        std::map<Command, std::vector<Connection>> inputs;
        struct ProxyOutput { std::size_t entity; std::string event; Connection connection; };
        std::map<Command, std::vector<ProxyOutput>> outputs;
        std::set<std::string> proxies;
        for (const auto& obj : content.objects)
            if (ciEquals(text(obj, "classname"), "func_instance_io_proxy")) proxies.insert(toLowerAscii(text(obj, "targetname")));
        for (std::size_t i = 0; i < content.objects.size(); ++i) {
            auto& obj = content.objects[i];
            const bool proxy = ciEquals(text(obj, "classname"), "func_instance_io_proxy");
            if (auto* rows = obj.find("connections")) {
                std::vector<std::size_t> removed;
                for (std::size_t r = 0; r < rows->children().size(); ++r) {
                    const auto& row = rows->children()[r];
                    auto connection = parseConnection(row.decodedValue().value_or(""));
                    if (proxy && ciEquals(row.key(), "OnProxyRelay")) inputs[{toLowerAscii(connection.target), toLowerAscii(connection.input)}].push_back(connection);
                    else if (ciEquals(connection.input, "ProxyRelay") && proxies.contains(toLowerAscii(connection.target))) {
                        outputs[{toLowerAscii(text(obj, "targetname")), toLowerAscii(row.key())}].push_back({i, std::string(row.key()), connection});
                        removed.push_back(r);
                    }
                }
                for (auto r = removed.rbegin(); r != removed.rend(); ++r) (void)rows->remove(*r);
            }
        }
        for (auto& obj : content.objects) {
            if (!block(obj, "entity") && !block(obj, "solid")) continue;
            auto types = [&](std::string_view cls, std::string_view key) {
                return options.keyTypes ? options.keyTypes(cls, key) : defaultInstanceKeyType(cls, key);
            };
            obj = fixedInstanceKeys(obj, fixup, types, options.isClassname);
            for (auto& value : obj.children()) {
                if (value.kind() != kv1::NodeKind::Value || types(text(obj, "classname"), value.key()) != InstanceKeyType::SideList) continue;
                const auto raw = value.decodedValue().value_or("");
                std::string mapped;
                for (auto word : split(raw, ' ')) {
                    const int old = Kv::value("", std::string(word)).asInt().value_or(0);
                    if (!sides.contains(old)) continue;
                    if (!mapped.empty()) mapped += ' ';
                    mapped += std::to_string(sides[old]);
                }
                (void)value.setValue(mapped);
            }
            if (block(obj, "entity")) transformEntity(obj, pose, options);
            else transformSolid(obj, pose);
        }
        for (auto& sibling : siblings) {
            auto* rows = sibling.find("connections");
            if (!rows) continue;
            std::vector<Kv> rewritten;
            bool changed = false;
            for (const auto& row : rows->children()) {
                const auto connection = parseConnection(row.decodedValue().value_or(""));
                const auto command = instanceCommand(connection.input);
                auto found = inputs.find(command);
                if (!ciEquals(connection.target, text(instance, "targetname")) || command.first.empty() || found == inputs.end()) {
                    rewritten.push_back(row); continue;
                }
                changed = true;
                for (auto direct : found->second) {
                    direct.target = fixup.fixName(fixup.substitute(direct.target));
                    direct.parameter = fixup.substitute(direct.parameter);
                    rewritten.push_back(Kv::value(std::string(row.key()), serialize(combine(connection, direct))));
                }
            }
            if (!changed) continue;
            auto replacement = Kv::block("connections");
            for (auto& row : rewritten) (void)replacement.append(std::move(row));
            *rows = std::move(replacement);
        }
        if (const auto* rows = instance.find("connections")) {
            for (const auto& row : rows->children()) {
                const auto found = outputs.find(instanceCommand(row.key()));
                if (found == outputs.end()) continue;
                for (const auto& [idx, event, first] : found->second) {
                    appendConnection(content.objects[idx], event, combine(first, parseConnection(row.decodedValue().value_or(""))));
                }
            }
        }
        for (std::size_t i = 0; i < content.objects.size();) {
            auto obj = content.objects[i];
            const auto cls = text(obj, "classname");
            if (ciEquals(cls, "func_instance_io_proxy") || ciEquals(cls, "func_instance_parms")) {
                content.objects.erase(content.objects.begin() + static_cast<std::ptrdiff_t>(i)); continue;
            }
            if (!ciEquals(cls, "func_instance")) { ++i; continue; }
            if (!lookup) return std::unexpected(invalid("nested instance requires a template lookup"));
            auto nested = lookup(templ, text(obj, "file"));
            if (!nested) return std::unexpected(nested.error());
            if (!*nested) return std::unexpected(invalid("null instance template"));
            const auto objectCount = content.objects.size();
            auto nestedSiblings = content.objects;
            nestedSiblings.insert(nestedSiblings.end(), siblings.begin(), siblings.end());
            auto expanded = expand(obj, **nested, nestedSiblings);
            if (!expanded) return std::unexpected(expanded.error());
            std::copy_n(nestedSiblings.begin(), objectCount, content.objects.begin());
            std::copy(nestedSiblings.begin() + static_cast<std::ptrdiff_t>(objectCount), nestedSiblings.end(), siblings.begin());
            if (const auto* membership = obj.find("editor")) {
                for (auto& created : expanded->objects) {
                    if (!block(created, "entity") && !block(created, "solid") && !block(created, "group")) continue;
                    if (!created.find("editor")) (void)created.append(Kv::block("editor"));
                    auto* editor = created.find("editor");
                    for (const auto& value : membership->children()) {
                        if (ciEquals(value.key(), "visgroupid") || (ciEquals(value.key(), "groupid") && !editor->find("groupid")))
                            (void)editor->append(value);
                    }
                }
            }
            content.objects.erase(content.objects.begin() + static_cast<std::ptrdiff_t>(i));
            content.objects.insert(content.objects.end(), expanded->objects.begin(), expanded->objects.end());
        }
        ancestors.pop_back();
        return content;
    }
};
}

Result<InstanceCollapseResult> collapseInstance(const Document& parent, NodeId instanceId,
                                                const Document& instanceTemplate,
                                                const InstanceTemplateLookup& lookup,
                                                const InstanceCollapseOptions& options) {
    const auto handle = parent.handle(instanceId);
    if (!handle || !std::holds_alternative<Handle<Entity>>(*handle)) return std::unexpected(invalid("instance is not an entity"));
    auto instance = parent.exportSubtree(instanceId);
    if (!ciEquals(text(instance, "classname"), "func_instance")) return std::unexpected(invalid("entity is not func_instance"));
    std::vector<Kv> siblings;
    std::vector<NodeId> ids;
    const auto walk = [&](auto&& self, NodeId id) -> void {
        auto h = parent.handle(id);
        if (!h) return;
        if (std::holds_alternative<Handle<Entity>>(*h) && id != instanceId) { siblings.push_back(parent.exportSubtree(id)); ids.push_back(id); }
        for (auto child : parent.get(*h)->children) self(self, child);
    };
    for (auto root : parent.roots()) walk(walk, root);
    Collapse collapse{lookup, options, options.nextAutoName, 1, 1, 1, {}};
    auto content = collapse.expand(instance, instanceTemplate, siblings);
    if (!content) return std::unexpected(content.error());
    InstanceCollapseResult result;
    result.nextAutoName = collapse.autoName;
    result.objectCount = content->objects.size();
    if (const auto* editor = instance.find("editor")) {
        for (const auto& value : editor->children())
            if (ciEquals(value.key(), "groupid") || ciEquals(value.key(), "visgroupid")) (void)result.parentMembership.append(value);
    }
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        const auto* changed = siblings[i].find("connections");
        if (!changed) continue;
        auto original = parent.exportSubtree(ids[i]);
        auto a = kv1::Document{}, b = kv1::Document{};
        (void)a.root().append(*changed);
        if (const auto* old = original.find("connections")) (void)b.root().append(*old);
        if (kv1::write(a).value_or("") != kv1::write(b).value_or("")) result.connections.push_back({ids[i], *changed});
    }
    kv1::Document fragment;
    for (auto& obj : content->objects) (void)fragment.root().append(std::move(obj));
    auto bytes = kv1::write(fragment);
    if (!bytes) return std::unexpected(bytes.error());
    result.fragment = std::move(*bytes);
    return result;
}

} // namespace apex::document
