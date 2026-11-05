#include "../common/bake_setup.hpp"
#include "lighting/bake_session.hpp"
#include "lighting/baker.hpp"
#include "render/lightmap_atlas.hpp"
#include "assets/game_profile.hpp"
#include "assets/material_cache.hpp"
#include "assets/model_cache.hpp"
#include "assets/vfs.hpp"
#include "document/document.hpp"
#include "compile/blocking_runner.hpp"
#include "document/vmf.hpp"
#include "editor/vrad_roundtrip.hpp"
#include "editor/instance_expansion.hpp"
#include "render/context.hpp"
#include "render/render_mirror.hpp"
#include "render/vrad_lightmaps.hpp"
#include "render/render_target.hpp"
#include "render/scene_target.hpp"
#include "viewport/camera.hpp"

#include <QFile>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
    fs::path vmf, game, out, cubemaps, vradBsp;
    int width = 1920, height = 1080;
    double fov = 90;
    int timeFrames = 0;
    bool timeReset = false;
    std::uint32_t shadowTileCap = render::kMaxShadowTile;
    bool indirectProbes = false;
    bool probeFastStart = true;
    int indirectFrames = 1;
    unsigned bakePasses = 4;
    lighting::BakerOptions bakeOptions{.bounces = 8, .bounceReflectivity = lighting::kVradBounceReflectivity};
    float bakeScale = render::kDefaultBakedScale;
    float vradFallbackScale = 0.62f; // Preserve VRAD CLI output when --bake-scale is omitted.
    std::optional<float> vradScale; // --vrad-scale; else --bake-scale.
    editor::VradOptions vrad;
    bool keepVrad = false;
    bool vradMask = false;
    bool noSpecular = false;
    float toolBrightness = 0.0f; // --tool-fixed-brightness; 0 = off.
    std::string mode = "lit";
    std::vector<std::string> sources; // --compare A,B: the light sources written side by side as N-compare.png.
    double split = 0.5;               // --compare ...,split: the fraction of the width that shows source A.
    bool density = false;             // --density: the lattice checker in place of baked/VRAD radiance.
    bool expandedInstances = false; // --instances expanded: bake and draw the collapsed document.
    fs::path baselineVmf, baselineBsp; // The hybrid's VRAD baseline: the map before the edit, and a kept BSP of it.
    double rebakeReach = 1;            // --rebake-reach: BakeSessionOptions::rebakeReach for the hybrid.
    bool bounceRing = false;           // --bounce-ring: the hybrid rebakes its bounce ring too.
    std::vector<std::string> diff;     // --diff A,B: per-luxel comparison of two sources on the --vmf's charts.
    double diffRange = 1;              // --diff-range: |log2(A/B)| drawn at full colour.
    std::vector<Pose> poses;
};

