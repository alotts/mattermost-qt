#include "MessageContentWidget.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBoundaryFinder>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

#include "MessageFormatter.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "ui/EmojiPresentation.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include "qsourcehighliter.h"
#endif

namespace Mattermost {
namespace {

const QSet<QString>& unicodeEmojiStrings()
{
    static const QSet<QString> emojiStrings = [] {
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

    return emojiStrings;
}

bool isEmojiOnlyMessage(const QString& message)
{
    if (message.trimmed().isEmpty()) {
        return false;
    }

    const QSet<QString>& emojiStrings = unicodeEmojiStrings();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, message);
    bool foundEmoji = false;
    int position = 0;

    while (position < message.size()) {
        if (message.at(position).isSpace()) {
            ++position;
            continue;
        }

        if (message.at(position) == QLatin1Char(':')) {
            const int end = message.indexOf(QLatin1Char(':'), position + 1);
            if (end > position + 1) {
                const QString name = message.mid(position + 1, end - position - 1);
                if (EmojiInfo::findByName(name)) {
                    foundEmoji = true;
                    position = end + 1;
                    continue;
                }
            }
        }

        finder.setPosition(position);
        const int end = finder.toNextBoundary();
        if (end <= position) {
            return false;
        }

        if (!emojiStrings.contains(message.mid(position, end - position))) {
            return false;
        }
        foundEmoji = true;
        position = end;
    }

    return foundEmoji;
}

void applyEmojiPresentation(QTextDocument& document, bool jumbo)
{
    const QString text = document.toPlainText();
    if (text.isEmpty()) {
        return;
    }

    const EmojiPresentation::Mode mode = jumbo
        ? EmojiPresentation::Mode::Jumbo
        : EmojiPresentation::Mode::Inline;
    const qreal scale = EmojiPresentation::fontScale(mode);
    const QSet<QString>& emojiStrings = unicodeEmojiStrings();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toStart();

    int start = 0;
    while (true) {
        const int end = finder.toNextBoundary();
        if (end < 0) {
            break;
        }

        const QString grapheme = text.mid(start, end - start);
        if (emojiStrings.contains(grapheme)) {
            QTextCursor cursor(&document);
            cursor.setPosition(start);

            qreal pointSize = cursor.charFormat().fontPointSize();
            if (pointSize <= 0.0) {
                pointSize = document.defaultFont().pointSizeF();
            }

            if (pointSize > 0.0) {
                cursor.setPosition(end, QTextCursor::KeepAnchor);
                QTextCharFormat emojiFormat;
                emojiFormat.setFontPointSize(pointSize * scale);
                QFont emojiFont = document.defaultFont();
                EmojiPresentation::preferEmojiFont(emojiFont);
                emojiFormat.setFontFamilies(emojiFont.families());
                cursor.mergeCharFormat(emojiFormat);
            }
        }

        start = end;
    }

    EmojiPresentation::apply(document, mode);
}

class WrappedRichText final : public QTextBrowser
{
public:
    explicit WrappedRichText(std::function<void()> heightChanged, QWidget* parent = nullptr)
        : QTextBrowser(parent)
        , heightChanged(std::move(heightChanged))
    {
        setObjectName(QStringLiteral("messageRichText"));
        setReadOnly(true);
        setOpenExternalLinks(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setContentsMargins(0, 0, 0, 0);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
        // Keep the viewport transparent without wrapping the QTextBrowser in
        // QStyleSheetStyle. A per-widget style sheet resolves palette roles at
        // construction time and prevents live application palette propagation.
        viewport()->setAutoFillBackground(false);
        document()->setDocumentMargin(0);
        applyWrapMode();
    }

    void setContentHtml(const QString& html, bool jumboEmoji = false)
    {
        setHtml(html);
        document()->setDocumentMargin(0);
        applyEmojiPresentation(*document(), jumboEmoji);
        applyWrapMode();
        scheduleHeightUpdate();
    }

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTextBrowser::resizeEvent(event);
        scheduleHeightUpdate();
    }

private:
    void applyWrapMode()
    {
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDefaultTextOption(option);
    }

    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        if (viewport()->width() <= 0) {
            return;
        }

