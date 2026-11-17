#include <apex/apex_plugin.h>
#include <stdlib.h>
static apex_status crash(void* user) { (void)user; abort(); }
static const apex_plugin plugin = { sizeof(apex_plugin), 1, 0, 0, crash, 0, 0, 0 };
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* host) { (void)host; return &plugin; }
