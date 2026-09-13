#include "MessageFormatter.h"

#include <algorithm>

#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QVarLengthArray>
#include <utility>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>

#include "backend/emoji/EmojiInfo.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QTextFragment>
#endif

namespace Mattermost {
namespace MessageFormatter {

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
static void replaceEmojis(QString& text)
{
    // A shortcode is a short word wrapped in colons. Use the same definition as
    // the Qt-6.10 path: the name is one or more characters that are neither a
    // colon nor whitespace. Matching on a regex here matters: the raw message
    // is full of colons belonging to http(s): URLs, and a naive "first colon to
    // next colon" scan pairs a URL's colon with an emoji's colon, producing a
    // name full of slashes/spaces that never resolves -- so the emoji is left
    // unmapped and shows up as literal ":white_check_mark:".
    static const QRegularExpression emojiExpression(QStringLiteral(R"(:([^:\s]+):)"));
    QRegularExpressionMatchIterator matches = emojiExpression.globalMatch(text);
    QVarLengthArray<std::pair<int, std::pair<int, QString>>> replacements;

    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const EmojiID emojiID = EmojiInfo::findByName(match.captured(1));
        if (!emojiID) {
            continue;
        }
        replacements.push_back({static_cast<int>(match.capturedStart(0)),
                                {static_cast<int>(match.capturedLength(0)),
                                 EmojiInfo::getEmoji(emojiID).unicodeString}});
    }

    // Positions/lengths were computed against the unmodified string, so apply
    // from the end and walk backwards to keep every earlier index valid.
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it) {
        text.replace(it->first, it->second.first, it->second.second);
    }
}
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
namespace {

bool isEscaped(const QString& text, int position)
{
    int backslashCount = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) {
        ++backslashCount;
    }
    return (backslashCount % 2) != 0;
}

