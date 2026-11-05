#include "ui/bake_controller.hpp"

#include "editor/bake_instances.hpp"
#include "core/log.hpp"
#include "render/gpu_bake.hpp"
#include "render/bake_setup.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSettings>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
#include <utility>

namespace apex::ui {
namespace {
lighting::BakeSessionOptions readOptions() {
    QSettings settings;
    lighting::BakeSessionOptions options;
    options.baker.bounces = unsigned(std::clamp(settings.value("bake/bounces", int(options.baker.bounces)).toInt(), 0, 8));
    options.baker.bounceRays = unsigned(std::clamp(settings.value("bake/bounceRays", int(options.baker.bounceRays)).toInt(), 1, 1024));
    options.baker.skyRays = unsigned(std::clamp(settings.value("bake/skyRays", int(options.baker.skyRays)).toInt(), 1, 1024));
    options.maxPasses = unsigned(std::clamp(settings.value("bake/maxPasses", int(options.maxPasses)).toInt(), 1, 1024));
    options.rebakeReach = std::clamp(settings.value("bake/rebakeReach", options.rebakeReach).toDouble(), 0.25, 4.0);
    options.rebakeBounceRing = settings.value("bake/hybridBounceRing", options.rebakeBounceRing).toBool();
    return options;
}
bool sameOptions(const lighting::BakeSessionOptions& a, const lighting::BakeSessionOptions& b) {
    return a.baker.bounces == b.baker.bounces && a.baker.bounceRays == b.baker.bounceRays &&
           a.baker.skyRays == b.baker.skyRays && a.maxPasses == b.maxPasses;
}
QString progressText(const lighting::BakeProgress& p, unsigned maximum) {
    if (p.idle) return QStringLiteral("idle");
    QString text = QStringLiteral("direct %1/%2").arg(p.passes[0]).arg(maximum);
    for (std::size_t i = 1; i < p.passes.size(); ++i)
        text += QStringLiteral(", bounce %1 %2/%3").arg(i).arg(p.passes[i]).arg(maximum);
    return text;
}
// Drift guard: bounce reach and light range are cut off at fixed bounds, so after this many incremental
// restarts in a row the next one bakes everything again.
constexpr unsigned kMaxIncrementalRestarts = 16;
// Past this share of dirty faces the hybrid view has drifted far enough that a fresh VRAD run is worth it.
constexpr double kRecommendRerunShare = 0.25;
}

void showBakeSettings(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Bake settings"));
    auto* layout = new QFormLayout(&dialog);
    QSettings settings;
    auto* scale = new QDoubleSpinBox(&dialog);
    scale->setRange(0.01, 16);
    scale->setSingleStep(0.1);
    scale->setValue(settings.value("bake/displayScale", render::kDefaultBakedScale).toDouble());
    layout->addRow(QObject::tr("Display scale"), scale);
    const auto options = readOptions();
    auto* device = new QComboBox(&dialog);
    device->addItem(QObject::tr("Auto"), QStringLiteral("auto"));
    device->addItem(QObject::tr("GPU"), QStringLiteral("gpu"));
    device->addItem(QObject::tr("CPU"), QStringLiteral("cpu"));
    device->setCurrentIndex(std::max(0, device->findData(settings.value("bake/device", "auto"))));
    layout->addRow(QObject::tr("Bake device"), device);
    const auto integer = [&](const QString& label, int value, int low, int high) {
        auto* spin = new QSpinBox(&dialog);
        spin->setRange(low, high);
        spin->setValue(value);
        layout->addRow(label, spin);
        return spin;
    };
    auto* bounces = integer(QObject::tr("Bounces"), int(options.baker.bounces), 0, 8);
    auto* bounceRays = integer(QObject::tr("Bounce rays"), int(options.baker.bounceRays), 1, 1024);
    auto* skyRays = integer(QObject::tr("Sky rays"), int(options.baker.skyRays), 1, 1024);
    auto* passes = integer(QObject::tr("Max passes per level"), int(options.maxPasses), 1, 1024);
    auto* reach = new QDoubleSpinBox(&dialog);
    reach->setRange(0.25, 4);
    reach->setSingleStep(0.25);
    reach->setValue(options.rebakeReach);
    reach->setToolTip(QObject::tr("How far around an edit faces are rebaked. Lower is faster but leaves stale light nearby."));
    layout->addRow(QObject::tr("Rebake reach"), reach);
    auto* ring = new QCheckBox(QObject::tr("Rebake the bounce ring"), &dialog);
    ring->setChecked(options.rebakeBounceRing);
    ring->setToolTip(QObject::tr("Hybrid: fully rebake the faces near an edit instead of keeping their VRAD light."));
    layout->addRow(QObject::tr("Hybrid"), ring);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    // Only values that differ from the defaults are stored, so a later change of default reaches untouched fields.
    const lighting::BakeSessionOptions defaults;
    const auto store = [&](const char* key, const QVariant& value, const QVariant& fallback) {
        if (value == fallback) settings.remove(key);
        else settings.setValue(key, value);
    };
    // The spin boxes round to their displayed decimals; compare at that precision.
    const auto rounded = [](const QDoubleSpinBox* spin, double value) {
        return QVariant(std::round(value * std::pow(10.0, spin->decimals())) / std::pow(10.0, spin->decimals()));
    };
    store("bake/device", device->currentData(), QStringLiteral("auto"));
    store("bake/displayScale", scale->value(), rounded(scale, render::kDefaultBakedScale));
    store("bake/bounces", bounces->value(), int(defaults.baker.bounces));
    store("bake/bounceRays", bounceRays->value(), int(defaults.baker.bounceRays));
    store("bake/skyRays", skyRays->value(), int(defaults.baker.skyRays));
    store("bake/maxPasses", passes->value(), int(defaults.maxPasses));
    store("bake/rebakeReach", reach->value(), rounded(reach, defaults.rebakeReach));
    store("bake/hybridBounceRing", ring->isChecked(), defaults.rebakeBounceRing);
}

BakeController::BakeController(document::Document& document, std::shared_ptr<const assets::Vfs> vfs,
                               render::Context& context, render::TextureHeap& heap,
                               render::RenderMirror& mirror, JobPool& jobs)
    : document_(document), vfs_(std::move(vfs)), context_(context), heap_(heap), mirror_(mirror), jobs_(jobs),
      options_(readOptions()), device_(QSettings().value("bake/device", "auto").toString()) {}
BakeController::~BakeController() { job_.cancel(); }

void BakeController::setDisplayed(bool displayed) {
    displayed_ = displayed;
    if (displayed_) { mirror_.setLightmaps(maps_); redraw_ = true; }
}

void BakeController::setDensity(bool density) {
    if (density_ == density) return;
    density_ = density;
    if (maps_) {
        const auto shown = density_ ? render::densityRadiance(maps_->atlas()) : radiance_;
        if (auto uploaded = maps_->upload(shown); !uploaded) status_ = failure(QString::fromStdString(uploaded.error().what()));
    }
    redraw_ = true;
}

QString BakeController::failure(const QString& text) const {
    return QStringLiteral("%1: %2").arg(hybrid_ ? QStringLiteral("Hybrid") : QStringLiteral("Baked"), text);
}

QString BakeController::status() const {
    if (!hybrid_) return status_;
    if (!baseline_) return QStringLiteral("Hybrid: waiting for the first VRAD compile");
    if (failed_) return status_;
    if (hybridFull_) return QStringLiteral("Hybrid: this change needs a VRAD rebake (Rebiuld VRAD Lightmap)");
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - baseline_->completed).count();
    const QString age = seconds >= 60 ? QStringLiteral("%1m").arg(seconds / 60) : QStringLiteral("%1s").arg(seconds);
    QString text = QStringLiteral("Hybrid (%5): VRAD baseline %1 | %2 of %3 faces rebaked | %4")
                       .arg(age).arg(hybridDirty_.size()).arg(hybridFaces_).arg(progress_.isEmpty() ? QStringLiteral("preparing snapshot") : progress_).arg(deviceStatus_.isEmpty() ? QStringLiteral("CPU") : deviceStatus_);
    if (hybridFaces_ && double(hybridDirty_.size()) > kRecommendRerunShare * double(hybridFaces_))
        text += QStringLiteral(" | VRAD Lightmap rebuild recommended");
    return text;
}

