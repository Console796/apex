#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "assets/vfs.hpp"
#include "ui/sound_preview.hpp"

#include <doctest/doctest.h>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QDataStream>
#include <QSignalSpy>
#include <QTest>

using namespace apex;
namespace {
// Apex-authored mono PCM WAV, generated here rather than borrowing game audio.
QByteArray soundWav() {
    constexpr quint32 frames = 4410;
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + frames * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(44100)
        << quint32(88200) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(frames * 2);
    for (quint32 i = 0; i < frames; ++i) out << qint16(i % 100 < 50 ? 1000 : -1000);
    return bytes;
}
std::shared_ptr<const assets::Vfs> soundVfs(GameTree& tree) {
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    const auto wav = soundWav();
    tree.put("Game/sound/first.wav", std::string_view(wav.constData(), static_cast<std::size_t>(wav.size())));
    tree.put("Game/sound/second.wav", std::string_view(wav.constData(), static_cast<std::size_t>(wav.size())));
    assets::GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    JobPool jobs(1);
    auto vfs = assets::Vfs::load(profile, jobs);
    REQUIRE(vfs);
    return std::make_shared<assets::Vfs>(std::move(*vfs));
}
void soundApp() {
    qputenv("PULSE_SERVER", "unix:/tmp/apex-sound-preview-no-pulse");
    ui_test::app();
}
}

TEST_CASE("Sound preview decodes mounted WAV to end with no audio device") {
    soundApp();
    GameTree tree;
    ui::SoundPreview preview(nullptr, ui::SoundPreview::Output::DecodeOnly);
    REQUIRE_MESSAGE(preview.available(), preview.unavailableReason().toStdString());
    preview.setVfs(soundVfs(tree));
    QSignalSpy ended(&preview, &ui::SoundPreview::finished);
    QSignalSpy errors(&preview, &ui::SoundPreview::errorOccurred);
    QSignalSpy states(&preview, &ui::SoundPreview::playingChanged);
    auto* decoded = preview.findChild<QAudioBufferOutput*>();
    REQUIRE(decoded);
    qint64 frames = 0;
    QObject::connect(decoded, &QAudioBufferOutput::audioBufferReceived, &preview,
                     [&](const QAudioBuffer& buffer) { frames += buffer.frameCount(); });
    preview.play("sound/first.wav");
    CHECK(preview.currentPath() == "sound/first.wav");
    if (ended.isEmpty()) CHECK(ended.wait(8000));
    CHECK(ended.count() == 1);
    CHECK(errors.isEmpty());
    CHECK_FALSE(preview.isPlaying());
    CHECK(frames == 4410);
    CHECK(states.count() >= 2);
    MESSAGE("Sound WAV: decoded frames=", frames, ", end-of-media signals=", ended.count());
}

TEST_CASE("Sound preview scripts cycle in order and wrap; stop and mount change release playback") {
    soundApp();
    GameTree tree;
    ui::SoundPreview preview(nullptr, ui::SoundPreview::Output::DecodeOnly);
    REQUIRE_MESSAGE(preview.available(), preview.unavailableReason().toStdString());
    preview.setVfs(soundVfs(tree));
    QSignalSpy errors(&preview, &ui::SoundPreview::errorOccurred);
    QSignalSpy paths(&preview, &ui::SoundPreview::currentPathChanged);
    preview.playScript({"sound/first.wav", "sound/second.wav"});
    CHECK(preview.currentPath() == "sound/first.wav");
    preview.next();
    CHECK(preview.currentPath() == "sound/second.wav");
    preview.next();
    CHECK(preview.currentPath() == "sound/first.wav");
    preview.stop();
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
    preview.play("sound/first.wav");
    for (int i = 0; i < 2000 && !preview.isPlaying(); ++i) QTest::qWait(1);
    REQUIRE(preview.isPlaying());
    preview.stop();
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
    preview.playScript({"sound/first.wav", "sound/second.wav"});
    for (int i = 0; i < 2000 && !preview.isPlaying(); ++i) QTest::qWait(1);
    REQUIRE(preview.isPlaying());
    GameTree replacement;
    preview.setVfs(soundVfs(replacement));
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
    preview.next(); // The old script must not survive a mount change.
    CHECK(preview.currentPath().isEmpty());
    QTest::qWait(150);
    CHECK_FALSE(preview.isPlaying());
    CHECK(errors.isEmpty());
    CHECK_FALSE(paths.isEmpty());
    preview.play("sound/second.wav");
    CHECK(preview.currentPath() == "sound/second.wav");
    preview.setVfs({});
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
}

TEST_CASE("Sound preview reports missing unreadable and invalid files without crashing") {
    soundApp();
    GameTree tree;
    ui::SoundPreview preview(nullptr, ui::SoundPreview::Output::DecodeOnly);
    REQUIRE_MESSAGE(preview.available(), preview.unavailableReason().toStdString());
    QSignalSpy errors(&preview, &ui::SoundPreview::errorOccurred);
    preview.play("sound/first.wav"); // No mount yet.
    CHECK(errors.count() == 1);
    preview.setVfs(soundVfs(tree));
    preview.play("sound/missing.wav");
    CHECK(errors.count() == 2);
    std::filesystem::remove(tree.root / "Game/sound/first.wav");
    preview.play("sound/first.wav"); // Still indexed, but unreadable.
    CHECK(errors.count() == 3);
    CHECK_FALSE(errors.last().front().toString().isEmpty());
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
    tree.put("Game/sound/second.wav", "Apex-authored invalid WAV");
    preview.play("sound/second.wav");
    if (errors.count() == 3) CHECK(errors.wait(8000));
    CHECK(errors.count() >= 4);
    CHECK(preview.available()); // A bad file does not disable the backend.
    preview.playScript({});
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
}

TEST_CASE("Sound preview unavailable backend reports a reason and no-op plays") {
    soundApp();
    GameTree tree;
    ui::SoundPreview preview(nullptr, ui::SoundPreview::Output::Unavailable);
    CHECK_FALSE(preview.available());
    CHECK_FALSE(preview.unavailableReason().isEmpty());
    preview.setVfs(soundVfs(tree));
    QSignalSpy errors(&preview, &ui::SoundPreview::errorOccurred);
    preview.play("sound/first.wav");
    preview.playScript({"sound/first.wav", "sound/second.wav"});
    preview.next();
    preview.stop();
    CHECK(errors.count() == 3);
    CHECK(errors.front().front().toString() == preview.unavailableReason());
    CHECK_FALSE(preview.isPlaying());
    CHECK(preview.currentPath().isEmpty());
}
