#pragma once

#include "document/document.hpp"
#include "document/instance_fixup.hpp"

namespace apex::document {

// Return cached immutable documents. Identity is the Document address, including aliases.
using InstanceTemplateLookup = std::function<Result<const Document*>(const Document& referring,
                                                                    std::string_view file)>;
struct InstanceConnectionRewrite {
    NodeId entity;
    kv1::Node connections;
};
struct InstanceCollapseOptions {
    InstanceKeyTypes keyTypes;
    std::function<bool(std::string_view)> isClassname;
    unsigned nextAutoName = 1;
};
struct InstanceCollapseResult {
    std::string fragment; // World-less, ready for stagePaste(inPlace=true).
    std::vector<InstanceConnectionRewrite> connections;
    // After stagePaste, add these visgroups to created objects. Attach fragment root
    // groups and ungrouped objects to this group, preserving their internal hierarchy.
    // These parent IDs must not go through the fragment's local ID remapping.
    kv1::Node parentMembership = kv1::Node::block("editor");
    unsigned nextAutoName = 1;
};

// No mutations, history, ID allocation in the parent, or filesystem access. Eight template
// levels are allowed. Missing templates, cycles and excessive depth fail atomically.
Result<InstanceCollapseResult> collapseInstance(const Document& parent, NodeId instance,
                                                const Document& instanceTemplate,
                                                const InstanceTemplateLookup& lookup,
                                                const InstanceCollapseOptions& options = {});

} // namespace apex::document
