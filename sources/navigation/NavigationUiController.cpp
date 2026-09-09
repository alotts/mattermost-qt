#include "NavigationUiController.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "mainwindow.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ThreadPaneLayout.h"

namespace Mattermost {
namespace {

constexpr int MaxNavigationHistory = 100;

void trimHistory(QVector<NavigationUiController::Location>& history)
{
    if (history.size() > MaxNavigationHistory) {
        history.remove(0, history.size() - MaxNavigationHistory);
    }
}

QIcon threadPresentationIcon(const QPalette& palette)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QPen pen(palette.color(QPalette::ButtonText));
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Deliberately use the literal "two overlapping squares" detach glyph
    // rather than SP_TitleBarNormalButton: desktop styles are free to render
    // that standard pixmap as a platform-specific window-management symbol.
    painter.drawRect(QRect(2, 5, 9, 8));
    painter.drawRect(QRect(5, 2, 9, 8));
    return QIcon(pixmap);
}

} // namespace

NavigationUiController& NavigationUiController::instance(MainWindow& window)
{
    if (auto* existing = window.findChild<NavigationUiController*>(
            QStringLiteral("navigationUiController"), Qt::FindDirectChildrenOnly)) {
        return *existing;
    }
    return *new NavigationUiController(window);
}

NavigationUiController::NavigationUiController(MainWindow& mainWindow)
    : QObject(&mainWindow)
    , window(mainWindow)
{
    setObjectName(QStringLiteral("navigationUiController"));
    setupMainWindow();
    qApp->installEventFilter(this);
}

NavigationUiController::~NavigationUiController()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    if (contentSplitter) {
        QSettings().setValue(QStringLiteral("content_splitter_state"),
                             contentSplitter->saveState());
    }
}

void NavigationUiController::setupMainWindow()
{
    channelTree = window.findChild<ChannelTree*>(QStringLiteral("channelList"));
    mainStack = window.findChild<QStackedWidget*>(QStringLiteral("chatAreaStackedWidget"));

    setupSidebarHeader();
    setupThreadPane();

    if (channelTree) {
        connect(channelTree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem*, QTreeWidgetItem*) {
            QTimer::singleShot(0, this, [this] {
                if (channelTree) {
                    recordArea(channelTree->getCurrentPage());
                }
            });
        });
        channelTree->installEventFilter(this);
        if (channelTree->viewport()) {
            channelTree->viewport()->installEventFilter(this);
        }
        recordArea(channelTree->getCurrentPage());
    }

    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        if (contentSplitter) {
            QSettings().setValue(QStringLiteral("content_splitter_state"),
                                 contentSplitter->saveState());
        }
    });

    updateIdentityTooltip();
    updateHistoryButtons();
}

void NavigationUiController::setupSidebarHeader()
{
    auto* header = window.findChild<QWidget*>(QStringLiteral("lefttop_frame"));
    auto* searchButton = window.findChild<QToolButton*>(QStringLiteral("searchButton"));
    auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    if (username) {
        username->hide();
    }
    if (status) {
        // PresenceStatusLabel keeps forwarding status changes into the avatar;
        // hiding the textual compatibility widget does not disable that path.
        status->hide();
    }
    if (!header || !searchButton) {
        return;
    }

    auto* layout = header->findChild<QHBoxLayout*>(QStringLiteral("horizontalLayout"));
    if (!layout) {
        return;
    }

    backButton = new QToolButton(header);
    backButton->setObjectName(QStringLiteral("navigationBackButton"));
    backButton->setAutoRaise(true);
    backButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    backButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowBack));
    backButton->setToolTip(tr("Back"));
    backButton->setAccessibleName(tr("Back"));

    forwardButton = new QToolButton(header);
    forwardButton->setObjectName(QStringLiteral("navigationForwardButton"));
    forwardButton->setAutoRaise(true);
    forwardButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    forwardButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowForward));
    forwardButton->setToolTip(tr("Forward"));
    forwardButton->setAccessibleName(tr("Forward"));

    int searchIndex = layout->indexOf(searchButton);
    if (searchIndex < 0) {
        searchIndex = layout->count();
    }
    layout->insertWidget(searchIndex, backButton);
    layout->insertWidget(searchIndex + 1, forwardButton);

    connect(backButton, &QToolButton::clicked, this, &NavigationUiController::goBack);
    connect(forwardButton, &QToolButton::clicked, this, &NavigationUiController::goForward);

    auto* backShortcut = new QShortcut(QKeySequence(QKeySequence::Back), &window);
    auto* forwardShortcut = new QShortcut(QKeySequence(QKeySequence::Forward), &window);
    connect(backShortcut, &QShortcut::activated, this, &NavigationUiController::goBack);
    connect(forwardShortcut, &QShortcut::activated, this, &NavigationUiController::goForward);
}

