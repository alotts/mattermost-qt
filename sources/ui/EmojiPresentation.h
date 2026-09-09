#pragma once

#include <algorithm>

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>

#include "EmojiFont.h"
#include "backend/emoji/EmojiInfo.h"

namespace Mattermost::EmojiPresentation {

enum class Mode {
    Inline,
    Jumbo,
    Reaction,
};

constexpr qreal InlineScale = 1.3;
constexpr qreal JumboScale = 4.0;
constexpr int ReactionExtent = 16;

inline qreal fontScale(Mode mode)
{
    switch (mode) {
    case Mode::Jumbo:
        return JumboScale;
    case Mode::Inline:
        return InlineScale;
    case Mode::Reaction:
        return 1.0;
    }
    return InlineScale;
}

inline QFont fontForMode(QFont font, Mode mode)
{
    font = EmojiFont::applySystemEmojiFamily(font);

    if (mode == Mode::Reaction) {
        font.setPixelSize(ReactionExtent);
        return font;
    }

    const qreal scale = fontScale(mode);
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(font.pointSizeF() * scale);
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(1, qRound(font.pixelSize() * scale)));
    }
    return font;
}

// Ordered strongest-first among known colour emoji families. The list mirrors
// the common human preference for Apple's design, then the JoyPixels/EmojiOne
// palette, then Noto, then Twemoji, then the Windows monochrome fallback. Only
// families installed on the current system are considered.
inline const QStringList& preferredEmojiFamilies()
{
    static const QStringList families = {
        QStringLiteral("Apple Color Emoji"),
        QStringLiteral("JoyPixels"),
        QStringLiteral("EmojiOne Color"),
        QStringLiteral("Noto Color Emoji"),
        QStringLiteral("Twemoji Mozilla"),
        QStringLiteral("Twitter Color Emoji"),
        QStringLiteral("Segoe UI Emoji"),
        QStringLiteral("Noto Emoji"),
    };
    return families;
}

inline const QString& preferredEmojiFamily()
{
    static const QString family = [] {
        const QFontDatabase database;
        const QStringList installed = database.families();
        for (const QString& candidate : preferredEmojiFamilies()) {
            if (installed.contains(candidate, Qt::CaseInsensitive)) {
                return candidate;
            }
        }
        return QString();
    }();
    return family;
}

inline void preferEmojiFont(QFont& font)
{
    const QString& family = preferredEmojiFamily();
    if (!family.isEmpty()) {
        font.setFamilies(QStringList{family});
    }
}

inline QFont emojiFontForMode(QFont font, Mode mode)
{
    preferEmojiFont(font);
    return fontForMode(std::move(font), mode);
}

// Renders a single emoji grapheme with the preferred emoji font into a
// transparent pixmap of the requested glyph size. Used for controls that only
// accept an icon (tab labels, tool buttons) rather than mixed rich text.
inline QPixmap renderEmojiPixmap(const QString& glyph, int size)
{
    QFont font;
    preferEmojiFont(font);
    font.setPixelSize(std::max(1, size));

    QFontMetricsF metrics(font);
    const int width = std::max(1, qRound(metrics.horizontalAdvance(glyph)));
    const int height = std::max(1, qRound(metrics.ascent() + metrics.descent()));

    QPixmap pixmap(width, height);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.drawText(QRectF(0, 0, width, height), Qt::AlignCenter, glyph);
    painter.end();
    return pixmap;
}

inline int extent(const QFont& font, Mode mode)
{
    if (mode == Mode::Reaction) {
        return ReactionExtent;
    }
    return std::max(1, qRound(QFontMetricsF(font).ascent() * fontScale(mode)));
}

inline QString imagePath(const QString& source)
{
    const QUrl url(source);
    return url.isLocalFile() ? url.toLocalFile() : source;
}

inline QSize imageSize(const QString& source, int targetExtent, bool capAtNativeSize)
{
    const QSize target(targetExtent, targetExtent);
    QImageReader reader(imagePath(source));
    // The custom-emoji cache historically stores every payload with a .gif
    // suffix even when the server returned PNG/JPEG. Inspect the bytes rather
    // than trusting the file name.
    reader.setDecideFormatFromContent(true);
    const QSize nativeSize = reader.size();
    if (!nativeSize.isValid() || nativeSize.isEmpty()) {
        return target;
    }

    QSize bounds = target;
    if (capAtNativeSize) {
        bounds.setWidth(std::min(bounds.width(), nativeSize.width()));
        bounds.setHeight(std::min(bounds.height(), nativeSize.height()));
    }

    const QSize scaled = nativeSize.scaled(bounds, Qt::KeepAspectRatio);
    return scaled.isValid() && !scaled.isEmpty() ? scaled : target;
}

