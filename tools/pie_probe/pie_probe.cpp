// Plan 0.3 D20 P0: does a translucent Qt::Popup over a window with a native child pane (1) map within
// a frame of the key press, (2) receive the trigger key's release even 30 ms after the press, (3) get
// pointer motion outside the drawn pie, and (4) show the pane through it? Prints one line per event
// and a summary per pie, to stdout and pie_probe.log.
//
// Focus the grey pane, then press Q: tap it quickly, hold it, flick the mouse while holding, and
// move far outside the circle. Esc or a click closes the pie. Close the window to print totals.

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineF>
#include <QPainter>
#include <QRasterWindow>
#include <QScreen>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <cmath>
#include <optional>

namespace {
QElapsedTimer clock_;
QFile logFile;

void log(const QString& line) {
    const auto text = QStringLiteral("%1 ms  %2").arg(clock_.elapsed(), 7).arg(line);
    QTextStream(stdout) << text << '\n';
    QTextStream(&logFile) << text << '\n';
    logFile.flush();
}

struct Trial {
    qint64 press = 0;
    std::optional<qint64> shown, painted, releasePopup, releasePane;
    int motion = 0, motionOutside = 0;
    double farthest = 0;
};
Trial trial;
int trials = 0, releasesMissed = 0, releasesToPane = 0, slowMaps = 0;

void summarize(const char* how) {
    ++trials;
    const auto ms = [](std::optional<qint64> t) {
        return t ? QString::number(*t - trial.press) : QStringLiteral("never");
    };
    if (!trial.releasePopup) ++releasesMissed;
    if (trial.releasePane) ++releasesToPane;
    if (!trial.painted || *trial.painted - trial.press > 17) ++slowMaps;
    log(QStringLiteral("PIE %1 closed by %2: shown +%3, first paint +%4, release at popup +%5, at pane +%6, "
                       "motion %7 (%8 outside the pie, farthest %9 px)")
            .arg(trials).arg(how).arg(ms(trial.shown), ms(trial.painted), ms(trial.releasePopup),
                                      ms(trial.releasePane))
            .arg(trial.motion).arg(trial.motionOutside).arg(trial.farthest, 0, 'f', 0));
}

constexpr double radius = 100;

class Popup : public QWidget {
public:
    Popup() : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
    }
    void open(QWidget* top, QPoint cursorGlobal) {
        centre_ = top->mapFromGlobal(cursorGlobal);
        released_ = false;
        setParent(top, Qt::Popup | Qt::FramelessWindowHint);  // Wayland popups need a parent.
        setGeometry(QRect(top->mapToGlobal(QPoint(0, 0)), top->size()));
        show();
        activateWindow();
        setFocus();
        grabKeyboard();
    }

protected:
    void showEvent(QShowEvent*) override {
        trial.shown = clock_.elapsed();
        log(QStringLiteral("popup show"));
    }
    void paintEvent(QPaintEvent*) override {
        if (!trial.painted) {
            trial.painted = clock_.elapsed();
            log(QStringLiteral("popup first paint"));
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0, 0, 0, 40));  // Translucent wash over the whole window.
        p.setPen(QPen(QColor(255, 255, 255, 220), 2));
        p.setBrush(QColor(40, 90, 160, 110));
        p.drawEllipse(QPointF(centre_), radius, radius);
        p.setBrush(QColor(255, 255, 255, 200));
        p.drawEllipse(QPointF(centre_), 12, 12);
        p.drawText(QRectF(centre_.x() - 120, centre_.y() + radius + 8, 240, 20), Qt::AlignCenter,
                   QStringLiteral("translucent? you should see the pane's stripes"));
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat()) return;
        log(QStringLiteral("popup key press %1").arg(QKeySequence(e->key()).toString()));
        if (e->key() == Qt::Key_Escape) finish("Esc");
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat() || e->key() != Qt::Key_Q || released_) return;
        released_ = true;
        trial.releasePopup = clock_.elapsed();
        const auto held = *trial.releasePopup - trial.press;
        log(QStringLiteral("popup got Q release, held %1 ms").arg(held));
        if (held >= 200 || trial.farthest >= 7) finish("release");
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        const auto d = QLineF(QPointF(centre_), e->position()).length();
        ++trial.motion;
        trial.farthest = std::max(trial.farthest, d);
        if (d > radius) ++trial.motionOutside;
    }
    void mousePressEvent(QMouseEvent*) override { finish("click"); }
    void hideEvent(QHideEvent*) override {
        if (open_) finish("hide (grab lost?)");
    }

