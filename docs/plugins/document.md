# apex.document/1: read the document

A read-only view of the open document as a tree of nodes (`apex_document_v1`):

| Function | Purpose |
|---|---|
| `info` | `is_open`, `revision`, `root_count` |
| `root_at(i)` | NodeId of the *i*-th root |
| `node(id)` | `kind`, `parent`, `child_count`, `key_count` |
| `child_at(id, i)` | NodeId of a child |
| `key_at(id, i, &key, &value)` | The *i*-th key/value pair of a node |
| `side_count(solid, &n)` | Number of sides of a solid (0 for other nodes). ABI 1.1 |
| `side_at(solid, i, &side)` | The *i*-th side, in source order: `id`, the three `plane` points, `material`, `uaxis`/`vaxis` with shifts and scales. ABI 1.1 |
| `side_lookup(side_id, &solid, &i)` | The solid and index holding a side ID (the first occurrence when an ID is duplicated). ABI 1.1 |

`apex_node_id` is a stable arena NodeId, not a VMF id. Check `is_open` first; calls with no open document
return `APEX_ERR_STATE`.

Sides are not nodes: a solid's `side` blocks live inside it, so reach them with the side functions, which ABI 1.1
appended to the table. On an older host they are missing; check with `apex::plugin::available(document,
&apex_document_v1::side_at)` (or compare `struct_size`). `apex_side_info` is an out parameter (rule 6): set
`struct_size`, and a value that is missing or does not parse reads as zero while the call still succeeds.
`side_at`'s `material`, like `key_at`'s strings, is valid until the next call to this interface. Selection IDs in
face mode and a `"sides"` section context are side IDs; `side_lookup` turns one into its solid and index.

Reading the root node and its class name, from the C example:

<!-- snippet: examples/hello-tool/hello.c -->
```c
static void read_document(void* user) {
    const apex_document_v1* doc = (const apex_document_v1*)host->get_interface(host->context, str("apex.document/1"));
    apex_document_info info = { sizeof(info), 0, 0, 0 };
    apex_node_id id;
    apex_node_info node = { sizeof(node), 0, 0, 0, {0, 0}, 0, 0 };
    size_t i;
    (void)user;
    if (!doc || doc->info(host->context, &info) != APEX_OK || !info.is_open || !info.root_count ||
        doc->root_at(host->context, 0, &id) != APEX_OK || doc->node(host->context, id, &node) != APEX_OK) return;
    for (i = 0; i < node.key_count; ++i) {
        apex_str key, value;
        if (doc->key_at(host->context, id, i, &key, &value) == APEX_OK && key.size == 9 && memcmp(key.data, "classname", 9) == 0) {
            host->log(host->context, 1, value);
            return;
        }
    }
}
```

In C++ (the face-stats example walks the whole tree):

<!-- snippet: examples/face-stats/face_stats.cpp -->
```cpp
bool countNodes(const apex_host& h, std::string_view kind, size_t& count) {
    const auto* document = static_cast<const apex_document_v1*>(h.get_interface(h.context, apex::plugin::string("apex.document/1")));
    apex_document_info info{}; info.struct_size = sizeof(info);
    if (!document || document->info(h.context, &info) != APEX_OK || !info.is_open) return false;
    std::vector<apex_node_id> pending;
    for (size_t i = 0; i < info.root_count; ++i) {
        apex_node_id root = 0;
        if (document->root_at(h.context, i, &root) == APEX_OK) pending.push_back(root);
    }
    count = 0;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        apex_node_info node{}; node.struct_size = sizeof(node);
        if (document->node(h.context, id, &node) != APEX_OK) continue;
        if (apex::plugin::view(node.kind) == kind) ++count;
        for (size_t i = 0; i < node.child_count; ++i) {
            apex_node_id child = 0;
            if (document->child_at(h.context, id, i, &child) == APEX_OK) pending.push_back(child);
        }
    }
    return true;
}
```
