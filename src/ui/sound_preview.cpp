#include "ui/sound_preview.hpp"

#include "assets/vfs.hpp"

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QBuffer>
#include <QMediaPlayer>
#include <QUrl>

namespace apex::ui {
namespace {
QString backendReason() {
    return QStringLiteral("Sound preview needs Qt Multimedia's FFmpeg backend, which this build does not include");
}
}

SoundPreview::SoundPreview(QObject* parent, Output output) : QObject(parent) {
    if (output == Output::Unavailable) {
        unavailableReason_ = backendReason();
        return;
    }
    player_ = std::make_unique<QMediaPlayer>();
    if (!player_->isAvailable()) {
        unavailableReason_ = backendReason();
        return;
    }
    if (output == Output::DecodeOnly)
        player_->setAudioBufferOutput(new QAudioBufferOutput(this));
    else
        player_->setAudioOutput(new QAudioOutput(this));
    connect(player_.get(), &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { emit playingChanged(isPlaying()); });
    connect(player_.get(), &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia) emit finished();
            });
    connect(player_.get(), &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& reason) {
                if (!player_->isAvailable() && available()) {
                    unavailableReason_ = backendReason();
                    emit availabilityChanged();
                }
                emit errorOccurred(available() ? reason : unavailableReason_);
            });
}

SoundPreview::~SoundPreview() = default;

void SoundPreview::setVfs(std::shared_ptr<const assets::Vfs> vfs) {
    if (vfs_ == vfs) return;
    stop();
    waves_.clear();
    wave_ = 0;
    vfs_ = std::move(vfs);
}

bool SoundPreview::isPlaying() const {
    return player_ && player_->playbackState() == QMediaPlayer::PlayingState;
}

void SoundPreview::setCurrentPath(QString path) {
    if (currentPath_ == path) return;
    currentPath_ = std::move(path);
    emit currentPathChanged(currentPath_);
}

void SoundPreview::stop() {
    if (player_) {
        player_->stop();
        player_->setSourceDevice(nullptr);
    }
    buffer_.reset();
    setCurrentPath({});
}

void SoundPreview::play(QString soundPath) {
    if (!available()) { emit errorOccurred(unavailableReason_); return; }
    waves_.clear();
    wave_ = 0;
    playWave(soundPath);
}

void SoundPreview::playScript(QStringList wavePaths) {
    if (!available()) { emit errorOccurred(unavailableReason_); return; }
    waves_ = std::move(wavePaths);
    wave_ = 0;
    if (waves_.isEmpty()) {
        stop();
        emit errorOccurred(QStringLiteral("Soundscript has no waves"));
        return;
    }
    playWave(waves_.front());
}

void SoundPreview::next() {
    if (!available()) { emit errorOccurred(unavailableReason_); return; }
    if (waves_.isEmpty()) return;
    wave_ = (wave_ + 1) % waves_.size();
    playWave(waves_[wave_]);
}

void SoundPreview::playWave(const QString& path) {
    stop();
    if (!vfs_) {
        emit errorOccurred(QStringLiteral("No game content is mounted for sound preview"));
        return;
    }
    const auto bytes = vfs_->open(path.toStdString());
    if (!bytes) {
        emit errorOccurred(QString::fromStdString(bytes.error().what()));
        return;
    }
    buffer_ = std::make_unique<QBuffer>();
    buffer_->setData(QByteArray(reinterpret_cast<const char*>(bytes->data()),
                               static_cast<qsizetype>(bytes->size())));
    buffer_->open(QIODevice::ReadOnly);
    setCurrentPath(path);
    player_->setSourceDevice(buffer_.get(), QUrl(path));
    player_->play();
}

} // namespace apex::ui