private:
    QPoint centre_;
    bool released_ = false, open_ = true;
    void finish(const char* how) {
        if (!open_) return;
        open_ = false;
        releaseKeyboard();
        summarize(how);
        hide();
        deleteLater();
    }
};

// Stands in for the native Vulkan pane (0.1 D15): a separate QWindow inside the main window.
class Pane : public QRasterWindow {
public:
    explicit Pane(QWidget* top) : top_(top) {}
    QWidget* container = nullptr;

protected:
    void mousePressEvent(QMouseEvent*) override {
        log(QStringLiteral("pane click"));
        requestActivate();
        if (container) container->setFocus();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(QRect(QPoint(), size()), QColor(70, 70, 70));
        p.setPen(QPen(QColor(230, 160, 40), 6));
        for (int x = -height(); x < width(); x += 40) p.drawLine(x, height(), x + height(), 0);
        p.setPen(Qt::white);
        p.drawText(QRect(QPoint(), size()), Qt::AlignCenter, QStringLiteral("Click here, then press Q"));
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() != Qt::Key_Q || e->isAutoRepeat()) return;
        trial = {};
        trial.press = clock_.elapsed();
        log(QStringLiteral("pane Q press"));
        (new Popup)->open(top_, QCursor::pos());
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (e->key() != Qt::Key_Q || e->isAutoRepeat()) return;
        trial.releasePane = clock_.elapsed();
        log(QStringLiteral("pane got Q release (the popup missed it)"));
    }

private:
    QWidget* top_;
};

// As src/ui/viewport_pane.cpp's KeyForwarder: on Wayland the embedded window never sees a key itself.
class KeyForwarder : public QObject {
public:
    KeyForwarder(QWidget* container, Pane* pane) : QObject(container), pane_(pane) {
        container->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return false;
        const auto* key = static_cast<QKeyEvent*>(e);
        QKeyEvent copy(key->type(), key->key(), key->modifiers(), key->text(), key->isAutoRepeat());
        QCoreApplication::sendEvent(pane_, &copy);
        return true;
    }

private:
    Pane* pane_;
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    clock_.start();
    logFile.setFileName(QStringLiteral("pie_probe.log"));
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return 1;
    log(QStringLiteral("platform %1, desktop %2, Qt %3, refresh %4 Hz")
            .arg(QGuiApplication::platformName(), qEnvironmentVariable("XDG_CURRENT_DESKTOP"), qVersion())
            .arg(QGuiApplication::primaryScreen()->refreshRate(), 0, 'f', 0));

    QWidget top;
    top.setWindowTitle(QStringLiteral("Apex pie probe (D20 P0)"));
    auto* layout = new QVBoxLayout(&top);
    layout->addWidget(new QLabel(QStringLiteral(
        "Click the striped pane, press Q. Try: quick taps, long holds, a flick while holding, and moving "
        "far outside the circle. Esc or click closes it. Close this window for the totals.")));
    auto* pane = new Pane(&top);
    auto* container = QWidget::createWindowContainer(pane, &top);
    container->setMinimumSize(800, 500);
    container->setFocusPolicy(Qt::StrongFocus);
    pane->container = container;
    new KeyForwarder(container, pane);
    layout->addWidget(container);
    top.show();

    const int code = app.exec();
    log(QStringLiteral("TOTAL %1 pies: %2 releases missed by the popup (%3 reached the pane), %4 first "
                       "paints later than 17 ms after the press")
            .arg(trials).arg(releasesMissed).arg(releasesToPane).arg(slowMaps));
    return code;
}