        document()->setTextWidth(viewport()->width());
        const int documentHeight = static_cast<int>(std::ceil(document()->size().height()));
        const int wantedHeight = std::max(fontMetrics().height(), documentHeight + 2 * frameWidth());
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
            if (heightChanged) {
                heightChanged();
            }
        }
    }

    std::function<void()> heightChanged;
};

class QuoteBar final : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Mid));
    }
};

class QuoteBlock final : public QWidget
{
public:
    QuoteBlock(const QString& html,
               std::function<void()> heightChanged,
               QWidget* parent = nullptr)
        : QWidget(parent)
        , heightChanged(std::move(heightChanged))
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        auto* bar = new QuoteBar(this);
        bar->setFixedWidth(3);
        layout->addWidget(bar);

        text = new WrappedRichText([this] {
            updateGeometry();
            if (this->heightChanged) {
                this->heightChanged();
            }
        }, this);
        text->setContentHtml(html);
        layout->addWidget(text, 1);
        updateMutedPalette();
    }

    WrappedRichText* browser() const { return text; }

    QSize sizeHint() const override
    {
        return QSize(0, text ? text->height() : fontMetrics().height());
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);
        if (event && (event->type() == QEvent::PaletteChange
                      || event->type() == QEvent::ApplicationPaletteChange)) {
            updateMutedPalette();
        }
    }

private:
    void updateMutedPalette()
    {
        if (!text) {
            return;
        }
        QPalette muted = text->palette();
        const QColor mutedText = palette().color(QPalette::Disabled, QPalette::Text);
        muted.setColor(QPalette::Text, mutedText);
        muted.setColor(QPalette::WindowText, mutedText);
        text->setPalette(muted);
        update();
    }

    WrappedRichText* text = nullptr;
    std::function<void()> heightChanged;
};

struct MessageSegment {
    bool quote = false;
    QString text;
};

