#pragma once

#include "editor/overlay_palette.hpp"

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QProxyStyle>
#include <QString>
#include <QStringList>

class QApplication;

namespace apex::ui {

enum class Theme { Dark, Light };

// The single design-token table (plan 0.1, D16). Everything visual in the shell derives from it:
// the QSS template substitutes @name@ placeholders from these fields, and ApexStyle reads the
// metrics for what QSS can't reach.
struct Tokens {
    // Surfaces, darkest to lightest.
    QColor window, panel, base, raised, hover, pressed;
    // Lines and text.
    QColor border, text, textDim, textOnAccent;
    // Accent and status.
    QColor accent, accentHover, selection, warn, error;
    // Viewport overlays, fed to the editor as an editor::OverlayPalette.
    QColor vertexHandle, vertexSelected, vertexInactive, vertexEdge, vertexInvalid, vertexSplit, faceHover;
    QColor gizmoX, gizmoY, gizmoZ, gizmoScreen, gizmoHighlight;
    QColor clip, clipDiscard, block;
    // Metrics in device-independent pixels.
    int spacing = 6;
    int radius = 4;
    int controlHeight = 24;
    int splitterWidth = 4;
    int fontPointSize = 10;
    QString fontFamily;  // Empty keeps the platform font.
};

const Tokens& tokens(Theme theme);
// The tokens of the theme applied last (Dark before the first applyTheme).
const Tokens& currentTokens();
Theme currentTheme();

// The QSS template with @token@ placeholders, and its expansion for a token table.
QString qssTemplate();
QString renderQss(const Tokens& tokens, const QString& templateText = qssTemplate());

// A selectable theme: a built-in, or a custom one loaded from a JSON file in themesDir().
struct ThemeEntry {
    QString id;    // "dark", "light", ... for built-ins; "custom:<file stem>" for custom themes.
    QString name;  // Display name.
    bool builtin = true;
    Tokens tokens;
};

// Names of the colour tokens in theme files, in editor order (the keys of the "colors" object).
const QStringList& colourTokenNames();
// The first viewport overlay token; the theme editor groups the tokens from here on.
inline constexpr const char* kFirstViewportToken = "vertexHandle";
QColor& colourToken(Tokens& tokens, const QString& name);
const QColor& colourToken(const Tokens& tokens, const QString& name);

// The viewport overlay colours of a token table.
editor::OverlayPalette overlayPalette(const Tokens& tokens);

// Built-in themes first, then the custom ones loaded by loadCustomThemes().
const QList<ThemeEntry>& themes();
const ThemeEntry* findTheme(const QString& id);
QString currentThemeId();

// Directory holding custom theme files (<AppData>/themes). Rescans it into the registry; a file
// that fails to parse is skipped and named in `errors`.
QString themesDir();
void loadCustomThemes(const QString& dir = themesDir(), QStringList* errors = nullptr);

// Theme file format: {"name": "...", "base": "dark", "colors": {"window": "#rrggbb", ...},
// "metrics": {"spacing": 6, ...}}. Missing keys inherit from "base" (default "dark").
QJsonObject themeToJson(const ThemeEntry& theme);
bool themeFromJson(const QJsonObject& json, ThemeEntry& out, QString* error = nullptr);

// Writes a custom theme to <dir>/<slug of name>.json and registers it. Returns its id (empty and
// `error` set on failure). Saving over an existing custom theme of the same name replaces it.
QString saveCustomTheme(const ThemeEntry& theme, const QString& dir = themesDir(), QString* error = nullptr);
bool removeCustomTheme(const QString& id, const QString& dir = themesDir());
// Copies a theme file into the themes dir (validating it first); returns the new id.
QString importThemeFile(const QString& path, const QString& dir = themesDir(), QString* error = nullptr);
bool exportTheme(const QString& id, const QString& path);

// Small proxy style: metrics and primitives QSS can't reach (focus rectangles, splitter,
// dock-separator and toolbar spacing).
class ApexStyle : public QProxyStyle {
public:
    explicit ApexStyle(const Tokens* tokens);
    void setTokens(const Tokens* tokens) { tokens_ = tokens; }
    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override;

private:
    const Tokens* tokens_;
};

// Installs ApexStyle (once), the token palette and the expanded QSS on the application.
void applyTheme(QApplication& app, Theme theme);
// Applies a registry theme by id; an unknown id falls back to "dark". Returns the id applied.
QString applyThemeId(QApplication& app, const QString& id);
// Applies explicit tokens (live preview in the theme editor); the id is reported as `id`.
void applyTokens(QApplication& app, const Tokens& tokens, const QString& id = {});

}  // namespace apex::ui
