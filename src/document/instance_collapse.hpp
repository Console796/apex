#pragma once

#include "document/document.hpp"
#include "document/instance_contents.hpp"
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
// Template identity of one solid in the fragment, keyed by the solid "id" the fragment carries.
struct InstanceFragmentSolid {
    std::vector<int> nested;   // Entity ids of the nested func_instances above the template; empty for the placed one.
    int templateSolid = 0;     // VMF solid id in its own template.
    std::map<int, int> sides;  // Fragment side id -> template side id.
    DMat4 transform{1.0};      // Template-local to fragment (world) space, as the fragment's brushes were moved.
};
struct InstanceCollapseResult {
    std::string fragment; // World-less, ready for stagePaste(inPlace=true).
    std::vector<InstanceConnectionRewrite> connections;
    // After stagePaste, add these visgroups to created objects. Attach fragment root
    // groups and ungrouped objects to this group, preserving their internal hierarchy.
    // These parent IDs must not go through the fragment's local ID remapping.
    kv1::Node parentMembership = kv1::Node::block("editor");
    unsigned nextAutoName = 1;
    std::map<int, InstanceFragmentSolid> origins;
    std::size_t objectCount = 0; // Root objects in the fragment; zero for a template with no contents.
};

// No mutations, history, ID allocation in the parent, or filesystem access. Eight template
// levels are allowed. Missing templates, cycles and excessive depth fail atomically.
Result<InstanceCollapseResult> collapseInstance(const Document& parent, NodeId instance,
                                                const Document& instanceTemplate,
                                                const InstanceTemplateLookup& lookup,
                                                const InstanceCollapseOptions& options = {});

} // namespace apex::document