int markdownFenceRun(const QString& line, QChar& fenceCharacter, int& contentStart)
{
    int position = 0;
    while (position < line.size() && position < 3 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    contentStart = position;
    if (position >= line.size()) {
        return 0;
    }

    const QChar character = line.at(position);
    if (character != QLatin1Char('`') && character != QLatin1Char('~')) {
        return 0;
    }

    int run = 0;
    while (position + run < line.size() && line.at(position + run) == character) {
        ++run;
    }
    if (run < 3) {
        return 0;
    }
    fenceCharacter = character;
    contentStart = position + run;
    return run;
}

bool extractQuoteLine(const QString& line, QString& content)
{
    int position = 0;
    while (position < line.size() && position < 3 && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    if (position >= line.size() || line.at(position) != QLatin1Char('>')) {
        return false;
    }

    ++position;
    if (position < line.size() && line.at(position) == QLatin1Char(' ')) {
        ++position;
    }
    content = line.mid(position);
    return true;
}

QVector<MessageSegment> splitMessageSegments(const QString& message)
{
    QVector<MessageSegment> result;
    const QStringList lines = message.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    bool inFence = false;
    QChar fenceCharacter;
    int fenceLength = 0;

    auto append = [&result](bool quote, const QString& line) {
        if (result.isEmpty() || result.back().quote != quote) {
            result.push_back(MessageSegment {quote, line});
        } else {
            result.back().text += QLatin1Char('\n');
            result.back().text += line;
        }
    };

    for (const QString& line : lines) {
        QString renderedLine = line;

        QChar candidateCharacter;
        int afterFence = 0;
        const int candidateLength = markdownFenceRun(line, candidateCharacter, afterFence);

        if (inFence) {
            append(false, renderedLine);
            if (candidateLength >= fenceLength && candidateCharacter == fenceCharacter
                && line.mid(afterFence).trimmed().isEmpty()) {
                inFence = false;
                fenceCharacter = QChar();
                fenceLength = 0;
            }
            continue;
        }

        if (candidateLength >= 3) {
            inFence = true;
            fenceCharacter = candidateCharacter;
            fenceLength = candidateLength;
            append(false, renderedLine);
            continue;
        }

        append(extractQuoteLine(line, renderedLine), renderedLine);
    }

    return result;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)

using SourceLanguage = QSourceHighlite::QSourceHighliter::Language;

std::optional<SourceLanguage> sourceLanguageForName(QString language)
{
    language = language.trimmed().toLower();
    const int whitespace = [&language] {
        const int space = language.indexOf(QLatin1Char(' '));
        const int tab = language.indexOf(QLatin1Char('\t'));
        if (space < 0) {
            return tab;
        }
        if (tab < 0) {
            return space;
        }
        return std::min(space, tab);
    }();
    if (whitespace >= 0) {
        language.truncate(whitespace);
    }

    if (language == QLatin1String("cpp") || language == QLatin1String("c++")
        || language == QLatin1String("cxx") || language == QLatin1String("cc")) {
        return SourceLanguage::CodeCpp;
    }
    if (language == QLatin1String("c")) {
        return SourceLanguage::CodeC;
    }
    if (language == QLatin1String("js") || language == QLatin1String("javascript")) {
        return SourceLanguage::CodeJs;
    }
    if (language == QLatin1String("bash") || language == QLatin1String("sh")
        || language == QLatin1String("shell")) {
        return SourceLanguage::CodeBash;
    }
    if (language == QLatin1String("php")) {
        return SourceLanguage::CodePHP;
    }
    if (language == QLatin1String("qml")) {
        return SourceLanguage::CodeQML;
    }
    if (language == QLatin1String("py") || language == QLatin1String("python")) {
        return SourceLanguage::CodePython;
    }
    if (language == QLatin1String("rs") || language == QLatin1String("rust")) {
        return SourceLanguage::CodeRust;
    }
    if (language == QLatin1String("java")) {
        return SourceLanguage::CodeJava;
    }
    if (language == QLatin1String("cs") || language == QLatin1String("c#")
        || language == QLatin1String("csharp")) {
        return SourceLanguage::CodeCSharp;
    }
    if (language == QLatin1String("go")) {
        return SourceLanguage::CodeGo;
    }
    if (language == QLatin1String("v")) {
        return SourceLanguage::CodeV;
    }
    if (language == QLatin1String("sql")) {
        return SourceLanguage::CodeSQL;
    }
    if (language == QLatin1String("json")) {
        return SourceLanguage::CodeJSON;
    }
    if (language == QLatin1String("xml") || language == QLatin1String("html")) {
        return SourceLanguage::CodeXML;
    }
    if (language == QLatin1String("css")) {
        return SourceLanguage::CodeCSS;
    }
    if (language == QLatin1String("ts") || language == QLatin1String("typescript")) {
        return SourceLanguage::CodeTypeScript;
    }
    if (language == QLatin1String("yaml") || language == QLatin1String("yml")) {
        return SourceLanguage::CodeYAML;
    }
    if (language == QLatin1String("ini")) {
        return SourceLanguage::CodeINI;
    }
    if (language == QLatin1String("vex")) {
        return SourceLanguage::CodeVex;
    }
    if (language == QLatin1String("cmake")) {
        return SourceLanguage::CodeCMake;
    }
    if (language == QLatin1String("make") || language == QLatin1String("makefile")) {
        return SourceLanguage::CodeMake;
    }
    if (language == QLatin1String("asm") || language == QLatin1String("assembly")) {
        return SourceLanguage::CodeAsm;
    }
    if (language == QLatin1String("lua")) {
        return SourceLanguage::CodeLua;
    }
    if (language == QLatin1String("rhai")) {
        return SourceLanguage::CodeRhai;
    }

    return std::nullopt;
}

class CodeBlockEdit final : public QPlainTextEdit
{
public:
    CodeBlockEdit(const QString& code,
                  const QString& language,
                  std::function<void()> heightChanged,
                  QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
        , heightChanged(std::move(heightChanged))
    {
        setObjectName(QStringLiteral("messageCodeBlock"));
        setProperty("codeLanguage", language);
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        document()->setDocumentMargin(6);

        QPalette codePalette = palette();
        codePalette.setColor(QPalette::Base, QColor(39, 40, 34));
        codePalette.setColor(QPalette::Text, QColor(227, 226, 214));
        setPalette(codePalette);

        setPlainText(code);

        if (const auto sourceLanguage = sourceLanguageForName(language)) {
            auto* highlighter = new QSourceHighlite::QSourceHighliter(
                document(), QSourceHighlite::QSourceHighliter::Themes::Monokai);
            highlighter->setCurrentLanguage(*sourceLanguage);
            highlighter->rehighlight();
            setProperty("sourceHighliteLanguage", static_cast<int>(*sourceLanguage));
        }

        scheduleHeightUpdate();
    }

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QPlainTextEdit::resizeEvent(event);
        scheduleHeightUpdate();
    }

private:
    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        const int lineHeight = fontMetrics().lineSpacing();
        const int lineCount = std::max(1, document()->blockCount());
        const int documentMargins =
            static_cast<int>(std::ceil(document()->documentMargin() * 2.0));
        const int scrollBarHeight = horizontalScrollBar()->maximum() > horizontalScrollBar()->minimum()
            ? horizontalScrollBar()->sizeHint().height()
            : 0;
        const QMargins widgetMargins = contentsMargins();
        const int wantedHeight = lineCount * lineHeight + documentMargins
            + widgetMargins.top() + widgetMargins.bottom()
            + 2 * frameWidth() + scrollBarHeight;
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
            if (heightChanged) {
                heightChanged();
            }
        }
    }

    std::function<void()> heightChanged;
};

