#include "ui/command_providers.hpp"

#include "editor/editor_session.hpp"
#include "ui/area_frame.hpp"
#include "ui/key_chord.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/viewport.hpp"

#include <QAction>

// Commands and options are written with designated initializers that leave defaults out.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

namespace apex::ui {

namespace {

constexpr const char* kView = "View";

QString displayModeName(viewport::DisplayMode mode) {
    switch (mode) {
    case viewport::DisplayMode::Wireframe: return QStringLiteral("Wireframe");
    case viewport::DisplayMode::Flat: return QStringLiteral("Flat");
    case viewport::DisplayMode::Textured: return QStringLiteral("Textured");
    case viewport::DisplayMode::Lit: return QStringLiteral("Lit");
    case viewport::DisplayMode::Baked: return QStringLiteral("Baked");
    case viewport::DisplayMode::Vrad: return QStringLiteral("VRAD");
    }
    return {};
}

struct OverlayEntry {
    const char* label;
    bool viewport::Overlays::*field;
};
constexpr OverlayEntry kOverlays[] = {{"Grid", &viewport::Overlays::grid},
                                      {"Entity helpers", &viewport::Overlays::entityHelpers},
                                      {"Bounds", &viewport::Overlays::bounds},
                                      {"Normals", &viewport::Overlays::normals},
                                      {"Nodraw faces", &viewport::Overlays::nodrawFaces},
                                      {"Tool textures: fixed brightness", &viewport::Overlays::toolFixedBrightness},
                                      {"Entity names", &viewport::Overlays::entityNames},
                                      {"Compile leak", &viewport::Overlays::leak},
                                      {"Compile portals", &viewport::Overlays::portals},
                                      {"Cordons", &viewport::Overlays::cordons},
                                      {"Backface culling", &viewport::Overlays::backfaceCulling},
                                      {"Luxel density (Baked, VRAD)", &viewport::Overlays::luxelDensity},
                                      {"Realtime indirect", &viewport::Overlays::realtimeIndirect},
                                      {"Approximate bounce (Lit)", &viewport::Overlays::approxBounce}};

}  // namespace

void registerViewportCommands(CommandRegistry& registry, ViewportArea& area) {
    registry.addProvider(QStringLiteral("viewport"), [&area](CommandRegistry& r) {
        const auto* vp = area.activeViewport();

        if (vp) {
            const auto mode = vp->mode;
            r.add({.id = "view.display_mode", .title = "Display mode", .category = kView,
                   .description = "Switch the active viewport between Wireframe, Flat, Textured, Lit, Baked and VRAD.",
                   .keywords = {"view", "type", "shading", "render"},
                   .options = [=, &area] {
                       std::vector<PaletteOption> options;
                       for (const auto m : {viewport::DisplayMode::Wireframe, viewport::DisplayMode::Flat,
                                            viewport::DisplayMode::Textured, viewport::DisplayMode::Lit,
                                            viewport::DisplayMode::Baked, viewport::DisplayMode::Vrad}) {
                           options.push_back({.label = displayModeName(m), .checked = m == mode,
                                              .run = [&area, m] {
                                                  area.editActiveViewport([m](viewport::Viewport& v) { v.mode = m; });
                                              }});
                       }
                       return options;
                   }});

            const auto camera = vp->camera;
            r.add({.id = "view.preset", .title = "View preset", .category = kView,
                   .description = "Reset the active viewport to the Camera, Top, Front or Side preset.",
                   .keywords = {"view", "type", "top", "front", "side", "camera", "2d", "3d"},
                   .options = [&area] {
                       std::vector<PaletteOption> options;
                       for (const auto p : {viewport::Preset::Camera, viewport::Preset::Top, viewport::Preset::Front,
                                            viewport::Preset::Side}) {
                           options.push_back({.label = QString::fromLatin1(viewport::nameOf(p)),
                                              .run = [&area, p] {
                                                  area.editActiveViewport(
                                                      [p](viewport::Viewport& v) { viewport::applyPreset(v, p); });
                                              }});
                       }
                       return options;
                   }});

            r.add({.id = "view.projection", .title = "Projection", .category = kView,
                   .description = "Perspective or orthographic projection for the active viewport.",
                   .keywords = {"view", "type", "perspective", "orthographic", "ortho"},
                   .options = [&area, camera] {
                       std::vector<PaletteOption> options;
                       for (const auto kind : {viewport::ProjectionKind::Perspective, viewport::ProjectionKind::Orthographic}) {
                           options.push_back(
                               {.label = kind == viewport::ProjectionKind::Perspective ? QStringLiteral("Perspective")
                                                                                       : QStringLiteral("Orthographic"),
                                .checked = camera.projection == kind,
                                .run = [&area, kind] {
                                    area.editActiveViewport([kind](viewport::Viewport& v) {
                                        v.camera.projection = kind;
                                        v.camera.controller = kind == viewport::ProjectionKind::Orthographic
                                                                  ? viewport::ControllerKind::Pan2D
                                                                  : viewport::ControllerKind::Fly;
                                    });
                                }});
                       }
                       return options;
                   }});

            r.add({.id = "view.axis", .title = "View axis", .category = kView,
                   .description = "Lock the active viewport to an axis (Top, Bottom, Front, Back, Side) or free.",
                   .keywords = {"view", "orientation", "top", "bottom", "front", "back", "side"},
                   .options = [&area, camera] {
                       struct Entry {
                           viewport::ViewAxis axis;
                           const char* label;
                       };
                       const Entry axes[] = {{viewport::ViewAxis::Free, "Free"}, {viewport::ViewAxis::NegZ, "Top (-Z)"},
                                             {viewport::ViewAxis::PosZ, "Bottom (+Z)"}, {viewport::ViewAxis::PosY, "Front (+Y)"},
                                             {viewport::ViewAxis::NegY, "Back (-Y)"}, {viewport::ViewAxis::NegX, "Side (-X)"},
                                             {viewport::ViewAxis::PosX, "Side (+X)"}};
                       std::vector<PaletteOption> options;
                       for (const auto& entry : axes) {
                           options.push_back({.label = QString::fromLatin1(entry.label),
                                              .checked = camera.axis == entry.axis,
                                              .run = [&area, axis = entry.axis] {
                                                  area.editActiveViewport(
                                                      [axis](viewport::Viewport& v) { viewport::setAxis(v.camera, axis); });
                                              }});
                       }
                       return options;
                   }});

            const auto overlays = vp->overlays;
            r.add({.id = "view.overlays", .title = "Overlays", .category = kView,
                   .description = "Toggle an overlay of the active viewport (grid, bounds, normals, leak, ...).",
                   .keywords = {"toggle", "show", "hide"},
                   .options = [&area, overlays] {
                       std::vector<PaletteOption> options;
                       for (const auto& entry : kOverlays) {
                           const bool on = overlays.*(entry.field);
                           options.push_back({.label = QString::fromLatin1(entry.label), .detail = on ? "on" : "off",
                                              .checked = on,
                                              .run = [&area, field = entry.field, on] {
                                                  area.editActiveViewport(
                                                      [field, on](viewport::Viewport& v) { v.overlays.*field = !on; });
                                              }});
                       }
                       return options;
                   }});

            r.add({.id = "view.compare", .title = "Compare light source", .category = kView,
                   .description = "A/B this viewport against another light source, e.g. Lit vs Baked vs VRAD.",
                   .keywords = {"ab", "a/b", "swap"},
                   .options = [&area, mode, compare = vp->compare] {
                       std::vector<PaletteOption> options;
                       for (const auto m : {viewport::DisplayMode::Lit, viewport::DisplayMode::Baked, viewport::DisplayMode::Vrad}) {
                           if (m == mode) continue;
                           options.push_back({.label = QStringLiteral("A/B with %1").arg(displayModeName(m)),
                                              .checked = compare == m,
                                              .run = [&area, m] {
                                                  area.editActiveViewport([m](viewport::Viewport& v) { v.compare = m; });
                                              }});
                       }
                       if (compare) {
                           options.push_back({.label = QStringLiteral("Swap A/B"), .run = [&area] {
                                                  area.editActiveViewport([](viewport::Viewport& v) {
                                                      if (v.compare) std::swap(v.mode, *v.compare);
                                                  });
                                              }});
                           options.push_back({.label = QStringLiteral("Compare off"), .run = [&area] {
                                                  area.editActiveViewport([](viewport::Viewport& v) { v.compare.reset(); });
                                              }});
                       }
                       return options;
                   }});
        }

        r.add({.id = "view.show_area", .title = "Show area", .category = kView,
               .description = "Reveal the outliner, inspector, materials and other areas.",
               .keywords = {"panel", "window", "open"},
               .options = [&area] {
                   std::vector<PaletteOption> options;
                   for (const auto role : viewport::kAreaRoles) {
                       if (role == viewport::AreaRole::Viewport) continue;
                       options.push_back({.label = roleTitle(role), .run = [&area, role] { area.showRole(role, false); }});
                   }
                   return options;
               }});
    });
}

void registerEditorActionCommands(CommandRegistry& registry, const std::map<editor::ActionId, QAction*>& actions,
                                  const editor::EditorSession& session) {
    registry.addProvider(QStringLiteral("editor actions"), [&actions, &session](CommandRegistry& r) {
        for (const auto& [id, action] : actions) {
            const auto* entry = editor::actionRegistry().find(id);
            if (!entry || id == "app.command_palette") continue;
            PaletteCommand command;
            command.id = QString::fromStdString(id);
            command.title = QString::fromStdString(entry->name);
            command.category = QString::fromStdString(entry->category);
            command.description = QString::fromStdString(entry->description);
            command.shortcut = shortcutText(session.preset(), id);
            command.enabled = [action] { return action->isEnabled(); };
            if (action->isCheckable()) command.checked = [action] { return std::optional<bool>(action->isChecked()); };
            command.run = [action] { action->trigger(); };
            r.add(std::move(command));
        }
    });
}

}  // namespace apex::ui
