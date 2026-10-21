#pragma once

#include <QColor>
#include <QProxyStyle>
#include <QString>

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

}  // namespace apex::ui