void help() {
    std::cout << "Usage: apex_render_pose --vmf PATH --game DIR [--width 1920 --height 1080]\n"
                 "  [--fov 90] [--mode lit|textured|fullbright|baked|vrad]\n"
                 "  [--indirect off|probes] [--indirect-frames N] [--no-probe-fast-start] [--time-reset] [--shadow-tile-cap 256|512|1024]\n"
                 "  [--compare A,B[,split]] [--density] [--cubemaps DIR] [--time N]\n"
                 "  [--bake-passes 4 --bake-bounces 8 --bake-bounce-rays 32 --bake-threads 3 --bake-seed 0 --bake-scale 1]\n"
                 "  [--bake-reflectivity 0.97]\n"
                 "  [--vrad-flags \"-noextra\"] [--no-vis] [--vrad-scale S] [--keep-vrad] [--vrad-bsp PATH] [--vrad-mask]\n"
                 "  [--no-specular] [--tool-fixed-brightness [VALUE]] [--instances mirror|expanded]\n"
                 "  [--baseline-vmf PATH [--baseline-bsp PATH]] [--rebake-reach 1] [--bounce-ring]\n"
                 "  [--diff A,B [--diff-range 1]]\n"
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
                 "Baked uses CPU brush direct + sky + bounce. --bake-scale is display-only (default 1).\n"
                 "--bake-reflectivity scales every albedo in bounce transport (default: the VRAD-parity fit).\n"
                 "--instances mirror|expanded (Baked only): mirror (default) bakes an expanded copy and draws the\n"
                 "instances through the viewport's own instance path, as the editor does; expanded collapses the\n"
                 "instances into the document first and draws that.\n"
                 "Vrad runs the editor's VRAD round trip (native vbsp, vvis -fast, vrad in a fresh temp directory) on\n"
                 "the VMF with instances expanded, then draws VRAD's own lightmaps through the Baked path; faces VRAD\n"
                 "gave no data (displacements, nolight, tools) are black. --vrad-flags replaces the extra vrad\n"
                 "arguments (default -noextra), --no-vis skips vvis, --vrad-scale is the display scale (else\n"
                 "--bake-scale), --keep-vrad keeps the temp directory and prints it. --vrad-bsp reuses a kept\n"
                 "BSP from the identical VMF and flags; the caller is responsible for cache validity.\n"
                 "--indirect probes enables realtime diffuse probes in Lit; --indirect-frames N accumulates N frames per pose.\n"
                 "--no-specular disables direct-light highlights for image comparisons in Lit or Baked.\n"
                 "--tool-fixed-brightness [VALUE] shows tools/* faces (normally hidden here) at a fixed linear\n"
                 "brightness in Lit or Baked instead of lighting (default VALUE 1, the Textured view's level).\n"
                 "Hybrid (--mode hybrid, or a --compare or --diff source) compiles --baseline-vmf, the map before the edit\n"
                 "(--baseline-bsp reuses a kept BSP of it), with the VRAD round trip, seeds the bake from that result as the\n"
                 "editor's Hybrid view does, and bakes --vmf until idle (--bake-passes per level).\n"
                 "--diff A,B (baked, hybrid or vrad) resamples both sources onto the --vmf's baker charts and compares them\n"
                 "per luxel: it prints a summary, writes luxels-A-B.csv (read by tools/lightmap_diff_report.py) and, per pose,\n"
                 "NAME-diff-A-B.png: a matte of log2(A/B) in luminance, white where equal, red where A is brighter, blue where\n"
                 "it is darker, in bands of 1/8 of --diff-range (default 1, so full colour is 2x or 0.5x), grey where B is\n"
                 "below the floor or VRAD gave no data. With hybrid it also writes NAME-hybrid-class.png: rebaked faces\n"
                 "orange, the bounce ring yellow, faces frozen at VRAD's values grey.\n"
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
        if (flag == "--tool-fixed-brightness") {
            o.toolBrightness = render::kToolFixedBrightness;
            if (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--") {
                const double value = number(next());
                if (value <= 0 || value > std::numeric_limits<float>::max())
                    throw std::runtime_error("--tool-fixed-brightness must be positive");
                o.toolBrightness = static_cast<float>(value);
            }
        }
        else if (flag == "--bake-passes") o.bakePasses = count(1, 10000);
        else if (flag == "--no-specular") o.noSpecular = true;
        else if (flag == "--bake-bounces") o.bakeOptions.bounces = count(0, 32);
        else if (flag == "--bake-bounce-rays") o.bakeOptions.bounceRays = count(1, 65536);
        else if (flag == "--bake-threads") o.bakeOptions.threads = count(1, 256);
        else if (flag == "--bake-seed") o.bakeOptions.seed = count(0, 0xFFFFFFFFu);
        else if (flag == "--bake-reflectivity") {
            const double value = number(next());
            if (!(value >= 0 && value <= 1)) throw std::runtime_error("--bake-reflectivity must be in [0,1]");
            o.bakeOptions.bounceReflectivity = value;
        }
        else if (flag == "--bake-scale") {
            const double value = number(next());
            if (value < 0 || value > std::numeric_limits<float>::max())
                throw std::runtime_error("--bake-scale must be finite and nonnegative");
            o.bakeScale = static_cast<float>(value);
            o.vradFallbackScale = o.bakeScale;
        }
        else if (flag == "--vrad-flags") o.vrad.flags = next();
        else if (flag == "--no-vis") o.vrad.vis = false;
        else if (flag == "--keep-vrad") o.keepVrad = true;
        else if (flag == "--vrad-bsp") o.vradBsp = next();
        else if (flag == "--vrad-mask") o.vradMask = true;
        else if (flag == "--vrad-scale") {
            const double value = number(next());
            if (value < 0 || value > std::numeric_limits<float>::max())
                throw std::runtime_error("--vrad-scale must be finite and nonnegative");
            o.vradScale = static_cast<float>(value);
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
        else if (flag == "--time-reset") o.timeReset = true;
        else if (flag == "--shadow-tile-cap") {
            o.shadowTileCap = count(256, 1024);
            if (o.shadowTileCap != 256 && o.shadowTileCap != 512 && o.shadowTileCap != 1024)
                throw std::runtime_error("--shadow-tile-cap takes 256, 512 or 1024");
        }
        else if (flag == "--indirect") {
            const auto value = next();
            if (value != "off" && value != "probes") throw std::runtime_error("--indirect takes off or probes");
            o.indirectProbes = value == "probes";
        }
        else if (flag == "--no-probe-fast-start") o.probeFastStart = false;
        else if (flag == "--indirect-frames") {
            const double value = number(next());
            if (value < 1 || value > 10000 || std::floor(value) != value)
                throw std::runtime_error("--indirect-frames takes a frame count in [1, 10000]");
            o.indirectFrames = static_cast<int>(value);
        }
        else if (flag == "--baseline-vmf") o.baselineVmf = next();
        else if (flag == "--baseline-bsp") o.baselineBsp = next();
        else if (flag == "--rebake-reach") {
            o.rebakeReach = number(next());
            if (!(o.rebakeReach > 0 && o.rebakeReach <= 16)) throw std::runtime_error("--rebake-reach must be in (0, 16]");
        }
        else if (flag == "--bounce-ring") o.bounceRing = true;
        else if (flag == "--diff") {
            std::stringstream list(next());
            for (std::string item; std::getline(list, item, ',');) o.diff.push_back(item);
            if (o.diff.size() != 2 || o.diff[0] == o.diff[1]) throw std::runtime_error("--diff takes two different sources A,B");
            for (const auto& source : o.diff)
                if (source != "baked" && source != "hybrid" && source != "vrad")
                    throw std::runtime_error("--diff sources are baked, hybrid or vrad");
        }
        else if (flag == "--diff-range") {
            o.diffRange = number(next());
            if (!(o.diffRange > 0 && o.diffRange <= 8)) throw std::runtime_error("--diff-range must be in (0, 8]");
        }
        else if (flag == "--mode") o.mode = next();
        else if (flag == "--density") o.density = true;
        else if (flag == "--compare") {
            std::stringstream list(next());
            for (std::string item; std::getline(list, item, ',');) o.sources.push_back(item);
            if (o.sources.size() == 3) {
                o.split = number(o.sources.back());
                o.sources.pop_back();
                if (!(o.split > 0 && o.split < 1)) throw std::runtime_error("--compare split must be in (0, 1)");
            }
            if (o.sources.size() != 2) throw std::runtime_error("--compare takes A,B[,split]");
            for (const auto& source : o.sources)
                if (source != "lit" && source != "baked" && source != "vrad" && source != "hybrid")
                    throw std::runtime_error("--compare sources are lit, baked, vrad or hybrid");
            o.mode = "compare";
        }
        else if (flag == "--instances") {
            const auto value = next();
            if (value != "mirror" && value != "expanded") throw std::runtime_error("--instances takes mirror or expanded");
            o.expandedInstances = value == "expanded";
        }
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
    if (o.mode != "lit" && o.mode != "textured" && o.mode != "fullbright" && o.mode != "baked" &&
        o.mode != "vrad" && o.mode != "hybrid" && o.mode != "compare")
        throw std::runtime_error("Mode must be lit, textured, fullbright, baked, vrad or hybrid");
    const bool hybrid = o.mode == "hybrid" || std::ranges::find(o.sources, "hybrid") != o.sources.end() ||
                        std::ranges::find(o.diff, "hybrid") != o.diff.end();
    if (hybrid == o.baselineVmf.empty()) throw std::runtime_error("--baseline-vmf is required for, and only used by, hybrid");
    if (!o.baselineBsp.empty() && o.baselineVmf.empty()) throw std::runtime_error("--baseline-bsp requires --baseline-vmf");
    if (!o.diff.empty() && o.density) throw std::runtime_error("--diff cannot be combined with --density");
    if (o.vradMask && o.mode != "vrad") throw std::runtime_error("--vrad-mask requires --mode vrad");
    if (o.vradMask && o.density) throw std::runtime_error("--vrad-mask cannot be combined with --density");
    return o;
}

std::string fileName(const Pose& pose, const Options& o) {
    return pose.name + (o.mode == "lit" ? "" : "-" + o.mode) + (o.density ? "-density" : "") + ".png";
}

template <typename T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().what());
    return std::move(*result);
}
void check(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().what());
}

