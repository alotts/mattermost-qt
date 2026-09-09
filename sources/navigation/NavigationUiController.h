#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QEvent;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace Mattermost {

class Backend;
class ChatArea;
class ChannelTree;
class MainWindow;

/**
 * Main-window presentation state that is orthogonal to Mattermost transport:
 * browser-like semantic navigation history and the docked/right-hand thread
 * surface. Kept outside MainWindow so every navigation producer uses the same
 * presentation policy.
 */
class NavigationUiController final : public QObject
{
    Q_OBJECT
public:
    struct Location {
        QString channelId;
        QString rootId;
        QString postId;

        bool isValid() const { return !channelId.isEmpty(); }
        bool sameDestination(const Location& other) const
        {
            return channelId == other.channelId && rootId == other.rootId;
        }
    };

    static NavigationUiController& instance(MainWindow& window);

    explicit NavigationUiController(MainWindow& window);
    ~NavigationUiController() override;

    ChatArea* findThread(const QString& channelId, const QString& rootId) const;
    void presentThread(ChatArea* area);

    /**
     * Close every detached thread window. Detached thread areas live as
     * top-level windows (they are reparented away from the MainWindow) and set
     * WA_DeleteOnClose, so they are not destroyed together with the MainWindow.
     * Closing them here guarantees the process stands down on quit even when a
     * thread window is currently detached.
     */
    void closeAllThreadWindows();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupMainWindow();
    void setupSidebarHeader();
    void setupThreadPane();
    void updateIdentityTooltip();
    void updateHistoryButtons();

    Location captureLocation(ChatArea* area) const;
    void recordArea(ChatArea* area);
    void navigateTo(const Location& location);
    void goBack();
    void goForward();

    void ensureThreadButton(ChatArea* area);
    void attachThread(ChatArea* area);
    void detachThread(ChatArea* area);
    void updateThreadButton(ChatArea* area);

    Backend* backend() const;

    MainWindow& window;
    ChannelTree* channelTree = nullptr;
    QStackedWidget* mainStack = nullptr;
    QSplitter* sidebarSplitter = nullptr;
    QPointer<QSplitter> contentSplitter;
    QStackedWidget* threadStack = nullptr;
    QToolButton* backButton = nullptr;
    QToolButton* forwardButton = nullptr;

    QPointer<ChatArea> activeArea;
    Location currentLocation;
    QVector<Location> backStack;
    QVector<Location> forwardStack;
    bool replayingHistory = false;
    bool threadSplitterStateRestored = false;
};

} // namespace Mattermost
