#pragma once

#include "compile/diagnostics.hpp"
#include "editor/compile_snapshot.hpp"
#include "editor/editor_session.hpp"
#include "ui/qprocess_runner.hpp"
#include <QWidget>
#include <optional>
#include <set>

class QCheckBox;
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
    void setEditorSession(editor::EditorSession* session) { session_ = session; }
    // The injected runner must outlive the panel's job. Changing it cancels the current job.
    void setProcessRunner(compile::ProcessRunner* runner);
    void setCacheDirectory(std::filesystem::path path) { cache_ = std::move(path); }
    void run();
    void cancel();
    void refresh();
    // Selects the diagnostic row's objects in the document and asks for them to be framed.
    void activateDiagnostic(int row);
    int profileCount() const { return static_cast<int>(profiles_.size()); }
signals:
    void frameRequested();
    // Asks to bring a world point into view (the leak start, a diagnostic that names a position).
    void focusRequested(DVec3 point);
private:
    void refreshDiagnostics();
    void loadOverlay();
    void overlayControlsChanged();
    void importProfiles();
    void editProfile();
    void deleteProfile();
    void saveProfiles() const;
    void stopJob();
    document::Document* document_ = nullptr;
    GameContent* content_ = nullptr;
    editor::EditorSession* session_ = nullptr;
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
    QComboBox* severity_;
    QCheckBox* showLeak_;
    QCheckBox* showPortals_;
    QComboBox* cluster_;
    QPushButton* goToLeak_;
    std::filesystem::path overlayDirectory_;
    std::string overlayName_;
    bool overlayLoaded_ = true;
    QLabel* summary_;
    QTableWidget* diagnosticsTable_;
    std::vector<compile::Diagnostic> diagnostics_;
    std::vector<int> shown_; // diagnostics_ index per table row
    std::size_t diagnosticLines_ = 0;
    QTimer* timer_;
    QString error_;
    bool cancelling_ = false;
};

} // namespace apex::ui
