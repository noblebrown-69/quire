#include "MonasteryEditor.h"

#include <QVBoxLayout>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QEventLoop>
#include <QTimer>
#include <QVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QRegularExpression>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QVariantMap>

struct FontProbe {
    QString family;
    int pt = 0;
    QString caretFamily;
    int caretPt = 0;
    bool found = false;
};

static FontProbe parseFontProbe(const QVariant &result)
{
    QVariantMap m = result.toMap();
    if (m.isEmpty() && result.canConvert<QString>()) {
        const QString s = result.toString().trimmed();
        if (s.startsWith(QLatin1Char('{'))) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError)
                m = doc.object().toVariantMap();
        }
    }
    FontProbe p;
    p.family = m.value(QStringLiteral("family")).toString().trimmed();
    p.pt = m.value(QStringLiteral("pt")).toInt();
    p.caretFamily = m.value(QStringLiteral("caretFamily")).toString().trimmed();
    p.caretPt = m.value(QStringLiteral("caretPt")).toInt();
    p.found = m.value(QStringLiteral("found")).toBool();
    if (p.caretFamily.isEmpty())
        p.caretFamily = p.family;
    if (p.caretPt <= 0)
        p.caretPt = p.pt;
    return p;
}


static QString jsStringLiteral(const QString &value)
{
    const QByteArray json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

static bool htmlLooksEmpty(const QString &html)
{
    QString t = html;
    t.replace(QRegularExpression("<[^>]+>"), QStringLiteral(" "));
    t.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    t.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    return t.trimmed().isEmpty();
}

MonasteryEditor::MonasteryEditor(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    m_webView = new QWebEngineView(this);

    QWebEnginePage *page = m_webView->page();
    QWebEngineSettings *settings = page->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, true);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
    settings->setAttribute(QWebEngineSettings::PrintElementBackgrounds, false);

    m_webView->load(QUrl("qrc:/editor.html"));
    m_webView->setStyleSheet("QWebEngineView { background: #3C2F2F; border: none; }");

    layout->addWidget(m_webView);
    setLayout(layout);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(900);
    connect(m_pollTimer, &QTimer::timeout, this, &MonasteryEditor::pollEditorState);

    connect(page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        m_isLoaded = ok;
        if (ok) {
            m_webView->page()->runJavaScript(
                "var ed = document.getElementById('editor');"
                "if (ed && !ed.innerHTML.trim()) { ed.innerHTML = '<p></p>'; }"
            );
            m_pollTimer->start();
            pollEditorState();
        }
        emit ready(ok);
    });
}

void MonasteryEditor::considerHtmlCache(const QString &html)
{
    if (htmlLooksEmpty(html))
        return;
    m_cachedHtml = html;
}

void MonasteryEditor::pollEditorState()
{
    if (!m_isLoaded)
        return;

    m_webView->page()->runJavaScript("getWordCount();", [this](const QVariant &result) {
        const int count = result.toInt();
        if (count != m_cachedWordCount) {
            m_cachedWordCount = count;
            emit wordCountChanged(count);
        }
    });

    m_webView->page()->runJavaScript(
        "document.getElementById('editor') ? document.getElementById('editor').innerHTML : '';",
        [this](const QVariant &result) {
            considerHtmlCache(result.toString());
        });

    m_webView->page()->runJavaScript("!!window.monasteryDirty;", [this](const QVariant &result) {
        const bool dirty = result.toBool();
        if (dirty != m_dirty) {
            m_dirty = dirty;
            emit dirtyChanged(m_dirty);
        }
    });

    pollSelectionFont();
}

void MonasteryEditor::pollSelectionFont()
{
    if (!m_isLoaded)
        return;
    m_webView->page()->runJavaScript(
        QStringLiteral("typeof getSelectionFont==='function'?getSelectionFont():null"),
        [this](const QVariant &result) {
            const FontProbe p = parseFontProbe(result);
            if (p.family.isEmpty() && p.pt <= 0)
                return;
            if (p.family == m_cachedSelFamily && p.pt == m_cachedSelPt)
                return;
            m_cachedSelFamily = p.family;
            m_cachedSelPt = p.pt;
            emit selectionFontChanged(p.family, p.pt);
        });
}

void MonasteryEditor::requestHeadingFont(const std::function<void(const QString &, int,
                                                                 const QString &, int,
                                                                 bool)> &callback)
{
    if (!m_isLoaded) {
        if (callback)
            callback(QString(), 0, QString(), 0, false);
        return;
    }
    m_webView->page()->runJavaScript(
        QStringLiteral("typeof getHeadingFont==='function'?getHeadingFont():null"),
        [callback](const QVariant &result) {
            const FontProbe p = parseFontProbe(result);
            if (callback)
                callback(p.family, p.pt, p.caretFamily, p.caretPt, p.found);
        });
}


void MonasteryEditor::insertChecklist()
{
    if (!m_isLoaded) return;
    m_webView->page()->runJavaScript(QStringLiteral("insertChecklist();"));
    markDirty();
}

