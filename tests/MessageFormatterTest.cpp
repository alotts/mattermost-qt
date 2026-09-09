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

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
    void pre610InlineMarkdownIsRendered()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("**bold** and *italic* and ~~strike~~ and `code`"));
        QVERIFY2(html.contains(QStringLiteral("<strong>bold</strong>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<em>italic</em>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<del>strike</del>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<code>code</code>")), qPrintable(html));
    }

    void pre610HeadingsAndRuleAreRendered()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("# Title\n\nparagraph\n\n---"));
        QVERIFY2(html.contains(QStringLiteral("<h1>Title</h1>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<hr>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<p>paragraph</p>")), qPrintable(html));
    }

    void pre610FencedCodeIsPreformatted()
    {
        const QString source = QStringLiteral(
            "```cpp\n"
            "#include <cstdio>\n"
            "int main() { return 0; }\n"
            "```");
        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.indexOf("<pre ") >= 0, qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("#include &lt;cstdio&gt;")), qPrintable(html));
    }

    void pre610ListsAreRendered()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("- one\n- two\n- three"));
        QVERIFY2(html.contains(QStringLiteral("<ul>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<li>one</li>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<li>two</li>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("</ul>")), qPrintable(html));
    }

    void pre610BareUrlFormattingCharsInHeadingAndListItemAreLiteral()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("# Title http://example.com/*x* \n"
                           "- item https://example.com/_y_"));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"http://example.com/*x*\">http://example.com/*x*</a>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"https://example.com/_y_\">https://example.com/_y_</a>")), qPrintable(html));

        const QString label = MessageFormatter::formatMessageText(
            QStringLiteral("link [http://example.com/*z*/](http://example.com/*z*/)"));
        QVERIFY2(label.contains(QStringLiteral("<a href=\"http://example.com/*z*/\">http://example.com/*z*/</a>")), qPrintable(label));
    }

    void pre610QuoteIsRendered()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("> quoted line"));
        QVERIFY2(html.contains(QStringLiteral("<blockquote>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("quoted line")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("</blockquote>")), qPrintable(html));
    }

    void pre610MarkdownLinkAndImageAreRendered()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("text [example](https://example.com) ![a](https://example.com/i.png)"));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"https://example.com\">example</a>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<img src=\"https://example.com/i.png\" alt=\"a\">")), qPrintable(html));
    }

    void pre610MarkdownLinkLabelFormattingCharsAreLiteral()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("text [*bold* _em_ ~del~](https://example.com)"));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"https://example.com\">*bold* _em_ ~del~</a>")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("<em>")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("<strong>")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("<del>")), qPrintable(html));
    }

    void pre610BareUrlIsClickableAndInsideCodeIsNot()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("see https://example.com/path and `https://example.com/code`"));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"https://example.com/path\">https://example.com/path</a>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<code>https://example.com/code</code>")), qPrintable(html));
    }

    void pre610FormattingCharsInsideBareUrlAreNotParsedAsEmphasis()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("check http://example.com/*/ path"));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"http://example.com/*/\">http://example.com/*/</a>")), qPrintable(html));

        const QString html2 = MessageFormatter::formatMessageText(
            QStringLiteral("see https://example.com/_bold_ text"));
        QVERIFY2(html2.contains(QStringLiteral("<a href=\"https://example.com/_bold_\">https://example.com/_bold_</a>")), qPrintable(html2));

        const QString html3 = MessageFormatter::formatMessageText(
            QStringLiteral("try **http://example.com/*bold* end"));
        QVERIFY2(!html3.contains(QStringLiteral("<strong>http://example.com/</strong>")), qPrintable(html3));
    }

    void pre610FormattingCharsOutsideBareUrlStillWork()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("**bold** http://example.com/*em* **bold2**"));
        QVERIFY2(html.contains(QStringLiteral("<strong>bold</strong>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<strong>bold2</strong>")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("<a href=\"http://example.com/*em*\">http://example.com/*em*</a>")), qPrintable(html));
    }

    void pre610EmojiAndUrlStillExpanded()
    {
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("hello :wave: world and https://example.com"));
        QVERIFY2(!html.contains(QStringLiteral(":wave:")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("https://example.com")), qPrintable(html));
    }
#endif
};

QTEST_MAIN(MessageFormatterTest)

#include "MessageFormatterTest.moc"