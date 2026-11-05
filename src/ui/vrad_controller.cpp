#include "ui/vrad_controller.hpp"

#include "render/renderer.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace apex::ui {

void showVradSettings(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("VRAD settings"));
    auto* layout = new QFormLayout(&dialog);
    QSettings settings;
    const editor::VradOptions defaults;
    auto* flags = new QLineEdit(settings.value("vrad/flags", QString::fromStdString(defaults.flags)).toString(), &dialog);
    layout->addRow(QObject::tr("Extra vrad flags"), flags);
    auto* vis = new QCheckBox(&dialog);
    vis->setChecked(settings.value("vrad/vis", defaults.vis).toBool());
    layout->addRow(QObject::tr("Run vvis -fast first"), vis);
    auto* scale = new QDoubleSpinBox(&dialog);
    scale->setRange(0.01, 16);
    scale->setSingleStep(0.1);
    scale->setValue(settings.value("vrad/displayScale", double(render::kDefaultVradScale)).toDouble());
    layout->addRow(QObject::tr("Display scale"), scale);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    settings.setValue("vrad/flags", flags->text());
    settings.setValue("vrad/vis", vis->isChecked());
    settings.setValue("vrad/displayScale", scale->value());
}

VradController::VradController(document::Document& document, compile::ProcessRunner& runner, Publish publish,
                               Loader loader)
    : document_(document), trip_(runner, files_), publish_(std::move(publish)), loader_(std::move(loader)) {}
VradController::~VradController() { trip_.cancel(); }

void VradController::cancel() {
    if (phase_ != Phase::Running) return;
    trip_.cancel();
    phase_ = Phase::Idle;
    detail_.clear();
}

void VradController::start(const std::filesystem::path& map, const editor::CompileGamePaths& game) {
    const QSettings settings;
    editor::VradOptions options;
    options.flags = settings.value("vrad/flags", QString::fromStdString(options.flags)).toString().toStdString();
    options.vis = settings.value("vrad/vis", options.vis).toBool();
    attempted_ = true;
    rerun_ = false;
    started_ = std::chrono::steady_clock::now();
    shown_ = std::chrono::seconds(-1);
    if (auto began = trip_.start(document_, map, game, options); !began) {
        phase_ = Phase::Failed;
        detail_ = QString::fromStdString(trip_.message().empty() ? began.error().what() : trip_.message());
    } else {
        phase_ = Phase::Running;
        detail_.clear();
    }
}

void VradController::finish() {
    auto loaded = loader_(trip_.bsp(), *trip_.exported(), trip_.origins());
    if (loaded) {
        stats_ = loaded->stats;
        // Set before publishing so the publish callback can read it; a failed publish keeps the old one.
        const auto earlier = std::exchange(baseline_, std::make_shared<const Baseline>(Baseline{
            std::shared_ptr<const document::Document>(trip_.exported()->snapshot()),
            trip_.origins(), trip_.revision(), std::chrono::steady_clock::now()}));
        if (auto published = publish_(std::move(*loaded)); !published) {
            baseline_ = earlier;
            phase_ = Phase::Failed;
            detail_ = QString::fromStdString(published.error().what());
            return;
        }
        phase_ = Phase::Ready;
        detail_.clear();
    } else {
        phase_ = Phase::Failed;
        detail_ = QString::fromStdString(loaded.error().what());
    }
}

bool VradController::update(bool active, const std::filesystem::path& map, const editor::CompileGamePaths& game) {
    bool changed = false;
    const QSettings settings;
    const auto scale = float(std::clamp(settings.value("vrad/displayScale", double(render::kDefaultVradScale)).toDouble(),
                                        0.01, 16.0));
    if (scale != scale_) { scale_ = scale; changed = true; }
    const bool entered = active && !active_;
    if (!active && active_ && phase_ == Phase::Running) { cancel(); changed = true; }
    active_ = active;
    if (active && rerun_ && phase_ == Phase::Running) { cancel(); changed = true; }
    if (active && (rerun_ || (entered && !attempted_) || (entered && phase_ == Phase::Idle))) {
        start(map, game);
        changed = true;
    }
    if (phase_ == Phase::Running) {
        const auto state = trip_.poll();
        if (state == editor::VradRoundTrip::State::Done) { finish(); changed = true; }
        else if (state == editor::VradRoundTrip::State::Failed) {
            phase_ = Phase::Failed;
            detail_ = QString::fromStdString(trip_.message());
            changed = true;
        } else if (state == editor::VradRoundTrip::State::Cancelled) { phase_ = Phase::Idle; changed = true; }
        else {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_);
            if (seconds != shown_) { shown_ = seconds; changed = true; }
        }
    }
    return changed;
}

QString VradController::status() const {
    const QString note = QStringLiteral("faces without VRAD data are black");
    switch (phase_) {
    case Phase::Idle: return QStringLiteral("VRAD: not run (Re-run VRAD to compile)");
    case Phase::Running:
        return QStringLiteral("VRAD: compiling %1s | %2").arg(std::max<long long>(shown_.count(), 0)).arg(note);
    case Phase::Failed: {
        const auto line = detail_.section('\n', 0, 0);
        return QStringLiteral("VRAD failed: %1 (hover for the log)").arg(line);
    }
    case Phase::Ready: break;
    }
    QString text = QStringLiteral("VRAD: %1 faces lit").arg(stats_.faces);
    if (stale()) text += QStringLiteral(" | STALE: edited since this run (Re-run VRAD)");
    return text + QStringLiteral(" | ") + note;
}

} // namespace apex::ui
