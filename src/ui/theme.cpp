#include "ui/theme.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonDocument>
#include <QPalette>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyleOption>

#include <algorithm>
#include <utility>
#include <cstring>

namespace apex::ui {
namespace {

// Every colour token: its theme-file name and field, in editor order.
struct ColourField {
    const char* name;
    QColor Tokens::*field;
};
constexpr ColourField kColourFields[] = {
    {"window", &Tokens::window}, {"panel", &Tokens::panel}, {"base", &Tokens::base},
    {"raised", &Tokens::raised}, {"hover", &Tokens::hover}, {"pressed", &Tokens::pressed},
    {"border", &Tokens::border}, {"text", &Tokens::text}, {"textDim", &Tokens::textDim},
    {"textOnAccent", &Tokens::textOnAccent}, {"accent", &Tokens::accent}, {"accentHover", &Tokens::accentHover},
    {"selection", &Tokens::selection}, {"warn", &Tokens::warn}, {"error", &Tokens::error},
    {"vertexHandle", &Tokens::vertexHandle}, {"vertexSelected", &Tokens::vertexSelected},
    {"vertexInactive", &Tokens::vertexInactive}, {"vertexEdge", &Tokens::vertexEdge},
    {"vertexInvalid", &Tokens::vertexInvalid}, {"vertexSplit", &Tokens::vertexSplit},
    {"faceHover", &Tokens::faceHover}, {"gizmoX", &Tokens::gizmoX}, {"gizmoY", &Tokens::gizmoY},
    {"gizmoZ", &Tokens::gizmoZ}, {"gizmoScreen", &Tokens::gizmoScreen}, {"gizmoHighlight", &Tokens::gizmoHighlight},
    {"clip", &Tokens::clip}, {"clipDiscard", &Tokens::clipDiscard}, {"block", &Tokens::block},
};

QColor toQColor(Vec4 c) { return QColor::fromRgbF(c.r, c.g, c.b); }
Vec4 toVec4(const QColor& c) {
    return {static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF()), 1.0f};
}

// The palette and token tables list the viewport colours in the same order.
constexpr std::pair<QColor Tokens::*, Vec4 editor::OverlayPalette::*> kViewportFields[] = {
    {&Tokens::vertexHandle, &editor::OverlayPalette::vertexHandle},
    {&Tokens::vertexSelected, &editor::OverlayPalette::vertexSelected},
    {&Tokens::vertexInactive, &editor::OverlayPalette::vertexInactive},
    {&Tokens::vertexEdge, &editor::OverlayPalette::vertexEdge},
    {&Tokens::vertexInvalid, &editor::OverlayPalette::vertexInvalid},
    {&Tokens::vertexSplit, &editor::OverlayPalette::vertexSplit},
    {&Tokens::faceHover, &editor::OverlayPalette::faceHover},
    {&Tokens::gizmoX, &editor::OverlayPalette::gizmoX},
    {&Tokens::gizmoY, &editor::OverlayPalette::gizmoY},
    {&Tokens::gizmoZ, &editor::OverlayPalette::gizmoZ},
    {&Tokens::gizmoScreen, &editor::OverlayPalette::gizmoScreen},
    {&Tokens::gizmoHighlight, &editor::OverlayPalette::gizmoHighlight},
    {&Tokens::clip, &editor::OverlayPalette::clip},
    {&Tokens::clipDiscard, &editor::OverlayPalette::clipDiscard},
    {&Tokens::block, &editor::OverlayPalette::block},
};

// Built-in themes share the editor's default overlay colours.
void setViewportDefaults(Tokens& t) {
    const editor::OverlayPalette defaults;
    for (const auto& [token, colour] : kViewportFields) t.*token = toQColor(defaults.*colour);
}

Tokens makeDark() {
    Tokens t;
    t.window = QColor("#1b1d21");
    t.panel = QColor("#23262b");
    t.base = QColor("#17191c");
    t.raised = QColor("#2c3036");
    t.hover = QColor("#353a41");
    t.pressed = QColor("#40464e");
    t.border = QColor("#383c43");
    t.text = QColor("#d9dce1");
    t.textDim = QColor("#8b919b");
    t.textOnAccent = QColor("#ffffff");
    t.accent = QColor("#3d8bfd");
    t.accentHover = QColor("#5a9dff");
    t.selection = QColor("#264f8a");
    t.warn = QColor("#e0b341");
    t.error = QColor("#ef6a6a");
    setViewportDefaults(t);
    return t;
}

Tokens makeLight() {
    Tokens t;
    t.window = QColor("#eceef1");
    t.panel = QColor("#f6f7f9");
    t.base = QColor("#ffffff");
    t.raised = QColor("#e2e5e9");
    t.hover = QColor("#d6dae0");
    t.pressed = QColor("#c7ccd4");
    t.border = QColor("#c3c8d0");
    t.text = QColor("#1d2026");
    t.textDim = QColor("#6a717c");
    t.textOnAccent = QColor("#ffffff");
    t.accent = QColor("#1f6feb");
    t.accentHover = QColor("#3d82f0");
    t.selection = QColor("#bcd5fa");
    t.warn = QColor("#9a6a00");
    t.error = QColor("#c62828");
    setViewportDefaults(t);
    return t;
}

// Colour order: window panel base raised hover pressed border text textDim textOnAccent accent
// accentHover selection warn error.
Tokens makeColours(const char* const (&c)[15]) {
    Tokens t;
    QColor* fields[] = {&t.window, &t.panel,  &t.base,        &t.raised,      &t.hover,
                        &t.pressed, &t.border, &t.text,       &t.textDim,     &t.textOnAccent,
                        &t.accent, &t.accentHover, &t.selection, &t.warn,      &t.error};
    for (int i = 0; i < 15; ++i) *fields[i] = QColor(QLatin1StringView(c[i]));
    setViewportDefaults(t);
    return t;
}

Tokens makeMidnight() {
    const char* const c[15] = {"#0b0e14", "#11151d", "#080a0f", "#18202c", "#212b3a", "#2b3749", "#222c3b", "#c9d1e0",
                               "#6f7b91", "#ffffff", "#7c5cff", "#977dff", "#2d2a6e", "#e0b341", "#f0707a"};
    return makeColours(c);
}

Tokens makeSolarizedDark() {
    const char* const c[15] = {"#00212b", "#002b36", "#001f27", "#073642", "#0d4452", "#14566a", "#0f4655", "#93a1a1",
                               "#657b83", "#fdf6e3", "#268bd2", "#4aa3e3", "#124f66", "#b58900", "#dc322f"};
    return makeColours(c);
}

Tokens makeSolarizedLight() {
    const char* const c[15] = {"#eee8d5", "#f5efdc", "#fdf6e3", "#e6dfc8", "#ddd5ba", "#d2c9ab", "#d3cbb0", "#586e75",
                               "#93a1a1", "#fdf6e3", "#268bd2", "#4aa3e3", "#cfe0d8", "#b58900", "#dc322f"};
    return makeColours(c);
}

Tokens makeHighContrast() {
    const char* const c[15] = {"#000000", "#0a0a0a", "#000000", "#1a1a1a", "#2e2e2e", "#454545", "#ffffff", "#ffffff",
                               "#c8c8c8", "#000000", "#ffd400", "#ffe45c", "#0b4fa8", "#ffd400", "#ff5c5c"};
    return makeColours(c);
}

QList<ThemeEntry>& registry() {
    static QList<ThemeEntry> r = [] {
        QList<ThemeEntry> l;
        const auto add = [&](const char* id, const char* name, Tokens t) {
            l.append({QString::fromLatin1(id), QString::fromLatin1(name), true, std::move(t)});
        };
        add("dark", "Dark", makeDark());
        add("light", "Light", makeLight());
        add("midnight", "Midnight", makeMidnight());
        add("solarized-dark", "Solarized Dark", makeSolarizedDark());
        add("solarized-light", "Solarized Light", makeSolarizedLight());
        add("high-contrast", "High Contrast", makeHighContrast());
        return l;
    }();
    return r;
}

Tokens& current() {
    static Tokens t = makeDark();
    return t;
}
QString g_themeId = QStringLiteral("dark");
ApexStyle* g_style = nullptr;

QString slugOf(const QString& name) {
    QString out;
    for (const QChar ch : name.toLower()) out += ch.isLetterOrNumber() ? ch : QLatin1Char('-');
    while (out.contains(QStringLiteral("--"))) out.replace(QStringLiteral("--"), QStringLiteral("-"));
    while (out.startsWith(QLatin1Char('-'))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('-'))) out.chop(1);
    return out.isEmpty() ? QStringLiteral("theme") : out;
}