std::unique_ptr<document::Document> loadDocument(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open VMF: " + path.string());
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    auto parsed = vmf::parse(text);
    if (!parsed) parsed = vmf::parse(text, {kv1::EscapeMode::Legacy});
    return std::make_unique<document::Document>(take(std::move(parsed)));
}

float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

Vec3 srgbToLinear(Vec3 c) {
    const auto channel = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {channel(c.x), channel(c.y), channel(c.z)};
}

// The --diff matte colour of log2(A/B) / range, in bands of 1/8 so equal-ratio contours read at a glance.
Vec3 divergingColour(double t) {
    t = std::round(std::clamp(t, -1.0, 1.0) * 8) / 8;
    const Vec3 white(0.95f), brighter(0.80f, 0.10f, 0.10f), darker(0.10f, 0.35f, 0.85f);
    return srgbToLinear(t >= 0 ? glm::mix(white, brighter, float(t)) : glm::mix(white, darker, float(-t)));
}

double segmentDistance(DVec3 p, DVec3 a, DVec3 b) {
    const DVec3 ab = b - a;
    const double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-12), 0.0, 1.0);
    return glm::length(p - (a + t * ab));
}

// Distance from p to a convex planar polygon (either winding).
double polygonDistance(DVec3 p, const lighting::BakeFace& face) {
    const auto& loop = face.vertices;
    const double height = glm::dot(p - loop[0], face.normal);
    const DVec3 q = p - height * face.normal;
    int positive = 0, negative = 0;
    double edge = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const DVec3 a = loop[i], b = loop[(i + 1) % loop.size()];
        (glm::dot(glm::cross(b - a, q - a), face.normal) >= 0 ? positive : negative)++;
        edge = std::min(edge, segmentDistance(p, a, b));
    }
    return positive == 0 || negative == 0 ? std::abs(height) : edge;
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

    const auto diffName = [&](const Pose& pose) { return pose.name + "-diff-" + o.diff[0] + "-" + o.diff[1] + ".png"; };
    const auto className = [](const Pose& pose) { return pose.name + "-hybrid-class.png"; };
    const bool hybridDiff = std::ranges::find(o.diff, "hybrid") != o.diff.end();
    const std::string luxelsName = o.diff.empty() ? "" : "luxels-" + o.diff[0] + "-" + o.diff[1] + ".csv";
    if (!o.diff.empty()) {
        if (fs::exists(out / luxelsName)) throw std::runtime_error("Output already exists: " + luxelsName);
        for (const auto& pose : o.poses) {
            if (fs::exists(out / diffName(pose))) throw std::runtime_error("Output already exists: " + diffName(pose));
            if (hybridDiff && fs::exists(out / className(pose)))
                throw std::runtime_error("Output already exists: " + className(pose));
        }
    }
    const auto documentOwner = loadDocument(o.vmf);
    document::Document& document = *documentOwner;
    assets::GameProfile profile;
    profile.gameDirectory = game / "p2ce";
    profile.addons.list = assets::AddonSettings::List::Game;
    JobPool jobs(2);
    auto vfs = take(assets::Vfs::load(profile, jobs));
    assets::MaterialCache materials(vfs, jobs);
    assets::ModelCache models(vfs, jobs);
    editor::InstanceTemplates templates;
    editor::InstanceExpansion instances(templates);
    const auto uses = [&](const char* source) {
        return o.mode == source || std::ranges::find(o.sources, source) != o.sources.end() ||
               std::ranges::find(o.diff, source) != o.diff.end();
    };
    if ((uses("baked") || uses("vrad")) && o.expandedInstances) tools::expandBakeInstances(document, fs::canonical(o.vmf), profile);
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
    std::shared_ptr<render::BakedLightmaps> bakeMaps, vradMaps, hybridMaps;
    std::vector<std::vector<Vec3>> vradRadiance, maskRadiance;
    // Radiance per source on the --vmf's baker charts, for --diff; vradCovered marks the faces VRAD gave data.
    std::map<std::string, std::vector<std::vector<Vec3>>> charted;
    std::vector<char> vradCovered;
    std::optional<lighting::BakeDiff> rebaked; // The hybrid's restart diff against its baseline.
    std::optional<render::LightmapAtlas> editAtlas;
    lighting::BakeScene editScene;
    if (uses("baked") || uses("hybrid") || !o.diff.empty()) {
        // The bake sees the expansion; the mirror keeps the original and draws the instances itself.
        auto bakeDocument = document.snapshot();
        const auto origins = o.expandedInstances ? std::vector<document::InstanceSolidOrigin>{} :
            tools::expandBakeInstances(*bakeDocument, fs::canonical(o.vmf), profile);
        editScene = tools::setupBake(*bakeDocument, &vfs).scene;
        editAtlas = take(render::LightmapAtlas::pack(editScene.faces));
        editAtlas->bindInstances(origins);
        std::size_t placedSolids = 0;
        for (const auto& [placement, solids] : editAtlas->instances) placedSolids += solids.size();
        std::cout << "bake instances placements=" << editAtlas->instances.size() << " baked-solids=" << placedSolids << '\n';
    }
    const auto chartMaps = [&](const std::vector<std::vector<Vec3>>& radiance) {
        auto maps = take(render::BakedLightmaps::create(*context, *heap, *editAtlas));
        check(maps->upload(radiance));
        return maps;
    };
    if (uses("baked")) {
        lighting::Baker baker(editScene, o.bakeOptions);
        for (unsigned level = 0; level <= o.bakeOptions.bounces; ++level) {
            for (unsigned pass = 0; pass < o.bakePasses; ++pass) {
                const auto step = level == 0 ? baker.step() : baker.bounce(level);
                if (!step.passComplete) throw std::runtime_error("Incomplete bake pass");
            }
            std::cout << "bake level=" << level << " passes=" << o.bakePasses << std::endl;
        }
        charted["baked"] = baker.resolve();
        bakeMaps = chartMaps(o.density ? render::densityRadiance(*editAtlas) : charted["baked"]);
    }
    struct VradRun {
        render::VradLightmaps loaded;
        std::unique_ptr<document::Document> exported; // The instance-expanded copy that was compiled.
        std::vector<document::InstanceSolidOrigin> origins;
    };
    const auto runVrad = [&](const document::Document& source, const fs::path& vmf, const fs::path& cached,
                             const char* label) {
        VradRun run;
        const auto started = std::chrono::steady_clock::now();
        if (cached.empty()) {
            compile::BlockingProcessRunner runner;
            compile::FilesystemFileOps files;
            editor::VradRoundTrip trip(runner, files);
            if (o.keepVrad) trip.keepFiles();
            editor::CompileGamePaths paths;
            paths.gameDirectory = profile.gameDirectory;
            paths.toolDirectory = profile.toolDirectory();
            paths.instanceRoot = profile.instanceSearchRoot();
            paths.contentGameInfo = take(vfs.compileGameInfo());
            for (const auto& [variable, tool] : {std::pair{"bsp_exe", "vbsp"}, {"vis_exe", "vvis"}, {"light_exe", "vrad"}})
                paths.tools[variable] = profile.toolPath(tool).string();
            check(trip.start(source, fs::canonical(vmf), paths, o.vrad));
            if (trip.poll() != editor::VradRoundTrip::State::Done)
                throw std::runtime_error("VRAD round trip failed: " + trip.message());
            run.loaded = take(render::loadVradLightmaps(trip.bsp(), *trip.exported(), trip.origins()));
            run.exported = trip.exported()->snapshot();
            run.origins = trip.origins();
            std::cout << label << " flags=\"" << o.vrad.flags << "\" vis=" << o.vrad.vis;
            if (o.keepVrad) std::cout << " directory=" << trip.directory().string();
        } else {
            run.exported = source.snapshot();
            run.origins = tools::expandBakeInstances(*run.exported, fs::canonical(vmf), profile);
            run.loaded = take(render::loadVradLightmaps(cached, *run.exported, run.origins));
            std::cout << label << " cached-bsp=" << cached.string() << " (flags not verified)";
        }
        const auto& stats = run.loaded.stats;
        std::cout << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                  << " fragments=" << stats.fragments << " mapped=" << stats.mapped << " faces=" << stats.faces
                  << " displacement=" << stats.displacement << " no-data=" << stats.noData
                  << " unowned=" << stats.unowned << " shared=" << stats.shared
                  << " mismatched=" << stats.mismatched << '\n';
        return run;
    };
    if (uses("vrad")) {
        auto run = runVrad(document, o.vmf, o.vradBsp, "vrad");
        auto& loaded = run.loaded;
        const document::Document* mappedDocument = run.exported.get();
        if (o.vradMask) {
            // lights.rad emitters are not necessarily marked $selfillum in their VMT.
            // Remove their atlas rows from the validity pass, not from the reference image.
            std::ifstream rad(profile.gameDirectory / "lights.rad");
            if (!rad) throw std::runtime_error("--vrad-mask requires the game's lights.rad");
            std::set<std::string> emitters;
            const auto lower = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                return value;
            };
            for (std::string line; std::getline(rad, line);) {
                std::istringstream fields(line);
                std::string material;
                double energy = 0;
                if (fields >> material >> energy && !material.starts_with("//") && energy > 0)
                    emitters.insert(lower(material));
            }
            std::set<std::pair<std::uint64_t, std::uint32_t>> emissiveSides;
            const auto walk = [&](const auto& self, document::NodeId id) -> void {
                const auto handle = mappedDocument->handle(id);
                if (!handle) return;
                if (const auto* solid = std::get_if<Handle<document::Solid>>(&*handle)) {
                    const auto* node = mappedDocument->get(*solid);
                    for (const auto* side : node->block->findAll("side")) {
                        const auto* sideId = side->find("id");
                        const auto* material = side->find("material");
                        if (!sideId || !material) continue;
                        const auto value = sideId->asInt();
                        if (value && emitters.contains(lower(material->decodedValue().value_or(""))))
                            emissiveSides.emplace(node->id, std::uint32_t(*value));
                    }
                }
                for (const auto child : mappedDocument->get(*handle)->children) self(self, child);
            };
            for (const auto root : mappedDocument->roots()) walk(walk, root);
            vradRadiance = loaded.radiance;
            maskRadiance = loaded.radiance;
            std::size_t excluded = 0;
            for (std::size_t i = 0; i < loaded.atlas.faces.size(); ++i) {
                const auto& face = loaded.atlas.faces[i];
                if (!emissiveSides.contains({face.solidId, face.sideId})) continue;
                std::fill(maskRadiance[i].begin(), maskRadiance[i].end(), Vec3(0));
                ++excluded;
            }
            std::cout << "vrad mask texlight materials=" << emitters.size() << " sides=" << emissiveSides.size()
                      << " excluded-fragments=" << excluded << '\n';
        }
        if (editAtlas) charted["vrad"] = editAtlas->retainRadiance(&loaded.atlas, loaded.radiance, &vradCovered);
        auto maps = take(render::BakedLightmaps::create(*context, *heap, std::move(loaded.atlas)));
        check(maps->upload(o.density ? render::densityRadiance(maps->atlas()) : loaded.radiance));
        vradMaps = std::move(maps);
    }
    if (uses("hybrid")) {
        const auto baseline = loadDocument(o.baselineVmf);
        const auto base = runVrad(*baseline, o.baselineVmf, o.baselineBsp, "baseline vrad");
        lighting::BakeSessionOptions options{.baker = o.bakeOptions, .initialPasses = std::min(4u, o.bakePasses),
                                             .maxPasses = o.bakePasses, .freezeBounceDirty = true,
                                             .rebakeReach = o.rebakeReach, .rebakeBounceRing = o.bounceRing};
        // As the editor's Hybrid view seeds it: the exported copy is already expanded, so it is set up as is.
        auto seedSetup = tools::setupBake(*base.exported, &vfs);
        auto seedAtlas = take(render::LightmapAtlas::pack(seedSetup.scene.faces));
        seedAtlas.bindInstances(base.origins);
        auto seed = render::hybridSeed(seedAtlas, seedSetup.scene, base.loaded, options);
        const auto started = std::chrono::steady_clock::now();
        lighting::BakeSession session(lighting::BakeScene{}, options);
        rebaked = session.restart(std::move(seed), editScene, options, 1, editAtlas->faceKeys());
        while (!session.progress().idle) session.advance(4096);
        std::size_t ring = 0;
        for (std::size_t i = 0; i < rebaked->dirty.size(); ++i) ring += rebaked->bounceDirty[i] && !rebaked->dirty[i];
        std::cout << "hybrid faces=" << editScene.faces.size() << " rebaked=" << rebaked->dirtyCount
                  << " frozen-ring=" << ring << " full=" << rebaked->full
                  << (rebaked->full ? " reason=\"" + rebaked->reason + "\"" : std::string())
                  << " reach=" << o.rebakeReach << " bounce-ring=" << o.bounceRing << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
        charted["hybrid"] = session.baker().resolve();
        hybridMaps = chartMaps(o.density ? render::densityRadiance(*editAtlas) : charted["hybrid"]);
    }
    // --diff: A against B per luxel on the --vmf's charts. The matte and the CSV share one validity rule.
    std::vector<std::vector<Vec3>> matte, classes;
    if (!o.diff.empty()) {
        // Below this linear luminance in B the ratio is mostly noise; such luxels are grey and marked invalid.
        constexpr float kDiffFloor = 0.002f;
        constexpr double kOpen = 256; // Corner distances are capped here: "no facing surface nearby".
        const auto& faces = editAtlas->faces;
        const auto& a = charted.at(o.diff[0]);
        const auto& b = charted.at(o.diff[1]);
        const bool needsVrad = std::ranges::find(o.diff, "vrad") != o.diff.end();
        std::vector<DVec3> centres(faces.size());
        std::vector<double> radii(faces.size());
        for (std::size_t i = 0; i < faces.size(); ++i) {
            DVec3 sum{0};
            for (const auto& v : faces[i].vertices) sum += v;
            centres[i] = sum / double(std::max<std::size_t>(faces[i].vertices.size(), 1));
            for (const auto& v : faces[i].vertices) radii[i] = std::max(radii[i], glm::length(v - centres[i]));
        }
        std::ofstream csv(out / luxelsName);
        if (!csv) throw std::runtime_error("Cannot create output: " + (out / luxelsName).string());
        csv << "face,solid,side,s,t,x,y,z,nx,ny,nz,area,class,edge,corner,dirty_gap,a_r,a_g,a_b,b_r,b_g,b_b,valid\n";
        struct Sample { double log2Ratio, area; };
        std::vector<Sample> samples;
        const Vec3 invalid = srgbToLinear(Vec3(0.45f));
        const Vec3 rebakedColour = srgbToLinear({1.0f, 0.55f, 0.1f}), ringColour = srgbToLinear({0.95f, 0.85f, 0.2f}),
                   frozenColour = srgbToLinear(Vec3(0.35f));
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const auto& face = faces[i];
            const auto& chart = face.chart;
            const double area = glm::length(glm::cross(chart.luxelToWorld[0], chart.luxelToWorld[1]));
            const int cls = rebaked ? (rebaked->dirty[i] ? 2 : rebaked->bounceDirty[i] ? 1 : 0) : -1;
            auto& row = matte.emplace_back(a[i].size(), invalid);
            if (rebaked) classes.emplace_back(a[i].size(), cls == 2 ? rebakedColour : cls == 1 ? ringColour : frozenColour);
            // Faces near enough to matter for the corner and dirty-distance columns.
            std::vector<std::size_t> facing, dirty;
            for (std::size_t j = 0; j < faces.size(); ++j) {
                if (j == i || glm::length(centres[j] - centres[i]) - radii[j] - radii[i] > kOpen) continue;
                if (glm::dot(face.normal, centres[j] - centres[i]) > 0.5 && glm::dot(faces[j].normal, centres[i] - centres[j]) > -radii[j])
                    facing.push_back(j);
            }
            if (rebaked) for (std::size_t j = 0; j < faces.size(); ++j) if (rebaked->dirty[j]) dirty.push_back(j);
            const int width = chart.dimensions[0];
            for (std::size_t luxel = 0; luxel < a[i].size(); ++luxel) {
                const int st[2] = {int(luxel) % width, int(luxel) / width};
                const DVec3 p = chart.worldAt(DVec2(st[0], st[1]));
                double edge = std::numeric_limits<double>::infinity();
                for (std::size_t k = 0; k < face.vertices.size(); ++k)
                    edge = std::min(edge, segmentDistance(p, face.vertices[k], face.vertices[(k + 1) % face.vertices.size()]));
                double corner = kOpen;
                for (const auto j : facing)
                    if (glm::dot(faces[j].normal, p - faces[j].vertices[0]) > 0.5)
                        corner = std::min(corner, polygonDistance(p, faces[j]));
                double dirtyGap = -1;
                if (rebaked) {
                    dirtyGap = cls == 2 ? 0 : std::numeric_limits<double>::infinity();
                    if (cls != 2) for (const auto j : dirty) dirtyGap = std::min(dirtyGap, polygonDistance(p, faces[j]));
                }
                const Vec3 va = a[i][luxel], vb = b[i][luxel];
                const bool valid = (!needsVrad || vradCovered[i]) && luminance(vb) >= kDiffFloor && luminance(va) > 0;
                if (valid) {
                    const double ratio = std::log2(double(luminance(va)) / double(luminance(vb)));
                    samples.push_back({ratio, area});
                    row[luxel] = divergingColour(ratio / o.diffRange);
                }
                csv << i << ',' << face.solidId << ',' << face.sideId << ',' << st[0] << ',' << st[1] << ','
                    << p.x << ',' << p.y << ',' << p.z << ',' << face.normal.x << ',' << face.normal.y << ','
                    << face.normal.z << ',' << area << ',' << cls << ',' << edge << ',' << corner << ',' << dirtyGap << ','
                    << va.x << ',' << va.y << ',' << va.z << ',' << vb.x << ',' << vb.y << ',' << vb.z << ',' << valid << '\n';
            }
        }
        // Area-weighted quantiles of log2(A/B), printed as ratios.
        std::ranges::sort(samples, {}, &Sample::log2Ratio);
        double total = 0, within10 = 0, within25 = 0;
        for (const auto& sample : samples) {
            total += sample.area;
            if (std::abs(sample.log2Ratio) <= std::log2(1.1)) within10 += sample.area;
            if (std::abs(sample.log2Ratio) <= std::log2(1.25)) within25 += sample.area;
        }
        const auto quantile = [&](double q) {
            double seen = 0;
            for (const auto& sample : samples) if ((seen += sample.area) >= q * total) return std::exp2(sample.log2Ratio);
            return std::numeric_limits<double>::quiet_NaN();
        };
        std::cout << "diff " << o.diff[0] << '/' << o.diff[1] << " luxels=" << samples.size() << " floor=" << kDiffFloor
                  << " p5=" << quantile(0.05) << " p25=" << quantile(0.25) << " median=" << quantile(0.5)
                  << " p75=" << quantile(0.75) << " p95=" << quantile(0.95)
                  << " within10%=" << (total > 0 ? within10 / total : 0) << " within25%=" << (total > 0 ? within25 / total : 0)
                  << " csv=" << luxelsName << '\n';
    }
    // The sources to draw, in order; a comparison draws each and joins the two images.
    const std::vector<std::string> modes = o.sources.empty() ? std::vector<std::string>{o.mode} : o.sources;
    const auto bind = [&](const std::string& mode) {
        mirror.setLightmaps(mode == "baked" ? bakeMaps : mode == "vrad" ? vradMaps : mode == "hybrid" ? hybridMaps : nullptr);
    };
    bind(modes.front());
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
    const auto write = [&](const QImage& png, const std::string& name) {
        const auto path = out / name;
        QFile file(QString::fromStdString(path.string()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw std::runtime_error("Cannot create output: " + path.string());
        if (!png.save(&file, "PNG")) throw std::runtime_error("PNG write failed: " + path.string());
    };
    const auto viewOptions = [&](const Pose& pose, const std::string& mode) {
        viewport::Camera camera;
        camera.position = pose.position;
        camera.angles = pose.angles;
        camera.fovYDegrees = viewport::sourceFovYDegrees(o.fov);
        render::MirrorViewOptions options;
        options.mode = mode == "textured" ? render::DisplayMode::Textured :
            (mode == "baked" || mode == "vrad" || mode == "hybrid") ? render::DisplayMode::Baked : render::DisplayMode::Lit;
        options.showEntityHelpers = false;
        options.showNodraw = false;
        options.showToolFaces = o.toolBrightness > 0.0f; // The game draws no trigger, clip or hint faces.
        options.toolFixedBrightness = o.toolBrightness > 0.0f;
        options.modelEdgeOverlay = false;
        options.view.cameraPosition = camera.position;
        options.view.viewProjection = camera.projectionMatrix(double(o.width) / o.height) * camera.viewMatrix();
        options.view.cameraRight = Vec3(camera.basis().right);
        options.view.cameraUp = Vec3(camera.basis().up);
        return options;
    };
    // The analysis passes (validity mask, difference matte) draw opaque mapped brushes only.
    const auto analysisOnly = [](render::DrawList& list) {
        list.sky.enabled = false;
        list.sky.camera.reset();
        list.fog.enabled = false;
        list.boxes.clear();
        list.sprites.clear();
        list.ghosts.clear();
        for (auto& mesh : list.meshes) {
            if (mesh.alpha != render::AlphaMode::Opaque || mesh.sky || mesh.skySurface || mesh.toolFixed)
                mesh.bakedBrush = false;
            // Conservative occlusion for transparent/alpha-tested objects: exclude
            // their whole footprint instead of letting a hidden surface count.
            mesh.alpha = render::AlphaMode::Opaque;
        }
    };
    std::vector<std::vector<QImage>> drawn(modes.size());
    for (std::size_t sourceIndex = 0; sourceIndex < modes.size(); ++sourceIndex) {
    const std::string& mode = modes[sourceIndex];
    if (sourceIndex) {
        bind(mode);
        mirror.buildDrawList(warmup);
        if (!mirror.settle(std::chrono::minutes(5))) throw std::runtime_error("RenderMirror settle timed out");
    }
    for (const auto& pose : o.poses) {
        auto list = mirror.buildDrawList(viewOptions(pose, mode));
        list.shadowTileCap = o.shadowTileCap;
        list.bakedScale = o.density ? 1.0f : mode == "vrad" ? o.vradScale.value_or(o.vradFallbackScale) : o.bakeScale;
        if (list.toolBrightness > 0.0f) list.toolBrightness = o.toolBrightness;
        if (o.noSpecular) for (auto& light : list.lights) light.specularMode = 0;
        list.fullbright = mode == "fullbright";
        list.realtimeIndirect = o.indirectProbes && mode == "lit";
        list.probeSettings.fastStart = o.probeFastStart;
        if (list.realtimeIndirect) {
            if (!context->info().rayQuery) throw std::runtime_error("--indirect probes requires a ray-query device");
            renderer->resetIndirect(scene);
            std::cout << "indirect: probes (approximation), frames=" << o.indirectFrames << '\n';
        }
        // Leave exposure at DrawList's default, as ViewportPane does.
        float clear[] = {0.1f, 0.1f, 0.1f, 1.0f};
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
        const auto accumulationStart = std::chrono::steady_clock::now();
        for (int frame = 0; frame < (list.realtimeIndirect ? o.indirectFrames : 1); ++frame) render();
        if (o.timeReset) {
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - accumulationStart).count();
            std::cout << pose.name << " reset-frames=" << (list.realtimeIndirect ? o.indirectFrames : 1)
                      << " reset-seconds=" << seconds << '\n';
        }
        const auto image = take(target.readback());
        const QImage png = QImage(image.pixels.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888).copy();
        if (o.sources.empty()) write(png, fileName(pose, o));
        else drawn[sourceIndex].push_back(png);
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
        if (o.vradMask) {
            check(vradMaps->upload(maskRadiance));
            list.lightmapValidityMask = true;
            analysisOnly(list);
            clear[0] = clear[1] = clear[2] = 0.0f;
            render();
            const auto mask = take(target.readback());
            const QImage maskPng = QImage(mask.pixels.data(), o.width, o.height, o.width * 4,
                                          QImage::Format_RGBA8888).copy();
            write(maskPng, pose.name + "-vrad-mask.png");
            check(vradMaps->upload(vradRadiance));
        }
    }
    }
    if (!o.sources.empty()) {
        // Source A fills the left part of the image and source B the rest, with a white divider.
        const int edge = std::clamp(int(std::lround(o.width * o.split)), 1, o.width - 1);
        for (std::size_t i = 0; i < o.poses.size(); ++i) {
            QImage joined = drawn[1][i];
            for (int y = 0; y < o.height; ++y) {
                std::memcpy(joined.scanLine(y), drawn[0][i].constScanLine(y), std::size_t(edge) * 4);
                for (int x = std::max(0, edge - 1); x < std::min(o.width, edge + 1); ++x)
                    reinterpret_cast<std::uint32_t*>(joined.scanLine(y))[x] = 0xffffffffu;
            }
            write(joined, fileName(o.poses[i], o));
            std::cout << fileName(o.poses[i], o) << " compare=" << o.sources[0] << '|' << o.sources[1]
                      << " split=" << edge << '\n';
        }
    }
    if (!o.diff.empty()) {
        // Each matte is uploaded as a lightmap on the --vmf's charts and drawn as is: no albedo, lighting or exposure.
        const auto drawMatte = [&](const std::vector<std::vector<Vec3>>& colours, const auto& name, bool legend) {
            mirror.setLightmaps(chartMaps(colours));
            mirror.buildDrawList(warmup);
            if (!mirror.settle(std::chrono::minutes(5))) throw std::runtime_error("RenderMirror settle timed out");
            for (const auto& pose : o.poses) {
                auto list = mirror.buildDrawList(viewOptions(pose, "baked"));
                list.lightmapMatte = true;
                list.exposure = 1.0f;
                analysisOnly(list);
                float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
                check(context->oneShot([&](VkCommandBuffer commands) {
                    renderer->prepare(commands, list, scene, clear);
                    target.begin(commands, clear);
                    renderer->record(commands, list);
                    target.end(commands);
                }));
                const auto image = take(target.readback());
                QImage png = QImage(image.pixels.data(), o.width, o.height, o.width * 4, QImage::Format_RGBA8888).copy();
                if (legend) {
                    // The scale along the bottom edge: -range on the left, +range on the right, a tick at equal.
                    const int barWidth = std::min(o.width - 16, 512), barHeight = std::min(o.height / 8, 16);
                    const int left = (o.width - barWidth) / 2, top = o.height - barHeight - 8;
                    for (int x = 0; x < barWidth; ++x) {
                        const Vec3 linear = divergingColour(2.0 * (x + 0.5) / barWidth - 1.0);
                        const auto encode = [](float v) {
                            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                            return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
                        };
                        const bool tick = std::abs(x - barWidth / 2) < 1;
                        const QColor colour = tick ? QColor(0, 0, 0) : QColor(encode(linear.x), encode(linear.y), encode(linear.z));
                        for (int y = 0; y < barHeight; ++y) png.setPixelColor(left + x, top + y, colour);
                    }
                }
                write(png, name(pose));
                std::cout << name(pose) << '\n';
            }
        };
        drawMatte(matte, diffName, true);
        std::cout << "diff matte legend: white A=B; full red A=" << std::exp2(o.diffRange) << "xB, full blue A="
                  << std::exp2(-o.diffRange) << "xB; bands of " << o.diffRange / 8 << " in log2; grey invalid\n";
        if (!classes.empty()) drawMatte(classes, className, false);
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
