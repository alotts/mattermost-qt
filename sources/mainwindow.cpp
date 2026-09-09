/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "mainwindow.h"

#include <algorithm>

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWindow>

#include "./ui_mainwindow.h"
#include "Settings.h"
#include "SettingsWindow.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "build-config.h"
#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree-dialogs/FilterListDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "chat-area/ChatArea.h"
#include "log.h"
#include "notifications/NotificationManager.h"
#include "post-collection/PostCollectionView.h"
#include "ui/EmojiPresentation.h"
#include "ui/IconUtils.h"

namespace Mattermost {
namespace {

bool filterTreeItem(QTreeWidgetItem* item, const QString& term)
{
	if (!item) {
		return false;
	}

	bool childMatches = false;
	for (int i = 0; i < item->childCount(); ++i) {
		childMatches = filterTreeItem(item->child(i), term) || childMatches;
	}

	const bool ownMatch = term.isEmpty()
		|| item->text(0).contains(term, Qt::CaseInsensitive);
	const bool visible = ownMatch || childMatches;
	item->setHidden(!visible);
	return visible;
}

} // namespace

MainWindow::MainWindow(QWidget* parent, QSystemTrayIcon& trayIcon, Backend& _backend)
    : QMainWindow(parent)
    , ui(std::make_unique<Ui::MainWindow>())
    , trayIcon(trayIcon)
    , notificationManager(std::make_unique<NotificationManager>(trayIcon))
    , chooseEmojiDialog(this)
    , backend(_backend)
    , currentTeamRestoredFromSettings(false)
    , doDeinit(false)
{
	LOG_DEBUG("MainWindow create start");

	ui->setupUi(this);
	ui->toolButton->installEventFilter(this);
    ui->searchButton->installEventFilter(this);
	setupChannelTabs();
	refreshMenuButtonIcon();
    refreshSearchButtonIcon();
    connect(ui->searchButton, &QToolButton::clicked,
            this, &MainWindow::openMessageSearch);
    connect(ui->channelList, &ChannelTree::virtualDestinationRequested,
            this, [this](int destination, const QString& teamId) {
        if (destination == SidebarItem::SavedDestination) {
            openSavedMessages(teamId);
        }
    });
	ui->channelList->setChatAreaStackedWidget(ui->chatAreaStackedWidget);
	ui->channelList->setFocus();

	createMenu();

	const BackendUser& currentUser = backend.getLoginUser();
	if (currentUser.id.isEmpty()) {
		ui->usericon_label->clear();
		qCritical() << "Current User's ID is empty string";
		return;
	}

	auto& sidebar = SidebarService::instance(backend);
	auto& userProfiles = UserProfileService::instance(backend);
	connect(&sidebar, &SidebarService::channelActivityChanged, this,
	        [this](const QString&) { refreshSidebarViews(); });
	connect(&sidebar, &SidebarService::channelActivityReset,
	        this, &MainWindow::refreshSidebarViews);
	connect(&sidebar, &SidebarService::categoriesChanged, this,
	        [this](const QString&) {
		QTimer::singleShot(0, this, &MainWindow::refreshSidebarViews);
	});

	connect(ui->channelList, &QTreeWidget::currentItemChanged, this,
	        [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
		if (!current
			|| current->data(0, ChannelTree::ItemKindRole).toInt()
				!= ChannelTree::ChannelItemKind) {
			return;
		}

		const QString channelId = current->data(0, ChannelTree::ItemIdRole).toString();
		if (channelId.isEmpty()) {
			return;
		}

		const bool channelsTabVisible = channelTabs && channelsPage
			&& channelTabs->currentWidget() == channelsPage;
		if (channelsTabVisible && unreadFilterButton && unreadFilterButton->isChecked()) {
			retainedUnreadFilterChannelId = channelId;
			QTimer::singleShot(0, this, &MainWindow::refreshChannelUnreadFilter);
		} else if (channelsTabVisible) {
			retainedUnreadFilterChannelId.clear();
		}
	});

	recentChannels->initialize(backend);
	attentionList->initialize(backend);
	installRealtimeUiSync();

	sidebar.clear();
	userProfiles.clear();
	sidebar.retrieveChannelMemberships();

	connect(&currentUser, &BackendUser::onStatusChanged, [this, &currentUser] {
		ui->statusLabel->setText(currentUser.status);
	});
	ui->usernameLabel->setText(currentUser.username);

	connect(&currentUser, &BackendUser::onAvatarChanged, [this, &currentUser] {
		LOG_DEBUG("Got User Image");
		ui->usericon_label->setPixmap(currentUser.avatar);
	});

	connect(&backend, &Backend::onAllTeamChannelsPopulated, [this] {
		ui->channelList->populateSidebars(backend);
		attentionList->refreshThreads();
		QTimer::singleShot(0, this, &MainWindow::refreshSidebarViews);
		initializationComplete();
	});

	connect(&backend, &Backend::onNewPost,
	        [this](BackendChannel& channel, const BackendPost& post) {
		messageNotify(channel, post);
	});

	connect(&backend, &Backend::onChannelViewed, [this](const BackendChannel& channel) {
		SidebarService::instance(backend).setChannelMentioned(channel.id, false);
	});

	connect(&backend, &Backend::onAddedToTeam, [this](BackendTeam& team) {
		ui->channelList->addTeam(backend, team);
	});

	backend.retrieveOwnTeams([this](BackendTeam& team) {
		ui->channelList->addTeam(backend, team);
	});

	LOG_DEBUG("MainWindow signal register finish");

	QSettings settings;
	restoreGeometry(settings.value("geometry", saveGeometry()).toByteArray());
	const QByteArray splitterState = settings.value("sidebar_splitter_state").toByteArray();
	if (sidebarSplitter && !splitterState.isEmpty()) {
		sidebarSplitter->restoreState(splitterState);
	} else if (sidebarSplitter) {
		sidebarSplitter->setSizes({280, std::max(360, width() - 280)});
	}

	connect(qApp, &QApplication::aboutToQuit, this, &MainWindow::saveState);
	LOG_DEBUG("MainWindow create finish");
}

MainWindow::~MainWindow() = default;

static QString infoText(QString("Version " PROJECT_VER "<br/>"
                                "An unofficial Mattermost Client, using the QT framework<br/>") +
R"(
<br/>
More information:<br/>
<a href='https://github.com/nuclear868/mattermost-qt'>https://github.com/nuclear868/mattermost-qt</a>
<br/>
<br/>
Mattermost QT Copyright 2021, 2022 Lyubomir Filipov<br/>
<br/>
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.<br/>
<br/>
You should have received a copy of the GNU Lesser General Public License
along with Mattermost-QT. if not, see <a href='https://www.gnu.org/licenses/'>https://www.gnu.org/licenses/</a>.<br/>
)");

void MainWindow::setupChannelTabs()
{
	ui->gridLayout_2->removeWidget(ui->lefttop_frame);
	ui->gridLayout_2->removeWidget(ui->channelList);
	ui->gridLayout_2->removeWidget(ui->chatAreaStackedWidget);

	channelTabs = new QTabWidget(ui->centralwidget);
	channelTabs->setDocumentMode(true);
	channelTabs->setSizePolicy(ui->channelList->sizePolicy());

	channelsPage = new QWidget(channelTabs);
	auto* channelsLayout = new QVBoxLayout(channelsPage);
	channelsLayout->setContentsMargins(0, 0, 0, 0);
	channelsLayout->setSpacing(0);
	channelsLayout->addWidget(ui->channelList, 1);

	recentChannels = new ChannelQuickList(channelTabs);
	attentionList = new AttentionList(channelTabs);
	attentionList->setContextMenuPolicy(Qt::CustomContextMenu);

	channelTabs->addTab(channelsPage, tr("Channels"));
	channelTabs->addTab(recentChannels, tr("Following"));
	channelTabs->addTab(attentionList, tr("Attention"));

	auto* leftSidebar = new QWidget(ui->centralwidget);
	auto* leftLayout = new QVBoxLayout(leftSidebar);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->setSpacing(0);
	leftLayout->addWidget(ui->lefttop_frame);

	auto* sidebarFilterRow = new QWidget(leftSidebar);
	auto* sidebarFilterLayout = new QHBoxLayout(sidebarFilterRow);
	sidebarFilterLayout->setContentsMargins(0, 0, 3, 0);
	sidebarFilterLayout->setSpacing(3);

	sidebarFilterEdit = new QLineEdit(sidebarFilterRow);
	sidebarFilterEdit->setClearButtonEnabled(true);
	sidebarFilterEdit->setPlaceholderText(tr("Filter channels or contacts…"));
	sidebarFilterEdit->setAccessibleName(tr("Filter channels or contacts"));
	sidebarFilterEdit->setContentsMargins(4, 2, 4, 2);

	unreadFilterButton = new QToolButton(sidebarFilterRow);
	unreadFilterButton->setCheckable(true);
	unreadFilterButton->setAutoRaise(true);
	unreadFilterButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	unreadFilterButton->setToolTip(tr("Show unread only"));
	unreadFilterButton->setAccessibleName(tr("Show unread only"));
	unreadFilterButton->installEventFilter(this);
	refreshUnreadFilterIcon();

	sidebarFilterLayout->addWidget(sidebarFilterEdit, 1);
	sidebarFilterLayout->addWidget(unreadFilterButton);
	leftLayout->addWidget(sidebarFilterRow);
	leftLayout->addWidget(channelTabs, 1);

	sidebarSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
	sidebarSplitter->setChildrenCollapsible(false);
	sidebarSplitter->setHandleWidth(4);
	sidebarSplitter->setOpaqueResize(true);
	sidebarSplitter->addWidget(leftSidebar);
	sidebarSplitter->addWidget(ui->chatAreaStackedWidget);
	sidebarSplitter->setStretchFactor(0, 0);
	sidebarSplitter->setStretchFactor(1, 1);
	ui->gridLayout_2->addWidget(sidebarSplitter, 0, 0, 2, 2);
	ui->gridLayout_2->setRowStretch(0, 1);
	ui->gridLayout_2->setRowStretch(1, 0);
	ui->gridLayout_2->setColumnStretch(0, 1);

	connect(sidebarFilterEdit, &QLineEdit::textChanged,
	        this, &MainWindow::refreshSidebarViews);
	connect(unreadFilterButton, &QToolButton::toggled, this, [this] {
		refreshUnreadFilterIcon();
		refreshChannelUnreadFilter();
	});

	connect(ui->channelList, &QTreeWidget::itemClicked, this,
	        [this](QTreeWidgetItem* item, int column) {
		if (!item || column != 1
			|| item->data(0, ChannelTree::ItemKindRole).toInt()
				!= ChannelTree::CategoryItemKind) {
			return;
		}

		const QString teamId = item->data(0, ChannelTree::ItemTeamIdRole).toString();
		const QString categoryId = item->data(0, ChannelTree::ItemIdRole).toString();
		const SidebarTeamState* state = SidebarService::instance(backend).teamState(teamId);
		const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
		if (category && category->type == QStringLiteral("direct_messages")) {
			openDirectMessageSearch();
		}
	});

	connect(recentChannels, &ChannelQuickList::channelSelected,
	        ui->channelList, &ChannelTree::openChannel);
	connect(recentChannels, &ChannelQuickList::channelContextMenuRequested,
	        ui->channelList, &ChannelTree::showChannelContextMenu);
	connect(attentionList, &AttentionList::channelSelected,
	        ui->channelList, &ChannelTree::openChannel);
	connect(attentionList, &QTreeWidget::customContextMenuRequested, this,
	        [this](const QPoint& pos) {
		const QString channelId = attentionList->channelIdAt(pos);
		if (!channelId.isEmpty()) {
			ui->channelList->showChannelContextMenu(
				channelId, attentionList->viewport()->mapToGlobal(pos));
		}
	});
	connect(attentionList, &AttentionList::attentionCountChanged, this,
	        [this](uint32_t count) {
		setNotificationsCountVisualization(count);
		const int index = channelTabs ? channelTabs->indexOf(attentionList) : -1;
		if (index < 0) {
			return;
		}

		const QString label = count == 0
			? tr("Attention")
			: tr("Attention") + QStringLiteral(" (%1)").arg(count);
		channelTabs->setTabText(index, label);

		if (count == 0) {
			channelTabs->setTabIcon(index, QIcon());
			channelTabs->setTabToolTip(index, QString());
			return;
		}

		QIcon icon = QIcon::fromTheme(QStringLiteral("mail-unread"));
		if (icon.isNull()) {
			icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
		}
		channelTabs->setTabIcon(index, icon);
		channelTabs->setTabToolTip(index,
			tr("%1 item(s) need your attention").arg(count));
	});

	connect(channelTabs, &QTabWidget::currentChanged, this, [this](int index) {
		QWidget* currentPage = channelTabs->widget(index);
		if (attentionList && currentPage != attentionList) {
			attentionList->releaseSelectionRetention();
		}
		if (currentPage != channelsPage && !retainedUnreadFilterChannelId.isEmpty()) {
			retainedUnreadFilterChannelId.clear();
			refreshChannelUnreadFilter();
		}
		if (attentionList && currentPage == attentionList) {
			attentionList->refresh();
			attentionList->refreshThreads();
		}
		refreshSidebarViews();
	});
}

void MainWindow::refreshSidebarViews()
{
	if (recentChannels) {
		recentChannels->refresh();
		applySidebarTextFilter(recentChannels);
	}
	if (attentionList) {
		attentionList->refresh();
		applySidebarTextFilter(attentionList);
	}
	refreshChannelUnreadFilter();
}

void MainWindow::applySidebarTextFilter(QTreeWidget* tree) const
{
	if (!tree) {
		return;
	}

	const QString term = sidebarFilterEdit ? sidebarFilterEdit->text().trimmed() : QString();
	for (int i = 0; i < tree->topLevelItemCount(); ++i) {
		filterTreeItem(tree->topLevelItem(i), term);
	}
}

void MainWindow::refreshChannelUnreadFilter()
{
	if (!ui || !ui->channelList || !unreadFilterButton) {
		return;
	}

	const bool unreadOnly = unreadFilterButton->isChecked();

	const QSettings independentSettings;
	const bool alwaysShowFollowing =
		independentSettings.value(ALWAYS_SHOW_FOLLOWING_TAB,
		                          ALWAYS_SHOW_FOLLOWING_TAB_DEFAULT).toBool();
	if (!alwaysShowFollowing && channelTabs && recentChannels) {
		const int followingIndex = channelTabs->indexOf(recentChannels);
		if (unreadOnly && followingIndex >= 0) {
			channelTabs->removeTab(followingIndex);
		} else if (!unreadOnly && followingIndex < 0) {
			channelTabs->insertTab(1, recentChannels, tr("Following"));
		}
	}

	const QString filterText = sidebarFilterEdit
		? sidebarFilterEdit->text().trimmed() : QString();
	const bool textFilterActive = !filterText.isEmpty();
	const bool anyFilterActive = unreadOnly || textFilterActive;

	if (!unreadOnly) {
		retainedUnreadFilterChannelId.clear();
	}
	const bool channelsTabVisible = channelTabs && channelsPage
		&& channelTabs->currentWidget() == channelsPage;
	auto& sidebar = SidebarService::instance(backend);

	for (int teamIndex = 0; teamIndex < ui->channelList->topLevelItemCount(); ++teamIndex) {
		QTreeWidgetItem* teamItem = ui->channelList->topLevelItem(teamIndex);
		if (!teamItem) {
			continue;
		}

		bool teamHasVisibleChannels = false;
		for (int categoryIndex = 0; categoryIndex < teamItem->childCount(); ++categoryIndex) {
			QTreeWidgetItem* categoryItem = teamItem->child(categoryIndex);
			if (!categoryItem) {
				continue;
			}

			const QString teamId = categoryItem->data(0, ChannelTree::ItemTeamIdRole).toString();
			const QString categoryId = categoryItem->data(0, ChannelTree::ItemIdRole).toString();
			const SidebarTeamState* state = sidebar.teamState(teamId);
			const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
			const bool directMessages = category
				&& category->type == QStringLiteral("direct_messages");

			categoryItem->setText(1, directMessages ? QStringLiteral("+") : QString());
			categoryItem->setTextAlignment(1, Qt::AlignCenter);
			categoryItem->setToolTip(1, directMessages ? tr("Start direct message") : QString());

			// Mattermost's category channel_ids order is not guaranteed to track
			// live DM activity. Keep the server category membership but display
			// direct conversations strictly newest-first. Reordering a current
			// QTreeWidgetItem can transiently make Qt select a neighbour, so keep
			// the model mutation selection-atomic and never expose that temporary
			// current item as navigation.
			if (directMessages && categoryItem->childCount() > 1) {
				QTreeWidgetItem* selectedItem = ui->channelList->currentItem();
				QSignalBlocker selectionSignals(ui->channelList);
				QVector<QTreeWidgetItem*> children;
				children.reserve(categoryItem->childCount());
				for (int i = 0; i < categoryItem->childCount(); ++i) {
					children.push_back(categoryItem->child(i));
				}
				std::stable_sort(children.begin(), children.end(),
				                 [this, &sidebar](QTreeWidgetItem* a, QTreeWidgetItem* b) {
					BackendChannel* channelA = backend.getStorage().getChannelById(
						a ? a->data(0, ChannelTree::ItemIdRole).toString() : QString());
					BackendChannel* channelB = backend.getStorage().getChannelById(
						b ? b->data(0, ChannelTree::ItemIdRole).toString() : QString());
					if (!channelA || !channelB) {
						return channelA != nullptr;
					}
					return sidebar.channelActivityTime(*channelA)
						> sidebar.channelActivityTime(*channelB);
				});

				for (int desiredIndex = 0; desiredIndex < children.size(); ++desiredIndex) {
					QTreeWidgetItem* child = children.at(desiredIndex);
					const int currentIndex = categoryItem->indexOfChild(child);
					if (currentIndex >= 0 && currentIndex != desiredIndex) {
						categoryItem->insertChild(desiredIndex,
						                          categoryItem->takeChild(currentIndex));
					}
				}
				if (selectedItem && selectedItem->treeWidget() == ui->channelList) {
					ui->channelList->setCurrentItem(selectedItem);
				}
			}

			bool categoryHasVisibleChannels = false;
			for (int channelIndex = 0; channelIndex < categoryItem->childCount(); ++channelIndex) {
				QTreeWidgetItem* channelItem = categoryItem->child(channelIndex);
				if (!channelItem) {
					continue;
				}

				const QString channelId = channelItem->data(0, ChannelTree::ItemIdRole).toString();
				BackendChannel* channel = backend.getStorage().getChannelById(channelId);
				bool matchesText = !textFilterActive
					|| channelItem->text(0).contains(filterText, Qt::CaseInsensitive);

				if (channel && channel->type == BackendChannel::directChannel && textFilterActive) {
					const BackendUser* user = backend.getStorage().getUserById(channel->name);
					if (user) {
						matchesText = matchesText
							|| user->getDisplayName().contains(filterText, Qt::CaseInsensitive)
							|| user->username.contains(filterText, Qt::CaseInsensitive)
							|| user->email.contains(filterText, Qt::CaseInsensitive);
					}
				}

				bool matchesUnread = true;
				if (unreadOnly) {
					const bool retained = channelsTabVisible
						&& channelId == retainedUnreadFilterChannelId;
					matchesUnread = channel
						&& !sidebar.isChannelMuted(*channel)
						&& (sidebar.isChannelUnread(*channel) || retained);
				}

				const bool visible = matchesText && matchesUnread;
				channelItem->setHidden(!visible);
				categoryHasVisibleChannels = categoryHasVisibleChannels || visible;
			}

			categoryItem->setHidden(anyFilterActive && !categoryHasVisibleChannels);
			teamHasVisibleChannels = teamHasVisibleChannels || categoryHasVisibleChannels;
		}

		teamItem->setHidden(anyFilterActive && !teamHasVisibleChannels);
	}
}

void MainWindow::showCollectionPage(PostCollectionView* page)
{
    if (!page || !ui || !ui->chatAreaStackedWidget) {
        return;
    }
    if (ChatArea* current = ui->channelList->getCurrentPage()) {
        current->onDeactivate();
    }
    if (ui->chatAreaStackedWidget->indexOf(page) < 0) {
        ui->chatAreaStackedWidget->addWidget(page);
    }
    ui->chatAreaStackedWidget->setCurrentWidget(page);
}

void MainWindow::openSavedMessages(const QString& teamId)
{
    Q_UNUSED(teamId)
    if (!savedMessagesPage) {
        savedMessagesPage = new PostCollectionView(
            backend, PostCollectionView::Mode::Saved, ui->chatAreaStackedWidget);
    }
    showCollectionPage(savedMessagesPage);
    savedMessagesPage->activateSaved();
}

void MainWindow::openMessageSearch()
{
    if (!searchMessagesPage) {
        searchMessagesPage = new PostCollectionView(
            backend, PostCollectionView::Mode::Search, ui->chatAreaStackedWidget);
    }

    QString preferredTeamId;
    if (BackendChannel* channel = backend.getCurrentChannel()) {
        if (channel->team) {
            preferredTeamId = channel->team->id;
        }
    }

    // Search is a transient destination, not the selected sidebar channel. Clear
    // currentItem so clicking the previously active channel will always navigate
    // back even though its tree row did not otherwise change.
    ui->channelList->setCurrentItem(nullptr);
    showCollectionPage(searchMessagesPage);
    searchMessagesPage->activateSearch(preferredTeamId);
}

void MainWindow::openDirectMessageSearch()
{
	FilterListDialogConfig config;
	config.title = tr("Start Direct Message");
	config.description = tr("Search the Mattermost user directory and select a person to message.");
	config.filterLabelText = tr("Search users");
	config.buttons = QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
	config.disabledItemTooltip = tr("This user cannot be selected");

	UserSearchOptions options;
	options.limit = 100;

	auto* dialog = new UserSearchDialog(backend, config, options, {}, this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	connect(dialog, &QDialog::accepted, this, [this, dialog] {
		const BackendUser* user = dialog->getSelectedUser();
		if (user) {
			backend.createDirectChannel(*user);
		}
	});
	dialog->show();
}

void MainWindow::createMenu()
{
	mainMenu = new QMenu(ui->toolButton);

	QMenu* fileMenu = mainMenu->addMenu("File");
	fileMenu->addAction("Logout", [this] {
		backend.logout([this] {
			doDeinit = true;
			QMainWindow::close();
			LOG_DEBUG("Logout done");
		});
	});

	mainMenu->addAction("Settings", [this] {
		settingsWindow = new SettingsWindow(this);
		connect(settingsWindow, &QDialog::accepted, [this] {
			if (QMessageBox::question(
					this, "Reload?",
					"In order to apply some settings, Mattermost has to be reloaded.\n"
					" Do you want to reload now? (If no, settings will be applied on the next startup)")
				== QMessageBox::Yes) {
				settingsWindow->applyNewSettings();
			}
			reload();
		});
		settingsWindow->show();
	});

	QMenu* helpMenu = mainMenu->addMenu("Help");
	helpMenu->addAction("About Mattermost", [this] {
		auto* msgBox = new QMessageBox(QMessageBox::Information,
		                               "About Mattermost", infoText);
		msgBox->setIconPixmap(windowIcon().pixmap(QSize(64, 64)));
		msgBox->setTextFormat(Qt::RichText);
		msgBox->setStandardButtons(QMessageBox::Ok);
		msgBox->setDefaultButton(QMessageBox::Ok);
		msgBox->open();
	});

	helpMenu->addAction("About QT", [this] {
		QMessageBox::aboutQt(this, "About QT");
	});

	ui->toolButton->setMenu(mainMenu);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (event && event->type() == QEvent::PaletteChange) {
		if (watched == ui->toolButton) {
			refreshMenuButtonIcon();
        } else if (watched == ui->searchButton) {
            refreshSearchButtonIcon();
		} else if (watched == unreadFilterButton) {
			refreshUnreadFilterIcon();
		}
	}
	return QMainWindow::eventFilter(watched, event);
}

void MainWindow::refreshMenuButtonIcon()
{
	if (!ui || !ui->toolButton) {
		return;
	}
	ui->toolButton->setIcon(IconUtils::tintedSymbolicIcon(
		QStringLiteral(":/icons/burger"),
		ui->toolButton->palette().color(QPalette::ButtonText)));
}

void MainWindow::refreshSearchButtonIcon()
{
    if (!ui || !ui->searchButton) {
        return;
    }
    ui->searchButton->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/search"),
        ui->searchButton->palette().color(QPalette::ButtonText)));
}

