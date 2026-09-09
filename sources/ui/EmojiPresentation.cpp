/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "EmojiPresentation.h"

#include <QSet>
#include <QTextBoundaryFinder>

#include "backend/emoji/EmojiInfo.h"

namespace Mattermost::EmojiPresentation {
namespace {

const QSet<QString>& unicodeEmojiStrings()
{
    static const QSet<QString> strings = [] {
        QSet<QString> result;
        for (int category = 0; category < EmojiCategory::COUNT; ++category) {
            if (category == EmojiCategory::custom) {
                continue;
            }
            const int skinToneCount = category == EmojiCategory::people
                ? EmojiSkinTone::COUNT
                : 1;
            for (int skinTone = 0; skinTone < skinToneCount; ++skinTone) {
                const QVector<Emoji> emojis = EmojiInfo::getAllEmojis(category, skinTone);
                for (const Emoji& emoji : emojis) {
                    const QString glyph = emoji.unicodeString.trimmed();
                    if (!glyph.isEmpty() && !glyph.contains(QStringLiteral("<img"))) {
                        result.insert(glyph);
                    }
                }
            }
        }
        return result;
    }();
    return strings;
}

} // namespace

bool isEmojiOnlyText(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        return false;
    }

    const QSet<QString>& emojiStrings = unicodeEmojiStrings();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    bool foundEmoji = false;
    int position = 0;

    while (position < text.size()) {
        if (text.at(position).isSpace()) {
            ++position;
            continue;
        }

        finder.setPosition(position);
        const int end = finder.toNextBoundary();
        if (end <= position) {
            return false;
        }

        if (!emojiStrings.contains(text.mid(position, end - position))) {
            return false;
        }
        foundEmoji = true;
        position = end;
    }

    return foundEmoji;
}

} // namespace Mattermost::EmojiPresentation