void BakeController::dropChain() {
    job_.cancel();
    session_.reset();
    previous_.reset();
    busy_ = false;
    failed_ = false;
    hybridFull_ = false;
    hybridDirty_.clear();
    hybridFaces_ = 0;
    incremental_ = 0;
    ++revision_;
    changed_ = std::chrono::steady_clock::now();
    progress_.clear();
    status_ = hybrid_ ? QStringLiteral("Hybrid: preparing snapshot") : QStringLiteral("Baked: waiting for edits");
    redraw_ = true;
}

void BakeController::setHybrid(bool hybrid) {
    if (hybrid_ == hybrid) return;
    hybrid_ = hybrid;
    dropChain();
}

void BakeController::setBaseline(std::shared_ptr<const HybridBaseline> baseline) {
    baseline_ = std::move(baseline);
    if (hybrid_) dropChain();
}

void BakeController::invalidate() {
    job_.cancel();
    if (session_) previous_ = std::move(session_);
    session_.reset();
    busy_ = false;
    failed_ = false;
    ++revision_;
    changed_ = std::chrono::steady_clock::now();
    progress_ = QStringLiteral("waiting for edits");
    status_ = QStringLiteral("Baked: waiting for edits");
    redraw_ = true;
}

bool BakeController::update(bool active, DVec3 camera, const std::filesystem::path& map,
                             const std::optional<std::filesystem::path>& extraRoot) {
    const auto options = readOptions();
    // The rebake reach and ring only shape the next edit's diff: they take effect without dropping the bake.
    const auto device = QSettings().value("bake/device", "auto").toString();
    const bool same = sameOptions(options_, options) && device_ == device;
    device_ = device;
    options_ = options;
    if (!same) invalidate();
    const auto scale = float(std::clamp(QSettings().value("bake/displayScale", render::kDefaultBakedScale).toDouble(), 0.01, 16.0));
    if (scale_ != scale) { scale_ = scale; redraw_ = true; }
    if (active_ != active) {
        active_ = active;
        if (!active) { job_.cancel(); session_.reset(); previous_.reset(); busy_ = false; }
        else { failed_ = false; status_ = QStringLiteral("Baked: preparing snapshot"); redraw_ = true; }
    }
    // Hybrid without a VRAD result has nothing to anchor to; status() says so.
    if (!active_ || (hybrid_ && !baseline_) || busy_ || failed_ || std::chrono::steady_clock::now() - changed_ < std::chrono::milliseconds(250))
        return std::exchange(redraw_, false);
    if (!session_) {
        // Copy arenas and immutable blocks on the owner thread; geometry/material/Embree work is off-thread.
        auto snapshot = document_.snapshot();
        const auto revision = revision_;
        busy_ = true;
        auto previous = maps_ ? maps_->layout() : nullptr;
        auto earlier = incremental_ < kMaxIncrementalRestarts ? previous_ : nullptr;
        // Hybrid starts from the VRAD seed; it chains from its own previous session until the drift limit,
        // then re-seeds from the baseline instead of baking everything.
        auto baseline = hybrid_ ? baseline_ : nullptr;
        const bool useGpu = device != QStringLiteral("cpu") && context_.info().rayQuery;
        const bool unavailable = device == QStringLiteral("gpu") && !context_.info().rayQuery;
        auto* context = &context_;
        auto jobOptions = options;
        jobOptions.freezeBounceDirty = hybrid_;
        job_ = jobs_.submit(Priority::Low,
            [snapshot = std::move(snapshot), vfs = vfs_, map, extraRoot, options = jobOptions, revision,
             previous = std::move(previous), radiance = radiance_, earlier = std::move(earlier),
             baseline = std::move(baseline), context, useGpu, unavailable](CancelToken cancel) {
                struct Prepared {
                    std::shared_ptr<lighting::BakeSession> session;
                    bool incremental = false;
                    std::optional<render::LightmapAtlas> atlas;
                    std::vector<std::vector<Vec3>> radiance;
                    std::vector<lighting::FaceKey> dirty; // Faces the restart rebakes (all when full).
                    std::size_t faces = 0;
                    bool full = false, reseeded = false;
                    QString error;
                } result;
                try {
                    if (cancel.cancelled()) return result;
                    const auto origins = editor::expandBakeInstances(*snapshot, map, extraRoot);
                    if (cancel.cancelled()) return result;
                    auto setup = render::setupBake(*snapshot, vfs.get());
                    if (cancel.cancelled()) return result;
                    auto atlas = render::LightmapAtlas::pack(setup.scene.faces);
                    if (!atlas) { result.error = QString::fromStdString(atlas.error().what()); return result; }
                    atlas->bindInstances(origins);
                    // Last cancellable point: after takeState the old state lives only in the new session.
                    if (cancel.cancelled()) return result;
                    auto state = earlier ? earlier->takeState() : lighting::BakeState{};
                    // A lost chain re-seeds from the baseline rather than baking everything without VRAD.
                    const auto seed = state.empty() ? baseline : nullptr;
                    if (seed) {
                        // The exported document is already instance-expanded: set it up as is, never expand again.
                        const auto seedDocument = seed->exported->snapshot();
                        auto seedSetup = render::setupBake(*seedDocument, vfs.get());
                        if (cancel.cancelled()) return result;
                        auto seedAtlas = render::LightmapAtlas::pack(seedSetup.scene.faces);
                        if (!seedAtlas) { result.error = QString::fromStdString(seedAtlas.error().what()); return result; }
                        seedAtlas->bindInstances(seed->origins);
                        result.radiance = atlas->retainRadiance(&seed->lightmaps.atlas, seed->lightmaps.radiance);
                        if (cancel.cancelled()) return result;
                        state = render::hybridSeed(*seedAtlas, seedSetup.scene, seed->lightmaps, options);
                    } else {
                        result.radiance = atlas->retainRadiance(previous.get(), radiance);
                    }
                    atlas->retainStale = true;
                    auto keys = atlas->faceKeys();
                    const auto allKeys = keys;
                    result.faces = setup.scene.faces.size();
                    result.reseeded = bool(seed);
                    lighting::BakeSession::KernelFactory factory;
                    if (useGpu) factory = [context](const auto& scene, const auto& bakerOptions) -> std::shared_ptr<lighting::BakeKernel> {
                        auto kernel = render::GpuBakeKernel::create(*context, scene, bakerOptions);
                        if (!kernel) {
                            log::error("GPU bake: {}", kernel.error().what());
                            throw std::runtime_error(kernel.error().what());
                        }
                        return *kernel;
                    };
                    else if (unavailable) factory = [](const auto&, const auto&) -> std::shared_ptr<lighting::BakeKernel> {
                        throw std::runtime_error("ray query unsupported");
                    };
                    result.session = std::make_shared<lighting::BakeSession>(lighting::BakeScene{}, options, revision,
                                                                           std::vector<lighting::FaceKey>{}, std::move(factory));
                    result.atlas = std::move(*atlas);
                    const auto diff = result.session->restart(std::move(state), std::move(setup.scene), options, revision,
                                                              std::move(keys));
                    result.incremental = !diff.full;
                    result.full = diff.full;
                    for (std::size_t i = 0; i < allKeys.size(); ++i)
                        if (diff.full || (i < diff.dirty.size() && diff.dirty[i])) result.dirty.push_back(allKeys[i]);
                } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
                return result;
            }, [this](auto prepared) {
                busy_ = false;
                if (!prepared.error.isEmpty()) {
                    status_ = failure(prepared.error);
                    // Retain the old atlas; retry only after another edit or mode/settings change.
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                if (!prepared.session || !prepared.atlas) return;
                auto maps = render::BakedLightmaps::create(context_, heap_, std::move(*prepared.atlas));
                if (!maps) {
                    status_ = failure(QString::fromStdString(maps.error().what()));
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                auto retained = std::move(prepared.radiance);
                if (auto uploaded = (*maps)->upload(density_ ? render::densityRadiance((*maps)->atlas()) : retained); !uploaded) {
                    status_ = failure(QString::fromStdString(uploaded.error().what()));
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                maps_ = std::move(*maps);
                radiance_ = std::move(retained);
                session_ = std::move(prepared.session);
                previous_.reset();
                // A re-seed counts drift from the baseline again.
                incremental_ = prepared.incremental && !prepared.reseeded ? incremental_ + 1 : 0;
                if (hybrid_) {
                    if (prepared.reseeded) hybridDirty_.clear(); // Counted from the baseline again.
                    hybridDirty_.insert(prepared.dirty.begin(), prepared.dirty.end());
                    hybridFaces_ = prepared.faces;
                    hybridFull_ = prepared.full;
                }
                if (displayed_) mirror_.setLightmaps(maps_);
                deviceStatus_ = session_->gpu() ? QStringLiteral("GPU") : QStringLiteral("CPU");
                if (!session_->kernelError().empty())
                    deviceStatus_ += QStringLiteral(", GPU unavailable: ") + QString::fromStdString(session_->kernelError()).left(100);
                progress_ = progressText(session_->progress(), options_.maxPasses);
                status_ = QStringLiteral("Baked (%1): ").arg(deviceStatus_) + progress_;
                redraw_ = true;
            });
    } else if (!session_->progress().idle) {
        busy_ = true;
        job_ = jobs_.submit(Priority::Low, [session = session_, camera](CancelToken cancel) {
            struct TickResult { lighting::BakePublication publication; QString error; } result;
            try {
                session->setCamera(camera);
                result.publication = session->tick(cancel, std::chrono::milliseconds(session->gpu() ? 8 : 40));
            } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
            return result;
        }, [this](auto result) {
            busy_ = false;
            if (!result.error.isEmpty()) {
                status_ = failure(result.error);
                failed_ = true;
                redraw_ = true;
            } else accept(std::move(result.publication));
        });
    }
    return std::exchange(redraw_, false);
}

void BakeController::accept(lighting::BakePublication publication) {
    if (publication.progress.revision != revision_) return;
    progress_ = progressText(publication.progress, options_.maxPasses);
    status_ = QStringLiteral("Baked (%1): ").arg(deviceStatus_) + progress_;
    std::vector<std::size_t> changed;
    for (auto& update : publication.faces) {
        for (std::size_t i = 0; i < update.radiance.size(); ++i)
            if (update.sampled[i]) radiance_[update.face][i] = update.radiance[i];
        changed.push_back(update.face);
    }
    if (!changed.empty() && !density_) {
        if (auto uploaded = maps_->upload(radiance_, changed); !uploaded)
            status_ = failure(QString::fromStdString(uploaded.error().what()));
    }
    redraw_ = true;
}
} // namespace apex::ui