bool isCodeBlock(const QTextBlock& block)
{
    const QTextBlockFormat format = block.blockFormat();
    return format.nonBreakableLines()
        || format.hasProperty(QTextFormat::BlockCodeFence)
        || format.hasProperty(QTextFormat::BlockCodeLanguage);
}

QString codeLanguage(const QTextBlock& block)
{
    return block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
}

QString fragmentHtml(QTextDocument& document, int start, int end)
{
    if (end <= start) {
        return {};
    }

    QTextCursor cursor(&document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    return QTextDocumentFragment(cursor).toHtml();
}

#endif

} // namespace

MessageContentWidget::MessageContentWidget(QWidget* parent)
    : QWidget(parent)
    , contentLayout(new QVBoxLayout(this))
{
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setMinimumWidth(0);

    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiAdded,
            this,
            [this](const QString& name) {
        if (_sourceMessage.isEmpty()) {
            return;
        }
        const QString token = QLatin1Char(':') + name + QLatin1Char(':');
        if (_sourceMessage.contains(token)) {
            setMessage(_sourceMessage);
        }
    });
}

void MessageContentWidget::setMessage(const QString& message)
{
    _sourceMessage = message;
    _jumboEmojiMessage = isEmojiOnlyMessage(message);
    clearContent();

    if (message.isEmpty()) {
        setVisible(false);
        scheduleDimensionsChanged();
        return;
    }

    setVisible(true);
    const QVector<MessageSegment> segments = splitMessageSegments(message);
    for (const MessageSegment& segment : segments) {
        if (segment.quote) {
            addQuote(MessageFormatter::formatMessageText(segment.text));
            continue;
        }
        if (segment.text.isEmpty()) {
            continue;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        addMarkdownContent(segment.text);
#else
        addRichText(MessageFormatter::formatMessageText(segment.text));
#endif
    }
    scheduleDimensionsChanged();
}

void MessageContentWidget::clear()
{
    _sourceMessage.clear();
    _jumboEmojiMessage = false;
    clearContent();
    setVisible(false);
    scheduleDimensionsChanged();
}

QString MessageContentWidget::selectedText() const
{
    QStringList selections;
    for (int i = 0; i < contentLayout->count(); ++i) {
        QWidget* widget = contentLayout->itemAt(i)->widget();
        if (!widget) {
            continue;
        }
        if (const auto* browser = qobject_cast<QTextBrowser*>(widget)) {
            if (browser->textCursor().hasSelection()) {
                selections.push_back(browser->textCursor().selectedText());
            }
        } else if (const auto* editor = qobject_cast<QPlainTextEdit*>(widget)) {
            if (editor->textCursor().hasSelection()) {
                selections.push_back(editor->textCursor().selectedText());
            }
        }
        for (const auto* browser : widget->findChildren<QTextBrowser*>()) {
            if (browser->textCursor().hasSelection()) {
                selections.push_back(browser->textCursor().selectedText());
            }
        }
        for (const auto* editor : widget->findChildren<QPlainTextEdit*>()) {
            if (editor->textCursor().hasSelection()) {
                selections.push_back(editor->textCursor().selectedText());
            }
        }
    }
    return selections.join(QLatin1Char('\n'));
}

void MessageContentWidget::clearContent()
{
    while (QLayoutItem* item = contentLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void MessageContentWidget::scheduleDimensionsChanged()
{
    updateGeometry();
    if (dimensionsChangePending) {
        return;
    }

    dimensionsChangePending = true;
    QTimer::singleShot(0, this, [this] {
        dimensionsChangePending = false;
        updateGeometry();
        if (parentWidget()) {
            parentWidget()->updateGeometry();
        }
        emit dimensionsChanged();
    });
}

void MessageContentWidget::addQuote(const QString& html)
{
    if (html.isEmpty()) {
        return;
    }

    auto* quote = new QuoteBlock(html, [this] { scheduleDimensionsChanged(); }, this);
    connect(quote->browser(),
            QOverload<const QUrl&>::of(&QTextBrowser::highlighted),
            this,
            [this](const QUrl& url) {
                emit linkHovered(url.toString());
            });
    contentLayout->addWidget(quote);
}

void MessageContentWidget::addRichText(const QString& html)
{
    if (html.isEmpty()) {
        return;
    }

    auto* richText = new WrappedRichText([this] { scheduleDimensionsChanged(); }, this);
    richText->setContentHtml(html, _jumboEmojiMessage);
    connect(richText,
            QOverload<const QUrl&>::of(&QTextBrowser::highlighted),
            this,
            [this](const QUrl& url) {
                emit linkHovered(url.toString());
            });
    contentLayout->addWidget(richText);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
void MessageContentWidget::addMarkdownContent(const QString& message)
{
    QTextDocument document;
    MessageFormatter::buildMarkdownDocument(document, message);

    int richStart = 0;
    QTextBlock block = document.begin();
    while (block.isValid()) {
        if (!isCodeBlock(block)) {
            block = block.next();
            continue;
        }

        const int codeStart = block.position();
        addRichText(fragmentHtml(document, richStart, codeStart));

        QString language = codeLanguage(block);
        QStringList codeLines;
        do {
            if (language.isEmpty()) {
                language = codeLanguage(block);
            }
            codeLines.push_back(block.text());
            block = block.next();
        } while (block.isValid() && isCodeBlock(block));

        addCodeBlock(codeLines.join(QLatin1Char('\n')), language);
        richStart = block.isValid() ? block.position() : document.characterCount() - 1;
    }

    addRichText(fragmentHtml(document, richStart, document.characterCount() - 1));
}

void MessageContentWidget::addCodeBlock(const QString& code, const QString& language)
{
    contentLayout->addWidget(new CodeBlockEdit(
        code, language, [this] { scheduleDimensionsChanged(); }, this));
}
#endif

} // namespace Mattermost