void MonasteryEditor::execCommand(const QString &cmd, const QString &value)
{
    if (!m_isLoaded) return;

    QString js;
    if (value.isEmpty()) {
        js = QString("execCommand(%1);").arg(jsStringLiteral(cmd));
    } else {
        js = QString("execCommand(%1, %2);").arg(jsStringLiteral(cmd), jsStringLiteral(value));
    }
    m_webView->page()->runJavaScript(js + "window.monasteryDirty = true;");
    markDirty();
}

QString MonasteryEditor::getHtml()
{
    if (m_isLoaded)
        fetchHtml([](const QString &) {});
    return m_cachedHtml;
}

void MonasteryEditor::fetchHtml(const std::function<void(const QString &)> &callback)
{
    if (!m_isLoaded) {
        if (callback)
            callback(m_cachedHtml);
        return;
    }

    m_webView->page()->runJavaScript(
        "document.getElementById('editor') ? document.getElementById('editor').innerHTML : '';",
        [this, callback](const QVariant &v) {
            const QString html = v.toString();
            considerHtmlCache(html);
            if (callback)
                callback(htmlLooksEmpty(html) ? m_cachedHtml : html);
        });
}

void MonasteryEditor::setHtml(const QString &html)
{
    m_cachedHtml = html;
    if (!m_isLoaded) {
        QTimer::singleShot(80, this, [this, html]() { setHtml(html); });
        return;
    }

    m_webView->page()->runJavaScript(QString("setContent(%1);").arg(jsStringLiteral(html)));
    QTimer::singleShot(80, this, [this]() { pollSelectionFont(); });
}

int MonasteryEditor::getWordCount()
{
    return m_cachedWordCount;
}

void MonasteryEditor::requestWordCount(const std::function<void(int)> &callback)
{
    if (!m_isLoaded) {
        if (callback)
            callback(m_cachedWordCount);
        return;
    }
    m_webView->page()->runJavaScript("getWordCount();", [this, callback](const QVariant &v) {
        m_cachedWordCount = v.toInt();
        if (callback)
            callback(m_cachedWordCount);
    });
}

bool MonasteryEditor::queryDirtyNow()
{
    if (!m_isLoaded)
        return m_dirty;

    bool result = m_dirty;
    QEventLoop loop;
    QTimer safety;
    safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);

    m_webView->page()->runJavaScript("!!window.monasteryDirty;", [&](const QVariant &v) {
        result = v.toBool();
        m_dirty = result;
        loop.quit();
    });

    safety.start(2000);
    loop.exec();
    return result;
}

void MonasteryEditor::markClean()
{
    m_dirty = false;
    if (m_isLoaded)
        m_webView->page()->runJavaScript("if (typeof markClean === 'function') markClean(); else window.monasteryDirty = false;");
    emit dirtyChanged(false);
}

void MonasteryEditor::markDirty()
{
    if (!m_dirty) {
        m_dirty = true;
        emit dirtyChanged(true);
    }
    if (m_isLoaded)
        m_webView->page()->runJavaScript("window.monasteryDirty = true;");
}

void MonasteryEditor::applyFontSize(int pointSize)
{
    if (!m_isLoaded) return;
    m_webView->page()->runJavaScript(QString("applyFontSize(%1);").arg(pointSize));
    markDirty();
}

void MonasteryEditor::applyFontFamily(const QString &name)
{
    if (!m_isLoaded) return;
    m_webView->page()->runJavaScript(QString("applyFontFamily(%1);").arg(jsStringLiteral(name)));
    markDirty();
}

void MonasteryEditor::refreshHighlighter()
{
    // Chromium native spellcheck (contenteditable spellcheck="true").
    // Hunspell remains linked for later dictionary work.
}

void MonasteryEditor::applyTheme(const Theme &t)
{
    const QString desk = t.desk.isEmpty() ? t.chromeBg : t.desk;
    m_webView->setStyleSheet(QStringLiteral("QWebEngineView { background: %1; border: none; }").arg(desk));

    if (!m_isLoaded) {
        QTimer::singleShot(80, this, [this, t]() { applyTheme(t); });
        return;
    }

    const QString js = QStringLiteral(
        "if (typeof applyTheme === 'function') applyTheme({"
        "id: %1, name: %2, pageBg: %3, pageText: %4, desk: %5,"
        "selectionBg: %6, selectionFg: %7, editorFont: %8, pageAsObject: %9"
        "});")
        .arg(jsStringLiteral(t.id),
             jsStringLiteral(t.name),
             jsStringLiteral(t.pageBg),
             jsStringLiteral(t.pageText),
             jsStringLiteral(t.desk),
             jsStringLiteral(t.selectionBg),
             jsStringLiteral(t.selectionFg),
             jsStringLiteral(t.editorFont),
             t.pageAsObject ? QStringLiteral("true") : QStringLiteral("false"));
    m_webView->page()->runJavaScript(js);
}
