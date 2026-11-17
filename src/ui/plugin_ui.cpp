#include "ui/plugin_ui.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}
PluginUi::PluginUi(plugin_host::Host& host, QObject* parent) : QObject(parent), host_(host) { host_.setUiBackend(this); }
PluginUi::~PluginUi() { host_.setUiBackend(nullptr); }
bool PluginUi::hasPanel(const QString& id) const { return host_.hasPanel(id.toStdString()); }
QWidget* PluginUi::createPanel(const QString& id, QWidget* parent) {
    if (!hasPanel(id)) return nullptr;
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* view = new PluginFormView(*this, id, scroll);
    scroll->setWidget(view);
    view->reload();
    return scroll;
}
void PluginUi::formChanged(std::string_view id) {
    const auto key = qs(id);
    if (queued_.contains(key)) return;
    queued_.insert(key);
    QTimer::singleShot(0, this, [this] { flush(); });
}
void PluginUi::flush() {
    const auto ids = std::exchange(queued_, {});
    for (const auto& id : ids) emit formRefreshed(id);
}
bool PluginUi::runDialog(std::string_view id, std::string_view title) {
    QDialog dialog;
    dialog.setWindowTitle(qs(title));
    auto* layout = new QVBoxLayout(&dialog);
    auto* view = new PluginFormView(*this, qs(id), &dialog);
    layout->addWidget(view);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    view->reload();
    if (dialogHook_) { const bool accepted = dialogHook_(dialog); flush(); return accepted; }
    return dialog.exec() == QDialog::Accepted;
}

PluginFormView::PluginFormView(PluginUi& ui, QString id, QWidget* parent)
    : QWidget(parent), ui_(&ui), id_(std::move(id)), form_(new PluginForm(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(form_);
    layout->addStretch(1);
    form_->setChange([this](std::string_view key, const apex_ui_value& value) {
        if (ui_) ui_->host().panelChanged(id_.toStdString(), key, value);
    });
    form_->setClick([this](std::string_view key) {
        if (ui_) ui_->host().panelClicked(id_.toStdString(), key);
    });
    connect(&ui, &PluginUi::formRefreshed, this, [this](const QString& changed) { if (changed == id_) reload(); });
}
void PluginFormView::setContext(std::optional<plugin_host::UiContext> context) {
    context_ = std::move(context);
    reload();
}
bool PluginFormView::reload() {
    if (!ui_) return false;
    const auto id = id_.toStdString();
    bool applied = false;
    const auto apply = [&](const apex_ui_form& form) { applied = form_->describe(form); };
    return (context_ ? ui_->host().describePanel(id, &*context_, apply) : ui_->host().describePanel(id, apply)) && applied;
}

PluginSections::PluginSections(QString target, QWidget* parent)
    : QWidget(parent), target_(std::move(target)), layout_(new QVBoxLayout(this)) {
    layout_->setContentsMargins(0, 0, 0, 0);
    setObjectName(QStringLiteral("PluginSections"));
    hide();
}
void PluginSections::setPluginUi(PluginUi* ui) {
    if (ui_ == ui) return;
    ui_ = ui;
    rebuild();
}
void PluginSections::setContext(plugin_host::UiContext context) {
    if (context.kind == context_.kind && context.names == context_.names && context.ids == context_.ids) return;
    context_ = std::move(context);
    for (auto* view : findChildren<PluginFormView*>()) view->setContext(context_);
}
void PluginSections::rebuild() {
    while (auto* item = layout_->takeAt(0)) {
        if (auto* widget = item->widget()) { widget->setObjectName({}); widget->hide(); widget->deleteLater(); }
        delete item;
    }
    const auto sections = ui_ ? ui_->host().sections(target_.toStdString()) : std::vector<plugin_host::UiSection>{};
    for (const auto& section : sections) {
        auto* box = new QGroupBox(qs(section.title), this);
        box->setObjectName(qs(section.id));
        auto* inner = new QVBoxLayout(box);
        auto* view = new PluginFormView(*ui_, qs(section.id), box);
        inner->addWidget(view);
        view->setContext(context_);
        layout_->addWidget(box);
    }
    setVisible(!sections.empty());
}
}