void MainWindow::refreshUnreadFilterIcon()
{
	if (!unreadFilterButton) {
		return;
	}

	unreadFilterButton->setIcon(QIcon());
	unreadFilterButton->setText(QStringLiteral("📬"));
	unreadFilterButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	QFont emojiFont = unreadFilterButton->font();
	EmojiPresentation::preferEmojiFont(emojiFont);
	emojiFont.setPixelSize(18);
	unreadFilterButton->setFont(emojiFont);

	unreadFilterButton->setToolTip(unreadFilterButton->isChecked()
		? tr("Show all channels and Following")
		: tr("Show unread only"));
}

void MainWindow::moveEvent(QMoveEvent*)
{
	ChatArea* currentPage = ui->channelList->getCurrentPage();
	if (currentPage) {
		currentPage->onMove(pos());
	}
}

void MainWindow::dragMoveEvent(QDragMoveEvent*)
{
	qDebug() << "dragMove" << mapToGlobal(pos());
}

void MainWindow::reload()
{
	QTimer::singleShot(0, [this] {
		backend.reset();
		doDeinit = true;
		QMainWindow::close();
	});
}

void MainWindow::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::ActivationChange) {
		if (isActiveWindow()) {
			ChatArea* currentPage = ui->channelList->getCurrentPage();
			if (currentPage) {
				currentPage->onMainWindowActivate();
			}
		}
	} else {
		qDebug() << event->type();
	}
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	qDebug() << "closeEvent";
	if (doDeinit) {
		qDebug() << "QMainWindow closeEvent";
		saveState();
		return QMainWindow::closeEvent(event);
	}

	if (trayIcon.isVisible()) {
		hide();
		event->ignore();
	}
}

