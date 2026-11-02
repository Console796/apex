#include "assets/game_profile.hpp"
#include "assets/vmt.hpp"
#include "assets/vtf.hpp"
#include "core/strings.hpp"
#include "document/instance_collapse.hpp"
#include "editor/instance_templates.hpp"
#include "lighting/bake_casters.hpp"
#include "lighting/baker.hpp"
#include "viewport/camera.hpp"

#include <QFile>
#include <QImage>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace {
using namespace apex;
using namespace apex::lighting;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
    fs::path vmf, game, captures, out;
    unsigned passes = 16, bounces = 2, bounceRays = 32, threads = 3, stride = 8;
    std::uint64_t seed = 0;
    bool noAssets = false;
};
struct Pose { DVec3 position, angles; };
const std::array<Pose, 4> poses{{
    {{-1135.97, -1135.97, 64.03}, {-9.13, 34.18, 0}},
    {{-645.80, -1078.80, 64.03}, {-7.55, 177.66, 0}},
    {{-1046.74, -693.74, 64.03}, {-7.03, -83.60, 0}},
    {{-1135.97, -973.72, 64.03}, {-10.20, -9.25, 0}}
}};

void help() {
    std::cout << "Usage: apex_bake_probe --vmf PATH --game DIR --captures DIR [--out DIR]\n"
                 "  [--passes 16 --bounces 2 --bounce-rays 32 --threads 3 --seed 0 --stride 8]\n"
                 "  [--no-assets]\n\n"
                 "CPU-only set-2 probe: 11/12.png, 21/22.png, 31/32.png, 41/42.png (lit/fullbright).\n"
                 "Game DIR is the P2CE install root; defaults to APEX_P2CE_DIR. Mounts game addons.\n"
                 "--no-assets uses linear albedo 0.5 and material-name flags; game still locates instances.\n"
                 "Default output: a new bakeprobe-TIMESTAMP directory in the current directory.\n"
                 "Existing output files are never overwritten. No display or GPU is used.\n"
                 "Camera assumes Source fov_desired 90 at 4:3, widened to 106.26 x 73.74 at 16:9.\n"
                 "One RGB log-least-squares scale fits direct+bounce over all clean sampled pixels;\n"
                 "the same scale is used for direct-only, all poses, channels and lighting subsets.\n"
                 "See tools/bake_probe/README.md for masks, metrics and image colours.\n";
}

Options parse(int argc, char** argv) {
    Options o;
    if (const auto* game = std::getenv("APEX_P2CE_DIR")) o.game = game;
    o.out = "bakeprobe-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&]() -> std::string {
            if (++i == argc) throw std::runtime_error("Missing value for " + flag);
            return argv[i];
        };
        const auto integer = [&](std::uint64_t low, std::uint64_t high) {
            const auto text = next();
            std::uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high)
                throw std::runtime_error("Invalid integer for " + flag + ": " + text);
            return value;
        };
        if (flag == "--vmf") o.vmf = next();
        else if (flag == "--game") o.game = next();
        else if (flag == "--captures") o.captures = next();
        else if (flag == "--out") o.out = next();
        else if (flag == "--passes") o.passes = static_cast<unsigned>(integer(1, 10000));
        else if (flag == "--bounces") o.bounces = static_cast<unsigned>(integer(0, 32));
        else if (flag == "--bounce-rays") o.bounceRays = static_cast<unsigned>(integer(1, 65536));
        else if (flag == "--threads") o.threads = static_cast<unsigned>(integer(1, 256));
        else if (flag == "--stride") o.stride = static_cast<unsigned>(integer(1, 1080));
        else if (flag == "--seed") o.seed = integer(0, std::numeric_limits<std::uint64_t>::max());
        else if (flag == "--no-assets") o.noAssets = true;
        else throw std::runtime_error("Unknown option: " + flag);
    }
    if (o.vmf.empty() || o.captures.empty() || (o.game.empty() && !o.noAssets))
        throw std::runtime_error("--vmf, --captures and --game (or APEX_P2CE_DIR) are required");
    return o;
}

template <typename T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().what());
    return std::move(*result);
}
std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
std::unique_ptr<document::Document> loadDocument(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open VMF: " + path.string());
    std::ostringstream bytes;
    bytes << input.rdbuf();
    auto parsed = vmf::parse(bytes.str());
    if (!parsed) parsed = vmf::parse(bytes.str(), {kv1::EscapeMode::Legacy});
    return std::make_unique<document::Document>(take(std::move(parsed)), 0);
}

