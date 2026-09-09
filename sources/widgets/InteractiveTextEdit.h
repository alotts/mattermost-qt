#pragma once

#include <functional>
#include <utility>

#include <QStringList>
#include <QTextEdit>
#include <QVector>

class QCompleter;
class QKeyEvent;
class QModelIndex;
class QStandardItemModel;

namespace Mattermost {

/**
 * Shared QTextEdit foundation for composer/search-like inputs.
 *
 * Completion behavior is configured by rules rather than hard-coded into the
 * widget. A rule owns a textual trigger (for example `in:`) and a provider of
 * candidates. The popup performs a case-insensitive contains match across the
 * human-facing label, optional detail text and provider-supplied filter keys.
 *
 * The selected candidate replaces only the text typed after the trigger. This
 * keeps the surrounding query/message untouched and lets each use case choose
 * its own canonical insertion text (channel name/id, username, etc.).
 */
class InteractiveTextEdit : public QTextEdit
{
public:
    struct CompletionCandidate {
        QString displayText;
        QString insertText;
        QString detailText;
        QStringList filterKeys;
    };

    using CompletionProvider = std::function<QVector<CompletionCandidate>()>;

    struct CompletionRule {
        QString prefix;
        CompletionProvider provider;
        bool appendSpace = true;
    };

    explicit InteractiveTextEdit(QWidget* parent = nullptr);
    ~InteractiveTextEdit() override;

    void setCompletionRules(QVector<CompletionRule> rules);
    void addCompletionRule(CompletionRule rule);
    void clearCompletionRules();

    /** Re-query the active completion provider after its data changes asynchronously. */
    void refreshCompletions() { refreshCompletion(); }

    void setSubmitOnEnter(bool enabled) { submitOnEnter = enabled; }
    void setSubmitOnCtrlEnter(bool enabled) { submitOnCtrlEnter = enabled; }
    void setSubmitHandler(std::function<void()> handler)
    {
        submitHandler = std::move(handler);
    }

    bool completionPopupVisible() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct ActiveCompletion {
        int ruleIndex = -1;
        int queryStart = -1;
        int queryEnd = -1;
        QString query;

        bool isValid() const
        {
            return ruleIndex >= 0 && queryStart >= 0 && queryEnd >= queryStart;
        }
    };

    ActiveCompletion activeCompletion() const;
    void refreshCompletion();
    void rebuildCompletionModel(const CompletionRule& rule);
    void acceptCompletion(const QModelIndex& index);
    void hideCompletion();

    QVector<CompletionRule> completionRules;
    QCompleter* completer = nullptr;
    QStandardItemModel* completionModel = nullptr;
    int activeRuleIndex = -1;
    int activeQueryStart = -1;
    int activeQueryEnd = -1;
    bool submitOnEnter = false;
    bool submitOnCtrlEnter = false;
    std::function<void()> submitHandler;
};

} // namespace Mattermost