void NavigationUiController::setupThreadPane()
{
    if (!mainStack) {
        return;
    }

    const auto splitters = window.findChildren<QSplitter*>();
    for (QSplitter* candidate : splitters) {
        if (candidate && candidate->indexOf(mainStack) >= 0) {
            sidebarSplitter = candidate;
            break;
        }
    }
    if (!sidebarSplitter) {
        return;
    }
    sidebarSplitter->setObjectName(QStringLiteral("sidebarSplitter"));

    const int oldIndex = sidebarSplitter->indexOf(mainStack);
    const QList<int> outerSizes = sidebarSplitter->sizes();

    contentSplitter = new QSplitter(Qt::Horizontal);
    contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter->setChildrenCollapsible(true);
    contentSplitter->setHandleWidth(4);
    contentSplitter->setOpaqueResize(true);

    contentSplitter->addWidget(mainStack);
    threadStack = new QStackedWidget(contentSplitter);
    threadStack->setObjectName(QStringLiteral("threadStack"));
    threadStack->setMinimumWidth(280);
    threadStack->hide();
    contentSplitter->addWidget(threadStack);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 1);

    sidebarSplitter->insertWidget(std::max(0, oldIndex), contentSplitter);
    if (!outerSizes.isEmpty()) {
        sidebarSplitter->setSizes(outerSizes);
    }

    const QByteArray state = QSettings().value(
        QStringLiteral("content_splitter_state")).toByteArray();
    if (!state.isEmpty()) {
        threadSplitterStateRestored = contentSplitter->restoreState(state);
    }
}

void NavigationUiController::updateIdentityTooltip()
{
    auto* avatar = window.findChild<QWidget*>(QStringLiteral("usericon_label"));
    if (!avatar) {
        return;
    }

    Backend* sourceBackend = backend();
    if (!sourceBackend) {
        if (auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"))) {
            if (!username->text().isEmpty()) {
                avatar->setToolTip(username->text());
            }
        }
        QTimer::singleShot(500, this, &NavigationUiController::updateIdentityTooltip);
        return;
    }

    const BackendUser& user = sourceBackend->getLoginUser();
    const QString displayName = user.getDisplayName().trimmed();
    const QString accountName = user.username.trimmed();
    QStringList lines;
    if (!displayName.isEmpty()) {
        lines.push_back(displayName);
    }
    if (!accountName.isEmpty()
        && QString::compare(displayName, accountName, Qt::CaseInsensitive) != 0) {
        lines.push_back(QStringLiteral("@") + accountName);
    }
    if (lines.isEmpty() && !accountName.isEmpty()) {
        lines.push_back(accountName);
    }
    avatar->setToolTip(lines.join(QLatin1Char('\n')));
}

Backend* NavigationUiController::backend() const
{
    return channelTree ? channelTree->backendInstance() : nullptr;
}

NavigationUiController::Location
NavigationUiController::captureLocation(ChatArea* area) const
{
    Location location;
    if (!area) {
        return location;
    }

    location.channelId = area->getChannel().id;
    if (area->isThread) {
        location.rootId = area->root_id;
    }

    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        QString postId;
        if (log->captureViewportBookmark(postId)) {
            location.postId = postId;
            return location;
        }
    }

    location.postId = area->storedNavigationBookmark();
    return location;
}

void NavigationUiController::recordArea(ChatArea* area)
{
    if (!area) {
        return;
    }

    const Location next = captureLocation(area);
    if (!next.isValid()) {
        return;
    }

    if (!currentLocation.isValid()) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (currentLocation.sameDestination(next)) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }

    if (!replayingHistory && currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
        forwardStack.clear();
    }

    currentLocation = next;
    activeArea = area;
    updateHistoryButtons();
}

