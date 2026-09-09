#include "MessageFormatter.h"

#include <algorithm>

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>

#include "backend/emoji/EmojiInfo.h"

#include <QRegularExpression>

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QTextFragment>
#endif

namespace Mattermost {
namespace MessageFormatter {

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
static void replaceEmojis(QString& text)
{
    int emojiStart = 0;
    int emojiEnd = 0;

    do {
        emojiStart = text.indexOf(':', emojiEnd);
        if (emojiStart == -1) {
            break;
        }

        emojiEnd = text.indexOf(':', emojiStart + 1);
        if (emojiEnd == -1) {
            break;
        }

        if (emojiEnd - emojiStart == 1) {
            ++emojiEnd;
            continue;
        }

        const int emojiNameSize = emojiEnd - emojiStart - 1;
        const QString emojiName = text.mid(emojiStart + 1, emojiNameSize);
        const EmojiID emojiID = EmojiInfo::findByName(emojiName);

        if (!emojiID) {
            ++emojiEnd;
            continue;
        }

        const Emoji emoji = EmojiInfo::getEmoji(emojiID);
        text.replace(emojiStart, emojiNameSize + 2, emoji.unicodeString);

        emojiEnd = emojiStart + emoji.unicodeString.size();
    } while (emojiStart != -1);
}

namespace {

QString htmlEscape(const QString& text)
{
    return text.toHtmlEscaped();
}

int delimiterRunLength(const QString& text, int position, QChar character)
{
    int length = 0;
    while (position + length < text.size() && text.at(position + length) == character) {
        ++length;
    }
    return length;
}

bool isMarkdownPunctuation(const QChar& c)
{
    return c == QLatin1Char('*') || c == QLatin1Char('_') || c == QLatin1Char('`')
        || c == QLatin1Char('[') || c == QLatin1Char(']') || c == QLatin1Char('(')
        || c == QLatin1Char(')') || c == QLatin1Char('!') || c == QLatin1Char('\\')
        || c == QLatin1Char('~') || c == QLatin1Char('#') || c == QLatin1Char('>');
}

// True if the position `start` lies within a `[label](url)` (or `![label](url)`)
// markdown link - either inside the label or inside the target URL. Both parts
// are rendered literally by renderInline(), so URL protection must not escape
// their formatting characters.
static bool isInsideMarkdownLink(const QString& text, int start)
{
    int lastOpen = -1;
    QChar lastOpenChar;
    for (int k = start - 1; k >= 0; --k) {
        const QChar c = text.at(k);
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            return false;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) {
            lastOpen = k;
            lastOpenChar = c;
            break;
        }
    }
    if (lastOpen == -1) {
        return false;
    }

    if (lastOpenChar == QLatin1Char('(')) {
        // Target: '(' immediately closes a ']' that ends a link label.
        if (lastOpen == 0 || text.at(lastOpen - 1) != QLatin1Char(']')) {
            return false;
        }
        for (int k = lastOpen - 2; k >= 0; --k) {
            const QChar c = text.at(k);
            if (c == QLatin1Char('[')) {
                return true;
            }
            if (c == QLatin1Char(']') || c == QLatin1Char(')') || c == QLatin1Char('(')
                || c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
                return false;
            }
        }
        return false;
    }

