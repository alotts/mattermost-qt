#include "InteractiveTextEdit.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QModelIndex>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

namespace Mattermost {
namespace {

constexpr int CompletionFilterRole = Qt::UserRole + 1;
constexpr int CompletionInsertRole = Qt::UserRole + 2;

} // namespace

InteractiveTextEdit::InteractiveTextEdit(QWidget* parent)
    : QTextEdit(parent)
    , completer(new QCompleter(this))
    , completionModel(new QStandardItemModel(this))
{
    completer->setWidget(this);
    completer->setModel(completionModel);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionRole(CompletionFilterRole);
    completer->setWrapAround(false);

    connect(completer,
            QOverload<const QModelIndex&>::of(&QCompleter::activated),
            this, [this](const QModelIndex& index) {
        acceptCompletion(index);
    });

    const auto scheduleRefresh = [this] {
        QTimer::singleShot(0, this, [this] { refreshCompletion(); });
    };
    connect(this, &QTextEdit::textChanged, this, scheduleRefresh);
    connect(this, &QTextEdit::cursorPositionChanged, this, scheduleRefresh);
}

InteractiveTextEdit::~InteractiveTextEdit() = default;

void InteractiveTextEdit::setCompletionRules(QVector<CompletionRule> rules)
{
    completionRules.clear();
    completionRules.reserve(rules.size());
    for (CompletionRule& rule : rules) {
        if (!rule.prefix.isEmpty() && rule.provider) {
            completionRules.push_back(std::move(rule));
        }
    }
    hideCompletion();
    refreshCompletion();
}

void InteractiveTextEdit::addCompletionRule(CompletionRule rule)
{
    if (rule.prefix.isEmpty() || !rule.provider) {
        return;
    }
    completionRules.push_back(std::move(rule));
    refreshCompletion();
}

void InteractiveTextEdit::clearCompletionRules()
{
    completionRules.clear();
    hideCompletion();
}

bool InteractiveTextEdit::completionPopupVisible() const
{
    return completer && completer->popup() && completer->popup()->isVisible();
}

InteractiveTextEdit::ActiveCompletion InteractiveTextEdit::activeCompletion() const
{
    ActiveCompletion result;
    if (completionRules.isEmpty() || isReadOnly()) {
        return result;
    }

    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return result;
    }

    const QTextBlock block = cursor.block();
    if (!block.isValid()) {
        return result;
    }

    const int blockOffset = cursor.position() - block.position();
    if (blockOffset < 0) {
        return result;
    }

    const QString blockText = block.text();
    const QString beforeCursor = blockText.left(blockOffset);
    int tokenStart = beforeCursor.size();
    while (tokenStart > 0 && !beforeCursor.at(tokenStart - 1).isSpace()) {
        --tokenStart;
    }

    const QString typedToken = beforeCursor.mid(tokenStart);
    const int exclusionOffset = typedToken.startsWith(QLatin1Char('-')) ? 1 : 0;
    const QString ruleText = typedToken.mid(exclusionOffset);

    for (int index = 0; index < completionRules.size(); ++index) {
        const CompletionRule& rule = completionRules.at(index);
        if (!ruleText.startsWith(rule.prefix, Qt::CaseInsensitive)) {
            continue;
        }

        const int queryOffset = exclusionOffset + rule.prefix.size();
        int tokenEnd = blockOffset;
        while (tokenEnd < blockText.size() && !blockText.at(tokenEnd).isSpace()) {
            ++tokenEnd;
        }

        result.ruleIndex = index;
        result.queryStart = block.position() + tokenStart + queryOffset;
        // Replace the complete value belonging to the active prefix, not just
        // the substring before the cursor. This keeps editing an existing token
        // deterministic: `in:geneXral` with the caret at X becomes exactly the
        // selected canonical channel value.
        result.queryEnd = block.position() + tokenEnd;
        result.query = typedToken.mid(queryOffset);
        return result;
    }

    return result;
}

