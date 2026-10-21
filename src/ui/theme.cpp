#include "ui/theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleOption>

namespace apex::ui {
namespace {

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
    return t;
}

Theme g_theme = Theme::Dark;
ApexStyle* g_style = nullptr;

}  // namespace

const Tokens& tokens(Theme theme) {
    static const Tokens dark = makeDark();
    static const Tokens light = makeLight();
    return theme == Theme::Dark ? dark : light;
}

const Tokens& currentTokens() { return tokens(g_theme); }
Theme currentTheme() { return g_theme; }

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
QStatusBar::item { border: none; }

QPushButton, QToolButton { background: @raised@; border: 1px solid @border@; border-radius: @radius@px; padding: 3px 10px; min-height: @controlHeight@px; }
QToolButton { min-height: 0; padding: 3px 6px; }
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @pressed@; }
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

/* Qt-ADS (side panels): the dock manager's own stylesheet is cleared so these rules apply. */
ads--CDockContainerWidget { background: @window@; }
ads--CDockAreaWidget { background: @panel@; border: 1px solid @border@; border-radius: @radius@px; }
ads--CDockAreaTitleBar { background: @panel@; border-bottom: 1px solid @border@; }
ads--CDockWidgetTab { background: @panel@; border: none; padding: 0 @spacing@px; }
ads--CDockWidgetTab[activeTab="true"] { background: @raised@; }
ads--CDockWidgetTab QLabel { background: transparent; color: @textDim@; }
ads--CDockWidgetTab[activeTab="true"] QLabel { color: @text@; }
ads--CDockWidget { background: @panel@; border: none; }
ads--CDockSplitter::handle { background: @window@; }
ads--CDockSplitter::handle:hover { background: @accent@; }

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
    g_theme = theme;
    const Tokens& t = tokens(theme);
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