    // Label: the label's closing ']' must be followed by '(' to form a link.
    const int close = text.indexOf(QLatin1Char(']'), start);
    return close != -1 && close + 1 < text.size()
        && text.at(close + 1) == QLatin1Char('(');
}

// Escape formatting characters (*, _, ~) inside bare URLs so they won't be
// parsed as markdown emphasis/strikethrough by renderInline(). Uses the same
// URL pattern as linkifyBareUrlsInHtml below. Idempotent: a formatting
// character that is already preceded by an escaping backslash is left alone, so
// applying protection again to a sub-string (e.g. a link label) cannot double
// escape it. URLs inside markdown links (label or target) are skipped because
// their formatting characters are rendered literally by renderInline().
static QString protectFormattingInBareUrls(const QString& text)
{
    static const QRegularExpression bareUrl(
        QStringLiteral(R"(https?://[^\s<>"'`)\]]+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString result = text;
    int offset = 0;
    for (QRegularExpressionMatchIterator it = bareUrl.globalMatch(text); it.hasNext();) {
        QRegularExpressionMatch match = it.next();
        const int matchStart = match.capturedStart();
        if (isInsideMarkdownLink(text, matchStart)) {
            continue;
        }
        int len = match.capturedLength();
        bool escaped = false;
        for (int k = 0; k < len; ++k) {
            const QChar& c = text.at(matchStart + k);
            if (c == QLatin1Char('\\')) {
                escaped = true;
                continue;
            }
            if ((c == QLatin1Char('*') || c == QLatin1Char('_') || c == QLatin1Char('~'))
                && !escaped) {
                result.insert(matchStart + k + offset, '\\');
                ++offset;
            }
            escaped = false;
        }
    }
    return result;
}

// Convert a run of inline Markdown to escaped HTML. Handles inline code,
// backslash escapes, images, links, strong/emphasis and strikethrough. Text is
// literally copied and the three HTML-sensitive characters escaped as it goes.
QString renderInline(const QString& rawText)
{
    // Any link in the message - bare http(s) URL or a markdown link target -
    // must keep its formatting characters (*, _, ~) literal. Escape them before
    // parsing so they are not interpreted as emphasis/strikethrough. Applied on
    // every entry so headings, list items and nested constructs are covered too.
    const QString text = protectFormattingInBareUrls(rawText);

    QString out;
    out.reserve(text.size() + 32);

    const int size = text.size();
    int i = 0;
    while (i < size) {
        const QChar c = text.at(i);

        if (c == QLatin1Char('\\') && i + 1 < size
            && isMarkdownPunctuation(text.at(i + 1))) {
            out += htmlEscape(text.mid(i + 1, 1));
            i += 2;
            continue;
        }

        // Preserve emoji tokens verbatim so markdown emphasis on '_' (or any
        // other inline construct) cannot corrupt ':name:' before replaceEmojis
        // runs over the rendered HTML.
        if (c == QLatin1Char(':')) {
            const int close = text.indexOf(QLatin1Char(':'), i + 1);
            if (close != -1) {
                const QString name = text.mid(i + 1, close - i - 1);
                if (!name.isEmpty() && !name.contains(QLatin1Char(' '))
                    && EmojiInfo::findByName(name)) {
                    out += htmlEscape(text.mid(i, close - i + 1));
                    i = close + 1;
                    continue;
                }
            }
        }

        if (c == QLatin1Char('`')) {
            const int run = delimiterRunLength(text, i, QLatin1Char('`'));
            bool found = false;
            int close = i + run;
            while (close + run <= size) {
                const int candidateRun = delimiterRunLength(text, close, QLatin1Char('`'));
                if (run > 2 || close < size - run + 1) {
                    if (candidateRun == run) {
                        found = true;
                        break;
                    }
                    if (candidateRun > 0) {
                        close += candidateRun;
                    } else {
                        ++close;
                    }
                } else {
                    break;
                }
            }
            if (run <= 2 && found) {
                QString code = text.mid(i + run, close - i - run);
                if (code.size() >= 2 && code.front().isSpace() && code.back().isSpace()
                    && !code.startsWith(QLatin1String("  "))
                    && !code.endsWith(QLatin1String("  "))) {
                    code = code.mid(1, code.size() - 2);
                }
                out += QStringLiteral("<code>");
                out += htmlEscape(code);
                out += QStringLiteral("</code>");
                i = close + run;
                continue;
            }
            out += QString(run, QLatin1Char('`'));
            i += run;
            continue;
        }

        const bool isImage = c == QLatin1Char('!') && i + 1 < size
            && text.at(i + 1) == QLatin1Char('[');
        if (c == QLatin1Char('[') || isImage) {
            const int labelStart = i + (isImage ? 2 : 1);
            const int closeBracket = text.indexOf(QLatin1Char(']'), labelStart);
            if (closeBracket != -1 && closeBracket + 1 < size
                && text.at(closeBracket + 1) == QLatin1Char('(')) {
                int depth = 1;
                int urlEnd = -1;
                for (int k = closeBracket + 2; k < size; ++k) {
                    const QChar pc = text.at(k);
                    if (pc == QLatin1Char('(')) {
                        ++depth;
                    } else if (pc == QLatin1Char(')')) {
                        --depth;
                        if (depth == 0) {
                            urlEnd = k;
                            break;
                        }
                    }
                }
                if (urlEnd != -1) {
                    QString target = text.mid(closeBracket + 2, urlEnd - closeBracket - 2).trimmed();
                    QString url = target;
                    if (const int space = target.indexOf(QLatin1Char(' ')); space != -1) {
                        url = target.left(space);
                    }
                    url = url.trimmed();
                    if (!url.isEmpty()) {
                        if (isImage) {
                            out += QStringLiteral("<img src=\"") + htmlEscape(url)
                                + QStringLiteral("\" alt=\"")
                                + htmlEscape(text.mid(labelStart, closeBracket - labelStart))
                                + QStringLiteral("\">");
                        } else {
                            out += QStringLiteral("<a href=\"") + htmlEscape(url)
                                + QStringLiteral("\">")
                                + htmlEscape(text.mid(labelStart, closeBracket - labelStart))
                                + QStringLiteral("</a>");
                        }
                        i = urlEnd + 1;
                        continue;
                    }
                }
            }
            if (isImage) {
                out += QLatin1Char('!');
                ++i;
                continue;
            }
            out += QLatin1Char('[');
            ++i;
            continue;
        }

        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            const int run = delimiterRunLength(text, i, c);
            const bool strong = run >= 2;
            const int fullRun = strong ? 2 : 1;
            int close = -1;
            for (int k = i + fullRun; k + 1 <= size; ++k) {
                const int candidateRun = delimiterRunLength(text, k, c);
                if (candidateRun > 0 && (strong ? candidateRun >= 2 : candidateRun >= 1)) {
                    close = k;
                    break;
                }
            }
            if (close != -1 && close > i + fullRun) {
                const QString tag = strong ? QStringLiteral("strong") : QStringLiteral("em");
                out += QLatin1Char('<') + tag + QLatin1Char('>');
                out += renderInline(text.mid(i + fullRun, close - i - fullRun));
                out += QStringLiteral("</") + tag + QLatin1Char('>');
                i = close + fullRun;
                continue;
            }
            out += QString(run, c);
            i += run;
            continue;
        }

        if (c == QLatin1Char('~')) {
            const int run = delimiterRunLength(text, i, QLatin1Char('~'));
            if (run >= 2) {
                int close = text.indexOf(QStringLiteral("~~"), i + 2);
                if (close != -1) {
                    out += QStringLiteral("<del>");
                    out += renderInline(text.mid(i + 2, close - i - 2));
                    out += QStringLiteral("</del>");
                    i = close + 2;
                    continue;
                }
            }
            out += QString(run, QLatin1Char('~'));
            i += run;
            continue;
        }

        out += htmlEscape(text.mid(i, 1));
        ++i;
    }

    return out;
}

struct BlockLine {
    enum Type { Paragraph, Heading, Fence, Quote, Unordered, Ordered, Rule, Blank };
    Type type = Paragraph;
    int fenceMarkers = 0;      // marker count/fence length
    QChar fenceCharacter;
};

// Classify a single line, recording its structural type and any secondary
// payload (heading level or fence marker length/character) on the result.
BlockLine classifyBlockLine(const QString& line)
{
    BlockLine result;

    int position = 0;
    while (position < line.size() && position < 4 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    if (position >= line.size()) {
        result.type = BlockLine::Blank;
        return result;
    }

    const QChar first = line.at(position);

    // Fenced code block: ``` or ~~~ with an optional info string.
    if (first == QLatin1Char('`') || first == QLatin1Char('~')) {
        int run = 0;
        while (position + run < line.size() && line.at(position + run) == first) {
            ++run;
        }
        if (run >= 3) {
            result.type = BlockLine::Fence;
            result.fenceCharacter = first;
            result.fenceMarkers = run;
            return result;
        }
    }

    if (first == QLatin1Char('>')) {
        result.type = BlockLine::Quote;
        return result;
    }

    // Horizontal rule: ***, ---, ___.
    if ((first == QLatin1Char('*') || first == QLatin1Char('-') || first == QLatin1Char('_'))) {
        int count = 0;
        bool onlyRule = true;
        for (int k = position; k < line.size(); ++k) {
            const QChar ch = line.at(k);
            if (ch == QLatin1Char(' ')) {
                continue;
            }
            if (ch == first) {
                ++count;
            } else {
                onlyRule = false;
                break;
            }
        }
        if (onlyRule && count >= 3) {
            result.type = BlockLine::Rule;
            return result;
        }
    }

    int hashes = 0;
    while (position + hashes < line.size() && line.at(position + hashes) == QLatin1Char('#')) {
        ++hashes;
    }
    if (hashes >= 1 && hashes <= 6
        && (position + hashes >= line.size() || line.at(position + hashes) == QLatin1Char(' '))) {
        result.type = BlockLine::Heading;
        result.fenceMarkers = hashes; // reuse as heading level
        return result;
    }

    if (first == QLatin1Char('-') || first == QLatin1Char('+') || first == QLatin1Char('*')) {
        if (position + 1 < line.size()
            && (line.at(position + 1) == QLatin1Char(' ') || line.at(position + 1) == QLatin1Char('\t'))) {
            result.type = BlockLine::Unordered;
            return result;
        }
    }

    int digits = 0;
    while (position + digits < line.size() && line.at(position + digits).isDigit()) {
        ++digits;
    }
    if (digits > 0 && position + digits + 1 < line.size()
        && line.at(position + digits) == QLatin1Char('.')
        && line.at(position + digits + 1) == QLatin1Char(' ')) {
        result.type = BlockLine::Ordered;
        return result;
    }

    result.type = BlockLine::Paragraph;
    return result;
}

bool isClosingFence(const QString& line, QChar character, int openingLength)
{
    int position = 0;
    while (position < line.size() && position < 4 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    int run = 0;
    while (position + run < line.size() && line.at(position + run) == character) {
        ++run;
    }
    if (run < openingLength) {
        return false;
    }
    for (int k = position + run; k < line.size(); ++k) {
        if (!line.at(k).isSpace()) {
            return false;
        }
    }
    return true;
}

QString stripQuotePrefix(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        int position = 0;
        while (position < line.size() && position < 4 && line.at(position) == QLatin1Char(' ')) {
            ++position;
        }
        if (position < line.size() && line.at(position) == QLatin1Char('>')) {
            ++position;
            if (position < line.size() && line.at(position) == QLatin1Char(' ')) {
                ++position;
            }
        }
        line = line.mid(position);
    }
    return lines.join(QLatin1Char('\n'));
}

QString renderParagraphHtml(const QString& paragraph)
{
    // Inline markdown is parsed on the raw, unescaped source so links, code
    // and emphasis are recognised before the final HTML escaping inside
    // renderInline(). Blank lines inside a paragraph become <br>.
    QString body = renderInline(paragraph);
    body.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    return QStringLiteral("<p>") + body + QStringLiteral("</p>");
}

QString renderMarkdownHtml(const QString& text)
{
    QString html;
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int count = lines.size();
    int i = 0;

    while (i < count) {
        const BlockLine block = classifyBlockLine(lines.at(i));
        const BlockLine::Type type = block.type;

        if (type == BlockLine::Blank) {
            ++i;
            continue;
        }

        if (type == BlockLine::Rule) {
            html += QStringLiteral("<hr>");
            ++i;
            continue;
        }

        if (type == BlockLine::Heading) {
            const int level = block.fenceMarkers;
            const int hashStart = lines.at(i).indexOf(QLatin1Char('#'));
            const QString body = lines.at(i).mid(hashStart + level).trimmed();
            html += QStringLiteral("<h%1>").arg(level) + renderInline(body)
                + QStringLiteral("</h%1>").arg(level);
            ++i;
            continue;
        }

        if (type == BlockLine::Fence) {
            ++i;
            QString code;
            while (i < count) {
                if (isClosingFence(lines.at(i), block.fenceCharacter, block.fenceMarkers)) {
                    ++i;
                    break;
                }
                if (!code.isEmpty()) {
                    code += QLatin1Char('\n');
                }
                code += lines.at(i);
                ++i;
            }
            html += QStringLiteral("<pre style=\"white-space:pre-wrap;\">") + htmlEscape(code) + QStringLiteral("</pre>");
            continue;
        }

        if (type == BlockLine::Quote) {
            QString quoteText;
            while (i < count) {
                if (classifyBlockLine(lines.at(i)).type != BlockLine::Quote) {
                    break;
                }
                if (!quoteText.isEmpty()) {
                    quoteText += QLatin1Char('\n');
                }
                quoteText += stripQuotePrefix(lines.at(i));
                ++i;
            }
            html += QStringLiteral("<blockquote>") + renderMarkdownHtml(quoteText)
                + QStringLiteral("</blockquote>");
            continue;
        }

        if (type == BlockLine::Unordered || type == BlockLine::Ordered) {
            const bool ordered = type == BlockLine::Ordered;
            const QString listTag = ordered ? QStringLiteral("ol") : QStringLiteral("ul");
            html += QLatin1Char('<') + listTag + QLatin1Char('>');
            while (i < count) {
                const BlockLine item = classifyBlockLine(lines.at(i));
                if (item.type != type) {
                    break;
                }
                QString itemLine = lines.at(i);
                int markerStart = 0;
                while (markerStart < itemLine.size()
                       && (itemLine.at(markerStart) == QLatin1Char(' ')
                           || itemLine.at(markerStart) == QLatin1Char('\t'))) {
                    ++markerStart;
                }
                if (ordered) {
                    int markerEnd = markerStart;
                    while (markerEnd < itemLine.size() && itemLine.at(markerEnd).isDigit()) {
                        ++markerEnd;
                    }
                    markerEnd += 1; // '.'
                    if (markerEnd < itemLine.size()) {
                        ++markerEnd; // ' '
                    }
                    itemLine = itemLine.mid(markerEnd);
                } else {
                    itemLine = itemLine.mid(markerStart + 1); // marker
                    if (!itemLine.isEmpty() && itemLine.at(0).isSpace()) {
                        itemLine = itemLine.mid(1);
                    }
                }
                html += QStringLiteral("<li>") + renderInline(itemLine) + QStringLiteral("</li>");
                ++i;
            }
            html += QStringLiteral("</") + listTag + QLatin1Char('>');
            continue;
        }

        QString paragraph;
        while (i < count) {
            const BlockLine t = classifyBlockLine(lines.at(i));
            if (t.type == BlockLine::Blank || t.type == BlockLine::Heading
                || t.type == BlockLine::Fence || t.type == BlockLine::Quote
                || t.type == BlockLine::Rule || t.type == BlockLine::Unordered
                || t.type == BlockLine::Ordered) {
                break;
            }
            if (!paragraph.isEmpty()) {
                paragraph += QLatin1Char('\n');
            }
            paragraph += lines.at(i);
            ++i;
        }
        html += renderParagraphHtml(paragraph);
    }

    return html;
}

// Linkify bare http(s) URLs that the Markdown pass left as plain text. Walk the
// generated HTML and only rewrite text outside <a> / <code> / <pre> content and
// outside attribute values.
QString linkifyBareUrlsInHtml(const QString& html)
{
    static const QRegularExpression urlExpression(
        QStringLiteral(R"(https?://[^\s<>"'`]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(html.size() + 64);

    const int size = html.size();
    int i = 0;
    int linkifyDepth = 0;   // >0 while inside a link/anchor
    int literalDepth = 0;   // >0 while inside <code> or <pre> (no linkification)

    while (i < size) {
        if (html.at(i) == QLatin1Char('<')) {
            const int tagEnd = html.indexOf(QLatin1Char('>'), i);
            if (tagEnd == -1) {
                out += html.mid(i);
                break;
            }
            const QString tag = html.mid(i, tagEnd - i + 1);
            const QString lower = tag.toLower();
            out += tag;
            if (lower.startsWith(QStringLiteral("<pre")) || lower.startsWith(QStringLiteral("<code"))) {
                ++literalDepth;
            } else if (lower.startsWith(QStringLiteral("</pre")) || lower.startsWith(QStringLiteral("</code"))) {
                literalDepth = qMax(0, literalDepth - 1);
            } else if (lower.startsWith(QStringLiteral("<a ")) || lower.startsWith(QStringLiteral("<a>"))) {
                ++linkifyDepth;
            } else if (lower.startsWith(QStringLiteral("</a"))) {
                linkifyDepth = qMax(0, linkifyDepth - 1);
            }
            i = tagEnd + 1;
            continue;
        }

        if (linkifyDepth == 0 && literalDepth == 0) {
            const QRegularExpressionMatch match = urlExpression.match(html, i);
            if (match.hasMatch() && match.capturedStart(0) == i) {
                QString url = match.captured(0);
                const int punctuation = [&url] {
                    int count = 0;
                    while (!url.isEmpty()
                           && (url.back() == QLatin1Char(',') || url.back() == QLatin1Char(';')
                               || url.back() == QLatin1Char('!') || url.back() == QLatin1Char('?'))) {
                        url.chop(1);
                        ++count;
                    }
                    return count;
                }();
                if (!url.isEmpty()) {
                    out += QStringLiteral("<a href=\"") + url
                        + QStringLiteral("\">") + url + QStringLiteral("</a>");
                } else {
                    out += html.mid(i, match.capturedLength(0));
                }
                i += match.capturedLength(0);
                if (punctuation > 0) {
                    // Re-emit the punctuation that was trimmed from the address.
                    out += html.mid(i, i >= size ? 0 : qMin(punctuation, size - i));
                    i = qMin(i + punctuation, size);
                }
                continue;
            }
        }

        out += html.at(i);
        ++i;
    }

    return out;
}

} // namespace

#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
namespace {

struct EmojiReplacement {
    int position = 0;
    int length = 0;
    Emoji emoji;
};

struct LinkReplacement {
    int position = 0;
    int length = 0;
    QString href;
};

bool isEscaped(const QString& text, int position)
{
    int backslashCount = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) {
        ++backslashCount;
    }
    return (backslashCount % 2) != 0;
}

int backtickRunLength(const QString& text, int position)
{
    int length = 0;
    while (position + length < text.size() && text.at(position + length) == QLatin1Char('`')) {
        ++length;
    }
    return length;
}

int longestBacktickRun(const QString& text)
{
    int longest = 0;
    for (int i = 0; i < text.size();) {
        if (text.at(i) != QLatin1Char('`')) {
            ++i;
            continue;
        }
        const int length = backtickRunLength(text, i);
        longest = std::max(longest, length);
        i += length;
    }
    return longest;
}

int fencedBlockEnd(const QString& text, int lineStart)
{
    if (lineStart != 0 && text.at(lineStart - 1) != QLatin1Char('\n')) {
        return -1;
    }

    int fenceStart = lineStart;
    int leadingSpaces = 0;
    while (fenceStart < text.size() && leadingSpaces < 3 && text.at(fenceStart) == QLatin1Char(' ')) {
        ++fenceStart;
        ++leadingSpaces;
    }
    if (fenceStart >= text.size()) {
        return -1;
    }

    const QChar fenceCharacter = text.at(fenceStart);
    if (fenceCharacter != QLatin1Char('`') && fenceCharacter != QLatin1Char('~')) {
        return -1;
    }

    int fenceLength = 0;
    while (fenceStart + fenceLength < text.size()
           && text.at(fenceStart + fenceLength) == fenceCharacter) {
        ++fenceLength;
    }
    if (fenceLength < 3) {
        return -1;
    }

    int nextLineStart = text.indexOf(QLatin1Char('\n'), fenceStart + fenceLength);
    if (nextLineStart == -1) {
        return text.size();
    }
    ++nextLineStart;

    while (nextLineStart < text.size()) {
        int candidate = nextLineStart;
        int closingLeadingSpaces = 0;
        while (candidate < text.size() && closingLeadingSpaces < 3
               && text.at(candidate) == QLatin1Char(' ')) {
            ++candidate;
            ++closingLeadingSpaces;
        }

        int closingLength = 0;
        while (candidate + closingLength < text.size()
               && text.at(candidate + closingLength) == fenceCharacter) {
            ++closingLength;
        }

        if (closingLength >= fenceLength) {
            const int lineEnd = text.indexOf(QLatin1Char('\n'), candidate + closingLength);
            const int contentEnd = lineEnd == -1 ? text.size() : lineEnd;
            bool onlyWhitespaceAfterFence = true;
            for (int i = candidate + closingLength; i < contentEnd; ++i) {
                if (text.at(i) != QLatin1Char(' ') && text.at(i) != QLatin1Char('\t')) {
                    onlyWhitespaceAfterFence = false;
                    break;
                }
            }
            if (onlyWhitespaceAfterFence) {
                return lineEnd == -1 ? text.size() : lineEnd + 1;
            }
        }

        const int lineEnd = text.indexOf(QLatin1Char('\n'), nextLineStart);
        if (lineEnd == -1) {
            break;
        }
        nextLineStart = lineEnd + 1;
    }

    // An unclosed Markdown fence owns the rest of the document. Keep it
    // untouched instead of trying to reinterpret backticks inside its body.
    return text.size();
}

QString promoteMultilineCodeSpans(const QString& text)
{
    QString result;
    result.reserve(text.size());

    int position = 0;
    while (position < text.size()) {
        if (position == 0 || text.at(position - 1) == QLatin1Char('\n')) {
            const int fenceEnd = fencedBlockEnd(text, position);
            if (fenceEnd != -1) {
                result += text.mid(position, fenceEnd - position);
                position = fenceEnd;
                continue;
            }
        }

        if (text.at(position) != QLatin1Char('`') || isEscaped(text, position)) {
            result += text.at(position);
            ++position;
            continue;
        }

        const int delimiterLength = backtickRunLength(text, position);
        if (delimiterLength > 2) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        int closingPosition = position + delimiterLength;
        while (closingPosition < text.size()) {
            if (text.at(closingPosition) != QLatin1Char('`')) {
                ++closingPosition;
                continue;
            }

            const int closingLength = backtickRunLength(text, closingPosition);
            if (closingLength == delimiterLength && !isEscaped(text, closingPosition)) {
                break;
            }
            closingPosition += closingLength;
        }

        if (closingPosition >= text.size()) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        const int contentStart = position + delimiterLength;
        const QString content = text.mid(contentStart, closingPosition - contentStart);
        if (!content.contains(QLatin1Char('\n'))) {
            result += text.mid(position, closingPosition + delimiterLength - position);
            position = closingPosition + delimiterLength;
            continue;
        }

        // CommonMark intentionally collapses whitespace inside multiline code
        // spans. Mattermost messages in the wild also contain multiline snippets
        // wrapped in one or two backticks, so promote those spans to a fenced
        // code block before handing the text to QTextDocument's Markdown parser.
        const int fenceLength = std::max(3, longestBacktickRun(content) + 1);
        const QString fence(fenceLength, QLatin1Char('`'));
        const bool startsAtLineStart = position == 0 || text.at(position - 1) == QLatin1Char('\n');
        const int afterClosing = closingPosition + delimiterLength;
        const bool endsAtLineEnd = afterClosing == text.size() || text.at(afterClosing) == QLatin1Char('\n');

        if (!startsAtLineStart) {
            if (!result.endsWith(QLatin1Char('\n'))) {
                result += QLatin1Char('\n');
            }
            result += QLatin1Char('\n');
        }

        result += fence;
        result += QLatin1Char('\n');
        result += content;
        if (!content.endsWith(QLatin1Char('\n'))) {
            result += QLatin1Char('\n');
        }
        result += fence;

        if (!endsAtLineEnd) {
            result += QStringLiteral("\n\n");
        }

        position = afterClosing;
    }

    return result;
}

bool rangeAlreadyFormattedAsLinkOrCode(QTextDocument& document, int position, int length)
{
    QTextCursor cursor(&document);
    for (int i = 0; i < length; ++i) {
        cursor.setPosition(position + i);
        const QTextCharFormat format = cursor.charFormat();
        if (format.isAnchor() || format.fontFixedPitch()) {
            return true;
        }
    }
    return false;
}

void linkifyBareUrls(QTextDocument& document)
{
    // QTextDocument's GitHub Markdown parser handles ordinary autolinks, but
    // Qt 6.10 leaves some valid real-world URLs as plain text (notably long
    // percent-encoded paths containing '+'). Run a conservative second pass on
    // the already parsed document so Markdown links and code remain untouched.
    static const QRegularExpression urlExpression(
        QStringLiteral(R"(https?://[^\s<>"'`]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QList<LinkReplacement> replacements;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QString blockText = block.text();
        QRegularExpressionMatchIterator matches = urlExpression.globalMatch(blockText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            QString href = match.captured(0);

            // Sentence punctuation immediately after a URL is not part of the
            // address. Do not strip URL-significant dots from the middle/end of
            // a path such as a date; only obvious prose delimiters are removed.
            while (!href.isEmpty()) {
                const QChar tail = href.back();
                if (tail == QLatin1Char(',') || tail == QLatin1Char(';')
                    || tail == QLatin1Char('!') || tail == QLatin1Char('?')) {
                    href.chop(1);
                } else {
                    break;
                }
            }
            if (href.isEmpty()) {
                continue;
            }

            const int position = block.position() + static_cast<int>(match.capturedStart(0));
            const int length = href.size();
            if (rangeAlreadyFormattedAsLinkOrCode(document, position, length)) {
                continue;
            }

            replacements.push_back(LinkReplacement {position, length, href});
        }
    }

    // Formatting does not change positions, nevertheless apply from the end so
    // this remains safe if the implementation later needs textual cleanup.
    std::sort(replacements.begin(), replacements.end(),
              [](const LinkReplacement& left, const LinkReplacement& right) {
        return left.position > right.position;
    });

    for (const LinkReplacement& replacement : replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        cursor.setPosition(replacement.position + replacement.length, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(replacement.href);
        format.setFontUnderline(true);
        cursor.mergeCharFormat(format);
    }
}

bool customEmojiImageFormat(const Emoji& emoji, QTextImageFormat& imageFormat)
{
    if (!emoji.unicodeString.contains(QStringLiteral("<img"))) {
        return false;
    }

    static const QRegularExpression imageExpression(
        QStringLiteral(R"(<img\s+src=["']([^"']+)["']\s+width=(\d+)\s+height=(\d+)\s*/?>)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = imageExpression.match(emoji.unicodeString);
    if (!match.hasMatch()) {
        return false;
    }

    imageFormat.setName(match.captured(1));
    imageFormat.setWidth(match.captured(2).toInt());
    imageFormat.setHeight(match.captured(3).toInt());
    return true;
}

void replaceEmojisInDocument(QTextDocument& document)
{
    static const QRegularExpression emojiExpression(QStringLiteral(R"(:([^:\s]+):)"));
    QList<EmojiReplacement> replacements;

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QString blockText = block.text();
        QRegularExpressionMatchIterator matches = emojiExpression.globalMatch(blockText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const EmojiID emojiID = EmojiInfo::findByName(match.captured(1));
            if (!emojiID) {
                continue;
            }

            replacements.push_back(EmojiReplacement {
                block.position() + static_cast<int>(match.capturedStart(0)),
                static_cast<int>(match.capturedLength(0)),
                EmojiInfo::getEmoji(emojiID),
            });
        }
    }

    std::sort(replacements.begin(), replacements.end(), [](const EmojiReplacement& left, const EmojiReplacement& right) {
        return left.position > right.position;
    });

    for (const EmojiReplacement& replacement: replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        const QTextCharFormat textFormat = cursor.charFormat();
        cursor.setPosition(replacement.position + replacement.length, QTextCursor::KeepAnchor);

        QTextImageFormat imageFormat;
        if (customEmojiImageFormat(replacement.emoji, imageFormat)) {
            cursor.removeSelectedText();
            cursor.insertImage(imageFormat);
        } else {
            cursor.insertText(replacement.emoji.unicodeString, textFormat);
        }
    }
}

bool hasNonImageText(const QString& text)
{
    for (const QChar character: text) {
        if (!character.isSpace() && character != QChar::ObjectReplacementCharacter) {
            return true;
        }
    }
    return false;
}

bool shouldRenderImageAsBlock(const QTextImageFormat& imageFormat)
{
    constexpr qreal maxInlineImageSize = 64.0;

    const qreal width = imageFormat.width();
    const qreal height = imageFormat.height();
    if (width > maxInlineImageSize || height > maxInlineImageSize) {
        return true;
    }

    // Markdown images generally have no explicit dimensions. They are content
    // images, not emoji, and must not share a QTextLine with message text.
    return width <= 0.0 && height <= 0.0;
}

void clearBlockMargins(const QTextBlock& block)
{
    if (!block.isValid()) {
        return;
    }

    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setTopMargin(0);
    format.setBottomMargin(0);
    cursor.setBlockFormat(format);
}

void separateLargeImages(QTextDocument& document)
{
    // Split one mixed text/image block at a time and restart after each edit,
    // because QTextFragment positions are invalidated by insertBlock().
    for (;;) {
        bool changed = false;

        for (QTextBlock block = document.begin(); block.isValid() && !changed; block = block.next()) {
            const QString blockText = block.text();

            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                    continue;
                }

                const QTextImageFormat imageFormat = fragment.charFormat().toImageFormat();
                if (!shouldRenderImageAsBlock(imageFormat)) {
                    continue;
                }

                const int offset = fragment.position() - block.position();
                const bool hasTextBefore = hasNonImageText(blockText.left(offset));
                const bool hasTextAfter = hasNonImageText(blockText.mid(offset + fragment.length()));

                if (!hasTextBefore && !hasTextAfter) {
                    clearBlockMargins(block);
                    continue;
                }

                if (hasTextAfter) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position() + fragment.length());
                    cursor.insertBlock();
                }

                if (hasTextBefore) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position());
                    cursor.insertBlock();
                }

                changed = true;
                break;
            }
        }

        if (!changed) {
            break;
        }
    }

