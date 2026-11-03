#include "ui/command_palette.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace apex::ui {

namespace {

enum Role { RightText = Qt::UserRole + 1, Checked, Enabled, Description, Completion };

// Title on the left (with a check mark column), category / shortcut greyed on the right.
class RowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {option.rect.width(), option.fontMetrics.height() + 12};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool selected = option.state & QStyle::State_Selected;
        const bool enabled = index.data(Enabled).toBool();
        if (selected) painter->fillRect(option.rect, option.palette.highlight());
        const auto textColour = selected ? option.palette.highlightedText().color()
                                         : option.palette.text().color();
        auto dim = textColour;
        dim.setAlphaF(enabled ? 0.6f : 0.35f);
        auto main = textColour;
        main.setAlphaF(enabled ? 1.0f : 0.45f);

        const QRect rect = option.rect.adjusted(10, 0, -10, 0);
        const int check = option.fontMetrics.horizontalAdvance(QStringLiteral("✓ "));
        painter->setFont(option.font);
        painter->setPen(main);
        if (index.data(Checked).toBool())
            painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("✓"));
        const QRect right = rect.adjusted(check, 0, 0, 0);
        const auto rightText = index.data(RightText).toString();
        const int rightWidth = option.fontMetrics.horizontalAdvance(rightText);
        painter->setPen(dim);
        painter->drawText(right, Qt::AlignVCenter | Qt::AlignRight, rightText);
        painter->setPen(main);
        const auto title = option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                                         std::max(40, right.width() - rightWidth - 16));
        painter->drawText(right, Qt::AlignVCenter | Qt::AlignLeft, title);
    }
};

// Matches `query` (spaces removed) as a subsequence of `text`.
std::optional<int> subsequenceScore(const QString& query, const QString& text) {
    int score = 0, previous = -2, qi = 0;
    for (int i = 0; i < text.size() && qi < query.size(); ++i) {
        if (text[i].toLower() != query[qi].toLower()) continue;
        score += 10;
        if (i == previous + 1) score += 15;
        if (i == 0 || !text[i - 1].isLetterOrNumber()) score += 12;
        if (qi == 0) score -= std::min(i, 20);
        else if (i > previous + 1) score -= std::min(i - previous - 1, 6);
        previous = i;
        ++qi;
    }
    if (qi < query.size()) return std::nullopt;
    return score;
}

}  // namespace

std::optional<int> fuzzyScore(const QString& query, const QString& text) {
    QString q = query;
    q.remove(QLatin1Char(' '));
    if (q.isEmpty()) return 0;
    auto score = subsequenceScore(q, text);
    // Prefer a contiguous hit over a scattered one.
    if (score && text.contains(query.trimmed(), Qt::CaseInsensitive)) *score += 30;
    return score;
}

void CommandRegistry::addProvider(QString name, Provider provider) {
    removeProvider(name);
    providers_.push_back({std::move(name), std::move(provider)});
}

void CommandRegistry::removeProvider(const QString& name) {
    std::erase_if(providers_, [&](const Entry& entry) { return entry.name == name; });
}

std::vector<PaletteCommand> CommandRegistry::collect() const {
    auto* self = const_cast<CommandRegistry*>(this);
    self->commands_.clear();
    for (const auto& entry : providers_) entry.provider(*self);
    std::vector<PaletteCommand> result;
    for (auto& command : self->commands_) {
        const auto same = std::ranges::find(result, command.id, &PaletteCommand::id);
        if (!command.id.isEmpty() && same != result.end()) *same = std::move(command);
        else result.push_back(std::move(command));
    }
    self->commands_.clear();
    return result;
}