// Reuse the editor's resolver and document collapse transform/fixups, with fresh document IDs.
void expandInstances(document::Document& doc, const fs::path& map, const assets::GameProfile& profile) {
    editor::InstanceTemplates templates;
    std::map<const document::Document*, fs::path> paths{{&doc, map}};
    const auto lookup = [&](const document::Document& referring, std::string_view file)
        -> Result<const document::Document*> {
        const auto path = editor::resolveInstanceFile(paths.at(&referring), file, profile.instanceSearchRoot());
        if (!path) return std::unexpected(Error{ErrorCode::NotFound, "Unresolved instance: " + std::string(file)});
        auto loaded = templates.load(*path);
        if (!loaded) return std::unexpected(loaded.error());
        paths[loaded->get()] = *path;
        return loaded->get();
    };
    std::vector<document::NodeId> instances;
    std::optional<document::NodeHandle> world;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = *doc.handle(id);
        const auto* node = doc.get(handle);
        if (ciEquals(node->block->key(), "hidden")) return;
        const auto cls = toLowerAscii(text(*node->block, "classname"));
        if (cls == "func_instance") instances.push_back(id);
        if (cls == "worldspawn") world = handle;
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) walk(walk, root);
    document::InstanceCollapseOptions options;
    for (const auto id : instances) {
        const auto handle = *doc.handle(id);
        const auto* templ = take(lookup(doc, text(*doc.get(handle)->block, "file")));
        auto collapsed = take(document::collapseInstance(doc, id, *templ, lookup, options));
        options.nextAutoName = collapsed.nextAutoName;
        auto fragment = take(kv1::parse(collapsed.fragment));
        auto tx = doc.begin("Probe instance expansion");
        for (const auto& node : fragment.root().children()) {
            const bool brush = ciEquals(node.key(), "solid") || ciEquals(node.key(), "group");
            (void)tx.create(node, brush ? world : std::nullopt);
        }
        tx.destroy(handle);
        tx.commit();
    }
    std::cout << "instances expanded=" << instances.size() << " (recursive collapse; no FGD table, as render_pose)\n";
}

void filterGeometry(document::Document& doc) {
    std::vector<document::NodeHandle> remove;
    const auto walk = [&](const auto& self, document::NodeId id, bool eligible) -> void {
        const auto handle = *doc.handle(id);
        const auto* node = doc.get(handle);
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            const auto cls = toLowerAscii(text(*node->block, "classname"));
            eligible = cls == "worldspawn" || cls == "func_detail";
        }
        if (ciEquals(node->block->key(), "hidden") ||
            (!eligible && std::holds_alternative<Handle<document::Solid>>(handle))) {
            remove.push_back(handle);
            return;
        }
        for (const auto child : node->children) self(self, child, eligible);
    };
    for (const auto root : doc.roots()) walk(walk, root, true);
    auto tx = doc.begin("Probe brush scope");
    for (const auto handle : remove) tx.destroy(handle);
    tx.commit();
    std::cout << "scope: world + func_detail; props/models, displacements, hidden geometry and other brush\n"
                 "entities excluded; sky/nodraw occlude; other tools/ faces neither bake nor occlude.\n";
}