// Each character CommonMark treats as backslash-escapable ASCII punctuation.
bool isEscapablePunctuation(const QChar& c)
{
    static const QString escapable =
        QStringLiteral("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    return escapable.contains(c);
}

int backtickRunLength(const QString& text, int position)
{
    int length = 0;
    while (position + length < text.size() && text.at(position + length) == QLatin1Char('`')) {
        ++length;
    }
    return length;
}

// The pre-6.10 fallback renders Markdown by hand instead of delegating to
// QTextDocument::setMarkdown(). The input is received already HTML-escaped
// (the '&', '<' and '>' the reader typed are literal text like '&amp;'), so
// the Markdown metacharacters we own here -- *, _ , `, ~ -- are still raw and
// are turned into the matching HTML tags. Everything that CommonMark treats as
// literal inside a code span or fence (URLs, strikethrough, emphasis) is kept
// literal because those spans are consumed atomically before any other inline
// rule sees their content.

// Substitute HTML-escaped angle brackets back around a scheme so we can then
// run the same regex on both <http://...> autolinks and bare URLs.
QString htmlAnchor(const QString& href)
{
    return QStringLiteral("<a href=\"%1\">%1</a>").arg(href);
}

QString stripTrailingProsePunctuation(QString url)
{
    // Sentence punctuation immediately after a URL is not part of the address.
    // A trailing '.' is almost always a sentence/paragraph terminator, but a
    // dot inside the path (a version, a date) is kept because this only ever
    // inspects the last character.
    while (!url.isEmpty()) {
        const QChar tail = url.back();
        if (tail == QLatin1Char('.') || tail == QLatin1Char(',')
            || tail == QLatin1Char(';') || tail == QLatin1Char('!')
            || tail == QLatin1Char('?')) {
            url.chop(1);
        } else {
            break;
        }
    }
    return url;
}

// Renders one line of prose (no '\n') into HTML: inline code, autolinks,
// Markdown links, strikethrough, bare URLs, bold and italic. The input is
// already HTML-escaped text.
QString renderInline(const QString& text)
{
    static const QRegularExpression urlExpression(
        QStringLiteral(R"(https?://[^\s<>"'`]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(text.size() + 32);

    int position = 0;
    while (position < text.size()) {
        const QChar current = text.at(position);

        // A backslash escapes the following Markdown punctuation: CommonMark
        // removes the backslash and shows the character literally, so "\*foo\*"
        // renders "*foo*" without the backslash. This must run before the
        // punctuation-specific branches so escaped metacharacters stay literal
        // and the backslash is dropped.
        if (current == QLatin1Char('\\') && position + 1 < text.size()) {
            const QChar escaped = text.at(position + 1);
            if (isEscapablePunctuation(escaped)) {
                out += escaped;
                position += 2;
                continue;
            }
        }

        // Inline code span: a run of N backticks (N <= 2) matched by a run of
        // exactly N backticks later on. Content is already escaped and must be
        // left completely untouched.
        if (current == QLatin1Char('`') && !isEscaped(text, position)) {
            const int delimiter = backtickRunLength(text, position);
            if (delimiter <= 2) {
                int closing = position + delimiter;
                while (closing < text.size()) {
                    if (text.at(closing) != QLatin1Char('`')) {
                        ++closing;
                        continue;
                    }
                    const int closingLength = backtickRunLength(text, closing);
                    if (closingLength == delimiter && !isEscaped(text, closing)) {
                        break;
                    }
                    closing += closingLength;
                }
                if (closing < text.size()) {
                    out += QStringLiteral("<code>");
                    out += text.mid(position + delimiter, closing - position - delimiter);
                    out += QStringLiteral("</code>");
                    position = closing + delimiter;
                    continue;
                }
            }
            // No matching closer: treat the backticks as literal text.
            out += current;
            ++position;
            continue;
        }

        // <scheme://...> autolink. The angle brackets arrived HTML-escaped.
        if (current == QLatin1Char('&') && text.mid(position, 4) == QStringLiteral("&lt;")) {
            const int urlStart = position + 4; // length of "&lt;"
            const int urlEnd = text.indexOf(QStringLiteral("&gt;"), urlStart);
            if (urlEnd != -1) {
                const QString url = text.mid(urlStart, urlEnd - urlStart);
                if (url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
                    || url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
                    || url.startsWith(QStringLiteral("ftp://"), Qt::CaseInsensitive)
                    || url.startsWith(QStringLiteral("mailto://"), Qt::CaseInsensitive)) {
                    const int end = urlEnd + 4; // include "&gt;"
                    out += htmlAnchor(url);
                    position = end;
                    continue;
                }
            }
        }

        // Markdown link [label](url "title"). The label is rendered as inline
        // content; the destination becomes the click target. Bracketed text
        // without a following "(url)" is left as ordinary prose.
        if (current == QLatin1Char('[')) {
            const int closeBracket = text.indexOf(QLatin1Char(']'), position + 1);
            const int openParen = closeBracket != -1 && closeBracket + 1 < text.size()
                                      && text.at(closeBracket + 1) == QLatin1Char('(')
                                      ? closeBracket + 1
                                      : -1;
            if (closeBracket != -1 && openParen == closeBracket + 1) {
                // Find the matching ')' balancing nested parentheses so a URL
                // like https://example.com/a_(b) keeps its integrity.
                int depth = 1;
                int cursor = openParen + 1;
                int closeParen = -1;
                while (cursor < text.size()) {
                    const QChar c = text.at(cursor);
                    if (c == QLatin1Char('(')) {
                        ++depth;
                        ++cursor;
                    } else if (c == QLatin1Char(')')) {
                        --depth;
                        if (depth == 0) {
                            closeParen = cursor;
                            break;
                        }
                        ++cursor;
                    } else {
                        ++cursor;
                    }
                }
                if (closeParen != -1) {
                    // Split the interior into the destination and an optional
                    // "title". Quotes arrived HTML-escaped as &quot;.
                    QString interior = text.mid(openParen + 1, closeParen - openParen - 1);
                    QString url = interior;
                    QString title;
                    const int titleQuote = interior.indexOf(QStringLiteral("&quot;"));
                    if (titleQuote != -1) {
                        const int afterQuote = titleQuote + 6; // strlen("&quot;")
                        const int titleClose = interior.indexOf(QStringLiteral("&quot;"), afterQuote);
                        if (titleClose != -1) {
                            url = interior.left(titleQuote).trimmed();
                            title = interior.mid(afterQuote, titleClose - afterQuote);
                        }
                    }
                    if (!url.isEmpty()) {
                        QString label = renderInline(
                            text.mid(position + 1, closeBracket - position - 1));
                        out += QStringLiteral("<a href=\"%1\"").arg(url);
                        if (!title.isEmpty()) {
                            out += QStringLiteral(" title=\"%1\"").arg(title);
                        }
                        out += QStringLiteral(">%1</a>").arg(label);
                        position = closeParen + 1;
                        continue;
                    }
                }
            }
        }

        // GFM strikethrough: ~~text~~ becomes <s>.
        if (current == QLatin1Char('~') && !isEscaped(text, position)) {
            const int strikeLength = 2;
            if (position + 2 <= text.size()
                && text.at(position + 1) == QLatin1Char('~')) {
                const int closing = text.indexOf(QStringLiteral("~~"), position + strikeLength);
                if (closing != -1) {
                    const QString body = renderInline(
                        text.mid(position + strikeLength, closing - position - strikeLength));
                    out += QStringLiteral("<s>%1</s>").arg(body);
                    position = closing + strikeLength;
                    continue;
                }
            }
        }

        // Bare http(s) URL, anchored at the current position.
        QRegularExpressionMatch urlMatch =
            urlExpression.match(text, position, QRegularExpression::NormalMatch,
                                QRegularExpression::AnchorAtOffsetMatchOption);
        if (urlMatch.hasMatch() && static_cast<int>(urlMatch.capturedStart(0)) == position) {
            const QString matched = urlMatch.captured(0);
            const int matchedLength = matched.size();
            const QString href = stripTrailingProsePunctuation(matched);
            if (!href.isEmpty()) {
                out += htmlAnchor(href);
                // Re-emit any sentence punctuation that was stripped off the
                // link itself as ordinary prose following it.
                out += matched.mid(href.size());
            } else {
                out += matched;
            }
            position += matchedLength;
            continue;
        }

        // Bold and italic: ***both***, **bold**, *italic* and _emphasis_ each
        // need a matching run of the same length and character on the right.
        if (current == QLatin1Char('*') || current == QLatin1Char('_')) {
            int runLength = 0;
            while (position + runLength < text.size() && text.at(position + runLength) == current) {
                ++runLength;
            }
            // Never start emphasis directly inside a word ("a*b*" stays flat).
            const bool canOpen = position == 0 || text.at(position - 1).isSpace()
                                 || text.at(position - 1) == QLatin1Char('(')
                                 || text.at(position - 1) == QLatin1Char('[');
            if (canOpen && runLength >= 1 && runLength <= 3) {
                int cursor = position + runLength;
                while (cursor < text.size()) {
                    if (text.at(cursor) != current) {
                        ++cursor;
                        continue;
                    }
                    int closingRun = 0;
                    while (cursor + closingRun < text.size()
                           && text.at(cursor + closingRun) == current) {
                        ++closingRun;
                    }
                    if (closingRun == runLength) {
                        const QString body = renderInline(
                            text.mid(position + runLength, cursor - position - runLength));
                        QString tag;
                        if (runLength == 3) {
                            tag = QStringLiteral("<b><i>%1</i></b>");
                        } else if (runLength == 2) {
                            tag = QStringLiteral("<b>%1</b>");
                        } else {
                            tag = QStringLiteral("<i>%1</i>");
                        }
                        out += tag.arg(body);
                        position = cursor + runLength;
                        break;
                    }
                    cursor += closingRun;
                }
                if (position < text.size() && text.at(position) == current) {
                    // No matching closer reached: emit the run literally.
                    const int literalRun = runLength;
                    out += text.mid(position, literalRun);
                    position += literalRun;
                    continue;
                }
                continue;
            }
        }

        out += current;
        ++position;
    }

    return out;
}

int leadingSpaces(const QString& line)
{
    int count = 0;
    while (count < line.size()
           && (line.at(count) == QLatin1Char(' ') || line.at(count) == QLatin1Char('\t'))) {
        ++count;
    }
    return count;
}

// Recognizes a list item marker at the given indentation: an unordered marker
// ('-', '*' or '+') or an ordered marker (digits followed by '.' or ')'), each
// followed by a space. On success sets typeChar to 'u'/'o' and contentStart to
// the index of the item text. A '*word*' emphasis line is deliberately not a
// marker because there is no space after the '*'.
bool parseListMarker(const QString& line, int indent, QChar& typeChar, int& contentStart)
{
    if (indent >= line.size()) {
        return false;
    }
    const QChar marker = line.at(indent);
    if (marker == QLatin1Char('-') || marker == QLatin1Char('*') || marker == QLatin1Char('+')) {
        if (indent + 1 < line.size() && line.at(indent + 1).isSpace()) {
            typeChar = QLatin1Char('u');
            int content = indent + 1;
            while (content < line.size() && line.at(content).isSpace()) {
                ++content;
            }
            contentStart = content;
            return true;
        }
        return false;
    }
    if (marker.isDigit()) {
        int digits = indent;
        while (digits < line.size() && line.at(digits).isDigit()) {
            ++digits;
        }
        if (digits < line.size()
            && (line.at(digits) == QLatin1Char('.') || line.at(digits) == QLatin1Char(')'))
            && digits + 1 < line.size() && line.at(digits + 1).isSpace()) {
            typeChar = QLatin1Char('o');
            int content = digits + 1;
            while (content < line.size() && line.at(content).isSpace()) {
                ++content;
            }
            contentStart = content;
            return true;
        }
    }
    return false;
}

// Renders a run of consecutive list items (with nested lists and continuation
// lines) into <ul>/<ol> elements and returns the index of the first unconsumed
// line. Each item's text is inline-rendered, and deeper-indented markers form
// nested lists, while other deeper-indented non-marker lines are folded into
// the current item as continuation prose.
int renderList(const QList<QString>& lines, int lineIndex, QString& out, int indent)
{
    QChar listType;
    int firstContent;
    if (!parseListMarker(lines.at(lineIndex), indent, listType, firstContent)) {
        return lineIndex;
    }
    const bool ordered = (listType == QLatin1Char('o'));
    out += ordered ? QStringLiteral("<ol>") : QStringLiteral("<ul>");

    int index = lineIndex;
    while (index < lines.size()) {
        QChar itemType;
        int itemContent;
        if (!parseListMarker(lines.at(index), indent, itemType, itemContent)
            || itemType != listType) {
            break;
        }

        out += QStringLiteral("<li>");
        out += renderInline(lines.at(index).mid(itemContent));
        ++index;

        // Continuation lines and nested lists owned by this item.
        bool consume = true;
        while (consume && index < lines.size()) {
            const int lineIndent = leadingSpaces(lines.at(index));
            QChar nestedType;
            int nestedContent;
            const bool isMarker = parseListMarker(lines.at(index), lineIndent, nestedType, nestedContent);
            if (isMarker && lineIndent > indent) {
                index = renderList(lines, index, out, lineIndent);
            } else if (!isMarker && lineIndent > indent
                       && !lines.at(index).trimmed().isEmpty()) {
                out += QLatin1Char(' ');
                out += renderInline(lines.at(index).trimmed());
                ++index;
            } else if (lines.at(index).trimmed().isEmpty()
                       && index + 1 < lines.size()) {
                // A blank line before another item keeps the list going
                // (loose list), otherwise it ends the block.
                QChar peekType;
                int peekContent;
                const int peekIndent = leadingSpaces(lines.at(index + 1));
                if (parseListMarker(lines.at(index + 1), peekIndent, peekType, peekContent)) {
                    ++index;
                } else {
                    consume = false;
                }
            } else {
                consume = false;
            }
        }

        out += QStringLiteral("</li>");
    }

    out += ordered ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
    return index;
}

// Renders a fenced code block starting at the given line back into `out` and
// returns the index of the first line after the fence.
int renderFencedBlock(const QList<QString>& lines, int lineIndex, const QString& line, QString& out)
{
    int fenceStart = 0;
    while (fenceStart < line.size() && fenceStart < 3 && line.at(fenceStart) == QLatin1Char(' ')) {
        ++fenceStart;
    }
    const QChar fenceChar = line.at(fenceStart);
    int fenceLength = 0;
    while (fenceStart + fenceLength < line.size()
           && line.at(fenceStart + fenceLength) == fenceChar) {
        ++fenceLength;
    }

    QString body;
    ++lineIndex; // skip the opening fence line
    while (lineIndex < lines.size()) {
        const QString& candidate = lines.at(lineIndex);
        int candidateStart = 0;
        while (candidateStart < candidate.size() && candidateStart < 3
               && candidate.at(candidateStart) == QLatin1Char(' ')) {
            ++candidateStart;
        }
        int closingLength = 0;
        bool sameChar = candidateStart < candidate.size()
                        && candidate.at(candidateStart) == fenceChar;
        if (sameChar) {
            while (candidateStart + closingLength < candidate.size()
                   && candidate.at(candidateStart + closingLength) == fenceChar) {
                ++closingLength;
            }
        }
        bool onlyWhitespaceAfter = true;
        for (int i = candidateStart + closingLength; i < candidate.size(); ++i) {
            if (candidate.at(i) != QLatin1Char(' ') && candidate.at(i) != QLatin1Char('\t')) {
                onlyWhitespaceAfter = false;
                break;
            }
        }
        if (sameChar && closingLength >= fenceLength && onlyWhitespaceAfter) {
            ++lineIndex;
            break;
        }
        if (!body.isEmpty()) {
            body += QLatin1Char('\n');
        }
        body += candidate;
        ++lineIndex;
    }

    out += QStringLiteral("<pre style=\"white-space:pre-wrap;\">");
    out += body;
    out += QStringLiteral("</pre>");
    return lineIndex;
}

// Skips leading blockquote markers ('>' or the HTML-escaped '&gt;') at the start
// of `line`, honoring up to three leading spaces as CommonMark allows. Returns
// either the index just past the marker (and any single following space) or -1
// if the line does not open a blockquote at this position.
int skipBlockquoteMarker(const QString& line, int from)
{
    int pos = from;
    int spaces = 0;
    while (pos < line.size() && spaces < 3 && line.at(pos) == QLatin1Char(' ')) {
        ++pos;
        ++spaces;
    }
    if (pos < line.size() && line.at(pos) == QLatin1Char('>')) {
        pos += 1;
    } else if (line.mid(pos, 4) == QStringLiteral("&gt;")) {
        pos += 4;
    } else {
        return -1;
    }
    if (pos < line.size() && line.at(pos) == QLatin1Char(' ')) {
        ++pos;
    }
    return pos;
}

// Renders a Markdown blockquote starting at `lineIndex` into `out` and returns
// the index of the first unconsumed line. A blockquote is a run of lines that
// either carry a '>' marker or are absorbed lazily as continuation prose. Nested
// markers (">> ", "> > ") open an inner blockquote, which is rendered via
// recursion on line content with one leading marker removed.
int renderBlockquote(const QList<QString>& lines, int lineIndex, QString& out)
{
    out += QStringLiteral("<blockquote>");

    int index = lineIndex;
    bool wroteContent = false;
    const auto appendContent = [&](const QString& text) {
        if (wroteContent) {
            out += QStringLiteral("<br>");
        }
        out += text;
        wroteContent = true;
    };

    while (index < lines.size()) {
        const QString& line = lines.at(index);
        const int markerEnd = skipBlockquoteMarker(line, 0);

        if (markerEnd == -1) {
            if (line.trimmed().isEmpty()) {
                // A blank line ends the blockquote rather than being absorbed.
                break;
            }
            // Lazy continuation: prose that does not start another block stays
            // inside the current blockquote.
            appendContent(renderInline(line));
            ++index;
            continue;
        }

        const int nestedEnd = skipBlockquoteMarker(line, markerEnd);
        if (nestedEnd != -1) {
            // Nested quote: strip one marker from each consecutive nested line and
            // render the reduced line set as an inner blockquote.
            QList<QString> inner;
            int innerIndex = index;
            while (innerIndex < lines.size()) {
                const int m = skipBlockquoteMarker(lines.at(innerIndex), 0);
                if (m == -1) {
                    break;
                }
                inner.append(lines.at(innerIndex).mid(m));
                ++innerIndex;
            }
            const int nestedLineCount = inner.size();
            const int before = out.size();
            renderBlockquote(inner, 0, out);
            if (out.size() > before) {
                wroteContent = true;
            }
            index += nestedLineCount;
            continue;
        }

        // A marked line of content. The lone ">" marker line is an empty quote
        // line (the blank separator used by the quoted-reply fallback); it is
        // emitted here as a line break to keep the block visually contiguous.
        const QString content = line.mid(markerEnd);
        appendContent(content.trimmed().isEmpty()
                          ? QStringLiteral("<br>")
                          : renderInline(content));
        ++index;
    }

    out += QStringLiteral("</blockquote>");
    return index;
}

QString renderFallbackMarkdown(QString result)
{
    const QList<QString> lines = result.split(QLatin1Char('\n'));

    QString out;
    out.reserve(result.size() + 64);
    // Sibling of the 6.10 path: zero the default block margins that QTextDocument
    // assigns to <hN>, <ul>/<ol>/<li> and <pre> so headings, lists and code
    // blocks sit tightly next to the surrounding prose instead of leaving a
    // large blank band.

    int lineIndex = 0;
    while (lineIndex < lines.size()) {
        const QString& line = lines.at(lineIndex);

        // A blank line is a paragraph separator. Emit a single line break so two
        // adjacent paragraphs do not run together, but not a trailing one.
        if (line.trimmed().isEmpty()) {
            if (lineIndex + 1 < lines.size()) {
                out += QStringLiteral("<br>");
            }
            ++lineIndex;
            continue;
        }

        // ATX heading: one to six '#' characters followed by a space (or the
        // end of the line). Trailing '#' characters finish the heading text.
        int headingStart = 0;
        while (headingStart < line.size() && headingStart < 3 && line.at(headingStart) == QLatin1Char(' ')) {
            ++headingStart;
        }
        int hashCount = 0;
        while (headingStart + hashCount < line.size()
               && line.at(headingStart + hashCount) == QLatin1Char('#')) {
            ++hashCount;
        }
        if (hashCount >= 1 && hashCount <= 6
            && (headingStart + hashCount == line.size()
                || line.at(headingStart + hashCount) == QLatin1Char(' '))) {
            QString headingText = line.mid(headingStart + hashCount);
            if (headingText.startsWith(QLatin1Char(' '))) {
                headingText.remove(0, 1);
            }
            int contentEnd = headingText.size();
            while (contentEnd > 0 && headingText.at(contentEnd - 1).isSpace()) {
                --contentEnd;
            }
            int closingHashStart = contentEnd;
            while (closingHashStart > 0 && headingText.at(closingHashStart - 1) == QLatin1Char('#')) {
                --closingHashStart;
            }
            if (closingHashStart < contentEnd) {
                headingText = headingText.left(closingHashStart).trimmed();
            }
            out += QStringLiteral("<h%1>").arg(hashCount);
            out += renderInline(headingText);
            out += QStringLiteral("</h%1>").arg(hashCount);
            ++lineIndex;
            continue;
        }

        // List items: a run of '-', '*', '+' or "1." markers forms a list.
        QChar listType;
        int listContent;
        const int listIndent = leadingSpaces(line);
        if (parseListMarker(line, listIndent, listType, listContent)) {
            lineIndex = renderList(lines, lineIndex, out, listIndent);
            continue;
        }

        // Fenced code block.
        int fenceStart = 0;
        while (fenceStart < line.size() && fenceStart < 3 && line.at(fenceStart) == QLatin1Char(' ')) {
            ++fenceStart;
        }
        const QChar fenceChar = fenceStart < line.size() ? line.at(fenceStart) : QChar();
        if (fenceChar == QLatin1Char('`') || fenceChar == QLatin1Char('~')) {
            int fenceLength = 0;
            while (fenceStart + fenceLength < line.size()
                   && line.at(fenceStart + fenceLength) == fenceChar) {
                ++fenceLength;
            }
            if (fenceLength >= 3) {
                lineIndex = renderFencedBlock(lines, lineIndex, line, out);
                continue;
            }
        }

        // Markdown blockquote: a line whose leading content is one or more '>'
        // markers. This includes the fallback blockquote this client emits, so
        // nested quoted replies render as a proper blockquote on Qt < 6.10 too.
        if (skipBlockquoteMarker(line, 0) != -1) {
            lineIndex = renderBlockquote(lines, lineIndex, out);
            continue;
        }

        // Consecutive prose lines form a single paragraph whose lines are
        // joined with <br>; the paragraph ends at a blank line or a block.
        QString paragraph;
        while (lineIndex < lines.size()) {
            const QString& currentLine = lines.at(lineIndex);
            if (currentLine.trimmed().isEmpty()) {
                break;
            }
            const int currentIndent = leadingSpaces(currentLine);
            QChar currentType;
            int currentContent;
            const bool isMarker = parseListMarker(currentLine, currentIndent, currentType, currentContent);
            int hs = 0;
            while (hs < currentLine.size() && hs < 3 && currentLine.at(hs) == QLatin1Char(' ')) {
                ++hs;
            }
            int hc = 0;
            while (hs + hc < currentLine.size() && currentLine.at(hs + hc) == QLatin1Char('#')) {
                ++hc;
            }
            const bool isHeading = hc >= 1 && hc <= 6
                                   && (hs + hc == currentLine.size()
                                       || currentLine.at(hs + hc) == QLatin1Char(' '));
            const bool isParagraph = !isHeading && !isMarker
                                     && skipBlockquoteMarker(currentLine, 0) == -1
                                     && !(currentLine.startsWith(QStringLiteral("```"))
                                          || currentLine.startsWith(QStringLiteral("~~~")));
            if (!isParagraph) {
                break;
            }
            if (!paragraph.isEmpty()) {
                paragraph += QStringLiteral("<br>");
            }
            paragraph += renderInline(currentLine);
            ++lineIndex;
        }
        if (!paragraph.isEmpty()) {
            out += paragraph;
        }
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
            // a path such as a date; the loop only inspects the last character,
            // so only a genuine trailing '.' (sentence terminator) is removed.
            while (!href.isEmpty()) {
                const QChar tail = href.back();
                if (tail == QLatin1Char('.') || tail == QLatin1Char(',')
                    || tail == QLatin1Char(';') || tail == QLatin1Char('!')
                    || tail == QLatin1Char('?')) {
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
    QString result(text.toHtmlEscaped());

    replaceEmojis(result);

    return renderFallbackMarkdown(result);
#endif
}

} // namespace MessageFormatter
} // namespace Mattermost