const char* const kCustomPrefix = "custom:";

}  // namespace

const Tokens& tokens(Theme theme) { return registry()[theme == Theme::Dark ? 0 : 1].tokens; }

const Tokens& currentTokens() { return current(); }
Theme currentTheme() { return current().window.lightness() < 128 ? Theme::Dark : Theme::Light; }
QString currentThemeId() { return g_themeId; }

const QStringList& colourTokenNames() {
    static const QStringList names = [] {
        QStringList l;
        for (const auto& f : kColourFields) l.append(QString::fromLatin1(f.name));
        return l;
    }();
    return names;
}

const QColor& colourToken(const Tokens& t, const QString& name) {
    const int i = colourTokenNames().indexOf(name);
    return t.*kColourFields[i < 0 ? 0 : i].field;
}

QColor& colourToken(Tokens& t, const QString& name) {
    return const_cast<QColor&>(colourToken(static_cast<const Tokens&>(t), name));
}

editor::OverlayPalette overlayPalette(const Tokens& t) {
    editor::OverlayPalette palette;
    for (const auto& [token, colour] : kViewportFields) palette.*colour = toVec4(t.*token);
    return palette;
}

const QList<ThemeEntry>& themes() { return registry(); }

const ThemeEntry* findTheme(const QString& id) {
    for (const auto& e : registry())
        if (e.id == id) return &e;
    return nullptr;
}