    // QLabel parses the generated HTML into another QTextDocument. Explicitly
    // zero margins on every block so the Markdown -> HTML -> RichText roundtrip
    // cannot reintroduce a large gap around image-only paragraphs.
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        clearBlockMargins(block);
    }
}

} // namespace

void buildMarkdownDocument(QTextDocument& document, const QString& text)
{
    document.clear();
    document.setDocumentMargin(0);

    // Parse the original Markdown verbatim. In particular, do not HTML-escape
    // quotes or ampersands before parsing: entities inside code spans are not
    // decoded by CommonMark, which used to turn a literal `"` into &quot;.
    // Raw HTML is disabled at the parser level instead.
    QTextDocument::MarkdownFeatures features(QTextDocument::MarkdownDialectGitHub);
    features.setFlag(QTextDocument::MarkdownNoHTML);
    document.setMarkdown(promoteMultilineCodeSpans(text), features);

    // Qt's GFM autolinker still misses some valid long percent-encoded URLs.
    // Complete only bare http(s) links after Markdown parsing so explicit links
    // and code spans keep their existing semantics.
    linkifyBareUrls(document);

    // Emoji are applied after Markdown parsing. Custom emoji are inserted as
    // QTextImageFormat objects, so enabling raw user HTML is unnecessary.
    replaceEmojisInDocument(document);
    separateLargeImages(document);
}
#endif

QString formatMessageText(const QString& text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    QTextDocument document;
    buildMarkdownDocument(document, text);
    return document.toHtml();
#else
    // Qt < 6.10 has no Markdown parser, so render a pragmatic subset of
    // CommonMark to HTML ourselves so messages do not show raw markup.
    QString result = renderMarkdownHtml(text);

    // Emoji replacement runs over the rendered HTML text. Emoji tokens are
    // never produced by the renderer, so this cannot corrupt generated tags.
    replaceEmojis(result);

    // Linkify bare http(s) addresses that the renderer left as plain text.
    // Only operate on the text nodes by skipping anything inside an existing
    // <a ...>...</a> or <code>/<pre> generated by the inline/block pass.
    result = linkifyBareUrlsInHtml(result);

    return result;
#endif
}

} // namespace MessageFormatter
} // namespace Mattermost