void MainWindow::initializationComplete()
{
	LOG_DEBUG("MainWindow initialization comlete");
	if (!currentTeamRestoredFromSettings) {
		// No persisted team was restored. ChannelTree will retain its first
		// usable selection until the user explicitly chooses another one.
	}
}

void MainWindow::messageNotify(BackendChannel& channel, const BackendPost& post)
{
	if (post.isOwnPost()) {
		return;
	}

	auto& sidebar = SidebarService::instance(backend);
	if (post.currentUserMentioned) {
		sidebar.setChannelMentioned(channel.id, true);
	}
	if (sidebar.isChannelMuted(channel)) {
		return;
	}

	const bool directConversation = channel.type == BackendChannel::directChannel
		|| channel.type == BackendChannel::groupChannel;
	if (post.root_id.isEmpty()) {
		// Ordinary activity in public/private channels belongs in unread state,
		// not in desktop attention. Only mentions and direct/group messages are
		// actionable enough to flash the taskbar or show a desktop notification.
		if (!directConversation && !post.currentUserMentioned) {
			return;
		}
	} else if (!post.currentUserMentioned) {
		// Until full followed-thread desktop notification preferences are modeled,
		// keep thread notifications mention-driven just as before.
		return;
	}

	if (post.root_id.isEmpty()) {
		if (isActiveWindow() && ui->channelList->isChannelActive(channel)) {
			return;
		}
	} else {
		ChatArea* parentArea = ui->channelList->getCurrentPage();
		if (parentArea && &parentArea->getChannel() == &channel) {
			for (ChatArea* threadArea : parentArea->threadsAreas) {
				if (threadArea && threadArea->root_id == post.root_id
					&& threadArea->isActiveWindow()) {
					return;
				}
			}
		}
	}

	QString title;
	if (!post.root_id.isEmpty()) {
		title = post.getDisplayAuthorName() + " mentioned you in a thread";
	} else if (channel.type == BackendChannel::directChannel) {
		title = post.getDisplayAuthorName() + " messaged you";
	} else {
		title = post.getDisplayAuthorName() + " posted in '" + channel.display_name + "'";
	}

	notificationManager->show(title, post.message,
	                          NotificationTarget {channel.id, post.id, post.root_id});
	qApp->alert(nullptr, 0);
}

void MainWindow::unreadMessagesNotify(const BackendChannel& channel)
{
	Q_UNUSED(channel);
}

void MainWindow::setNotificationsCountVisualization(uint32_t notificationsCount)
{
	if (notificationsCount == 0) {
		setWindowTitle(qApp->applicationName());
	} else {
		setWindowTitle("(" + QString::number(notificationsCount) + ") "
		               + qApp->applicationName());
	}

	const uint32_t iconCount = std::min(notificationsCount, 6u);
	const QString iconName(":/icons/img/icon" + QString::number(iconCount) + ".ico");
	trayIcon.setIcon(QIcon(iconName));
}

void MainWindow::saveState()
{
	LOG_DEBUG("MainWindow saveState");
	QSettings settings;
	settings.setValue("geometry", saveGeometry());
	if (sidebarSplitter) {
		settings.setValue("sidebar_splitter_state", sidebarSplitter->saveState());
	}
}

} /* namespace Mattermost */