void NavigationUiController::updateHistoryButtons()
{
    if (backButton) {
        backButton->setEnabled(!backStack.isEmpty());
    }
    if (forwardButton) {
        forwardButton->setEnabled(!forwardStack.isEmpty());
    }
}

void NavigationUiController::goBack()
{
    if (backStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        forwardStack.push_back(currentLocation);
        trimHistory(forwardStack);
    }

    const Location target = backStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::goForward()
{
    if (forwardStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
    }

    const Location target = forwardStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::navigateTo(const Location& location)
{
    Backend* sourceBackend = backend();
    if (!sourceBackend || !channelTree || !location.isValid()) {
        return;
    }

    BackendChannel* channel = sourceBackend->getStorage().getChannelById(location.channelId);
    if (!channel) {
        return;
    }

    ChatArea* area = nullptr;
    if (location.rootId.isEmpty()) {
        channelTree->openStoredChannel(location.channelId);
        area = channelTree->getCurrentPage();
        if (!area || &area->getChannel() != channel) {
            return;
        }
        recordArea(area);
    } else {
        area = findThread(location.channelId, location.rootId);
        if (!area) {
            ChatArea* parent = channelTree->getCurrentPage();
            if (!parent || &parent->getChannel() != channel) {
                parent = nullptr;
            }
            area = new ChatArea(*sourceBackend, *channel, location.rootId, parent);
            if (parent) {
                parent->threadsAreas.insert(area);
            }
        }
        presentThread(area);
    }

    currentLocation = location;
    activeArea = area;
    if (location.postId.isEmpty() || !area) {
        return;
    }

    // A history entry whose semantic bookmark is still present can be restored
    // locally without a flash. If its source no longer knows the post, do not
    // start another ChatArea-level state machine: resolve and present it through
    // the same application navigation path used by every external post target.
    auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
    if (log && log->restoreViewportBookmark(location.postId)) {
        return;
    }

    AppNavigationService::instance(*sourceBackend).openPost(location.postId);
}

ChatArea* NavigationUiController::findThread(const QString& channelId,
                                             const QString& rootId) const
{
    if (channelId.isEmpty() || rootId.isEmpty()) {
        return nullptr;
    }

    const auto widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || area->root_id != rootId) {
            continue;
        }
        if (area->getChannel().id == channelId) {
            return area;
        }
    }
    return nullptr;
}

void NavigationUiController::closeAllThreadWindows()
{
    // Detached thread areas are reparented to null and shown as top-level
    // windows. Each sets WA_DeleteOnClose, so closing them destroys them.
    // Docked thread areas are children of threadStack and are torn down with
    // the MainWindow; only the top-level (detached) ones need explicit closing.
    QList<QPointer<ChatArea>> detached;
    const auto widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (area && area->isThread && area->isWindow()) {
            detached.append(area);
        }
    }

    for (const QPointer<ChatArea>& area : detached) {
        if (area) {
            area->close();
        }
    }
}

void NavigationUiController::ensureThreadButton(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }

    auto* layout = area->findChild<QHBoxLayout*>(QStringLiteral("propertieslLayout"));
    if (!layout) {
        return;
    }

    auto* presentationButton = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!presentationButton) {
        presentationButton = new QToolButton(area);
        presentationButton->setObjectName(QStringLiteral("threadPresentationButton"));
        presentationButton->setAutoRaise(true);
        presentationButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        presentationButton->setIconSize(QSize(16, 16));
        presentationButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(presentationButton, 0, Qt::AlignVCenter);

        connect(presentationButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }
            if (area->property("threadDetached").toBool()) {
                attachThread(area);
            } else {
                detachThread(area);
            }
        });
    }

    auto* closeButton = area->findChild<QToolButton*>(QStringLiteral("threadCloseButton"));
    if (!closeButton) {
        closeButton = new QToolButton(area);
        closeButton->setObjectName(QStringLiteral("threadCloseButton"));
        closeButton->setAutoRaise(true);
        closeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        closeButton->setIconSize(QSize(16, 16));
        closeButton->setCursor(Qt::PointingHandCursor);
        closeButton->setIcon(area->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        const QString label = tr("Close thread");
        closeButton->setToolTip(label);
        closeButton->setAccessibleName(label);
        layout->addWidget(closeButton, 0, Qt::AlignVCenter);

        connect(closeButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }

            const bool wasActive = activeArea == area;
            if (threadStack && threadStack->indexOf(area) >= 0) {
                const bool wasCurrent = threadStack->currentWidget() == area;
                area->hide();
                threadStack->removeWidget(area);
                if (wasCurrent) {
                    threadStack->hide();
                }
            }

            area->close();
            if (wasActive) {
                QTimer::singleShot(0, this, [this] {
                    if (channelTree) {
                        recordArea(channelTree->getCurrentPage());
                    }
                });
            }
        });
    }

    updateThreadButton(area);
}

