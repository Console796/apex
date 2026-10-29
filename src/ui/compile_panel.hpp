#pragma once

#include "compile/diagnostics.hpp"
#include "editor/compile_snapshot.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "ui/qprocess_runner.hpp"
#include <QWidget>
#include <QPointer>
#include <functional>
#include <optional>
#include <set>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class QScrollArea;

namespace apex::ui {
class CordonEditor;
class GameContent;
class CompilePanelView;

class CompilePanel final : public QWidget {
    Q_OBJECT
public:
    explicit CompilePanel(QWidget* parent = nullptr);
    ~CompilePanel() override;
    void setDocument(document::Document* document);
    void setSourcePath(std::filesystem::path path) { source_ = std::move(path); }
    void setGameContent(GameContent* content) { content_ = content; }
    void setEditorSession(editor::EditorSession* session);
    // The injected runner must outlive the panel's job. Changing it cancels the current job.
    void setProcessRunner(compile::ProcessRunner* runner);
    void setCacheDirectory(std::filesystem::path path) { cache_ = std::move(path); }
    // Compiles with the selected profile; the game starts afterwards only when the profile's run game
    // after option is on.
    void run();
    // Compiles, then starts the game whatever the profile says (D9).
    void compileAndRun();
    // Starts the game in the last compiled map without compiling; also the Reload map command.
    void runGame();
    void cancel();
    void refresh();
    // Selects the diagnostic row's objects in the document and asks for them to be framed.
    void activateDiagnostic(int row);
    // The game's console log from the start of the last launch (-condebug), polled while it is watched.
    QString gameLogText() const;
    void pollGameLog();
    std::filesystem::path addonTarget() const;
    int profileCount() const { return static_cast<int>(profiles_.size()); }
    // Runs the map checks (D8) and fills the Problems list; returns what was found.
    const std::vector<editor::Problem>& checkProblems();
    // Selects a problem row's objects and asks for them to be framed.
    void activateProblem(int row);
    // Asked before a compile when the checks found errors; false cancels the compile. Warnings and notes
    // never ask. The default shows a question box.
    using ProblemGate = std::function<bool(int errors, int warnings)>;
    void setProblemGate(ProblemGate gate) { gate_ = std::move(gate); }
signals:
    void frameRequested();
    // Asks to bring a world point into view (the leak start, a diagnostic that names a position).
    void focusRequested(DVec3 point);
private:
    friend class CompilePanelView;
    QPointer<CompilePanelView> view_;
    void startCompile(bool withGame);
    void watchGameLog();
    void refreshDiagnostics();
    void refreshProblemList();
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
    CordonEditor* cordons_;
    QComboBox* problemFilter_;
    QLabel* problemSummary_;
    QTableWidget* problemsTable_;
    std::vector<editor::Problem> problems_;
    std::vector<int> shownProblems_; // problems_ index per table row
    std::optional<std::pair<int, int>> problemsBefore_; // errors, warnings at the last compile start
    ProblemGate gate_;
    std::filesystem::path overlayDirectory_;
    std::string overlayName_;
    bool overlayLoaded_ = true;
    QLabel* summary_;
    QTableWidget* diagnosticsTable_;
    std::vector<compile::Diagnostic> diagnostics_;
    std::vector<int> shown_; // diagnostics_ index per table row
    std::size_t diagnosticLines_ = 0;
    QTimer* timer_;
    QTimer* gameLogTimer_;
    QPlainTextEdit* gameLog_;
    QPushButton* runGame_;
    QPushButton* compileRun_;
    QPushButton* reloadMap_;
    std::filesystem::path gameLogFile_;
    std::uintmax_t gameLogOffset_ = 0;
    QString error_;
    QString addonNotice_;
    QStringList addonsUsed_;
    bool cancelling_ = false;
};

// Area/window presentations share one persistent panel, so moving between workspaces keeps
// the current job, profiles and output. Only the presentation being used hosts its controls.
class CompilePanelView final : public QWidget {
    Q_OBJECT
public:
    CompilePanelView(CompilePanel* panel, QWidget* owner, QWidget* parent = nullptr);
    ~CompilePanelView() override;
    CompilePanel* compilePanel() const { return panel_; }
    void activate();
protected:
    void showEvent(QShowEvent* event) override;
private:
    void release();
    QPointer<CompilePanel> panel_;
    QPointer<QWidget> owner_;
    QScrollArea* scroll_;
    QWidget* placeholder_;
};

} // namespace apex::ui