inline QSize imageSize(const QString& source, const QFont& font, Mode mode)
{
    return imageSize(source, extent(font, mode), mode == Mode::Jumbo);
}

inline void apply(QTextImageFormat& imageFormat, const QFont& font, Mode mode)
{
    if (!EmojiInfo::isCustomEmojiPath(imageFormat.name())) {
        return;
    }

    const QSize size = imageSize(imageFormat.name(), font, mode);
    imageFormat.setWidth(size.width());
    imageFormat.setHeight(size.height());
    imageFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
}

inline void apply(QTextDocument& document, Mode mode)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                continue;
            }

            QTextImageFormat imageFormat = fragment.charFormat().toImageFormat();
            if (!EmojiInfo::isCustomEmojiPath(imageFormat.name())) {
                continue;
            }

            apply(imageFormat, document.defaultFont(), mode);
            QTextCursor cursor(&document);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.setCharFormat(imageFormat);
        }
    }
}

inline QString normalizeHtml(const QString& html, const QFont& font, Mode mode)
{
    static const QRegularExpression imageExpression(
        QStringLiteral(R"(<img\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sourceExpression(
        QStringLiteral(R"(\bsrc\s*=\s*["']([^"']+)["'])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression widthExpression(
        QStringLiteral(R"(\bwidth\s*=\s*["']?\d+(?:\.\d+)?["']?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression heightExpression(
        QStringLiteral(R"(\bheight\s*=\s*["']?\d+(?:\.\d+)?["']?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression styleExpression(
        QStringLiteral(R"(\bstyle\s*=\s*["']([^"']*)["'])"),
        QRegularExpression::CaseInsensitiveOption);

    QString result;
    result.reserve(html.size());
    int previousEnd = 0;

    QRegularExpressionMatchIterator matches = imageExpression.globalMatch(html);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const int start = static_cast<int>(match.capturedStart());
        const int end = static_cast<int>(match.capturedEnd());
        result += html.mid(previousEnd, start - previousEnd);

        QString tag = match.captured();
        const QRegularExpressionMatch sourceMatch = sourceExpression.match(tag);
        if (sourceMatch.hasMatch() && EmojiInfo::isCustomEmojiPath(sourceMatch.captured(1))) {
            const QSize size = imageSize(sourceMatch.captured(1), font, mode);
            const QString width = QStringLiteral("width=\"%1\"").arg(size.width());
            const QString height = QStringLiteral("height=\"%1\"").arg(size.height());

            if (widthExpression.match(tag).hasMatch()) {
                tag.replace(widthExpression, width);
            } else {
                int insertion = tag.lastIndexOf(QLatin1Char('>'));
                if (insertion > 0 && tag.at(insertion - 1) == QLatin1Char('/')) {
                    --insertion;
                }
                tag.insert(insertion, QStringLiteral(" ") + width);
            }

            if (heightExpression.match(tag).hasMatch()) {
                tag.replace(heightExpression, height);
            } else {
                int insertion = tag.lastIndexOf(QLatin1Char('>'));
                if (insertion > 0 && tag.at(insertion - 1) == QLatin1Char('/')) {
                    --insertion;
                }
                tag.insert(insertion, QStringLiteral(" ") + height);
            }

            const QRegularExpressionMatch styleMatch = styleExpression.match(tag);
            if (styleMatch.hasMatch()) {
                QString style = styleMatch.captured(1);
                if (!style.contains(QStringLiteral("vertical-align"), Qt::CaseInsensitive)) {
                    if (!style.isEmpty() && !style.endsWith(QLatin1Char(';'))) {
                        style += QLatin1Char(';');
                    }
                    style += QStringLiteral("vertical-align:middle;");
                    tag.replace(styleMatch.capturedStart(1),
                                styleMatch.capturedLength(1),
                                style);
                }
            } else {
                int insertion = tag.lastIndexOf(QLatin1Char('>'));
                if (insertion > 0 && tag.at(insertion - 1) == QLatin1Char('/')) {
                    --insertion;
                }
                tag.insert(insertion,
                           QStringLiteral(" style=\"vertical-align:middle;\""));
            }
        }

        result += tag;
        previousEnd = end;
    }

    result += html.mid(previousEnd);
    return result;
}

// Returns true only when every non-whitespace grapheme in @p text is a known
// unicode emoji. Used to decide whether a glyph should be rendered with the
// preferred emoji font rather than the surrounding label font.
bool isEmojiOnlyText(const QString& text);

} // namespace Mattermost::EmojiPresentation