QString themesDir() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/themes");
}

QJsonObject themeToJson(const ThemeEntry& theme) {
    QJsonObject colors;
    for (const auto& n : colourTokenNames()) colors[n] = colourToken(theme.tokens, n).name(QColor::HexRgb);
    QJsonObject metrics;
    metrics[QStringLiteral("spacing")] = theme.tokens.spacing;
    metrics[QStringLiteral("radius")] = theme.tokens.radius;
    metrics[QStringLiteral("controlHeight")] = theme.tokens.controlHeight;
    metrics[QStringLiteral("splitterWidth")] = theme.tokens.splitterWidth;
    metrics[QStringLiteral("fontPointSize")] = theme.tokens.fontPointSize;
    if (!theme.tokens.fontFamily.isEmpty()) metrics[QStringLiteral("fontFamily")] = theme.tokens.fontFamily;
    QJsonObject o;
    o[QStringLiteral("name")] = theme.name;
    o[QStringLiteral("base")] = theme.tokens.window.lightness() < 128 ? QStringLiteral("dark") : QStringLiteral("light");
    o[QStringLiteral("colors")] = colors;
    o[QStringLiteral("metrics")] = metrics;
    return o;
}

bool themeFromJson(const QJsonObject& json, ThemeEntry& out, QString* error) {
    const auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    const QString name = json.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) return fail(QStringLiteral("missing \"name\""));
    const QString baseId = json.value(QStringLiteral("base")).toString(QStringLiteral("dark"));
    const ThemeEntry* base = findTheme(baseId);
    if (!base || !base->builtin) return fail(QStringLiteral("unknown base theme \"%1\"").arg(baseId));
    ThemeEntry t;
    t.name = name;
    t.builtin = false;
    t.tokens = base->tokens;
    const QJsonObject colors = json.value(QStringLiteral("colors")).toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        if (!colourTokenNames().contains(it.key())) continue;  // Unknown keys are ignored (forward compatible).
        const QString text = it.value().toString();
        if (!QColor::isValidColorName(text)) return fail(QStringLiteral("bad colour \"%1\" for %2").arg(text, it.key()));
        colourToken(t.tokens, it.key()) = QColor(text);
    }
    const QJsonObject m = json.value(QStringLiteral("metrics")).toObject();
    const auto metric = [&](const char* key, int& field, int lo, int hi) {
        if (m.contains(QLatin1StringView(key))) field = std::clamp(m.value(QLatin1StringView(key)).toInt(field), lo, hi);
    };
    metric("spacing", t.tokens.spacing, 0, 24);
    metric("radius", t.tokens.radius, 0, 16);
    metric("controlHeight", t.tokens.controlHeight, 16, 48);
    metric("splitterWidth", t.tokens.splitterWidth, 1, 12);
    metric("fontPointSize", t.tokens.fontPointSize, 6, 24);
    if (m.contains(QStringLiteral("fontFamily"))) t.tokens.fontFamily = m.value(QStringLiteral("fontFamily")).toString();
    out = std::move(t);
    return true;
}

