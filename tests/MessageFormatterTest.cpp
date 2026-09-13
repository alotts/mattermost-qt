#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QImage>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>

#include "backend/emoji/EmojiInfo.h"
#include "chat-area/post/MessageFormatter.h"

using namespace Mattermost;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
namespace {

bool blockHasText(const QTextBlock& block)
{
    for (const QChar character: block.text()) {
        if (!character.isSpace() && character != QChar::ObjectReplacementCharacter) {
            return true;
        }
    }
    return false;
}

bool blockHasImage(const QTextBlock& block)
{
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
            return true;
        }
    }
    return false;
}

bool hasMixedTextAndImageBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasText(block) && blockHasImage(block)) {
            return true;
        }
    }
    return false;
}

int imageBlockCount(const QTextDocument& document)
{
    int count = 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasImage(block)) {
            ++count;
        }
    }
    return count;
}

QString renderedPlainText(const QString& source)
{
    QTextDocument rendered;
    rendered.setHtml(MessageFormatter::formatMessageText(source));
    return rendered.toPlainText();
}

QString firstAnchorHref(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor()) {
                return fragment.charFormat().anchorHref();
            }
        }
    }
    return {};
}

QTextBlock firstTextBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasText(block)) {
            return block;
        }
    }
    return {};
}

QTextBlock firstImageBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasImage(block)) {
            return block;
        }
    }
    return {};
}

} // namespace
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
namespace {

QString renderedPlainText(const QString& source)
{
    QTextDocument rendered;
    rendered.setHtml(MessageFormatter::formatMessageText(source));
    return rendered.toPlainText();
}

bool documentHasImage(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace
#endif

class MessageFormatterTest : public QObject
{
    Q_OBJECT

private slots:
    void multilineSingleBacktickCodeBecomesPreformattedBlock()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "`{\n"
            "  \"name\": \"ContextOverflowError\",\n"
            "  \"data\": {\n"
            "    \"message\": \"Compaction exhausted: context still exceeds model limits after 3 attempts\"\n"
            "  }\n"
            "}`");

        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        const QString plain = rendered.toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("{\n  \"name\": \"ContextOverflowError\",\n  \"data\": {")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("\n    \"message\": \"Compaction exhausted")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&quot;")), qPrintable(plain));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void singleLineInlineCodeStaysInline()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(QStringLiteral("before `foo()` after"));
        QVERIFY2(!html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("before `foo()` after")), QStringLiteral("before foo() after"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void fencedCodePreservesLineBreaksQuotesAndAmpersands()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "```json\n"
            "{\n"
            "  \"quoted\": \"value\",\n"
            "  \"entity\": \"A & B\"\n"
            "}\n"
            "```");

        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        const QString plain = rendered.toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("{\n  \"quoted\": \"value\",\n  \"entity\": \"A & B\"\n}")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&quot;")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&amp;")), qPrintable(plain));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void rawHtmlIsNotInterpreted()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("before <b>bold</b> <script>alert('x')</script> after"));

        QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!html.contains(QStringLiteral("<b>bold</b>"), Qt::CaseInsensitive));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void bareUrlIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("see https://example.com/path?q=1"));
        QCOMPARE(firstAnchorHref(document), QStringLiteral("https://example.com/path?q=1"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void longPercentEncodedBareUrlIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString url = QStringLiteral(
            "https://example.com/docs/1234567890/"
            "long+percent+encoded+bare+url+regression+"
            "%D0%BF%D1%80%D0%B8%D0%BC%D0%B5%D1%80+"
            "%D0%B4%D0%BB%D0%B8%D0%BD%D0%BD%D0%BE%D0%B9+"
            "%D1%81%D1%82%D1%80%D0%BE%D0%BA%D0%B8+"
            "%D1%81+%D0%BF%D1%80%D0%BE%D0%B1%D0%B5%D0%BB%D0%B0%D0%BC%D0%B8+"
            "%D0%B8+%D1%81%D0%B8%D0%BC%D0%B2%D0%BE%D0%BB%D0%B0%D0%BC%D0%B8+"
            "2026-08-04");

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, url);
        QCOMPARE(firstAnchorHref(document), url);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void inlineCodeUrlIsNotLinkified()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(
            document, QStringLiteral("`https://example.com/not-a-link`"));
        QVERIFY(firstAnchorHref(document).isEmpty());
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void markdownLinkIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("[example](https://example.com/path)"));
        QCOMPARE(firstAnchorHref(document), QStringLiteral("https://example.com/path"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void unicodeEmojiAliasIsExpanded()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("hello :wave: world"));
        QVERIFY(!document.toPlainText().contains(QStringLiteral(":wave:")));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void customEmojiStaysInline()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString name = QStringLiteral("message_formatter_test_custom");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/tmp/custom-emoji/message-formatter-test.gif"));

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("before :") + name + QStringLiteral(": after"));

        QCOMPARE(document.blockCount(), 1);
        QVERIFY(blockHasText(document.firstBlock()));
        QVERIFY(blockHasImage(document.firstBlock()));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageGetsOwnParagraph()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "before ![large image](https://example.com/large.png) after");

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, source);
        QVERIFY(!hasMixedTextAndImageBlock(document));
        QCOMPARE(imageBlockCount(document), 1);

        // Rich message fragments are serialized to HTML before being shown in
        // the wrapped text child. Verify that image separation survives it.
        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        QVERIFY(!hasMixedTextAndImageBlock(rendered));
        QCOMPARE(imageBlockCount(rendered), 1);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageAfterTextGetsOwnParagraph()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "last line of text ![large image](https://example.com/large.png)");

        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        QVERIFY(!hasMixedTextAndImageBlock(rendered));
        QCOMPARE(imageBlockCount(rendered), 1);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageDoesNotInflateTextLine()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QUrl imageUrl(QStringLiteral("https://example.com/large.png"));
        const QString source = QStringLiteral("last line of text ![large image](https://example.com/large.png)");

        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        rendered.addResource(QTextDocument::ImageResource, imageUrl, QImage(320, 240, QImage::Format_ARGB32));
        rendered.setTextWidth(640);
        rendered.documentLayout()->documentSize();

        const QTextBlock textBlock = firstTextBlock(rendered);
        const QTextBlock imageBlock = firstImageBlock(rendered);
        QVERIFY(textBlock.isValid());
        QVERIFY(imageBlock.isValid());
        QVERIFY(textBlock != imageBlock);
        QCOMPARE(textBlock.blockFormat().topMargin(), 0.0);
        QCOMPARE(textBlock.blockFormat().bottomMargin(), 0.0);
        QCOMPARE(imageBlock.blockFormat().topMargin(), 0.0);
        QCOMPARE(imageBlock.blockFormat().bottomMargin(), 0.0);

        const QTextLayout* textLayout = textBlock.layout();
        QVERIFY(textLayout != nullptr);
        QVERIFY(textLayout->lineCount() > 0);

        const qreal normalLineHeight = QFontMetrics(rendered.defaultFont()).height();
        const qreal actualLineHeight = textLayout->lineAt(0).height();
        QVERIFY2(actualLineHeight <= normalLineHeight * 1.5,
                 qPrintable(QStringLiteral("text line height %1, normal %2").arg(actualLineHeight).arg(normalLineHeight)));

        const QRectF textRect = rendered.documentLayout()->blockBoundingRect(textBlock);
        const QRectF imageRect = rendered.documentLayout()->blockBoundingRect(imageBlock);
        const qreal gap = imageRect.top() - textRect.bottom();
        QVERIFY2(gap <= normalLineHeight,
                 qPrintable(QStringLiteral("unexpected text/image gap %1, line height %2").arg(gap).arg(normalLineHeight)));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void pre610StrikethroughIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("before ~~struck text~~ after"));
        QVERIFY2(html.contains(QStringLiteral("<s>struck text</s>")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("before ~~struck text~~ after")),
                 QStringLiteral("before struck text after"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610EscapedCharactersShowWithoutBackslash()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        // Backslash-escaped Markdown metacharacters render as the literal
        // character with the backslash removed, matching CommonMark.
        QCOMPARE(renderedPlainText(QStringLiteral("\\*literal asterisks\\*")),
                 QStringLiteral("*literal asterisks*"));
        QCOMPARE(renderedPlainText(QStringLiteral("\\_underscores\\_")),
                 QStringLiteral("_underscores_"));
        QCOMPARE(renderedPlainText(QStringLiteral("\\~~struck~~")),
                 QStringLiteral("~~struck~~"));
        QCOMPARE(renderedPlainText(QStringLiteral("\\`backtick\\`")),
                 QStringLiteral("`backtick`"));
        // An escaped backslash collapses to a single backslash.
        QCOMPARE(renderedPlainText(QStringLiteral("back\\\\slash")),
                 QStringLiteral("back\\slash"));
        // Escaped punctuation does not trigger Markdown structure.
        QCOMPARE(renderedPlainText(QStringLiteral("\\# not a heading \\- not a list")),
                 QStringLiteral("# not a heading - not a list"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610AutoLinkIsClickable()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("visit <https://example.com/path?q=1&x=2> now"));
        QVERIFY2(html.contains(QStringLiteral("<a href="), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("https://example.com/path?q=1&amp;x=2")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("&lt;")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("&gt;")), qPrintable(html));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610BareUrlStaysClickable()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("see https://example.com/path?q=1 now"));
        QVERIFY2(html.contains(QStringLiteral("<a href="), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("https://example.com/path?q=1")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("see https://example.com/path?q=1 now")),
                 QStringLiteral("see https://example.com/path?q=1 now"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610InlineCodeUrlIsNotLinkified()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("`https://example.com/not-a-link`"));
        QVERIFY2(!html.contains(QStringLiteral("<a href="), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<code>https://example.com/not-a-link</code>")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("`https://example.com/not-a-link`")),
                 QStringLiteral("https://example.com/not-a-link"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610BareUrlTrailingPeriodIsNotPartOfLink()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("See https://example.com/foo."));
        QVERIFY2(html.contains(QStringLiteral("\"https://example.com/foo\"")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("\"https://example.com/foo.\"")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral(">https://example.com/foo<")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("See https://example.com/foo.")),
                 QStringLiteral("See https://example.com/foo."));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610FencedCodeUrlIsNotLinkified()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "```\n"
            "https://example.com/code\n"
            "```");
        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(!html.contains(QStringLiteral("<a href="), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(source), QStringLiteral("https://example.com/code"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610StrikethroughInsideCodeStaysLiteral()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("`~~struck~~` in code"));
        QVERIFY2(!html.contains(QStringLiteral("<s>"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<code>~~struck~~</code>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("`~~struck~~` in code")),
                 QStringLiteral("~~struck~~ in code"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610BoldIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("before **bold text** after"));
        QVERIFY2(html.contains(QStringLiteral("<b>bold text</b>")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("before **bold text** after")),
                 QStringLiteral("before bold text after"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610ItalicIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("before *italic text* after"));
        QVERIFY2(html.contains(QStringLiteral("<i>italic text</i>")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("before *italic text* after")),
                 QStringLiteral("before italic text after"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610BoldItalicIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("***both***"));
        QVERIFY2(html.contains(QStringLiteral("<b><i>both</i></b>")), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("***both***")), QStringLiteral("both"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610FencedCodeBlockIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "```cpp\n"
            "int main() { return 0; }\n"
            "```");
        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("int main() { return 0; }")), qPrintable(html));
        QCOMPARE(renderedPlainText(source), QStringLiteral("int main() { return 0; }"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610MarkdownLinkIsClickable()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("[label](https://example.com/path)"));
        QVERIFY2(html.contains(QStringLiteral("href=\"https://example.com/path\""), Qt::CaseInsensitive),
                 qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral(">label</a>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("[label](https://example.com/path)")),
                 QStringLiteral("label"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610HeaderIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(QStringLiteral("## Heading"));
        QVERIFY2(html.contains(QStringLiteral("<h2"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("Heading")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("</h2>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("## Heading")), QStringLiteral("Heading"));
        QCOMPARE(renderedPlainText(QStringLiteral("# Top")), QStringLiteral("Top"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610UnorderedListIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("- one\n- two\n- three"));
        QVERIFY2(html.contains(QStringLiteral("<ul"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.count(QStringLiteral("<li"), Qt::CaseInsensitive) == 3, qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral(">one</li>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("- one\n- two\n- three")),
                 QStringLiteral("one\ntwo\nthree"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610OrderedListIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("1. first\n2. second"));
        QVERIFY2(html.contains(QStringLiteral("<ol"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral(">first</li>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("1. first\n2. second")),
                 QStringLiteral("first\nsecond"));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610BlockquoteIsFormatted()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("> quoted line\n> second quote"));
        QVERIFY2(html.contains(QStringLiteral("<blockquote"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("quoted line")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("second quote")), qPrintable(html));

        // The variable-width fallback blockquote emits a trailing <br> after the
        // last consumed line; the quoted text is present before the closing tag.
        QVERIFY2(html.contains(QStringLiteral("</blockquote>"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("> quoted line")),
                 QStringLiteral("quoted line"));

        // A wire-format quoted reply fallback is also a blockquote.
        const QString reply = MessageFormatter::formatMessageText(
            QStringLiteral("> [Replying to Alice](/_redirect/pl/post-id)\n> Quote text\n>\n\nReply body"));
        QVERIFY2(reply.contains(QStringLiteral("<blockquote"), Qt::CaseInsensitive), qPrintable(reply));
        QVERIFY2(reply.contains(QStringLiteral("Quote text")), qPrintable(reply));

        // The fallback is rendered as Markdown, so the quoted lines appear before
        // the reply body; the '>' markers must not leak into the plain text.
        const QString plain = renderedPlainText(QStringLiteral(
            "> [Replying to Alice](/_redirect/pl/post-id)\n> Quote text\n>\n\nReply body"));
        QVERIFY2(!plain.contains(QLatin1Char('>')), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("Reply body")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("Quote text")), qPrintable(plain));

        // Nested blockquote markers ("quote inside quote") produce nested
        // <blockquote> elements rather than literal '>' characters.
        const QString nested = MessageFormatter::formatMessageText(
            QStringLiteral("> outer\n>> inner"));
        QCOMPARE(nested.count(QStringLiteral("<blockquote"), Qt::CaseInsensitive), 2);
        QVERIFY2(nested.contains(QStringLiteral("inner")), qPrintable(nested));
        const QString nestedPlain = renderedPlainText(QStringLiteral("> outer\n>> inner"));
        QVERIFY2(!nestedPlain.contains(QLatin1Char('>')), qPrintable(nestedPlain));
        QVERIFY2(nestedPlain.contains(QStringLiteral("inner")), qPrintable(nestedPlain));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610CustomEmojiInsideBoldBecomesImage()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString name = QStringLiteral("fallback_test_custom_bold");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/tmp/custom-emoji/fallback-bold.gif"));
        const QString token = QStringLiteral(":") + name + QStringLiteral(":");

        const QString html = MessageFormatter::formatMessageText(QStringLiteral("**a ") + token + QStringLiteral(" b**"));
        QVERIFY2(html.contains(QStringLiteral("<b>"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        QVERIFY2(documentHasImage(rendered), "custom emoji must resolve to an image inside bold");
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610CustomEmojiInsideListAndHeaderBecomesImage()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString name = QStringLiteral("fallback_test_custom_block");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/tmp/custom-emoji/fallback-block.gif"));
        const QString token = QStringLiteral(":") + name + QStringLiteral(":");

        // Custom emoji must survive markdown block parsing (a list item and a
        // heading) and still resolve to an embedded image.
        const QString source = QStringLiteral("# ") + token + QStringLiteral(" title\n- ") + token
            + QStringLiteral(" item");
        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<h1"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<li"), Qt::CaseInsensitive), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<img"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        QVERIFY2(documentHasImage(rendered), "custom emoji must resolve to an image inside blocks");
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610UnicodeEmojiInsideMarkdownRoundTrips()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        const QString message = QString::fromUtf8("**a \xE2\x9C\x85 b**\n# \xE2\x9C\x85 title"); // ✅
        const QString expected = QString::fromUtf8("a \xE2\x9C\x85 b\n\xE2\x9C\x85 title");
        QCOMPARE(renderedPlainText(message), expected);
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }

    void pre610ShortcodeAsLinkLabelIsResolved()
    {
#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
        // Regression: a nightly-test report uses ":white_check_mark:" /
        // ":warning:" as the label of Markdown links. The naive colon-scanning
        // emoji pass paired a link's http(s): colon with the emoji's closing
        // colon, so the token never resolved and was shown literally.
        const QString message = QStringLiteral(
            "[:white_check_mark:  tatlin_object_web_ui_tests](https://obj-jenkins.spb.yadro.com/job/tatlin_object_web_ui_tests/1363/): 87.21%\n"
            "[:warning:  aio_s3_compatibility_tests](https://obj-jenkins.spb.yadro.com/job/aio_s3_compatibility_tests/1815/): UNSTABLE\n");
        const QString html = MessageFormatter::formatMessageText(message);
        QVERIFY2(!html.contains(QStringLiteral(":white_check_mark:")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral(":warning:")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<a href="), Qt::CaseInsensitive), qPrintable(html));
        // The shortcodes must have resolved to their Unicode emoji rather than
        // being left as literal tokens.
        QVERIFY2(html.contains(QString::fromUtf8("\xE2\x9C\x85")), qPrintable(html)); // ✅
        QVERIFY2(html.contains(QString::fromUtf8("\xE2\x9A\xA0")), qPrintable(html)); // ⚠
        QVERIFY2(!renderedPlainText(message).contains(QStringLiteral(":white_check_mark:")),
                 qPrintable(message));
#else
        QSKIP("Pre-Qt-6.10 fallback formatter test");
#endif
    }
};

QTEST_MAIN(MessageFormatterTest)

#include "MessageFormatterTest.moc"