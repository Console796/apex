#pragma once

// The command palette: a searchable list of everything the editor can do, opened by the
// "app.command_palette" action. It knows nothing about the editor; features register commands.
//
// Adding a command: write a provider (a function that appends commands to a CommandRegistry) and
// pass it to CommandRegistry::addProvider. Providers run each time the palette opens, so titles,
// checked state and options always reflect the current editor state. See command_providers.cpp.
//
//   registry.addProvider("my feature", [](CommandRegistry& r) {
//       r.add({.id = "my.thing", .title = "Do the thing", .category = "My feature", .run = [] { ... }});
//       r.add({.id = "my.pick", .title = "Pick a flavour", .category = "My feature",
//              .options = [] { return std::vector<PaletteOption>{{.label = "Sweet", .run = [] { ... }}}; }});
//   });

#include <QFrame>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <vector>

class QLineEdit;
class QListWidget;
class QLabel;

namespace apex::ui {

// One choice of a command that asks a follow-up question ("Display mode" -> Wireframe, Flat, ...).
struct PaletteOption {
    QString label;
    QString detail;      // Optional grey text after the label.
    bool checked = false;  // Drawn with a check mark (the current value, or an overlay that is on).
    std::function<void()> run;
};

struct PaletteCommand {
    QString id;           // Stable and unique, e.g. "view.display_mode".
    QString title;
    QString category;     // Groups and prefixes the entry: "View", "Edit", ...
    QString description;  // Shown in the footer while the entry is highlighted.
    QStringList keywords; // Extra search terms that are not in the title.
    QString shortcut;     // Display only.
    std::function<void()> run;                          // Runs the command directly...
    std::function<std::vector<PaletteOption>()> options; // ...or lists options to pick from instead.
    std::function<bool()> enabled;                      // Absent: always enabled.
    std::function<std::optional<bool>()> checked;       // Present: a toggle showing its state.
};

class CommandRegistry {
public:
    using Provider = std::function<void(CommandRegistry&)>;

    void add(PaletteCommand command) { commands_.push_back(std::move(command)); }
    void addProvider(QString name, Provider provider);
    void removeProvider(const QString& name);
    // Runs every provider into a fresh list. Later commands with a repeated id replace earlier ones.
    std::vector<PaletteCommand> collect() const;

private:
    struct Entry {
        QString name;
        Provider provider;
    };
    std::vector<Entry> providers_;
    std::vector<PaletteCommand> commands_;  // Filled by add() during collect() only.
};

// Subsequence match, case-insensitive: the score (higher is better) or nullopt when `query` is not
// contained in `text` in order. Rewards consecutive runs, word starts and early matches. Spaces in
// the query match anything, so "disp mode" finds "Display mode".
std::optional<int> fuzzyScore(const QString& query, const QString& text);

class CommandPalette final : public QFrame {
    Q_OBJECT
public:
    explicit CommandPalette(QWidget* parent);
    CommandRegistry& registry() { return registry_; }
    // Collects the commands and shows the palette centred in the parent's upper half.
    void open();

    // Test hooks.
    QLineEdit* input() const { return input_; }
    QListWidget* list() const { return list_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void refresh();
    void activateCurrent();
    void autocomplete();
    void enterOptions(const PaletteCommand& command);
    void leaveOptions();
    void step(int delta);
    void updateFooter();

    CommandRegistry registry_;
    std::vector<PaletteCommand> commands_;
    std::vector<PaletteOption> options_;
    std::optional<PaletteCommand> parent_;  // Set while the list shows a command's options.
    std::vector<std::function<void()>> actions_;  // Per list row: what Enter runs.
    std::vector<bool> keepOpen_;                  // Per list row: Enter leaves the palette open.
    QLineEdit* input_;
    QListWidget* list_;
    QLabel* footer_;
};

}  // namespace apex::ui