CommandPalette::CommandPalette(QWidget* parent) : QFrame(parent, Qt::Popup) {
    setObjectName(QStringLiteral("commandPalette"));
    setFrameShape(QFrame::StyledPanel);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(6, 6, 6, 6);
    column->setSpacing(4);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("commandPaletteInput"));
    input_->setClearButtonEnabled(true);
    input_->installEventFilter(this);
    column->addWidget(input_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("commandPaletteList"));
    list_->setItemDelegate(new RowDelegate(list_));
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setFrameShape(QFrame::NoFrame);
    column->addWidget(list_, 1);
    footer_ = new QLabel(this);
    footer_->setObjectName(QStringLiteral("commandPaletteFooter"));
    footer_->setWordWrap(true);
    column->addWidget(footer_);

    connect(input_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { activateCurrent(); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { activateCurrent(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { updateFooter(); });
}

void CommandPalette::open() {
    commands_ = registry_.collect();
    std::ranges::stable_sort(commands_, [](const PaletteCommand& a, const PaletteCommand& b) {
        return std::pair(a.category, a.title) < std::pair(b.category, b.title);
    });
    parent_.reset();
    options_.clear();
    {
        const QSignalBlocker block(input_);
        input_->clear();
        input_->setPlaceholderText(tr("Type a command…"));
    }
    refresh();
    if (auto* host = parentWidget()) {
        const QRect area = host->window()->geometry();
        const int width = std::clamp(area.width() / 2, 420, 720);
        const int height = std::clamp(area.height() / 2, 260, 480);
        setFixedSize(width, height);
        QWidget::move(host->window()->mapToGlobal(QPoint((area.width() - width) / 2, area.height() / 8)));
    }
    show();
    raise();
    input_->setFocus();
}

void CommandPalette::refresh() {
    list_->clear();
    actions_.clear();
    keepOpen_.clear();
    const QString query = input_->text();
    // Rows are appended through this so the list and actions_ stay parallel.
    const auto addRow = [&](const QString& title, const QString& right, bool checked, bool enabled,
                            const QString& description, const QString& completion, std::function<void()> action,
                            bool keepOpen) {
        auto* item = new QListWidgetItem(title);
        item->setData(RightText, right);
        item->setData(Checked, checked);
        item->setData(Enabled, enabled);
        item->setData(Description, description);
        item->setData(Completion, completion);
        list_->addItem(item);
        actions_.push_back(enabled ? std::move(action) : std::function<void()>{});
        keepOpen_.push_back(keepOpen);
    };

    struct Candidate {
        int score;
        std::function<void()> add;
    };
    std::vector<Candidate> candidates;
    const auto consider = [&](std::optional<int> score, std::function<void()> add) {
        if (score) candidates.push_back({*score, std::move(add)});
    };

    if (parent_) {
        input_->setPlaceholderText(parent_->title + QStringLiteral(" ›"));
        for (const auto& option : options_) {
            consider(fuzzyScore(query, option.label), [=, this] {
                addRow(option.label, option.detail, option.checked, true, parent_->description, option.label,
                       option.run, false);
            });
        }
    } else {
        input_->setPlaceholderText(tr("Type a command…"));
        for (const auto& command : commands_) {
            const bool enabled = !command.enabled || command.enabled();
            const QString right = command.shortcut.isEmpty() ? command.category
                                                             : command.category + QStringLiteral("   ") + command.shortcut;
            const bool menu = static_cast<bool>(command.options);
            const QString title = menu ? command.title + QStringLiteral("…") : command.title;
            const bool checked = command.checked && command.checked().value_or(false);
            std::optional<int> score = 0;
            if (!query.trimmed().isEmpty()) {
                score = fuzzyScore(query, command.title);
                if (score) *score += 50;
                for (const auto& text : {command.category + QLatin1Char(' ') + command.title, command.keywords.join(' ')}) {
                    const auto other = fuzzyScore(query, text);
                    if (other && (!score || *other - 10 > *score)) score = *other - 10;
                }
            }
            consider(score, [=, this] {
                addRow(title, right, checked, enabled, command.description,
                       menu ? command.title + QStringLiteral(": ") : command.title,
                       [this, command] {
                           if (command.options) enterOptions(command);
                           else command.run();
                       },
                       menu);
            });
            // Typing narrows into a command's options: "display lit" lists "Display mode: Lit" directly.
            if (menu && !query.trimmed().isEmpty() && enabled) {
                for (auto& option : command.options()) {
                    const QString full = command.title + QStringLiteral(": ") + option.label;
                    auto optionScore = fuzzyScore(query, full);
                    if (optionScore) *optionScore -= 5;
                    consider(optionScore, [=] {
                        addRow(full, option.detail.isEmpty() ? command.category : option.detail, option.checked, true,
                               command.description, full, option.run, false);
                    });
                }
            }
        }
    }
    if (!query.trimmed().isEmpty())
        std::ranges::stable_sort(candidates, [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    for (auto& candidate : candidates) candidate.add();
    // Skip disabled rows when choosing the initial highlight.
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(Enabled).toBool()) {
            list_->setCurrentRow(row);
            break;
        }
    }
    if (list_->currentRow() < 0 && list_->count() > 0) list_->setCurrentRow(0);
    updateFooter();
}

void CommandPalette::updateFooter() {
    const auto* item = list_->currentItem();
    QString text = item ? item->data(Description).toString() : QString();
    if (list_->count() == 0) text = tr("No matching commands");
    footer_->setText(text.isEmpty() ? tr("Enter: run   Tab: complete   Esc: close") : text);
}

void CommandPalette::activateCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(actions_.size()) || !actions_[row]) return;
    auto action = actions_[row];
    // A command that lists options keeps the palette open; anything else closes it first so focus is
    // back in the editor before the command runs.
    if (!keepOpen_[row]) hide();
    action();
}

void CommandPalette::autocomplete() {
    if (const auto* item = list_->currentItem()) input_->setText(item->data(Completion).toString());
}

void CommandPalette::enterOptions(const PaletteCommand& command) {
    parent_ = command;
    options_ = command.options();
    {
        const QSignalBlocker block(input_);
        input_->clear();
    }
    refresh();
}

void CommandPalette::leaveOptions() {
    parent_.reset();
    options_.clear();
    input_->clear();
    refresh();
}

void CommandPalette::step(int delta) {
    const int count = list_->count();
    if (count == 0) return;
    int row = list_->currentRow();
    for (int step = 0; step < count; ++step) {
        row = (row + delta + count) % count;
        if (list_->item(row)->data(Enabled).toBool()) break;
    }
    list_->setCurrentRow(row);
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == input_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();  // Keep Tab, Escape and friends out of the window's shortcuts.
        return true;
    }
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down: step(1); return true;
        case Qt::Key_Up: step(-1); return true;
        case Qt::Key_PageDown: for (int i = 0; i < 8; ++i) step(1); return true;
        case Qt::Key_PageUp: for (int i = 0; i < 8; ++i) step(-1); return true;
        case Qt::Key_Tab: autocomplete(); return true;
        case Qt::Key_Backtab: return true;
        case Qt::Key_Escape:
            if (parent_) leaveOptions();
            else hide();
            return true;
        case Qt::Key_Backspace:
            if (parent_ && input_->text().isEmpty()) {
                leaveOptions();
                return true;
            }
            break;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void CommandPalette::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    commands_.clear();
    options_.clear();
    parent_.reset();
    actions_.clear();
    keepOpen_.clear();
}

}  // namespace apex::ui
