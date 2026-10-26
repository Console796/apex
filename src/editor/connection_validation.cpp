#include "editor/connection_validation.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace apex::editor {
namespace {
bool matches(std::string_view pattern, std::string_view name) {
    auto p = toLowerAscii(pattern);
    const bool first = p.starts_with('*'), last = p.ends_with('*');
    if (first) p.erase(0, 1);
    if (last && !p.empty()) p.pop_back();
    if (p.find('*') != std::string::npos) return false;
    return first && last ? name.find(p) != std::string::npos : first ? name.ends_with(p) :
           last ? name.starts_with(p) : name == p;
}
const assets::EntityIO* input(const assets::EntityClass* cls, std::string_view name) {
    if (cls) for (const auto& io : cls->inputs) if (ciEquals(io.name, name)) return &io;
    return nullptr;
}
template<class T> bool number(std::string_view text, T& value) {
    if (text.starts_with('+')) {
        text.remove_prefix(1);
        if (text.starts_with('+') || text.starts_with('-')) return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && error == std::errc{} && end == text.data() + text.size();
}
bool floating(std::string_view text) {
    double value = 0;
    return number(text, value) && std::isfinite(value);
}
bool parameter(std::string_view type, std::string_view text) {
    if (text.empty()) return true;
    if (ciEquals(type, "float")) return floating(text);
    if (ciEquals(type, "bool")) return text == "0" || text == "1";
    if (ciEquals(type, "integer")) { long long value = 0; return number(text, value); }
    return true;
}
}

std::vector<std::string> specialTargets() {
    return {"!activator", "!caller", "!self", "!player", "!picker", "!speechtarget", "!enemy", "!friend"};
}
bool isSpecialTarget(std::string_view target) {
    return std::ranges::any_of(specialTargets(), [&](const auto& name) { return ciEquals(name, target); });
}

ConnectionValidator::ConnectionValidator(const document::Document& doc, const NameIndex& names,
    const assets::EntityClassTable& classes, InstanceTemplates* templates, std::filesystem::path map,
    std::optional<std::filesystem::path> extraRoot, std::optional<bool> instanceFile)
    : doc_(doc), names_(names), classes_(classes), templates_(templates), map_(std::move(map)),
      extraRoot_(std::move(extraRoot)), instanceFile_(instanceFile.value_or(looksLikeInstanceFile(doc))) {
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto h = *doc.handle(id);
        if (std::holds_alternative<Handle<document::Entity>>(h))
            classnames_[toLowerAscii(value(id, "classname"))].push_back(id);
        for (auto child : doc.get(h)->children) self(self, child);
    };
    for (auto root : doc.roots()) walk(walk, root);
}
std::string ConnectionValidator::value(document::NodeId id, std::string_view key) const {
    const auto h = doc_.handle(id);
    const auto* node = h ? doc_.get(*h) : nullptr;
    const auto* field = node && node->block ? node->block->find(key) : nullptr;
    return field ? field->decodedValue().value_or("") : "";
}
const assets::EntityClass* ConnectionValidator::entityClass(document::NodeId id) const {
    return classes_.find(value(id, "classname"));
}
bool ConnectionValidator::isInstance(document::NodeId id) const { return ciEquals(value(id, "classname"), "func_instance"); }
std::vector<document::NodeId> ConnectionValidator::matching(std::string_view target) const {
    auto named = names_.matching(target);
    std::set<document::NodeId> result(named.begin(), named.end());
    for (const auto& [name, entities] : classnames_)
        if (!name.empty() && matches(target, name)) result.insert(entities.begin(), entities.end());
    return {result.begin(), result.end()};
}
std::optional<std::vector<std::string>> ConnectionValidator::instanceProxies(document::NodeId id) const {
    if (!templates_ || !isInstance(id)) return {};
    const auto path = resolveInstanceFile(map_, value(id, "file"), extraRoot_);
    if (!path) return {};
    auto loaded = templates_->load(*path);
    if (!loaded) return {};
    return proxyNames(**loaded);
}
std::vector<std::string> ConnectionValidator::instanceInputs(document::NodeId id) const {
    std::vector<std::string> result;
    if (!templates_ || !isInstance(id)) return result;
    const auto path = resolveInstanceFile(map_, value(id, "file"), extraRoot_);
    if (!path) return result;
    auto loaded = templates_->load(*path);
    if (!loaded) return result;
    const auto& doc = **loaded;
    const auto walk = [&](const auto& self, document::NodeId nodeId) -> void {
        const auto handle = *doc.handle(nodeId);
        const auto* node = doc.get(handle);
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            const auto* name = node->block->find("targetname");
            const auto* classname = node->block->find("classname");
            if (name && classname) {
                const auto targetname = name->decodedValue().value_or("");
                if (const auto* cls = classes_.find(classname->decodedValue().value_or("")))
                    for (const auto& io : cls->inputs)
                        result.push_back("instance:" + targetname + ";" + io.name);
            }
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return result;
}
std::optional<bool> ConnectionValidator::instanceOutput(document::NodeId id, std::string_view output) const {
    if (!templates_ || !isInstance(id)) return {};
    const auto path = resolveInstanceFile(map_, value(id, "file"), extraRoot_);
    if (!path) return {};
    auto loaded = templates_->load(*path);
    if (!loaded) return {};
    if (!toLowerAscii(output).starts_with("instance:")) return {};
    const auto semicolon = output.find(';', 9);
    if (semicolon == std::string_view::npos || semicolon == 9 || semicolon + 1 == output.size()) return false;
    const auto name = output.substr(9, semicolon - 9);
    const auto internalOutput = output.substr(semicolon + 1);
    bool found = false, valid = false;
    const auto& doc = **loaded;
    const auto walk = [&](const auto& self, document::NodeId nodeId) -> void {
        const auto handle = *doc.handle(nodeId);
        const auto* node = doc.get(handle);
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            const auto* targetname = node->block->find("targetname");
            const auto* classname = node->block->find("classname");
            if (targetname && ciEquals(targetname->decodedValue().value_or(""), name)) {
                found = true;
                const auto* cls = classname ? classes_.find(classname->decodedValue().value_or("")) : nullptr;
                if (!cls || std::ranges::any_of(cls->outputs,
                    [&](const auto& io) { return ciEquals(io.name, internalOutput); })) valid = true;
            }
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    return found && valid;
}
std::vector<ConnectionIssue> ConnectionValidator::validate(document::NodeId source,
    const document::Connection& c, std::optional<std::string_view> output) const {
    using Kind = ConnectionIssueKind;
    using Field = ConnectionIssue::Field;
    std::vector<ConnectionIssue> issues;
    const auto add = [&](Kind kind, Field field, std::string message) { issues.push_back({kind, field, std::move(message)}); };
    if (!c.wellFormed) {
        add(Kind::Malformed, Field::Parameter, "connection does not have five fields");
        return issues;
    }
    const auto* sourceClass = entityClass(source);
    if (output) {
        const auto instanceOutputForm = isInstance(source) && toLowerAscii(*output).starts_with("instance:");
        if (instanceOutputForm) {
            const auto valid = instanceOutput(source, *output);
            if (valid && !*valid)
                add(Kind::UnknownOutput, Field::Output, "instance template has no matching named output " + std::string(*output));
        } else {
            const auto proxyRelay = ciEquals(value(source, "classname"), "func_instance_io_proxy") &&
                toLowerAscii(*output).starts_with("onproxyrelay") &&
                std::ranges::all_of(std::string_view(*output).substr(std::string_view("OnProxyRelay").size()),
                    [](char digit) { return digit >= '0' && digit <= '9'; });
            if (!proxyRelay && sourceClass && !std::ranges::any_of(sourceClass->outputs,
                [&](const auto& io) { return ciEquals(io.name, *output); }))
                add(Kind::UnknownOutput, Field::Output, "source class does not declare output " + std::string(*output));
        }
    }
    const auto entities = matching(c.target);
    if (!isSpecialTarget(c.target)) {
        if (entities.empty() && !instanceFile_ && !c.target.starts_with("@") && c.target.find('$') == std::string::npos)
            add(Kind::UnmatchedTarget, Field::Target, "no entity matches target " + c.target);
        if (toLowerAscii(c.input).starts_with("instance:")) {
            const auto semicolon = c.input.find(';', 9);
            const auto name = semicolon == std::string::npos ? std::string_view{} :
                std::string_view(c.input).substr(9, semicolon - 9);
            const auto internalInput = semicolon == std::string::npos ? std::string_view{} :
                std::string_view(c.input).substr(semicolon + 1);
            bool valid = false, unavailable = false, candidate = false, found = false, known = false, unknownClass = false, declared = false;
            for (auto id : entities) {
                if (!isInstance(id)) continue;
                candidate = true;
                if (!templates_) { unavailable = true; continue; }
                const auto path = resolveInstanceFile(map_, value(id, "file"), extraRoot_);
                if (!path) { unavailable = true; continue; }
                auto loaded = templates_->load(*path);
                if (!loaded) { unavailable = true; continue; }
                const auto& templateDoc = **loaded;
                const auto walk = [&](const auto& self, document::NodeId nodeId) -> void {
                    const auto handle = *templateDoc.handle(nodeId);
                    const auto* node = templateDoc.get(handle);
                    if (std::holds_alternative<Handle<document::Entity>>(handle)) {
                        const auto* targetname = node->block->find("targetname");
                        if (targetname && !name.empty() && ciEquals(targetname->decodedValue().value_or(""), name)) {
                            found = true;
                            const auto* classname = node->block->find("classname");
                            const auto* cls = classname ? classes_.find(classname->decodedValue().value_or("")) : nullptr;
                            if (!cls) unknownClass = true;
                            else {
                                known = true;
                                for (const auto& io : cls->inputs) {
                                    if (!ciEquals(io.name, internalInput)) continue;
                                    declared = true;
                                    if (parameter(io.type, c.parameter)) valid = true;
                                }
                            }
                        }
                    }
                    for (const auto child : node->children) self(self, child);
                };
                for (const auto root : templateDoc.roots()) walk(walk, root);
            }
            if (!valid && !unavailable) {
                if (found && known && !unknownClass && declared)
                    add(Kind::BadParameter, Field::Parameter, "parameter does not parse as the declared input type");
                else if ((candidate && !found) || (!candidate && !entities.empty() && !isSpecialTarget(c.target)) ||
                         (found && known && !unknownClass))
                    add(Kind::UnknownInput, Field::Input, "instance template has no matching named input for " + c.input);
            }
        } else {
            bool known = false, declared = false, validParameter = false;
            for (auto id : entities) {
                const auto* cls = entityClass(id);
                known |= cls != nullptr;
                if (const auto* io = input(cls, c.input)) {
                    declared = true;
                    validParameter |= parameter(io->type, c.parameter);
                }
            }
            if (known && !declared) add(Kind::UnknownInput, Field::Input, "matched classes do not declare input " + c.input);
            if (declared && !validParameter) add(Kind::BadParameter, Field::Parameter, "parameter does not parse as the declared input type");
        }
    }
    if (!c.delay.empty() && !floating(c.delay)) add(Kind::BadDelay, Field::Delay, "delay must be a finite number");
    long long times = 0;
    if (!number(c.times, times) || times < -1) add(Kind::BadTimes, Field::Times, "times must be an integer >= -1");
    return issues;
}

} // namespace apex::editor
