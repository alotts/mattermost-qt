/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#include "ChatArea.h"

#include <algorithm>

#include <QEvent>
#include <QFont>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QTimer>

#include "ChatLogWidget.h"
#include "QuotedReplyController.h"
#include "backend/Backend.h"
#include "backend/MentionGroupService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "integrations/KTalkIntegration.h"
#include "ui/ThemeIconWidgets.h"
#include "ui_ChatArea.h"

namespace Mattermost {
namespace {

constexpr int LoadingIndicatorDelayMs = 150;
constexpr int ActionButtonExtent = 30;
constexpr int ActionIconExtent = 24;

} // namespace

void ChatArea::setupComposerUi()
{
    auto configureActionButton = [](QPushButton& button) {
        button.setFixedSize(ActionButtonExtent, ActionButtonExtent);
        button.setCursor(Qt::PointingHandCursor);
    };

    ui->addEmojiButton->setText(QStringLiteral("😀"));
    ui->addEmojiButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    configureActionButton(*ui->addEmojiButton);

    ui->attachButton->setText(QStringLiteral("📎"));
    ui->attachButton->setIconSize(QSize(ActionIconExtent, ActionIconExtent));
    ui->attachButton->setToolTip(tr("Add"));
    ui->attachButton->setAccessibleName(tr("Add"));
    configureActionButton(*ui->attachButton);

    // Keep the composer surface compact: the paperclip is the single entry
    // point for things added to a message. QPushButton::setMenu() opens the
    // menu from the button's pressed path using QMenu::popup(), so this does not
    // introduce a nested event loop.
    auto* attachMenu = new QMenu(ui->attachButton);
    QAction* fileAction = attachMenu->addAction(tr("File…"));
    QAction* pollAction = attachMenu->addAction(tr("Poll…"));
    connect(fileAction, &QAction::triggered,
            ui->outgoingPostCreator, &OutgoingPostCreator::onAttachButtonClick);
    connect(pollAction, &QAction::triggered,
            ui->outgoingPostCreator, &OutgoingPostCreator::createPoll);

    // KTalk is server-provided, so keep the action hidden until discovery says
    // this login exposes a compatible plugin. The same composer setup is used
    // for channels and thread windows; a thread passes its root id so the
    // meeting post appears in the context from which it was started.
    auto* ktalk = &KTalkIntegration::instance(backend);
    QAction* ktalkAction = attachMenu->addAction(ktalk->icon(), tr("KTalk Meeting…"));
    ktalkAction->setVisible(ktalk->isAvailable());
    connect(ktalk, &KTalkIntegration::availabilityChanged,
            attachMenu, [ktalkAction](bool available) {
        ktalkAction->setVisible(available);
    });
    connect(ktalk, &KTalkIntegration::iconChanged,
            attachMenu, [ktalk, ktalkAction] {
        ktalkAction->setIcon(ktalk->icon());
    });
    connect(ktalkAction, &QAction::triggered, this, [this, ktalk] {
        ktalk->startMeeting(this,
                            channel.id,
                            isThread ? root_id : QString());
    });

    ui->attachButton->setMenu(attachMenu);

    configureActionButton(*ui->sendButton);
    QFont sendFont = ui->sendButton->font();
    if (sendFont.pointSizeF() > 0.0) {
        sendFont.setPointSizeF(sendFont.pointSizeF() + 2.0);
    } else if (sendFont.pixelSize() > 0) {
        sendFont.setPixelSize(sendFont.pixelSize() + 3);
    }
    ui->sendButton->setFont(sendFont);

    // No textual status belongs to the left of the input: it changes the
    // editor's horizontal geometry. Transient send and history-loading state
    // share the fixed attach-action slot, and persistent edit/reply state lives
    // above the editor as a compact context preview.
    ui->composerStatusLabel->hide();

    // The action buttons are owner-drawn by ThemeIconButton. Do not attach a
    // stylesheet merely to suppress Breeze's hover frame: QStyleSheetStyle was
    // also the source of stale inherited palette roles during theme changes.

    // The editor is the only vertically growing child. The other controls are
    // bottom-aligned in ChatArea.ui, so new lines grow upward from the action row.
    const int verticalPadding = std::max(
        2, ui->outgoingPostCreator->fontMetrics().lineSpacing() * 2 / 5);
    ui->composerLayout->setContentsMargins(0, verticalPadding, 0, verticalPadding);

    InteractiveTextEdit::CompletionRule mentionRule;
    mentionRule.prefix = QStringLiteral("@");
    mentionRule.provider = [this] {
        using Candidate = InteractiveTextEdit::CompletionCandidate;
        QVector<Candidate> candidates;
        QSet<QString> seen;

        const auto appendCandidate = [&candidates, &seen](Candidate candidate) {
            const QString key = candidate.insertText.toCaseFolded();
            if (key.isEmpty() || seen.contains(key)) {
                return;
            }
            seen.insert(key);
            candidates.push_back(std::move(candidate));
        };

        const auto appendSpecial = [&appendCandidate, this](const QString& name,
                                                             const QString& detail) {
            Candidate candidate;
            candidate.displayText = QStringLiteral("@") + name;
            candidate.insertText = name;
            candidate.detailText = detail;
            appendCandidate(std::move(candidate));
        };
        appendSpecial(QStringLiteral("channel"), tr("Notify everyone in this channel"));
        appendSpecial(QStringLiteral("all"), tr("Notify everyone in this channel"));
        appendSpecial(QStringLiteral("here"), tr("Notify online members in this channel"));

        const QString teamId = channel.team ? channel.team->id : QString();
        if (!teamId.isEmpty()) {
            auto& groupService = MentionGroupService::instance(backend);
            QVector<const MentionGroup*> groups;
            const QHash<QString, QString> groupIds = groupService.mentionIds(teamId);
            groups.reserve(groupIds.size());
            for (auto it = groupIds.cbegin(); it != groupIds.cend(); ++it) {
                if (const MentionGroup* group = groupService.groupById(teamId, it.value())) {
                    groups.push_back(group);
                }
            }
            std::sort(groups.begin(), groups.end(),
                      [](const MentionGroup* lhs, const MentionGroup* rhs) {
                return QString::localeAwareCompare(lhs->name, rhs->name) < 0;
            });
            for (const MentionGroup* group : groups) {
                if (!group || group->name.isEmpty()) {
                    continue;
                }
                Candidate candidate;
                candidate.displayText = QStringLiteral("@") + group->name;
                candidate.insertText = group->name;
                candidate.detailText = group->displayName;
                if (group->memberCount > 0) {
                    const QString countText = tr("%n member(s)", nullptr, group->memberCount);
                    candidate.detailText = candidate.detailText.isEmpty()
                        ? countText : candidate.detailText + QStringLiteral(" · ") + countText;
                }
                candidate.filterKeys.push_back(group->displayName);
                appendCandidate(std::move(candidate));
            }
        }

        QVector<const BackendUser*> users;
        const auto& storedUsers = backend.getStorage().getAllUsers();
        users.reserve(static_cast<int>(storedUsers.size()));
        for (const auto& entry : storedUsers) {
            if (!entry.second.username.isEmpty()) {
                users.push_back(&entry.second);
            }
        }
        std::sort(users.begin(), users.end(), [](const BackendUser* lhs, const BackendUser* rhs) {
            const QString lhsName = lhs->getDisplayName().isEmpty()
                ? lhs->username : lhs->getDisplayName();
            const QString rhsName = rhs->getDisplayName().isEmpty()
                ? rhs->username : rhs->getDisplayName();
            return QString::localeAwareCompare(lhsName, rhsName) < 0;
        });
        for (const BackendUser* user : users) {
            if (!user) {
                continue;
            }
            Candidate candidate;
            candidate.displayText = user->getDisplayName();
            if (candidate.displayText.isEmpty()) {
                candidate.displayText = user->username;
            }
            candidate.insertText = user->username;
            candidate.detailText = QStringLiteral("@") + user->username;
            if (!user->nickname.isEmpty()) {
                candidate.filterKeys.push_back(user->nickname);
            }
            if (!user->first_name.isEmpty()) {
                candidate.filterKeys.push_back(user->first_name);
            }
            if (!user->last_name.isEmpty()) {
                candidate.filterKeys.push_back(user->last_name);
            }
            appendCandidate(std::move(candidate));
        }

        return candidates;
    };
    ui->outgoingPostCreator->setCompletionRules({std::move(mentionRule)});

    const QString teamId = channel.team ? channel.team->id : QString();
    if (!teamId.isEmpty()) {
        auto& groupService = MentionGroupService::instance(backend);
        auto* editor = ui->outgoingPostCreator;
        connect(&groupService, &MentionGroupService::groupsChanged, editor,
                [editor, teamId](const QString& changedTeamId) {
            if (changedTeamId == teamId) {
                editor->refreshCompletions();
            }
        });
        groupService.ensureTeamGroups(teamId);
    }

    // Install the context controller eagerly so it also observes edit mode,
    // which can be entered without first using quoted replies.
    QuotedReplyController::instance(*this);

    loadingDelayTimer = new QTimer(this);
    loadingDelayTimer->setSingleShot(true);
    loadingDelayTimer->setInterval(LoadingIndicatorDelayMs);
    connect(loadingDelayTimer, &QTimer::timeout, this, [this] {
        if (pendingMessageLoads > 0 && ui && ui->attachButton) {
            ui->attachButton->setProperty(ComposerMessageLoadingProperty, true);
        }
    });

    connect(ui->listWidget, &LongListWidget::rangeRequested, this,
            [this](int, int, LongListWidget::RequestReason, quint64) {
        beginMessageLoading();
    });
    connect(ui->listWidget, &LongListWidget::rangeRequestFinished, this,
            [this](int, int) {
        endMessageLoading();
    });
}

void ChatArea::focusComposer()
{
    if (ui && ui->outgoingPostCreator) {
        ui->outgoingPostCreator->setFocus(Qt::OtherFocusReason);
    }
}

void ChatArea::beginMessageLoading()
{
    ++pendingMessageLoads;
    if (pendingMessageLoads == 1 && loadingDelayTimer
        && ui && ui->attachButton
        && !ui->attachButton->property(ComposerMessageLoadingProperty).toBool()) {
        loadingDelayTimer->start();
    }
}

void ChatArea::endMessageLoading()
{
    if (pendingMessageLoads <= 0) {
        return;
    }

    --pendingMessageLoads;
    if (pendingMessageLoads != 0) {
        return;
    }

    if (loadingDelayTimer) {
        loadingDelayTimer->stop();
    }
    if (ui && ui->attachButton) {
        ui->attachButton->setProperty(ComposerMessageLoadingProperty, false);
    }
}

void ChatArea::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event) {
        return;
    }

    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        if (ui) {
            ui->addEmojiButton->update();
            ui->attachButton->update();
            ui->sendButton->update();
            refreshHeaderActionIcons();
        }
    }
}

} // namespace Mattermost
