/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ThemeIconWidgets.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPalette>

#include "IconUtils.h"
#include "ui/EmojiPresentation.h"

namespace Mattermost {
namespace {

constexpr qreal RestingOpacity = 0.8;
constexpr int BusyAnimationIntervalMs = 70;
constexpr int BusyAnimationSteps = 12;
constexpr int BusyIndicatorExtent = 18;

QString tintKey(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

} // namespace

ThemeIconButton::ThemeIconButton(QWidget* parent)
    : QPushButton(parent)
{
    setCursor(Qt::PointingHandCursor);

    _busyAnimationTimer.setInterval(BusyAnimationIntervalMs);
    connect(&_busyAnimationTimer, &QTimer::timeout, this, [this] {
        _busyPhase = (_busyPhase + 1) % BusyAnimationSteps;
        update();
    });
}

QString ThemeIconButton::symbolicResource() const
{
    const QString configuredResource = property(ThemeIconResourceProperty).toString();
    if (!configuredResource.isEmpty()) {
        return configuredResource;
    }
    // The composer's emoji and attach actions are rendered as emoji glyphs
    // instead of embedded SVG resources; see setupComposerUi().
    if (objectName() == QStringLiteral("addEmojiButton")) {
        return {};
    }
    if (objectName() == QStringLiteral("attachButton")) {
        return {};
    }
    return {};
}

bool ThemeIconButton::isBusy() const
{
    if (property(ThemeIconBusyProperty).toBool()) {
        return true;
    }
    return objectName() == QStringLiteral("attachButton")
        && (!property(ComposerBusyTextProperty).toString().isEmpty()
            || property(ComposerMessageLoadingProperty).toBool());
}

void ThemeIconButton::syncBusyAnimation()
{
    if (isBusy()) {
        if (!_busyAnimationTimer.isActive()) {
            _busyAnimationTimer.start();
        }
    } else {
        _busyAnimationTimer.stop();
        _busyPhase = 0;
    }
    update();
}

void ThemeIconButton::invalidateRenderedIcon()
{
    _renderedTint.clear();
    _renderedResource.clear();
    _renderedSize = {};
    _renderedPixmap = {};
}

bool ThemeIconButton::event(QEvent* event)
{
    const QEvent::Type type = event ? event->type() : QEvent::None;
    const bool result = QPushButton::event(event);

    if (type == QEvent::DynamicPropertyChange) {
        invalidateRenderedIcon();
        syncBusyAnimation();
    } else if (type == QEvent::PaletteChange
               || type == QEvent::ApplicationPaletteChange
               || type == QEvent::StyleChange) {
        invalidateRenderedIcon();
        update();
    } else if (type == QEvent::Enter
               || type == QEvent::Leave
               || type == QEvent::EnabledChange) {
        update();
    }
    return result;
}

void ThemeIconButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    const QPalette currentPalette = qApp ? qApp->palette() : palette();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    if (isBusy()) {
        QColor busyColor = currentPalette.color(QPalette::WindowText);
        busyColor.setAlpha(190);
        painter.setPen(QPen(busyColor, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);

        const qreal indicatorExtent = BusyIndicatorExtent;
        const QRectF ring((width() - indicatorExtent) / 2.0 + 2.5,
                          (height() - indicatorExtent) / 2.0 + 2.5,
                          indicatorExtent - 5.0,
                          indicatorExtent - 5.0);
        painter.drawArc(ring, (-90 + _busyPhase * 30) * 16, 105 * 16);
        return;
    }

    const QPalette::ColorGroup group = isEnabled()
        ? QPalette::Active : QPalette::Disabled;
    QColor color = currentPalette.color(group, QPalette::ButtonText);
    if (!underMouse()) {
        color.setAlphaF(color.alphaF() * RestingOpacity);
    }

    const QString resource = symbolicResource();
    if (!resource.isEmpty()) {
        const QSize targetSize = iconSize().isValid() ? iconSize() : QSize(24, 24);
        const QString desiredTint = tintKey(color);
        if (_renderedTint != desiredTint
            || _renderedResource != resource
            || _renderedSize != targetSize) {
            _renderedPixmap = IconUtils::tintedSymbolicIcon(resource, color).pixmap(targetSize);
            _renderedTint = desiredTint;
            _renderedResource = resource;
            _renderedSize = targetSize;
        }

        if (!_renderedPixmap.isNull()) {
            const QPoint topLeft((width() - _renderedPixmap.width()) / 2,
                                 (height() - _renderedPixmap.height()) / 2);
            painter.drawPixmap(topLeft, _renderedPixmap);
        }
        return;
    }

    painter.setPen(color);
    const QString buttonText = text();
    if (EmojiPresentation::isEmojiOnlyText(buttonText)) {
        QFont emojiFont = font();
        EmojiPresentation::preferEmojiFont(emojiFont);
        const QSize targetSize = iconSize().isValid() ? iconSize() : QSize(24, 24);
        emojiFont.setPixelSize(std::max(12, targetSize.height()));
        painter.setFont(emojiFont);
    } else {
        painter.setFont(font());
    }
    painter.drawText(rect(), Qt::AlignCenter, buttonText);
}

} // namespace Mattermost