void loadCustomThemes(const QString& dir, QStringList* errors) {
    auto& r = registry();
    r.erase(std::remove_if(r.begin(), r.end(), [](const ThemeEntry& e) { return !e.builtin; }), r.end());
    const auto files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        QFile f(fi.absoluteFilePath());
        QString err;
        ThemeEntry t;
        QJsonParseError pe;
        if (!f.open(QIODevice::ReadOnly)) {
            err = QStringLiteral("cannot open");
        } else {
            const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
            if (!doc.isObject()) err = pe.errorString();
            else themeFromJson(doc.object(), t, &err);
        }
        if (!err.isEmpty()) {
            if (errors) errors->append(QStringLiteral("%1: %2").arg(fi.fileName(), err));
            continue;
        }
        t.id = QLatin1StringView(kCustomPrefix) + fi.completeBaseName();
        r.append(std::move(t));
    }
}

QString saveCustomTheme(const ThemeEntry& theme, const QString& dir, QString* error) {
    const QString stem = slugOf(theme.name);
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(dir);
        return {};
    }
    QSaveFile f(dir + QLatin1Char('/') + stem + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(themeToJson(theme)).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return {};
    }
    loadCustomThemes(dir);
    return QLatin1StringView(kCustomPrefix) + stem;
}

bool removeCustomTheme(const QString& id, const QString& dir) {
    if (!id.startsWith(QLatin1StringView(kCustomPrefix))) return false;
    const QString stem = id.mid(static_cast<int>(std::strlen(kCustomPrefix)));
    if (stem.isEmpty() || stem.contains(QLatin1Char('/')) || stem.contains(QLatin1Char('\\'))) return false;
    const bool ok = QFile::remove(dir + QLatin1Char('/') + stem + QStringLiteral(".json"));
    loadCustomThemes(dir);
    return ok;
}

QString importThemeFile(const QString& path, const QString& dir, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1").arg(path);
        return {};
    }
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    ThemeEntry t;
    if (!doc.isObject()) {
        if (error) *error = pe.errorString();
        return {};
    }
    if (!themeFromJson(doc.object(), t, error)) return {};
    return saveCustomTheme(t, dir, error);
}

bool exportTheme(const QString& id, const QString& path) {
    const ThemeEntry* t = findTheme(id);
    if (!t) return false;
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(themeToJson(*t)).toJson()) >= 0 && f.commit();
}