void InteractiveTextEdit::refreshCompletion()
{
    if (!hasFocus()) {
        hideCompletion();
        return;
    }

    const ActiveCompletion active = activeCompletion();
    if (!active.isValid()) {
        hideCompletion();
        return;
    }

    activeRuleIndex = active.ruleIndex;
    activeQueryStart = active.queryStart;
    activeQueryEnd = active.queryEnd;

    rebuildCompletionModel(completionRules.at(active.ruleIndex));
    if (completionModel->rowCount() == 0) {
        hideCompletion();
        return;
    }

    completer->setCompletionPrefix(active.query);
    if (completer->completionCount() <= 0) {
        hideCompletion();
        return;
    }

    QRect rect = cursorRect();
    rect.setWidth(std::max(280, viewport()->width()));
    completer->complete(rect);

    if (completer->popup() && completer->popup()->model()->rowCount() > 0) {
        completer->popup()->setCurrentIndex(completer->popup()->model()->index(0, 0));
    }
}

void InteractiveTextEdit::rebuildCompletionModel(const CompletionRule& rule)
{
    completionModel->clear();
    QVector<CompletionCandidate> candidates = rule.provider ? rule.provider()
                                                            : QVector<CompletionCandidate> {};

    for (const CompletionCandidate& candidate : candidates) {
        const QString insertText = candidate.insertText.isEmpty()
            ? candidate.displayText : candidate.insertText;
        if (candidate.displayText.isEmpty() || insertText.isEmpty()) {
            continue;
        }

        QString display = candidate.displayText;
        if (!candidate.detailText.isEmpty()
            && candidate.detailText != candidate.displayText) {
            display += QStringLiteral("  —  ") + candidate.detailText;
        }

        QStringList filterParts;
        filterParts.reserve(candidate.filterKeys.size() + 2);
        filterParts.push_back(candidate.displayText);
        if (!candidate.detailText.isEmpty()) {
            filterParts.push_back(candidate.detailText);
        }
        filterParts.append(candidate.filterKeys);

        auto* item = new QStandardItem(display);
        item->setData(filterParts.join(QLatin1Char('\n')), CompletionFilterRole);
        item->setData(insertText, CompletionInsertRole);
        completionModel->appendRow(item);
    }
}

void InteractiveTextEdit::acceptCompletion(const QModelIndex& index)
{
    if (!index.isValid() || activeRuleIndex < 0
        || activeRuleIndex >= completionRules.size()
        || activeQueryStart < 0 || activeQueryEnd < activeQueryStart) {
        hideCompletion();
        return;
    }

    const QString insertText = index.data(CompletionInsertRole).toString();
    if (insertText.isEmpty()) {
        hideCompletion();
        return;
    }

    QTextCursor cursor = textCursor();
    cursor.setPosition(activeQueryStart);
    cursor.setPosition(activeQueryEnd, QTextCursor::KeepAnchor);
    cursor.insertText(insertText);
    if (completionRules.at(activeRuleIndex).appendSpace) {
        cursor.insertText(QStringLiteral(" "));
    }
    setTextCursor(cursor);
    hideCompletion();
}

void InteractiveTextEdit::hideCompletion()
{
    activeRuleIndex = -1;
    activeQueryStart = -1;
    activeQueryEnd = -1;
    if (completer && completer->popup()) {
        completer->popup()->hide();
    }
}

void InteractiveTextEdit::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    if (completionPopupVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            // Normally QCompleter's event filter owns Return while the popup is
            // visible. Some platform plugins cannot establish the popup keyboard
            // grab, so make the editor a deterministic fallback instead of
            // accidentally submitting the unfinished text.
            if (completer && completer->popup()) {
                const QModelIndex current = completer->popup()->currentIndex();
                if (current.isValid()) {
                    acceptCompletion(current);
                }
            }
            event->accept();
            return;
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            // QCompleter normally owns these keys too. If they reach the editor,
            // keep them from being interpreted as composer input/submission.
            event->ignore();
            return;
        default:
            break;
        }
    }

    const bool plainEnter = (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        && !(event->modifiers() & Qt::ShiftModifier);
    const bool ctrlEnter = plainEnter
        && (event->modifiers() & Qt::ControlModifier);
    const bool shouldSubmit = submitOnEnter
        && (submitOnCtrlEnter ? ctrlEnter : plainEnter);
    if (shouldSubmit) {
        if (submitHandler) {
            submitHandler();
        }
        event->accept();
        return;
    }

    QTextEdit::keyPressEvent(event);
    QTimer::singleShot(0, this, [this] { refreshCompletion(); });
}

} // namespace Mattermost
