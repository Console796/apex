#include "ui/asset_report_builder.hpp"

#include "ui/game_content.hpp"

namespace apex::ui {

std::optional<std::string> assetVfsPath(editor::AssetKind kind, std::string_view name) {
    if (kind == editor::AssetKind::Material) return "materials/" + std::string(name) + ".vmt";
    if (kind == editor::AssetKind::Model) return std::string(name);
    if (kind == editor::AssetKind::Sound) {
        const auto key = assets::SoundKind::key(name);
        if (key.ends_with(".wav") || key.ends_with(".mp3")) return key;
    }
    return std::nullopt;
}

std::unique_ptr<editor::AssetReport> buildAssetReport(const document::Document& document,
                                                      GameContent* content, const editor::EditorSession* session) {
    const auto exists = [content, session](editor::AssetKind kind, std::string_view name) {
        if (!content) return false;
        if (const auto path = assetVfsPath(kind, name)) return content->assetExists(*path);
        if (kind == editor::AssetKind::Sound) {
            const auto snapshot = content->soundSnapshot();
            return !snapshot || snapshot->find(name) != nullptr;
        }
        if (kind == editor::AssetKind::Particle) {
            const auto snapshot = content->particleSnapshot();
            return !snapshot || snapshot->find(name) != nullptr;
        }
        return !session || editor::resolveInstanceFile(session->connectionMapPath(), name, session->connectionExtraRoot());
    };
    const auto provider = [content](editor::AssetKind kind, std::string_view name) -> std::optional<std::string> {
        const auto path = assetVfsPath(kind, name);
        return content && path ? content->assetProviderName(*path) : std::nullopt;
    };
    const editor::AssetProvider unmountedAddon = [content](editor::AssetKind kind, std::string_view name) -> std::optional<std::string> {
        auto path = assetVfsPath(kind, name);
        if (!content || !path) return {};
        auto addon = content->unmountedAddon(*path);
        return addon ? std::optional(content->addonName(*addon).toStdString()) : std::nullopt;
    };
    const auto* classes = content ? content->classes() : nullptr;
    return std::make_unique<editor::AssetReport>(document, exists, unmountedAddon, provider, classes);
}

std::vector<editor::Problem> collectMapProblems(const document::Document& document, GameContent* content,
                                                const editor::EditorSession* session) {
    editor::ProblemContext context{document};
    editor::Problems assets;
    if (content) {
        const auto report = buildAssetReport(document, content, session);
        const auto* classes = content->classes();
        const auto known = [classes](std::string_view classname) { return !classes || classes->find(classname) != nullptr; };
        if (session && session->document() == &document) assets = report->problems(document, session->connectionValidator(), known);
        else assets.missingAssets = report->missing();
        context.assets = &assets;
        context.classes = classes;
        if (session && session->document() == &document) context.instances = &session->instances().problems();
        return editor::ProblemRegistry::builtin().run(context);
    }
    if (session && session->document() == &document) context.instances = &session->instances().problems();
    return editor::ProblemRegistry::builtin().run(context);
}

} // namespace apex::ui
