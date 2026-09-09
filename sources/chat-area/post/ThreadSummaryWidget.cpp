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

#include "ThreadSummaryWidget.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPointer>
#include <QSet>

#include "ReactionChipStyle.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"
#include "ui/AvatarUtils.h"
#include "ui/ClickableLabel.h"
#include "ui/EmojiPresentation.h"

namespace Mattermost {

namespace {

constexpr int ParticipantAvatarSize = 16;
constexpr int MaxParticipantAvatars = 5;

} // namespace

ThreadSummaryWidget::ThreadSummaryWidget(Backend& backend,
                                         BackendChannel& channel,
                                         BackendPost& rootPost,
                                         QWidget* parent)
    : QWidget(parent)
    , backend(backend)
    , channel(channel)
    , rootPost(rootPost)
    , layout(new QHBoxLayout(this))
    , chip(new QWidget(this))
{
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* chipLayout = new QHBoxLayout(chip);
    ReactionChipStyle::apply(chip, chipLayout, QStringLiteral("threadReactionChip"));
    chip->setToolTip(tr("Open thread"));
    chip->setAccessibleName(tr("Open thread"));
    chip->installEventFilter(this);

    chipIcon = new QLabel(chip);
    chipIcon->setObjectName(QStringLiteral("threadSummaryIcon"));
    chipIcon->setFixedSize(ReactionChipStyle::IconExtent, ReactionChipStyle::IconExtent);
    chipIcon->setAlignment(Qt::AlignCenter);
    chipIcon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    chipLayout->addWidget(chipIcon, 0, Qt::AlignVCenter);

    chipCount = new QLabel(chip);
    chipCount->setMaximumSize(20, ReactionChipStyle::IconExtent);
    chipCount->setAlignment(Qt::AlignCenter);
    QFont countFont = chipCount->font();
    countFont.setPointSize(ReactionChipStyle::CountPointSize);
    chipCount->setFont(countFont);
    chipCount->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    chipLayout->addWidget(chipCount, 0, Qt::AlignBottom);

    connect(&channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& edited) {
        if (edited.id == this->rootPost.id) {
            refresh();
        }
    });
    connect(&channel, &BackendChannel::onThreadSummaryChanged, this,
            [this](BackendPost& changedRoot) {
        if (changedRoot.id == this->rootPost.id) {
            refresh();
        }
    });

    refresh();
}

bool ThreadSummaryWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == chip && event && event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            emit clicked();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ThreadSummaryWidget::refresh()
{
    rebuildParticipantAvatars();
    if (rootPost.reply_count > 0) {
        chipCount->setText(QString::number(rootPost.reply_count));
        chipCount->show();
    } else {
        chipCount->clear();
        chipCount->hide();
    }
    refreshTheme();
    chip->adjustSize();
    updateGeometry();
}

void ThreadSummaryWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshTheme();
    }
}

void ThreadSummaryWidget::refreshTheme()
{
    if (!chipIcon || !chipCount) {
        return;
    }

    const QPalette currentPalette = qApp ? qApp->palette() : palette();
    chipCount->setPalette(currentPalette);

    // Render the thread glyph with the preferred emoji font so it matches the
    // rest of the interface instead of the tinted symbolic balloon icon.
    chipIcon->setPixmap(EmojiPresentation::renderEmojiPixmap(
        QStringLiteral("💬"), ReactionChipStyle::IconExtent));
    chipIcon->update();
    chipCount->update();
}

void ThreadSummaryWidget::rebuildParticipantAvatars()
{
    while (layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        QWidget* widget = item ? item->widget() : nullptr;
        if (widget && widget != chip) {
            widget->deleteLater();
        }
        delete item;
    }

    QStringList participantIds;
    QSet<QString> seen;

    // Locally materialized replies are authoritative for the very latest live
    // activity and complement the transient participant sample returned on the
    // root post by Mattermost collapsed-thread responses.
    for (auto it = channel.posts.rbegin(); it != channel.posts.rend(); ++it) {
        if (it->root_id != rootPost.id || it->user_id.isEmpty() || seen.contains(it->user_id)) {
            continue;
        }
        seen.insert(it->user_id);
        participantIds.push_back(it->user_id);
        if (participantIds.size() >= MaxParticipantAvatars) {
            break;
        }
    }

    // Mattermost stores thread participants oldest -> newest. Walk the sample
    // backwards so the chip shows the most recent unique participants first,
    // without having to load the thread itself.
    for (auto it = rootPost.threadParticipantUserIds.crbegin();
         it != rootPost.threadParticipantUserIds.crend()
         && participantIds.size() < MaxParticipantAvatars; ++it) {
        if (it->isEmpty() || seen.contains(*it)) {
            continue;
        }
        seen.insert(*it);
        participantIds.push_back(*it);
    }

    QStringList missingUserIds;
    for (const QString& userId : participantIds) {
        BackendUser* user = backend.getStorage().getUserById(userId);
        if (!user) {
            missingUserIds.push_back(userId);
            continue;
        }

        watchUser(user);
        if (user->avatar.isNull()
            || user->avatar_picture_update != user->last_picture_update) {
            UserProfileService::instance(backend).ensureAvatar(*user);
        }

        auto* avatar = new ClickableLabel(this);
        avatar->setFixedSize(ParticipantAvatarSize, ParticipantAvatarSize);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setToolTip(user->getDisplayName());
        if (!user->avatar.isNull()) {
            avatar->setPixmap(AvatarUtils::circular(user->avatar, ParticipantAvatarSize));
        }
        connect(avatar, &ClickableLabel::clicked, this, [this, user] {
            UserProfileDialog::showTransient(backend, *user, this);
        });
        layout->addWidget(avatar, 0, Qt::AlignVCenter);
    }

    layout->addWidget(chip, 0, Qt::AlignVCenter);

    if (!missingUserIds.isEmpty()) {
        QPointer<ThreadSummaryWidget> guard(this);
        UserProfileService::instance(backend).ensureUsers(
            missingUserIds, [guard] {
                if (guard) {
                    guard->refresh();
                }
            });
    }
}

void ThreadSummaryWidget::watchUser(const BackendUser* user)
{
    if (!user) {
        return;
    }
    connect(user, &BackendUser::onAvatarChanged,
            this, &ThreadSummaryWidget::refresh, Qt::UniqueConnection);
}

} // namespace Mattermost