double luminance(DVec3 v) { return glm::dot(v, DVec3{0.2126, 0.7152, 0.0722}); }
double decode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}
DVec3 pixel(const QImage& image, int x, int y) {
    const auto* rgba = image.constScanLine(y) + x * 4;
    return {decode(rgba[0] / 255.0), decode(rgba[1] / 255.0), decode(rgba[2] / 255.0)};
}
void put(QImage& image, int x, int y, DVec3 v) {
    auto* rgba = image.scanLine(y) + x * 4;
    for (int c = 0; c < 3; ++c) rgba[c] = static_cast<uchar>(std::lround(255 * encode(v[c])));
    rgba[3] = 255;
}
struct Capture { QImage lit, fullbright; viewport::Camera camera; };
DVec3 ray(const viewport::Camera& camera, double x, double y, int width, int height) {
    const auto half = camera.halfExtents(double(width) / height);
    const auto basis = camera.basis();
    return glm::normalize(basis.forward + basis.right * ((2 * x / width - 1) * half.x) +
                          basis.up * ((1 - 2 * y / height) * half.y));
}
struct Sample {
    int pose, x, y;
    std::size_t face;
    DVec3 point, baked, direct, lit, fullbright, ratio;
    bool contaminated;
};
using Radiance = std::vector<std::vector<Vec3>>;
DVec3 at(const BakeFace& face, const std::vector<Vec3>& values, DVec3 point) {
    std::array<int, 2> st{};
    for (int axis = 0; axis < 2; ++axis) {
        const double local = glm::dot(point, face.chart.worldToLuxel[axis]) + face.chart.offset[axis] - face.chart.mins[axis];
        st[axis] = static_cast<int>(std::round(std::clamp(local, 0.0, double(face.chart.dimensions[axis] - 1))));
    }
    return DVec3(values[static_cast<std::size_t>(st[1] * face.chart.dimensions[0] + st[0])]);
}
std::optional<Sample> sample(int pose, int x, int y, const Capture& capture,
                             const BakeScene& scene, const Radiance& total, const Radiance& direct) {
    const auto direction = ray(capture.camera, x + 0.5, y + 0.5, capture.lit.width(), capture.lit.height());
    const auto hit = scene.occlusion.accel->closest(Vec3(capture.camera.position), Vec3(direction), 65536);
    if (!hit) return {};
    const auto face = scene.occlusion.faceOf(*hit);
    if (!face) return {};
    const auto point = capture.camera.position + direction * double(hit->distance);
    const auto lit = pixel(capture.lit, x, y), full = pixel(capture.fullbright, x, y);
    if (glm::any(glm::lessThan(full, DVec3{0.02}))) return {};
    const auto albedo = DVec3(scene.faces[*face].albedo);
    const bool contaminated = glm::any(glm::greaterThan(full, 2.0 * albedo + DVec3{0.05})) ||
                              glm::any(glm::greaterThan(lit, DVec3{0.8}));
    return Sample{pose, x, y, *face, point, at(scene.faces[*face], total[*face], point),
                  at(scene.faces[*face], direct[*face], point), lit, full, lit / full, contaminated};
}

double fitScale(const std::vector<Sample>& samples, std::ostream& out) {
    double sum = 0;
    std::size_t count = 0;
    for (const auto& s : samples) if (!s.contaminated)
        for (int c = 0; c < 3; ++c) if (s.baked[c] > 1e-8 && s.ratio[c] > 1e-8) {
            sum += std::log(s.ratio[c]) - std::log(s.baked[c]);
            ++count;
        }
    if (!count) throw std::runtime_error("No positive clean samples for global scale fit");
    out << "global-scale=" << std::exp(sum / double(count)) << " fit-channel-samples=" << count
        << " (multiply baked; shared by direct and total)\n";
    return std::exp(sum / double(count));
}

void report(std::ostream& out, const std::string& label, const std::vector<Sample>& samples, int pose, double scale) {
    std::size_t compared = 0, contaminated = 0;
    for (const auto& s : samples) if (pose < 0 || s.pose == pose) { ++compared; contaminated += s.contaminated; }
    out << label << " compared=" << compared << " contaminated=" << contaminated << " clean=" << compared - contaminated << '\n';
    for (int subset = 0; subset < 3; ++subset) {
        const auto selected = [&](const Sample& s) {
            const bool shadow = luminance(s.direct) <= 1e-6;
            return (pose < 0 || s.pose == pose) && !s.contaminated &&
                   (subset == 0 || (subset == 2 ? shadow : !shadow));
        };
        for (int mode = 0; mode < 2; ++mode) {
            out << "  " << std::array{"all", "direct-lit", "shadow"}[subset] << ' '
                << (mode == 0 ? "direct" : "direct+bounce");
            for (int channel = -1; channel < 3; ++channel) {
                std::vector<double> ratios;
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
                for (const auto& s : samples) if (selected(s)) {
                    const auto baked = scale * (mode == 0 ? s.direct : s.baked);
                    const double x = channel < 0 ? luminance(baked) : baked[channel];
                    const double y = channel < 0 ? luminance(s.ratio) : s.ratio[channel];
                    if (y <= 1e-8) continue;
                    ratios.push_back(x / y);
                    sx += x; sy += y; sxx += x*x; syy += y*y; sxy += x*y;
                }
                out << ' ' << (channel < 0 ? 'Y' : "RGB"[channel]) << "[n=" << ratios.size();
                if (ratios.empty()) { out << " r=nan p10/med/p90=nan]"; continue; }
                const double n = double(ratios.size());
                const double denominator = std::sqrt(std::max(0.0, n*sxx - sx*sx) * std::max(0.0, n*syy - sy*sy));
                std::ranges::sort(ratios);
                const auto quantile = [&](double q) {
                    const double index = q * double(ratios.size() - 1);
                    const auto lo = static_cast<std::size_t>(index);
                    const auto hi = std::min(lo + 1, ratios.size() - 1);
                    return std::lerp(ratios[lo], ratios[hi], index - double(lo));
                };
                out << " r=" << (denominator > 0 ? (n*sxy - sx*sy) / denominator : std::numeric_limits<double>::quiet_NaN())
                    << " p10/med/p90=" << quantile(0.1) << '/' << quantile(0.5) << '/' << quantile(0.9) << ']';
            }
            out << '\n';
        }
    }
}