void NavigationUiController::updateThreadButton(ChatArea* area)
{
    if (!area) {
        return;
    }
    auto* button = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!button) {
        return;
    }

    const bool detached = area->property("threadDetached").toBool();
    const QString label = detached ? tr("Attach thread") : tr("Detach thread");
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setIcon(threadPresentationIcon(button->palette()));
}

void NavigationUiController::attachThread(ChatArea* area)
{
    if (!area || !threadStack || !contentSplitter) {
        return;
    }

    ensureThreadButton(area);
    area->hide();
    if (threadStack->indexOf(area) < 0) {
        area->setWindowFlag(Qt::Window, false);
        threadStack->addWidget(area);
    }
    area->setProperty("threadDetached", false);
    threadStack->setCurrentWidget(area);

    // A hidden splitter child is reported as size 0 regardless of the width
    // encoded in restoreState(). Make the pane visible first, then decide
    // whether the restored/current geometry is actually usable.
    threadStack->show();
    area->show();

    if (ensureThreadPaneExpanded(*contentSplitter, !threadSplitterStateRestored)) {
        threadSplitterStateRestored = true;
    }
    updateThreadButton(area);
    recordArea(area);
}

void NavigationUiController::detachThread(ChatArea* area)
{
    if (!area || !threadStack) {
        return;
    }

    const bool wasCurrent = threadStack->currentWidget() == area;
    area->hide();
    threadStack->removeWidget(area);
    area->setParent(nullptr);
    area->setWindowFlag(Qt::Window, true);
    area->setAttribute(Qt::WA_DeleteOnClose, true);
    area->setProperty("threadDetached", true);
    updateThreadButton(area);

    if (wasCurrent) {
        threadStack->hide();
    }
    area->show();
    area->raise();
    area->activateWindow();
    recordArea(area);
}

void NavigationUiController::presentThread(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }

    ensureThreadButton(area);
    if (area->property("threadDetached").toBool()) {
        area->show();
        area->raise();
        area->activateWindow();
        recordArea(area);
        return;
    }

    attachThread(area);
}

bool NavigationUiController::eventFilter(QObject* watched, QEvent* event)
{
    const bool channelPointerSurface = channelTree
        && (watched == channelTree || watched == channelTree->viewport());
    if (channelPointerSurface && event
        && event->type() == QEvent::MouseButtonRelease) {
        QTimer::singleShot(0, this, [this] {
            if (channelTree) {
                recordArea(channelTree->getCurrentPage());
            }
        });
    }

    if (event && event->type() == QEvent::Show) {
        auto* area = qobject_cast<ChatArea*>(watched);
        if (area && area->isThread && !area->property("threadDetached").toBool()
            && (!threadStack || threadStack->indexOf(area) < 0)) {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (guard && !guard->property("threadDetached").toBool()) {
                    attachThread(guard);
                }
            });
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost

namespace {

class NavigationUiBootstrap final : public QObject
{
public:
    explicit NavigationUiBootstrap(QObject* parent)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event && event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<Mattermost::MainWindow*>(watched)) {
                if (!window->property("navigationUiSetupScheduled").toBool()) {
                    window->setProperty("navigationUiSetupScheduled", true);
                    QPointer<Mattermost::MainWindow> guard(window);
                    QTimer::singleShot(0, window, [guard] {
                        if (guard) {
                            Mattermost::NavigationUiController::instance(*guard);
                        }
                    });
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void installNavigationUiBootstrap()
{
    if (!qApp) {
        return;
    }
    auto* bootstrap = new NavigationUiBootstrap(qApp);
    qApp->installEventFilter(bootstrap);
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installNavigationUiBootstrap)