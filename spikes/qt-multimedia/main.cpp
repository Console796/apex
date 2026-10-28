// Plays each file given on the command line through QMediaPlayer with no audio output attached (silent,
// so it is safe on a desktop with a live sound server), and reports duration, errors and whether the
// position advanced to the end. Exit code 0 only if every file played to EndOfMedia.
#include <QCoreApplication>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        QMediaPlayer player;
        QAudioBufferOutput decoded; // decoded PCM goes here instead of a sound device
        player.setAudioBufferOutput(&decoded);
        qint64 frames = 0;
        QObject::connect(&decoded, &QAudioBufferOutput::audioBufferReceived, [&](const QAudioBuffer& buffer) { frames += buffer.frameCount(); });
        bool ended = false, errored = false;
        qint64 maxPosition = 0;
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, [&](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::EndOfMedia) ended = true;
        });
        QObject::connect(&player, &QMediaPlayer::errorOccurred, [&](QMediaPlayer::Error, const QString& text) {
            std::printf("  error: %s\n", text.toUtf8().constData());
            errored = true;
        });
        QObject::connect(&player, &QMediaPlayer::positionChanged, [&](qint64 position) { maxPosition = std::max(maxPosition, position); });
        player.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[i])));
        player.play();
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus s) {
            if (s == QMediaPlayer::EndOfMedia || s == QMediaPlayer::InvalidMedia) loop.quit();
        });
        timeout.start(8000);
        loop.exec();
        const bool ok = ended && !errored && player.duration() > 0 && maxPosition >= player.duration() - 50 && frames > 0;
        std::printf("%s: duration=%lld ms, max position=%lld ms, endOfMedia=%d, error=%d, status=%d, state=%d, decoded frames=%lld -> %s\n", argv[i],
                    static_cast<long long>(player.duration()), static_cast<long long>(maxPosition), ended, errored,
                    static_cast<int>(player.mediaStatus()), static_cast<int>(player.playbackState()), static_cast<long long>(frames), ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
    return failures ? 1 : 0;
}
