#pragma once

#include "editor/compile_snapshot.hpp"
#include "ui/qprocess_runner.hpp"
#include <QWidget>
#include <optional>
#include <set>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace apex::ui {
class GameContent;

class CompilePanel final : public QWidget {
    Q_OBJECT
public:
    explicit CompilePanel(QWidget* parent = nullptr);
    ~CompilePanel() override;
    void setDocument(document::Document* document);
    void setSourcePath(std::filesystem::path path) { source_ = std::move(path); }
    void setGameContent(GameContent* content) { content_ = content; }
    // The injected runner must outlive the panel's job. Changing it cancels the current job.
    void setProcessRunner(compile::ProcessRunner* runner);
    void setCacheDirectory(std::filesystem::path path) { cache_ = std::move(path); }
    void run();
    void cancel();
    void refresh();
private:
    void importProfiles();
    void stopJob();
    document::Document* document_ = nullptr;
    GameContent* content_ = nullptr;
    std::filesystem::path source_, cache_;
    QProcessRunner realRunner_;
    compile::ProcessRunner* runner_ = &realRunner_;
    compile::FilesystemFileOps files_;
    std::unique_ptr<compile::CompileJob> job_;
    std::optional<std::uint64_t> revision_;
    std::vector<compile::CompileProfile> profiles_;
    std::set<int> confirmed_;
    int builtinCount_ = 0;
    std::vector<std::size_t> logged_;
    QComboBox* profilesCombo_;
    QLabel* header_;
    QPushButton* run_;
    QPushButton* cancel_;
    QTableWidget* steps_;
    QPlainTextEdit* log_;
    QTimer* timer_;
    QString error_;
    bool cancelling_ = false;
};

} // namespace apex::ui
