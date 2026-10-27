#pragma once

#include <QObject>
#include <QStringList>
#include <memory>

class QBuffer;
class QMediaPlayer;

namespace apex::assets { class Vfs; }

namespace apex::ui {

// Main-thread controller. DecodeOnly sends PCM to QAudioBufferOutput without a sound device.
class SoundPreview final : public QObject {
    Q_OBJECT
public:
    enum class Output { AudioDevice, DecodeOnly, Unavailable };
    explicit SoundPreview(QObject* parent = nullptr, Output output = Output::AudioDevice);
    ~SoundPreview() override;
    void setVfs(std::shared_ptr<const assets::Vfs> vfs);
    void play(QString soundPath);
    void playScript(QStringList wavePaths);
    void next();
    void stop();
    bool isPlaying() const;
    QString currentPath() const { return currentPath_; }
    bool available() const { return unavailableReason_.isEmpty(); }
    QString unavailableReason() const { return unavailableReason_; }

signals:
    void playingChanged(bool playing);
    void currentPathChanged(const QString& path);
    void availabilityChanged();
    void errorOccurred(const QString& reason);
    void finished();

private:
    void playWave(const QString& path);
    void setCurrentPath(QString path);
    std::shared_ptr<const assets::Vfs> vfs_;
    // Destroy the player before its source device, including during controller destruction.
    std::unique_ptr<QBuffer> buffer_;
    std::unique_ptr<QMediaPlayer> player_;
    QStringList waves_;
    qsizetype wave_ = 0;
    QString currentPath_;
    QString unavailableReason_;
};

} // namespace apex::ui
