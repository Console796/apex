#include <apex/apex_plugin.h>
static const apex_plugin plugin = { sizeof(apex_plugin), 99, 0, 0, 0, 0, 0, 0, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* host) { (void)host; return &plugin; }
