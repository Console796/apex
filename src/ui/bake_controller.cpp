#include "ui/bake_controller.hpp"

#include "editor/bake_instances.hpp"
#include "render/bake_setup.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSettings>
#include <QSpinBox>

#include <algorithm>
#include <utility>

namespace apex::ui {
namespace {
lighting::BakeSessionOptions readOptions() {
    QSettings settings;
    lighting::BakeSessionOptions options;
    options.baker.bounces = unsigned(std::clamp(settings.value("bake/bounces", 2).toInt(), 0, 8));
    options.baker.bounceRays = unsigned(std::clamp(settings.value("bake/bounceRays", 32).toInt(), 1, 1024));
    options.baker.skyRays = unsigned(std::clamp(settings.value("bake/skyRays", 64).toInt(), 1, 1024));
    options.maxPasses = unsigned(std::clamp(settings.value("bake/maxPasses", 64).toInt(), 1, 1024));
    return options;
}
bool sameOptions(const lighting::BakeSessionOptions& a, const lighting::BakeSessionOptions& b) {
    return a.baker.bounces == b.baker.bounces && a.baker.bounceRays == b.baker.bounceRays &&
           a.baker.skyRays == b.baker.skyRays && a.maxPasses == b.maxPasses;
}
QString progressText(const lighting::BakeProgress& p, unsigned maximum) {
    if (p.idle) return QStringLiteral("Baking (idle)");
    QString text = QStringLiteral("Baked: direct %1/%2").arg(p.passes[0]).arg(maximum);
    for (std::size_t i = 1; i < p.passes.size(); ++i)
        text += QStringLiteral(", bounce %1 %2/%3").arg(i).arg(p.passes[i]).arg(maximum);
    return text;
}
// Drift guard: bounce reach and light range are cut off at fixed bounds, so after this many incremental
// restarts in a row the next one bakes everything again.
constexpr unsigned kMaxIncrementalRestarts = 16;
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
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    settings.setValue("bake/displayScale", scale->value());
    settings.setValue("bake/bounces", bounces->value());
    settings.setValue("bake/bounceRays", bounceRays->value());
    settings.setValue("bake/skyRays", skyRays->value());
    settings.setValue("bake/maxPasses", passes->value());
}

BakeController::BakeController(document::Document& document, std::shared_ptr<const assets::Vfs> vfs,
                               render::Context& context, render::TextureHeap& heap,
                               render::RenderMirror& mirror, JobPool& jobs)
    : document_(document), vfs_(std::move(vfs)), context_(context), heap_(heap), mirror_(mirror), jobs_(jobs),
      options_(readOptions()) {}
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
        if (auto uploaded = maps_->upload(shown); !uploaded) status_ = QStringLiteral("Baked: %1").arg(QString::fromStdString(uploaded.error().what()));
    }
    redraw_ = true;
}

void BakeController::invalidate() {
    job_.cancel();
    if (session_) previous_ = std::move(session_);
    session_.reset();
    busy_ = false;
    failed_ = false;
    ++revision_;
    changed_ = std::chrono::steady_clock::now();
    status_ = QStringLiteral("Baked: waiting for edits");
    redraw_ = true;
}

