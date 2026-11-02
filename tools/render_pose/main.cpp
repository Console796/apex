#include "../common/bake_setup.hpp"
#include "lighting/baker.hpp"
#include "render/lightmap_atlas.hpp"
#include "assets/game_profile.hpp"
#include "assets/material_cache.hpp"
#include "assets/model_cache.hpp"
#include "assets/vfs.hpp"
#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/instance_expansion.hpp"
#include "render/context.hpp"
#include "render/render_mirror.hpp"
#include "render/render_target.hpp"
#include "render/scene_target.hpp"
#include "viewport/camera.hpp"

#include <QFile>
#include <QImage>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace apex;
namespace fs = std::filesystem;

struct Pose {
    std::string name;
    DVec3 position, angles;
};
struct Options {
    fs::path vmf, game, out, cubemaps;
    int width = 1920, height = 1080;
    double fov = 90;
    int timeFrames = 0;
    unsigned bakePasses = 16;
    lighting::BakerOptions bakeOptions{.bounces = 2};
    float bakeScale = 1.0f;
    std::string mode = "lit";
    std::vector<Pose> poses;
};

void help() {
    std::cout << "Usage: apex_render_pose --vmf PATH --game DIR [--width 1920 --height 1080]\n"
                 "  [--fov 90] [--mode lit|textured|fullbright|baked] [--cubemaps DIR] [--time N]\n"
                 "  [--bake-passes 16 --bake-bounces 2 --bake-bounce-rays 32 --bake-threads 3 --bake-seed 0 --bake-scale 1]\n"
                 "  --pose NAME X Y Z PITCH YAW ROLL [--pose ...] --out DIR\n\n"
                 "Game DIR is the P2CE install root (containing p2ce). Mounts the game's addon list.\n"
                 "Poses are Source cl_showpos eye positions and QAngles; negative pitch looks up.\n"
                 "FOV assumption: Source fov is horizontal at 4:3 and widens with aspect ratio:\n"
                 "  horizontal = 2*atan(tan(fov/2)*(width/height)/(4/3)).\n"
                 "At 1920x1080, fov 90 gives 106.26 horizontal, 73.74 vertical degrees.\n"
                 "--cubemaps DIR gives each env_cubemap DIR/c<x>_<y>_<z>.hdr.vtf, else .vtf, named by its\n"
                 "origin rounded to integers as VBSP names them (e.g. files extracted from a compiled BSP's\n"
                 "pakfile). Prints one line per probe with the file used, or \"default\" for the default cube.\n"
                 "Writes NAME.png for lit, NAME-MODE.png otherwise; refuses to overwrite existing files. No display required.\n"
                 "Baked uses CPU brush direct + sky + bounce. --bake-scale is display-only (default 1);\n"
                 "set-2 probe fitted baker-to-display scale: 2.02 (pass --bake-scale 2.02 to compare).\n"
                 "--time N also renders each pose N more times after 3 warm-up frames and prints the mean frame\n"
                 "time (submit + wait, no readback) with the cluster and shadow-budget overflow counts.\n";
}

double number(const std::string& text) {
    std::size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::runtime_error("Invalid number: " + text);
    return value;
}

Options parse(int argc, char** argv) {
    Options o;
    std::set<std::string> names;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("Missing value for " + flag);
            return argv[i];
        };
        const auto dimension = [&]() {
            const double value = number(next());
            if (value < 1 || value > 16384 || std::floor(value) != value)
                throw std::runtime_error("Image dimensions must be integers in [1, 16384]");
            return static_cast<int>(value);
        };
        const auto count = [&](unsigned low, unsigned high) {
            const double value = number(next());
            if (value < low || value > high || std::floor(value) != value)
                throw std::runtime_error("Invalid count for " + flag);
            return static_cast<unsigned>(value);
        };
        if (flag == "--bake-passes") o.bakePasses = count(1, 10000);
        else if (flag == "--bake-bounces") o.bakeOptions.bounces = count(0, 32);
        else if (flag == "--bake-bounce-rays") o.bakeOptions.bounceRays = count(1, 65536);
        else if (flag == "--bake-threads") o.bakeOptions.threads = count(1, 256);
        else if (flag == "--bake-seed") o.bakeOptions.seed = count(0, 0xFFFFFFFFu);
        else if (flag == "--bake-scale") {
            const double value = number(next());
            if (value < 0 || value > std::numeric_limits<float>::max())
                throw std::runtime_error("--bake-scale must be finite and nonnegative");
            o.bakeScale = static_cast<float>(value);
        }
        else if (flag == "--vmf") o.vmf = next();
        else if (flag == "--game") o.game = next();
        else if (flag == "--out") o.out = next();
        else if (flag == "--width") o.width = dimension();
        else if (flag == "--height") o.height = dimension();
        else if (flag == "--fov") o.fov = number(next());
        else if (flag == "--time") {
            const double value = number(next());
            if (value < 1 || value > 10000 || std::floor(value) != value)
                throw std::runtime_error("--time takes a frame count in [1, 10000]");
            o.timeFrames = static_cast<int>(value);
        }
        else if (flag == "--mode") o.mode = next();
        else if (flag == "--cubemaps") o.cubemaps = next();
        else if (flag == "--pose") {
            Pose pose;
            pose.name = next();
            if (pose.name.empty() || pose.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
                throw std::runtime_error("Pose names must contain only letters, digits, '-' or '_'");
            if (!names.insert(pose.name).second) throw std::runtime_error("Duplicate pose name: " + pose.name);
            for (int axis = 0; axis < 3; ++axis) pose.position[axis] = number(next());
            for (int axis = 0; axis < 3; ++axis) pose.angles[axis] = number(next());
            o.poses.push_back(pose);
        } else throw std::runtime_error("Unknown option: " + flag);
    }
    if (o.vmf.empty() || o.game.empty() || o.out.empty() || o.poses.empty())
        throw std::runtime_error("--vmf, --game, --out and at least one --pose are required");
    if (!o.cubemaps.empty() && !fs::is_directory(o.cubemaps))
        throw std::runtime_error("--cubemaps must name a directory");
    if (!(o.fov > 0 && o.fov < 180)) throw std::runtime_error("FOV must be between 0 and 180 degrees");
    if (o.mode != "lit" && o.mode != "textured" && o.mode != "fullbright" && o.mode != "baked")
        throw std::runtime_error("Mode must be lit, textured, fullbright or baked");
    return o;
}

