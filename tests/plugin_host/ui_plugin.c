#include <apex/apex_plugin.h>
#include <stdio.h>
#include <string.h>
static const apex_host* host;
static const apex_ui_v1* ui;
static apex_str str(const char* s) { apex_str v = {s, strlen(s)}; return v; }
static apex_ui_control controls[2];
static apex_ui_form form;
static apex_status describe(void* user, const apex_ui_context* context, const apex_ui_form** out) {
    (void)user; (void)context; *out = &form; return APEX_OK;
}
static void change(void* user, apex_str key, const apex_ui_value* value) {
    (void)user; (void)key; (void)value;
    host->log(host->context, 1, str("changed"));
    ui->refresh(host->context, str("plugin.ui.panel"));
}
static apex_ui_callbacks dialogCallbacks;
static apex_ui_control dialogControl;
static apex_ui_form dialogForm;
static apex_status describeDialog(void* user, const apex_ui_context* context, const apex_ui_form** out) {
    (void)user; (void)context; *out = &dialogForm; return APEX_OK;
}
static void dialogChange(void* user, apex_str key, const apex_ui_value* value) {
    (void)user; (void)key; (void)value; host->log(host->context, 1, str("dialog changed"));
}
static void click(void* user, apex_str key) {
    uint32_t accepted = 0;
    (void)user;
    if (key.size == 4 && memcmp(key.data, "open", 4) == 0) {
        dialogControl.struct_size = sizeof(dialogControl); dialogControl.kind = APEX_UI_TEXT; dialogControl.key = str("d");
        dialogControl.value.struct_size = sizeof(dialogControl.value);
        dialogForm.struct_size = sizeof(dialogForm); dialogForm.controls = &dialogControl; dialogForm.control_count = 1;
        dialogCallbacks.struct_size = sizeof(dialogCallbacks); dialogCallbacks.describe = describeDialog; dialogCallbacks.on_change = dialogChange;
        if (ui->show_dialog(host->context, str("Options"), 0, &dialogCallbacks, &accepted) == APEX_OK)
            host->log(host->context, 1, accepted ? str("dialog accepted") : str("dialog cancelled"));
        return;
    }
    host->log(host->context, 1, str("clicked"));
}
static char summary[256];
static apex_ui_control sectionControl;
static apex_ui_form sectionForm;
static apex_status describeSection(void* user, const apex_ui_context* context, const apex_ui_form** out) {
    (void)user;
    if (!context) return APEX_ERR_INVALID;
    snprintf(summary, sizeof(summary), "%.*s names=%zu ids=%zu first=%.*s", (int)context->kind.size, context->kind.data,
             context->name_count, context->id_count, context->name_count ? (int)context->names[0].size : 0,
             context->name_count ? context->names[0].data : "");
    sectionControl.struct_size = sizeof(sectionControl); sectionControl.kind = APEX_UI_LABEL; sectionControl.key = str("summary");
    sectionControl.value.struct_size = sizeof(sectionControl.value); sectionControl.value.text = str(summary);
    sectionForm.struct_size = sizeof(sectionForm); sectionForm.controls = &sectionControl; sectionForm.control_count = 1;
    *out = &sectionForm; return APEX_OK;
}
static apex_status activate(void* user) {
    static apex_ui_callbacks callbacks, sectionCallbacks;
    apex_status result;
    (void)user;
    ui = (const apex_ui_v1*)host->get_interface(host->context, str("apex.ui/1"));
    if (!ui) return APEX_ERR_UNSUPPORTED;
    controls[0].struct_size = sizeof(controls[0]); controls[0].kind = APEX_UI_TEXT; controls[0].key = str("field");
    controls[0].value.struct_size = sizeof(controls[0].value); controls[0].value.text = str("value");
    controls[1].struct_size = sizeof(controls[1]); controls[1].kind = APEX_UI_BUTTON; controls[1].key = str("open"); controls[1].label = str("Open");
    controls[1].value.struct_size = sizeof(controls[1].value);
    form.struct_size = sizeof(form); form.controls = controls; form.control_count = 2;
    callbacks.struct_size = sizeof(callbacks); callbacks.describe = describe; callbacks.on_change = change; callbacks.on_click = click;
    result = ui->register_panel(host->context, str("plugin.ui.panel"), str("Test panel"), 0, &callbacks);
    if (result != APEX_OK) return result;
    sectionCallbacks.struct_size = sizeof(sectionCallbacks); sectionCallbacks.describe = describeSection;
    result = ui->register_section(host->context, str("plugin.ui.section"), str("Test section"), str("materials"), 0, &sectionCallbacks);
    if (result != APEX_OK) return result;
    result = ui->register_section(host->context, str("plugin.ui.inspector"), str("Test inspector section"), str("inspector"), 0, &sectionCallbacks);
    if (result != APEX_OK) return result;
    return ui->register_section(host->context, str("plugin.ui.faces"), str("Test face section"), str("face_properties"), 0, &sectionCallbacks);
}
static const apex_plugin plugin = {sizeof(plugin), 1, 0, 0, activate, 0, 0, 0, 0};
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