QString qssTemplate() {
    return QStringLiteral(R"(
* { font-size: @font@pt; }
QWidget { background: @window@; color: @text@; selection-background-color: @selection@; selection-color: @text@; }
QMainWindow, QDialog { background: @window@; }
QWidget:disabled { color: @textDim@; }
QToolTip { background: @raised@; color: @text@; border: 1px solid @border@; padding: 2px 4px; }

QMenuBar { background: @window@; border-bottom: 1px solid @border@; }
QMenuBar::item { padding: 4px 10px; background: transparent; border-radius: @radius@px; }
QMenuBar::item:selected { background: @hover@; }
QMenu { background: @panel@; border: 1px solid @border@; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 20px; border-radius: @radius@px; }
QMenu::item:selected { background: @accent@; color: @textOnAccent@; }
QMenu::item:disabled { color: @textDim@; background: transparent; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }

QToolBar { background: @window@; border: none; border-bottom: 1px solid @border@; spacing: @spacing@px; padding: 2px 4px; }
QToolBar QLabel { background: transparent; color: @textDim@; }
QStatusBar { background: @window@; border-top: 1px solid @border@; }
QStatusBar QLabel { background: transparent; color: @textDim@; padding: 0 @spacing@px; }
QStatusBar QLabel#statusMode { color: @accent@; font-weight: bold; }
QStatusBar::item { border: none; }

QPushButton, QToolButton { background: @raised@; border: 1px solid @border@; border-radius: @radius@px; padding: 3px 10px; min-height: @controlHeight@px; }
QToolButton { min-height: 0; padding: 3px 6px; }
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @pressed@; }
QToolButton:checked { background: @accent@; color: @textOnAccent@; border-color: @accent@; }
QToolButton:checked:hover { background: @accentHover@; }
QPushButton:default { border-color: @accent@; }

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: @base@; border: 1px solid @border@; border-radius: @radius@px; padding: 2px 6px; min-height: @controlHeight@px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: @panel@; border: 1px solid @border@; selection-background-color: @accent@; selection-color: @textOnAccent@; outline: 0; }

QTreeView, QTreeWidget, QListView, QTableView, QPlainTextEdit, QTextEdit { background: @base@; alternate-background-color: @panel@; border: 1px solid @border@; border-radius: @radius@px; outline: 0; }
QTreeView::item, QListView::item { padding: 2px 0; }
QTreeView::item:hover, QListView::item:hover { background: @hover@; }
QTreeView::item:selected, QListView::item:selected { background: @selection@; color: @text@; }
QHeaderView::section { background: @panel@; color: @textDim@; border: none; border-right: 1px solid @border@; padding: 3px 6px; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:vertical { background: @raised@; min-height: 24px; border-radius: 4px; margin: 2px; }
QScrollBar::handle:horizontal { background: @raised@; min-width: 24px; border-radius: 4px; margin: 2px; }
QScrollBar::handle:hover { background: @hover@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QSplitter::handle { background: @window@; }
QSplitter::handle:hover { background: @accent@; }

/* Areas (0.2.12): every panel is a role of a leaf in the area tree. */
QFrame#AreaFrame { background: @panel@; border: 1px solid @border@; }
QWidget#AreaHeader { background: @panel@; }
QToolButton#AreaRoleButton { background: transparent; border: none; font-weight: bold; padding: 1px 6px; }
QToolButton#AreaRoleButton:hover { background: @hover@; }
QToolButton#AreaRoleButton::menu-indicator { image: none; width: 0; }
QToolButton#AreaCollapseButton, QToolButton#AreaMenuButton, QToolButton#PaneMenuButton { background: transparent; border: none; padding: 1px 5px; }
QToolButton#AreaCollapseButton:hover, QToolButton#AreaMenuButton:hover, QToolButton#PaneMenuButton:hover { background: @hover@; }
QToolButton#AreaMenuButton::menu-indicator, QToolButton#PaneMenuButton::menu-indicator { image: none; width: 0; }
QWidget#WorkspaceBar { background: @window@; border-bottom: 1px solid @border@; }
QTabBar#WorkspaceTabs { background: transparent; }
QTabBar#WorkspaceTabs::tab { background: transparent; color: @textDim@; border: none; border-bottom: 2px solid transparent; padding: 3px 12px; }
QTabBar#WorkspaceTabs::tab:hover { color: @text@; background: @hover@; }
QTabBar#WorkspaceTabs::tab:selected { color: @text@; border-bottom-color: @accent@; }
QToolButton#WorkspaceAdd { background: transparent; border: none; padding: 1px 6px; }
QToolButton#WorkspaceAdd:hover { background: @hover@; }
QToolButton#WorkspaceAdd::menu-indicator { image: none; width: 0; }

QWidget#ViewportArea { background: @base@; }
)");
}