bool BakeController::update(bool active, DVec3 camera, const std::filesystem::path& map,
                             const std::optional<std::filesystem::path>& extraRoot) {
    const auto options = readOptions();
    if (!sameOptions(options_, options)) { options_ = options; invalidate(); }
    const auto scale = float(std::clamp(QSettings().value("bake/displayScale", render::kDefaultBakedScale).toDouble(), 0.01, 16.0));
    if (scale_ != scale) { scale_ = scale; redraw_ = true; }
    if (active_ != active) {
        active_ = active;
        if (!active) { job_.cancel(); session_.reset(); previous_.reset(); busy_ = false; }
        else { failed_ = false; status_ = QStringLiteral("Baked: preparing snapshot"); redraw_ = true; }
    }
    if (!active_ || busy_ || failed_ || std::chrono::steady_clock::now() - changed_ < std::chrono::milliseconds(250))
        return std::exchange(redraw_, false);
    if (!session_) {
        // Copy arenas and immutable blocks on the owner thread; geometry/material/Embree work is off-thread.
        auto snapshot = document_.snapshot();
        const auto revision = revision_;
        busy_ = true;
        auto previous = maps_ ? maps_->layout() : nullptr;
        auto earlier = incremental_ < kMaxIncrementalRestarts ? previous_ : nullptr;
        job_ = jobs_.submit(Priority::Low,
            [snapshot = std::move(snapshot), vfs = vfs_, map, extraRoot, options, revision,
             previous = std::move(previous), radiance = radiance_, earlier = std::move(earlier)](CancelToken cancel) {
                struct Prepared {
                    std::shared_ptr<lighting::BakeSession> session;
                    bool incremental = false;
                    std::optional<render::LightmapAtlas> atlas;
                    std::vector<std::vector<Vec3>> radiance;
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
                    result.radiance = atlas->retainRadiance(previous.get(), radiance);
                    atlas->retainStale = true;
                    // Last cancellable point: after takeState the old state lives only in the new session.
                    if (cancel.cancelled()) return result;
                    auto state = earlier ? earlier->takeState() : lighting::BakeState{};
                    auto keys = atlas->faceKeys();
                    result.session = std::make_shared<lighting::BakeSession>(lighting::BakeScene{}, options, revision);
                    result.atlas = std::move(*atlas);
                    const auto diff = result.session->restart(std::move(state), std::move(setup.scene), options, revision,
                                                              std::move(keys));
                    result.incremental = !diff.full;
                } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
                return result;
            }, [this](auto prepared) {
                busy_ = false;
                if (!prepared.error.isEmpty()) {
                    status_ = QStringLiteral("Baked: %1").arg(prepared.error);
                    // Retain the old atlas; retry only after another edit or mode/settings change.
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                if (!prepared.session || !prepared.atlas) return;
                auto maps = render::BakedLightmaps::create(context_, heap_, std::move(*prepared.atlas));
                if (!maps) {
                    status_ = QStringLiteral("Baked: %1").arg(QString::fromStdString(maps.error().what()));
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                auto retained = std::move(prepared.radiance);
                if (auto uploaded = (*maps)->upload(density_ ? render::densityRadiance((*maps)->atlas()) : retained); !uploaded) {
                    status_ = QStringLiteral("Baked: %1").arg(QString::fromStdString(uploaded.error().what()));
                    failed_ = true;
                    redraw_ = true;
                    return;
                }
                maps_ = std::move(*maps);
                radiance_ = std::move(retained);
                session_ = std::move(prepared.session);
                previous_.reset();
                incremental_ = prepared.incremental ? incremental_ + 1 : 0;
                if (displayed_) mirror_.setLightmaps(maps_);
                status_ = progressText(session_->progress(), options_.maxPasses);
                redraw_ = true;
            });
    } else if (!session_->progress().idle) {
        busy_ = true;
        job_ = jobs_.submit(Priority::Low, [session = session_, camera](CancelToken cancel) {
            struct TickResult { lighting::BakePublication publication; QString error; } result;
            try {
                session->setCamera(camera);
                result.publication = session->tick(cancel);
            } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
            return result;
        }, [this](auto result) {
            busy_ = false;
            if (!result.error.isEmpty()) {
                status_ = QStringLiteral("Baked: %1").arg(result.error);
                failed_ = true;
                redraw_ = true;
            } else accept(std::move(result.publication));
        });
    }
    return std::exchange(redraw_, false);
}

void BakeController::accept(lighting::BakePublication publication) {
    if (publication.progress.revision != revision_) return;
    status_ = progressText(publication.progress, options_.maxPasses);
    std::vector<std::size_t> changed;
    for (auto& update : publication.faces) {
        for (std::size_t i = 0; i < update.radiance.size(); ++i)
            if (update.sampled[i]) radiance_[update.face][i] = update.radiance[i];
        changed.push_back(update.face);
    }
    if (!changed.empty() && !density_) {
        if (auto uploaded = maps_->upload(radiance_, changed); !uploaded)
            status_ = QStringLiteral("Baked: %1").arg(QString::fromStdString(uploaded.error().what()));
    }
    redraw_ = true;
}
} // namespace apex::ui
