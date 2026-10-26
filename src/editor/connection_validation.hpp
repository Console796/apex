#pragma once

#include "core/entity_class.hpp"
#include "document/connection_codec.hpp"
#include "editor/instance_templates.hpp"
#include "editor/name_index.hpp"

namespace apex::editor {

enum class ConnectionIssueKind { UnknownOutput, UnmatchedTarget, UnknownInput, BadParameter, BadDelay, BadTimes, Malformed };
struct ConnectionIssue {
    enum class Field { Output, Target, Input, Parameter, Delay, Times };
    ConnectionIssueKind kind;
    Field field;
    std::string message;
};

bool isSpecialTarget(std::string_view target);
std::vector<std::string> specialTargets();

// Snapshot: rebuild after document changes. All referenced metadata must outlive it.
class ConnectionValidator {
public:
    ConnectionValidator(const document::Document& doc, const NameIndex& names,
        const assets::EntityClassTable& classes, InstanceTemplates* templates = nullptr,
        std::filesystem::path map = {}, std::optional<std::filesystem::path> extraRoot = {},
        std::optional<bool> instanceFile = {});
    // Output is stored on the connection row, not in the five-field codec value.
    std::vector<ConnectionIssue> validate(document::NodeId source, const document::Connection& connection,
                                         std::optional<std::string_view> output = {}) const;
    std::vector<document::NodeId> matching(std::string_view target) const;
    const assets::EntityClass* entityClass(document::NodeId id) const;
    bool isInstance(document::NodeId id) const;
    // nullopt means unavailable, including unresolved files; an empty vector is a loaded file without proxies.
    std::optional<std::vector<std::string>> instanceProxies(document::NodeId id) const;
    // Named entities inside an instance template, formatted for input completion.
    std::vector<std::string> instanceInputs(document::NodeId id) const;
    std::optional<bool> instanceOutput(document::NodeId id, std::string_view output) const;
    const assets::EntityClassTable& classes() const { return classes_; }

private:
    const document::Document& doc_;
    const NameIndex& names_;
    const assets::EntityClassTable& classes_;
    InstanceTemplates* templates_;
    std::filesystem::path map_;
    std::optional<std::filesystem::path> extraRoot_;
    bool instanceFile_;
    std::map<std::string, std::vector<document::NodeId>> classnames_;
    std::string value(document::NodeId id, std::string_view key) const;
};

} // namespace apex::editor