QString renderQss(const Tokens& t, const QString& templateText) {
    QString out = templateText;
    const auto sub = [&](const char* name, const QString& value) {
        out.replace(QStringLiteral("@%1@").arg(QLatin1StringView(name)), value);
    };
    const auto col = [&](const char* name, const QColor& c) { sub(name, c.name(QColor::HexRgb)); };
    col("window", t.window);
    col("panel", t.panel);
    col("base", t.base);
    col("raised", t.raised);
    col("hover", t.hover);
    col("pressed", t.pressed);
    col("border", t.border);
    col("text", t.text);
    col("textDim", t.textDim);
    col("textOnAccent", t.textOnAccent);
    col("accent", t.accent);
    col("accentHover", t.accentHover);
    col("selection", t.selection);
    col("warn", t.warn);
    col("error", t.error);
    sub("spacing", QString::number(t.spacing));
    sub("radius", QString::number(t.radius));
    // QSS min-height excludes padding and border.
    sub("controlHeight", QString::number(t.controlHeight - 8));
    sub("splitterWidth", QString::number(t.splitterWidth));
    sub("font", QString::number(t.fontPointSize));
    return out;
}

ApexStyle::ApexStyle(const Tokens* tokens)
    : QProxyStyle(QStringLiteral("Fusion")), tokens_(tokens) {}

int ApexStyle::pixelMetric(PixelMetric metric, const QStyleOption* option,
                           const QWidget* widget) const {
    switch (metric) {
    case PM_SplitterWidth:
    case PM_DockWidgetSeparatorExtent: return tokens_->splitterWidth;
    case PM_ToolBarItemSpacing: return tokens_->spacing;
    case PM_ToolBarSeparatorExtent: return tokens_->spacing * 2;
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing: return tokens_->spacing;
    default: return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

void ApexStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                              QPainter* painter, const QWidget* widget) const {
    // The dotted focus rectangle is noise in a token-styled UI; focus shows as an accent border.
    if (element == PE_FrameFocusRect) return;
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void applyTheme(QApplication& app, Theme theme) {
    applyThemeId(app, theme == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
}

QString applyThemeId(QApplication& app, const QString& id) {
    const ThemeEntry* e = findTheme(id);
    if (!e) e = &registry().front();
    applyTokens(app, e->tokens, e->id);
    return e->id;
}

void applyTokens(QApplication& app, const Tokens& tokens, const QString& id) {
    g_themeId = id;
    current() = tokens;
    const Tokens& t = current();
    if (!g_style) {
        g_style = new ApexStyle(&t);  // Owned by QApplication once set.
        app.setStyle(g_style);
    }
    g_style->setTokens(&t);
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.panel);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.raised);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::ToolTipBase, t.raised);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textDim);
    app.setPalette(p);
    if (!t.fontFamily.isEmpty()) {
        QFont f = app.font();
        f.setFamily(t.fontFamily);
        app.setFont(f);
    }
    app.setStyleSheet(renderQss(t));
}

}  // namespace apex::ui
