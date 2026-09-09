/**
 * @file MessageTextEditWidget.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Jan 7, 2022
 *
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

#include "MessageTextEditWidget.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QFrame>
#include <QKeyEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QSettings>
#include <QTextDocument>
#include <QTimer>

#include "Settings.h"

namespace Mattermost {
namespace {

constexpr int ComposerMaximumHeight = 300;

} // namespace

MessageTextEditWidget::MessageTextEditWidget(QWidget* parent)
    : InteractiveTextEdit(parent)
{
    setSubmitOnEnter(true);
    const QSettings settings;
    setSubmitOnCtrlEnter(
        settings.value(COMPOSER_SEND_ON_CTRL_ENTER,
                       COMPOSER_SEND_ON_CTRL_ENTER_DEFAULT).toBool());
    setSubmitHandler([this] { emit enterPressed(); });

    // Keep the composer visually continuous with the action row below it.
    // BackgroundRole references remain palette-driven instead of baking the
    // current theme color into the editor.
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Window);
    viewport()->setAutoFillBackground(true);
    document()->setDocumentMargin(3.0);

    connect(this, &QTextEdit::textChanged, this, [this] {
        updateHeightToContents();
        // The block inserted by Shift+Enter may finish layout after the key
        // event. Measure once more on the next event-loop turn.
        QTimer::singleShot(0, this, &MessageTextEditWidget::updateHeightToContents);
    });
    connect(document(), &QTextDocument::blockCountChanged, this,
            [this](int) { updateHeightToContents(); });
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateHeightToContents(); });

    QTimer::singleShot(0, this, &MessageTextEditWidget::updateHeightToContents);
}

MessageTextEditWidget::~MessageTextEditWidget() = default;

void MessageTextEditWidget::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    // Arrow/escape keys belong to the completion popup while it is open. In
    // particular, Up must not start editing the previous post at that moment.
    if (!completionPopupVisible()) {
        switch (event->key()) {
        case Qt::Key_Up:
            emit upArrowPressed();
            break;
        case Qt::Key_Escape:
            emit escapePressed();
            break;
        default:
            break;
        }
    }

    InteractiveTextEdit::keyPressEvent(event);
}

void MessageTextEditWidget::resizeEvent(QResizeEvent* event)
{
    QTextEdit::resizeEvent(event);
    updateHeightToContents();
}

void MessageTextEditWidget::updateHeightToContents()
{
    if (!document() || !document()->documentLayout()) {
        return;
    }

    const QMargins margins = contentsMargins();
    const int chromeHeight = margins.top() + margins.bottom() + 2 * frameWidth();
    const int documentMargins = static_cast<int>(std::ceil(document()->documentMargin() * 2.0));
    const int lineHeight = fontMetrics().lineSpacing();
    const int oneLineHeight = lineHeight + documentMargins + chromeHeight;
    const int laidOutHeight = static_cast<int>(std::ceil(
        document()->documentLayout()->documentSize().height())) + chromeHeight;
    const int explicitLineHeight = std::max(1, document()->blockCount()) * lineHeight
        + documentMargins + chromeHeight;
    const int wantedHeight = std::clamp(
        std::max({oneLineHeight, laidOutHeight, explicitLineHeight}),
        oneLineHeight, ComposerMaximumHeight);

    if (height() != wantedHeight) {
        // Do not derive the next height limit from maximumHeight():
        // setFixedHeight() deliberately changes maximumHeight() to the current
        // value, which would otherwise permanently cap the editor at its first
        // one-line measurement.
        setFixedHeight(wantedHeight);
        updateGeometry();
    }
}

bool MessageTextEditWidget::hasNonEmptyText()
{
	return document()->characterCount() > 1;
}

} /* namespace Mattermost */