std::string fileName(const Pose& pose, const Options& o) {
    return o.mode == "lit" ? pose.name + ".png" : pose.name + "-" + o.mode + ".png";
}

template <typename T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().what());
    return std::move(*result);
}
void check(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().what());
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

int run(const Options& o) {
    const auto game = fs::canonical(o.game);
    const auto out = fs::weakly_canonical(o.out);
    if (within(out, game)) throw std::runtime_error("Output must be outside the game install");
    for (auto parent = game; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path())
        if (parent.filename() == "steamapps" && within(out, parent / "workshop"))
            throw std::runtime_error("Output must be outside Steam workshop");
    for (const auto& pose : o.poses)
        if (fs::exists(out / (fileName(pose, o))))
            throw std::runtime_error("Output already exists: " + fileName(pose, o));

    std::ifstream input(o.vmf, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open VMF: " + o.vmf.string());
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    auto parsed = vmf::parse(text);
    if (!parsed) parsed = vmf::parse(text, {kv1::EscapeMode::Legacy});
    document::Document document(take(std::move(parsed)));
    assets::GameProfile profile;
    profile.gameDirectory = game / "p2ce";
    profile.addons.list = assets::AddonSettings::List::Game;
    JobPool jobs(2);
    auto vfs = take(assets::Vfs::load(profile, jobs));
    assets::MaterialCache materials(vfs, jobs);
    assets::ModelCache models(vfs, jobs);
    editor::InstanceTemplates templates;
    editor::InstanceExpansion instances(templates);
    if (o.mode == "baked") tools::expandBakeInstances(document, fs::canonical(o.vmf), profile);
    instances.update(document, fs::canonical(o.vmf), profile.instanceSearchRoot());
    for (const auto& problem : instances.problems())
        std::cerr << "instance-failed " << problem.file << ": " << problem.message << '\n';
    if (!instances.problems().empty()) throw std::runtime_error("Instance resolution failed");

    auto context = take(render::Context::create());
    auto heap = take(render::TextureHeap::create(*context));
    auto renderer = take(render::Renderer::create(*context, *heap, render::RenderTarget::kColorFormat,
                                                render::RenderTarget::kDepthFormat));
    auto target = take(render::RenderTarget::create(*context, static_cast<std::uint32_t>(o.width),
                                                   static_cast<std::uint32_t>(o.height)));
    render::SceneTarget scene(*context, *heap);
    check(scene.resize(static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height)));
    render::RenderMirror mirror(*context, *heap, jobs, materials, nullptr, &models,
                               [&](const std::string& path) { return vfs.exists(path); });
    if (!o.cubemaps.empty()) {
        mirror.setCubemapSource([dir = o.cubemaps](DVec3 origin) -> std::optional<assets::Texture> {
            const auto stem = "c" + std::to_string(std::llround(origin.x)) + "_" + std::to_string(std::llround(origin.y)) +
                              "_" + std::to_string(std::llround(origin.z));
            std::ostringstream text;
            text << "cubemap " << origin.x << ' ' << origin.y << ' ' << origin.z << ": ";
            const auto label = text.str();
            for (const auto* suffix : {".hdr.vtf", ".vtf"}) {
                const auto path = dir / (stem + suffix);
                std::ifstream stream(path, std::ios::binary);
                if (!stream) continue;
                assets::Blob bytes;
                for (char c; stream.get(c);) bytes.push_back(std::byte(static_cast<unsigned char>(c)));
                auto texture = assets::decodeTexture(bytes);
                if (texture && texture->faces >= 6) {
                    std::cout << label << path.filename().string() << '\n';
                    return std::move(*texture);
                }
                std::cerr << label << path.filename().string() << " is not a cube texture"
                          << (texture ? "" : ": " + std::string(texture.error().what())) << '\n';
            }
            std::cout << label << "default\n";
            return std::nullopt;
        });
    }
    // No FGD class table: entity models then come only from explicit model keys, so the
    // editor-only helper models (light_rt_spot cones, player start, etc.) stay out of the capture.
    if (o.mode == "baked") {
        auto setup = tools::setupBake(document, &vfs);
        lighting::Baker baker(std::move(setup.scene), o.bakeOptions);
        for (unsigned level = 0; level <= o.bakeOptions.bounces; ++level) {
            for (unsigned pass = 0; pass < o.bakePasses; ++pass) {
                const auto step = level == 0 ? baker.step() : baker.bounce(level);
                if (!step.passComplete) throw std::runtime_error("Incomplete bake pass");
            }
            std::cout << "bake level=" << level << " passes=" << o.bakePasses << std::endl;
        }
        auto maps = take(render::BakedLightmaps::create(*context, *heap,
            take(render::LightmapAtlas::pack(baker.scene().faces))));
        check(maps->upload(baker.resolve()));
        mirror.setLightmaps(std::move(maps));
    }
    mirror.rebuild(document);
    mirror.setInstanceContents(instances.contents());
    // Lit draw-list construction requests the sky grid; settle must see that request.
    render::MirrorViewOptions warmup;
    warmup.mode = render::DisplayMode::Lit;
    warmup.showEntityHelpers = false;
    mirror.buildDrawList(warmup);
    if (!mirror.settle(std::chrono::minutes(5))) throw std::runtime_error("RenderMirror settle timed out");
    const auto materialNames = materials.requestedNames();
    std::size_t failedMaterials = 0, failedModels = 0;
    for (const auto& name : materialNames) {
        if (materials.state(name) != assets::MaterialState::Failed) continue;
        ++failedMaterials;
        std::cerr << "material-failed " << name << ": " << materials.error(name)->what() << '\n';
    }
    for (const auto& name : models.requestedNames()) {
        if (models.state(name) != assets::ModelState::Failed) continue;
        ++failedModels;
        std::cerr << "model-failed " << name << ": " << models.error(name)->what() << '\n';
    }
    fs::create_directories(out);
    for (const auto& pose : o.poses) {
        viewport::Camera camera;
        camera.position = pose.position;
        camera.angles = pose.angles;
        camera.fovYDegrees = viewport::sourceFovYDegrees(o.fov);
        render::MirrorViewOptions options;
        options.mode = o.mode == "textured" ? render::DisplayMode::Textured :
            o.mode == "baked" ? render::DisplayMode::Baked : render::DisplayMode::Lit;
        options.showEntityHelpers = false;
        options.showNodraw = false;
        options.showToolFaces = false; // The game draws no trigger, clip or hint faces.
        options.modelEdgeOverlay = false;
        options.view.cameraPosition = camera.position;
        options.view.viewProjection = camera.projectionMatrix(double(o.width) / o.height) * camera.viewMatrix();
        options.view.cameraRight = Vec3(camera.basis().right);
        options.view.cameraUp = Vec3(camera.basis().up);
        auto list = mirror.buildDrawList(options);
        list.bakedScale = o.bakeScale;
        list.fullbright = o.mode == "fullbright";
        // Leave exposure at DrawList's default, as ViewportPane does.
        constexpr float clear[] = {0.1f, 0.1f, 0.1f, 1.0f};
        VkCommandBuffer slot = VK_NULL_HANDLE;
        const auto render = [&] {
            check(context->oneShot([&](VkCommandBuffer commands) {
                slot = commands;
                renderer->prepare(commands, list, scene, clear);
                target.begin(commands, clear);
                renderer->record(commands, list);
                target.end(commands);
            }));
        };
        render();
        const auto image = take(target.readback());
        const auto path = out / (fileName(pose, o));
        QFile file(QString::fromStdString(path.string()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw std::runtime_error("Cannot create output: " + path.string());
        const QImage png(image.pixels.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888);
        if (!png.save(&file, "PNG")) throw std::runtime_error("PNG write failed: " + path.string());
        std::cout << fileName(pose, o) << " exposure=" << list.exposure
                  << " lights=" << list.lights.size() << " materials=" << materialNames.size()
                  << " failed-materials=" << failedMaterials << " failed-models=" << failedModels
                  << " sky-grid-revision=" << mirror.skyVisibilityState().revision << '\n';
        if (o.timeFrames) {
            for (int warm = 0; warm < 3; ++warm) render();
            const auto start = std::chrono::steady_clock::now();
            for (int frame = 0; frame < o.timeFrames; ++frame) render();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                / o.timeFrames;
            std::cout << fileName(pose, o) << " device=\"" << context->info().name << "\" frame-ms=" << ms
                      << " frames=" << o.timeFrames << " cluster-overflow=" << renderer->clusterOverflow(slot)
                      << " shadow-overflow=" << renderer->shadowOverflow(slot) << '\n';
        }
    }
    context->waitIdle();
    if (context->validation().errorCount()) throw std::runtime_error("Vulkan validation errors");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { help(); return 0; }
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "apex_render_pose: " << error.what() << '\n';
        return 1;
    }
}