void openNew(QFile& file, const fs::path& path) {
    file.setFileName(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("Cannot create output: " + path.string());
}
void writeText(const fs::path& path, const std::string& bytes) {
    QFile file;
    openNew(file, path);
    if (file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) || !file.flush())
        throw std::runtime_error("Write failed: " + path.string());
}
void outputs(const Options& o, const std::array<Capture, 4>& captures, const BakeScene& scene,
             const Radiance& total, const Radiance& direct, const std::vector<Sample>& samples, double scale) {
    for (int pose = 0; pose < 4; ++pose) {
        const auto stem = "pose" + std::to_string(pose + 1);
        std::ostringstream csv;
        csv << std::setprecision(9) << "x,y,face,solid,side,hit_x,hit_y,hit_z,baked_r,baked_g,baked_b,direct_r,direct_g,direct_b,"
               "lit_r,lit_g,lit_b,fullbright_r,fullbright_g,fullbright_b,ratio_r,ratio_g,ratio_b,contaminated,shadow\n";
        for (const auto& s : samples) if (s.pose == pose) {
            csv << s.x << ',' << s.y << ',' << s.face << ',' << scene.faces[s.face].solidId << ',' << scene.faces[s.face].sideId;
            for (const auto v : {s.point, s.baked, s.direct, s.lit, s.fullbright, s.ratio})
                for (int c = 0; c < 3; ++c) csv << ',' << v[c];
            csv << ',' << s.contaminated << ',' << (luminance(s.direct) <= 1e-6) << '\n';
        }
        writeText(o.out / (stem + "-samples.csv"), csv.str());
        const auto& capture = captures[pose];
        const int w = capture.lit.width(), h = capture.lit.height();
        QImage image(w * 3, h, QImage::Format_RGBA8888);
        image.fill(Qt::black);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            put(image, x, y, pixel(capture.lit, x, y));
            const auto s = sample(pose, x, y, capture, scene, total, direct);
            if (!s) { put(image, 2*w + x, y, {0.2, 0, 0.2}); continue; }
            put(image, w + x, y, scale * s->baked * s->fullbright);
            if (s->contaminated) { put(image, 2*w + x, y, {0.2, 0, 0.2}); continue; }
            const double error = std::clamp(std::log2(std::max(1e-8, scale * luminance(s->baked)) /
                                                    std::max(1e-8, luminance(s->ratio))) / 2, -1.0, 1.0);
            put(image, 2*w + x, y, {std::max(0.0, error), 1 - std::abs(error), std::max(0.0, -error)});
        }
        QFile file;
        openNew(file, o.out / (stem + "-compare.png"));
        if (!image.save(&file, "PNG") || !file.flush()) throw std::runtime_error("PNG write failed: " + stem);
    }
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}
int run(Options o) {
    o.out = fs::weakly_canonical(o.out);
    if (!o.game.empty()) o.game = fs::canonical(o.game);
    const auto home = std::getenv("HOME");
    if ((!o.game.empty() && within(o.out, o.game)) || within(o.out, fs::canonical(o.captures)) ||
        within(o.out, fs::canonical(o.vmf).parent_path()) || (home && within(o.out, fs::path(home) / "maps/apex")))
        throw std::runtime_error("Output must be outside game, source map and capture directories");
    for (auto parent = o.game; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path())
        if (parent.filename() == "steamapps" && within(o.out, parent / "workshop"))
            throw std::runtime_error("Output must be outside Steam workshop");
    for (int pose = 1; pose <= 4; ++pose)
        for (const auto* suffix : {"-samples.csv", "-compare.png"})
            if (fs::exists(o.out / ("pose" + std::to_string(pose) + suffix)))
                throw std::runtime_error("Output already exists for pose " + std::to_string(pose));
    if (fs::exists(o.out / "summary.txt")) throw std::runtime_error("Output already exists: summary.txt");
    std::array<Capture, 4> captures;
    for (int pose = 0; pose < 4; ++pose) {
        const auto load = [&](int frame) {
            const auto path = o.captures / (std::to_string((pose + 1) * 10 + frame) + ".png");
            QImage image(QString::fromStdString(path.string()));
            if (image.isNull() || image.width() != 1920 || image.height() != 1080)
                throw std::runtime_error("Expected 1920x1080 PNG: " + path.string());
            return image.convertToFormat(QImage::Format_RGBA8888);
        };
        captures[pose] = {load(1), load(2), {}};
        captures[pose].camera.position = poses[pose].position;
        captures[pose].camera.angles = poses[pose].angles;
        captures[pose].camera.fovYDegrees = viewport::sourceFovYDegrees(90);
    }
    const auto start = Clock::now();
    auto doc = loadDocument(o.vmf);
    assets::GameProfile profile;
    profile.gameDirectory = o.game / "p2ce";
    profile.addons.list = assets::AddonSettings::List::Game;
    expandInstances(*doc, fs::canonical(o.vmf), profile);
    filterGeometry(*doc);
    JobPool jobs(2);
    std::optional<assets::Vfs> vfs;
    if (!o.noAssets) {
        auto loaded = assets::Vfs::load(profile, jobs);
        if (!loaded) throw std::runtime_error("Asset mount failed: " + std::string(loaded.error().what()) + "; retry with --no-assets");
        vfs.emplace(std::move(*loaded));
    }
    std::map<std::string, BakeMaterial> materials;
    std::size_t failedMaterials = 0, failedTextures = 0;
    const auto material = [&](std::string_view name) -> BakeMaterial {
        const auto key = toLowerAscii(name);
        if (const auto found = materials.find(key); found != materials.end()) return found->second;
        BakeMaterial result;
        result.sky = key.starts_with("tools/toolsskybox");
        result.nodraw = key == "tools/toolsnodraw";
        result.noLight = key.starts_with("tools/") && !result.sky && !result.nodraw;
        if (vfs) {
            auto vmt = assets::loadMaterial(*vfs, key);
            if (!vmt) {
                ++failedMaterials;
                std::cerr << "material-failed " << key << ": " << vmt.error().what() << " (using fallback)\n";
            } else {
                const auto flag = [&](const char* name) {
                    const auto found = vmt->compileFlags.find(name);
                    return found != vmt->compileFlags.end() && !found->second.empty() && found->second != "0";
                };
                result.sky = result.sky || flag("%compilesky");
                result.nodraw = result.nodraw || flag("%compilenodraw");
                result.noLight = result.noLight || flag("%compilenolight");
                if (!vmt->baseTexture.empty()) {
                    auto texture = assets::loadTexture(*vfs, vmt->baseTexture);
                    if (texture) {
                        for (int c = 0; c < 3; ++c) result.albedo[c] = std::clamp(texture->reflectivity[c], 0.0f, 1.0f);
                    } else {
                        ++failedTextures;
                        std::cerr << "texture-failed " << vmt->baseTexture << ": " << texture.error().what() << " (albedo 0.5)\n";
                    }
                }
            }
        }
        materials.emplace(key, result);
        return result;
    };
    auto scene = take(bakeSceneFromDocument(*doc, {}, material));
    scene.occlusion = take(bakeCastersFromDocument(*doc, scene.faces, material));
    std::cout << "assets=" << (vfs ? "game VFS + addons" : "DISABLED: flat linear albedo 0.5, name flags")
              << " materials=" << materials.size() << " failed-materials=" << failedMaterials << " failed-textures=" << failedTextures << '\n';
    std::map<std::string, std::pair<std::size_t, std::size_t>> classes;
    std::map<std::string, std::vector<document::NodeId>> ignored;
    const auto entities = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = *doc->handle(id);
        const auto* node = doc->get(handle);
        if (std::holds_alternative<Handle<document::Entity>>(handle)) {
            const auto cls = text(*node->block, "classname");
            auto& count = classes[cls];
            ++count.first;
            // Bake IDs encode the source entity ID plus the light part.
            const auto lights = static_cast<std::size_t>(std::ranges::count_if(scene.lights, [&](const auto& light) { return (light.id >> 3) == id; }));
            count.second += lights;
            if (lights == 0) ignored[cls].push_back(id);
        }
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc->roots()) entities(entities, root);
    for (const auto& [cls, count] : classes) {
        std::cout << "entity-class " << cls << " entities=" << count.first << " baked-lights=" << count.second
                  << " ignored-entity-ids=";
        for (const auto id : ignored[cls]) std::cout << id << ',';
        std::cout << '\n';
    }
    std::ostringstream summary;
    summary << std::setprecision(6) << "passes=" << o.passes << " bounces=" << o.bounces << " bounce-rays=" << o.bounceRays
            << " threads=" << o.threads << " seed=" << o.seed << " stride=" << o.stride
            << " assets=" << (vfs ? "mounted" : "disabled") << " failed-materials=" << failedMaterials
            << " failed-textures=" << failedTextures << '\n';
    Baker baker(std::move(scene), {.seed = o.seed, .threads = o.threads, .bounces = o.bounces, .bounceRays = o.bounceRays});
    std::size_t luxels = 0;
    for (const auto& buffer : baker.buffers()) luxels += buffer.radiance.size();
    summary << "scene faces=" << baker.scene().faces.size() << " luxels=" << luxels << " lights=" << baker.scene().lights.size()
            << " setup-seconds=" << std::chrono::duration<double>(Clock::now() - start).count() << '\n';
    for (unsigned level = 0; level <= o.bounces; ++level) {
        const auto begin = Clock::now();
        for (unsigned pass = 0; pass < o.passes; ++pass) {
            const auto step = level == 0 ? baker.step() : baker.bounce(level);
            if (!step.passComplete) throw std::runtime_error("Incomplete bake pass");
        }
        const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::cout << "bake level=" << level << " passes=" << o.passes << " seconds=" << seconds << std::endl;
        summary << "bake level=" << level << " passes=" << o.passes << " seconds=" << seconds << '\n';
    }
    const auto total = baker.resolve(), direct = baker.resolveLevel(0);
    std::vector<Sample> samples;
    summary << "FOV assumption: 106.26 horizontal / 73.74 vertical; roll=0; nearest luxel.\n";
    for (int pose = 0; pose < 4; ++pose) {
        const auto& capture = captures[pose];
        const auto& occlusion = baker.scene().occlusion;
        std::size_t grid = 0, hits = 0, mapped = 0;
        for (int y = 0; y < capture.lit.height(); y += static_cast<int>(o.stride))
            for (int x = 0; x < capture.lit.width(); x += static_cast<int>(o.stride)) {
                ++grid;
                const auto direction = ray(capture.camera, x + 0.5, y + 0.5, capture.lit.width(), capture.lit.height());
                const auto hit = occlusion.accel->closest(Vec3(capture.camera.position), Vec3(direction), 65536);
                if (hit) { ++hits; mapped += occlusion.faceOf(*hit).has_value(); }
                if (auto s = sample(pose, x, y, capture, baker.scene(), total, direct)) samples.push_back(*s);
            }
        summary << "pose" << pose + 1 << " alignment grid=" << grid << " brush-hit-fraction=" << double(hits) / double(grid)
                << " bake-face-fraction=" << double(mapped) / double(grid);
        const auto direction = capture.camera.forward();
        const auto centre = occlusion.accel->closest(Vec3(capture.camera.position), Vec3(direction), 65536);
        if (centre) {
            const auto p = capture.camera.position + direction * double(centre->distance);
            const auto face = occlusion.faceOf(*centre);
            summary << " centre-distance=" << centre->distance << " world=" << p.x << ',' << p.y << ',' << p.z;
            if (face) summary << " face=" << *face << " solid=" << baker.scene().faces[*face].solidId << " side=" << baker.scene().faces[*face].sideId;
            else summary << " face=unmapped";
        } else summary << " centre=miss";
        summary << '\n';
    }
    const double scale = fitScale(samples, summary);
    summary << "Clean statistics; shadow means direct Rec.709 luminance <= 1e-6; r=linear Pearson; ratios=scaled bake/game.\n";
    for (int pose = 0; pose < 4; ++pose) report(summary, "pose" + std::to_string(pose + 1), samples, pose, scale);
    report(summary, "overall", samples, -1, scale);
    std::cout << summary.str() << std::flush;
    fs::create_directories(o.out);
    outputs(o, captures, baker.scene(), total, direct, samples, scale);
    writeText(o.out / "summary.txt", summary.str());
    std::cout << "outputs=" << o.out << " total-seconds=" << std::chrono::duration<double>(Clock::now() - start).count() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { help(); return 0; }
    try { return run(parse(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "apex_bake_probe: " << error.what() << '\n'; return 1; }
}
