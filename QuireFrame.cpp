#include "QuireFrame.h"
#include "MonasteryEditor.h"
#include "EpubWriter.h"
#include "DocumentIo.h"
#include "ScrivenerImport.h"

#include <algorithm>

#include <QAbstractProxyModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCollator>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QFile>
#include <QIODevice>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QFont>
#include <QFontDatabase>
#include <QFontComboBox>
#include <QComboBox>
#include <QCheckBox>
#include <QColor>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QEventLoop>
#include <QStandardPaths>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QWebEnginePage>
#include <QUrl>
#include <QWebEngineFindTextResult>
#include <QPrinter>
#include <QPrintDialog>
#include <QPrinterInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QSizeF>
#include <QMarginsF>
#include <QProcess>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QSignalBlocker>
#include <QShortcut>
#include <QXmlStreamReader>
#include <cstdio>

namespace {

const QString kQuireVersion = QStringLiteral("0.3.28");

QString canonicalOrAbs(const QString &path)
{
    const QFileInfo info(path);
    const QString c = info.canonicalFilePath();
    if (!c.isEmpty())
        return QDir::fromNativeSeparators(c);
    return QDir::fromNativeSeparators(QDir::cleanPath(info.absoluteFilePath()));
}

QString projectTitleFromInput(const QString &input)
{
    QString title = input.trimmed();
    if (title.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive))
        title.chop(3);
    return title.trimmed();
}

QString projectFolderName(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive))
        return trimmed;
    return trimmed + QStringLiteral(".qr");
}

bool pathEquals(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return canonicalOrAbs(a) == canonicalOrAbs(b);
}

bool pathIsUnder(const QString &path, const QString &root)
{
    const QString c = canonicalOrAbs(path);
    const QString r = canonicalOrAbs(root);
    if (c.isEmpty() || r.isEmpty())
        return false;
    return c == r || c.startsWith(r + QLatin1Char('/'));
}

class BinderProxy : public QSortFilterProxyModel {
public:
    explicit BinderProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    }

    QStringList *order = nullptr;
    QString manuscriptRoot;
    QString projectRoot;
    QString notesRoot;

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::DisplayRole && index.isValid()) {
            const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
            const QModelIndex src = mapToSource(index);
            if (fs && src.isValid()) {
                if (fs->isDir(src)) {
                    const QString path = fs->filePath(src);
                    if (pathEquals(path, manuscriptRoot))
                        return QStringLiteral("Manuscript");
                    if (pathEquals(path, notesRoot))
                        return QStringLiteral("Notes");
                } else {
                    const QString name = fs->fileName(src);
                    if (name.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
                        return name.left(name.size() - 5);
                }
            }
        }
        return QSortFilterProxyModel::data(index, role);
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        Qt::ItemFlags f = QSortFilterProxyModel::flags(index);
        const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
        const QModelIndex src = mapToSource(index);
        if (fs && src.isValid() && fs->isDir(src)) {
            const QString path = fs->filePath(src);
            if (pathEquals(path, manuscriptRoot) || pathEquals(path, notesRoot))
                f &= ~Qt::ItemIsEditable;
        }
        return f;
    }

    QString relativeOf(const QModelIndex &src) const
    {
        const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
        if (!fs || !src.isValid())
            return {};
        if (manuscriptRoot.isEmpty())
            return fs->fileName(src);
        const QString rel = QDir(manuscriptRoot).relativeFilePath(fs->filePath(src));
        if (rel == QLatin1String("."))
            return {};
        return QDir::fromNativeSeparators(rel);
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
        if (!fs || projectRoot.isEmpty())
            return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
        const QModelIndex idx = fs->index(sourceRow, 0, sourceParent);
        if (!idx.isValid())
            return false;
        const QString name = fs->fileName(idx);
        if (name.startsWith(QLatin1Char('.')))
            return false;
        if (!sourceParent.isValid())
            return true;
        const QString parentPath = fs->filePath(sourceParent);
        if (pathEquals(parentPath, projectRoot))
            return name == QLatin1String("manuscript") || name == QLatin1String("notes");
        return true;
    }

    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const auto *fs = qobject_cast<const QFileSystemModel *>(sourceModel());
        if (!fs)
            return QSortFilterProxyModel::lessThan(left, right);
        const QString relL = relativeOf(left);
        const QString relR = relativeOf(right);
        const int idxL = (order && !relL.isEmpty() && !relL.startsWith(QLatin1String("..")))
            ? order->indexOf(relL) : -1;
        const int idxR = (order && !relR.isEmpty() && !relR.startsWith(QLatin1String("..")))
            ? order->indexOf(relR) : -1;
        if (idxL >= 0 && idxR >= 0)
            return idxL < idxR;
        if (idxL >= 0 && idxR < 0)
            return true;
        if (idxL < 0 && idxR >= 0)
            return false;
        const bool leftDir = fs->isDir(left);
        const bool rightDir = fs->isDir(right);
        if (leftDir != rightDir)
            return leftDir;
        return m_collator.compare(fs->fileName(left), fs->fileName(right)) < 0;
    }

private:
    QCollator m_collator;
};

class BinderDelegate : public QStyledItemDelegate {
public:
    QStringList *exclude = nullptr;
    QString manuscriptRoot;
    using QStyledItemDelegate::QStyledItemDelegate;

    QString relativeOf(const QFileSystemModel *fs, const QModelIndex &src) const
    {
        if (!fs || !src.isValid() || manuscriptRoot.isEmpty())
            return {};
        const QString rel = QDir(manuscriptRoot).relativeFilePath(fs->filePath(src));
        if (rel == QLatin1String("."))
            return {};
        return QDir::fromNativeSeparators(rel);
    }

    bool excluded(const QModelIndex &index) const
    {
        if (!exclude || !index.isValid())
            return false;
        const auto *proxy = qobject_cast<const QAbstractProxyModel *>(index.model());
        const QModelIndex src = proxy ? proxy->mapToSource(index) : index;
        const auto *fs = qobject_cast<const QFileSystemModel *>(src.model());
        QString rel = relativeOf(fs, src);
        while (!rel.isEmpty()) {
            if (exclude->contains(rel))
                return true;
            const int slash = rel.lastIndexOf(QLatin1Char('/'));
            rel = (slash < 0) ? QString() : rel.left(slash);
        }
        return false;
    }

    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (excluded(index))
            option->font.setItalic(true);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (excluded(index)) {
            painter->save();
            painter->setOpacity(0.55);
            QStyledItemDelegate::paint(painter, option, index);
            painter->restore();
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
};


const QString kOrg = QStringLiteral("Sociopathletic");
const QString kApp = QStringLiteral("Quire");

bool looksLikeHtmlDocument(const QString &html)
{
    const QString t = html.trimmed().left(200).toLower();
    return t.startsWith(QLatin1String("<!doctype")) || t.startsWith(QLatin1String("<html"));
}

QString sceneBody(const QString &html)
{
    if (!looksLikeHtmlDocument(html))
        return html;
    const int start = html.indexOf(QLatin1String("<body"), 0, Qt::CaseInsensitive);
    if (start < 0)
        return html;
    const int gt = html.indexOf(QLatin1Char('>'), start);
    if (gt < 0)
        return html;
    const int end = html.indexOf(QLatin1String("</body>"), gt, Qt::CaseInsensitive);
    if (end < 0)
        return html.mid(gt + 1);
    return html.mid(gt + 1, end - (gt + 1));
}

int countWordsInHtml(const QString &html)
{
    QString t = sceneBody(html);
    static const QRegularExpression commentRe(QStringLiteral("<!--.*?-->"),
                                              QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    t.remove(commentRe);
    t.replace(tagRe, QStringLiteral(" "));
    t.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
    t.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    t = t.simplified();
    if (t.isEmpty())
        return 0;
    return t.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
}

QString htmlSearchText(const QString &html)
{
    QString t = sceneBody(html);
    static const QRegularExpression commentRe(QStringLiteral("<!--.*?-->"),
                                              QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    t.remove(commentRe);
    t.replace(tagRe, QStringLiteral(" "));
    t.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
    t.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    t.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
    t.replace(QStringLiteral("&lt;"), QStringLiteral("<"), Qt::CaseInsensitive);
    t.replace(QStringLiteral("&gt;"), QStringLiteral(">"), Qt::CaseInsensitive);
    t.replace(QStringLiteral("&quot;"), QString(QLatin1Char('"')), Qt::CaseInsensitive);
    return t;
}

bool htmlContainsNeedle(const QString &html, const QString &needle)
{
    if (needle.isEmpty())
        return false;
    return htmlSearchText(html).contains(needle, Qt::CaseInsensitive);
}

const QString kFindSelOffsetJs = QStringLiteral(
    "(function(){"
    "var ed=document.getElementById('editor');"
    "if(!ed)return -1;"
    "var sel=window.getSelection();"
    "if(!sel||!sel.rangeCount)return 0;"
    "var r=sel.getRangeAt(0);"
    "var pre=document.createRange();"
    "pre.selectNodeContents(ed);"
    "try{pre.setEnd(r.startContainer,r.startOffset);}catch(e){return 0;}"
    "return pre.toString().length;"
    "})()");

int runJsInt(QWebEnginePage *page, const QString &js, int fallback = 0)
{
    if (!page)
        return fallback;
    int result = fallback;
    bool done = false;
    QEventLoop loop;
    QTimer safety;
    safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->runJavaScript(js, [&](const QVariant &v) {
        result = v.toInt();
        done = true;
        loop.quit();
    });
    if (!done) {
        safety.start(1500);
        loop.exec();
    }
    return result;
}

enum class MatterKind { Front, Middle, Back };

bool nameEqualsAny(const QString &name, const QStringList &names)
{
    for (const QString &n : names) {
        if (name.compare(n, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

MatterKind matterKindForName(const QString &name)
{
    static const QStringList front{
        QStringLiteral("Front Matter"),
        QStringLiteral("Frontmatter"),
        QStringLiteral("Title"),
        QStringLiteral("Title Page"),
        QStringLiteral("Dedication"),
        QStringLiteral("Copyright"),
        QStringLiteral("Copyright Page"),
        QStringLiteral("Preface"),
        QStringLiteral("Intro"),
        QStringLiteral("Introduction"),
    };
    static const QStringList back{
        QStringLiteral("Back Matter"),
        QStringLiteral("Appendix"),
        QStringLiteral("Epilogue"),
        QStringLiteral("Afterword"),
    };
    if (nameEqualsAny(name, front))
        return MatterKind::Front;
    if (nameEqualsAny(name, back))
        return MatterKind::Back;
    return MatterKind::Middle;
}

void annotateCompileScene(EpubWriter::Scene *sc, QString *lastChapter)
{
    if (!sc->folderTrail.isEmpty())
        sc->chapterTitle = sc->folderTrail.first();
    else if (!sc->title.isEmpty())
        sc->chapterTitle = sc->title;
    if (!sc->chapterTitle.isEmpty())
        sc->frontMatter = (matterKindForName(sc->chapterTitle) == MatterKind::Front);
    else
        sc->frontMatter = (matterKindForName(sc->title) == MatterKind::Front);
    if (sc->frontMatter)
        return;
    if (!sc->chapterTitle.isEmpty() && sc->chapterTitle != *lastChapter) {
        sc->startChapter = true;
        *lastChapter = sc->chapterTitle;
    }
}


QString gelasioPrintFontFaceCss(const QString &urlPrefix = QStringLiteral("fonts/gelasio/"))
{
    const QString prefix = urlPrefix.isEmpty()
                               ? QStringLiteral("fonts/gelasio/")
                               : (urlPrefix.endsWith(QLatin1Char('/'))
                                      ? urlPrefix
                                      : urlPrefix + QLatin1Char('/'));
    auto faceUrl = [&](const QString &fileName) -> QString {
        return prefix + fileName;
    };
    const QString regular = faceUrl(QStringLiteral("Gelasio-Regular.ttf"));
    const QString italic = faceUrl(QStringLiteral("Gelasio-Italic.ttf"));
    const QString bold = faceUrl(QStringLiteral("Gelasio-Bold.ttf"));
    const QString boldItalic = faceUrl(QStringLiteral("Gelasio-BoldItalic.ttf"));
    QString css;
    auto face = [&](const QString &family, const QString &url, const char *weight, const char *style) {
        css += QStringLiteral(
            "@font-face {\n"
            "  font-family: \"%1\";\n"
            "  src: url(\"%2\") format(\"truetype\");\n"
            "  font-weight: %3;\n"
            "  font-style: %4;\n"
            "}\n").arg(family, url, QLatin1String(weight), QLatin1String(style));
    };
    // Shared (File→Print): one family, four faces — Chromium print path may still
    // pick Italic for body; paperback uses gelasioPaperbackFontFaceCss instead.
    face(QStringLiteral("Gelasio"), regular, "400", "normal");
    face(QStringLiteral("Gelasio"), italic, "400", "italic");
    face(QStringLiteral("Gelasio"), bold, "700", "normal");
    face(QStringLiteral("Gelasio"), boldItalic, "700", "italic");
    face(QStringLiteral("Georgia"), regular, "400", "normal");
    face(QStringLiteral("Georgia"), italic, "400", "italic");
    face(QStringLiteral("Georgia"), bold, "700", "normal");
    face(QStringLiteral("Georgia"), boldItalic, "700", "italic");
    return css;
}

// Distinct families so Chromium/WebEngine printToPdf cannot paint body with Italic.
QString gelasioPaperbackFontFaceCss(const QString &urlPrefix = QStringLiteral("fonts/gelasio/"))
{
    const QString prefix = urlPrefix.isEmpty()
                               ? QStringLiteral("fonts/gelasio/")
                               : (urlPrefix.endsWith(QLatin1Char('/'))
                                      ? urlPrefix
                                      : urlPrefix + QLatin1Char('/'));
    auto faceUrl = [&](const QString &fileName) -> QString {
        return prefix + fileName;
    };
    const QString regular = faceUrl(QStringLiteral("Gelasio-Regular.ttf"));
    const QString italic = faceUrl(QStringLiteral("Gelasio-Italic.ttf"));
    const QString bold = faceUrl(QStringLiteral("Gelasio-Bold.ttf"));
    const QString boldItalic = faceUrl(QStringLiteral("Gelasio-BoldItalic.ttf"));
    QString css;
    auto face = [&](const QString &family, const QString &url, const char *weight, const char *style) {
        css += QStringLiteral(
            "@font-face {\n"
            "  font-family: \"%1\";\n"
            "  src: url(\"%2\") format(\"truetype\");\n"
            "  font-weight: %3;\n"
            "  font-style: %4;\n"
            "}\n").arg(family, url, QLatin1String(weight), QLatin1String(style));
    };
    // Unique family names (not "Gelasio") so fontconfig's multi-face Gelasio cannot
    // win over @font-face. font-style:normal on every face — italic/bold glyphs are
    // already in the TTF; asking for style:italic restarts Chromium's bad matcher.
    face(QStringLiteral("QuireRoman"), regular, "400", "normal");
    face(QStringLiteral("QuireItalic"), italic, "400", "normal");
    face(QStringLiteral("QuireBold"), bold, "700", "normal");
    face(QStringLiteral("QuireBoldItalic"), boldItalic, "700", "normal");
    return css;
}

bool copyGelasioFontsToDir(const QString &fontOutDir, QString *error)
{
    if (!QDir().mkpath(fontOutDir)) {
        if (error)
            *error = QStringLiteral("Could not create font directory: %1").arg(fontOutDir);
        return false;
    }
    const QStringList gelasioFaces = {
        QStringLiteral("Gelasio-Regular.ttf"),
        QStringLiteral("Gelasio-Italic.ttf"),
        QStringLiteral("Gelasio-Bold.ttf"),
        QStringLiteral("Gelasio-BoldItalic.ttf"),
        QStringLiteral("OFL.txt"),
    };
    for (const QString &name : gelasioFaces) {
        const QString dest = fontOutDir + QLatin1Char('/') + name;
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Failed writing font %1.").arg(name);
            return false;
        }
        QFile in(QStringLiteral(":/fonts/gelasio/") + name);
        if (!in.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Missing bundled font resource: %1").arg(name);
            return false;
        }
        out.write(in.readAll());
    }
    return true;
}

QPageLayout defaultPrintPageLayout()
{
    return QPageLayout(QPageSize(QPageSize::Letter),
                       QPageLayout::Portrait,
                       QMarginsF(0.75, 0.75, 0.75, 0.75),
                       QPageLayout::Inch);
}

QPageLayout paperbackPdfPageLayout(double leftIn, double rightIn)
{
    // 5×8in trim, no bleed. QMarginsF(left, top, right, bottom).
    // Top/bottom lock 0.75in measured ink. Outside 0.5; inside/gutter from caller.
    // WebEngine printToPdf + Ghostscript pdfwrite paint first-line ink ~4.3pt into the
    // nominal top margin on 5×8; bump layout top so pdftotext yMin lands ≈54pt (0.75in).
    // Bottom stays 0.75 (GS shift only adds slack below). True mirror via recto/verso
    // QPageLayouts + pdfunite in writePaperbackPdfFile (CSS @page :left/:right ignored).
    constexpr double kTopBiasPt = 4.6; // measured 54 - ~49.7 on 0.3.26; 4.3→4.6 after 0.746in verify
    const double topIn = 0.75 + kTopBiasPt / 72.0;
    const QPageSize trim(QSizeF(5.0, 8.0), QPageSize::Inch,
                         QStringLiteral("Quire 5x8"));
    return QPageLayout(trim,
                       QPageLayout::Portrait,
                       QMarginsF(leftIn, topIn, rightIn, 0.75),
                       QPageLayout::Inch);
}

QPageLayout pageLayoutFromPrinter(const QPrinter &printer)
{
    const QPageLayout layout = printer.pageLayout();
    return layout.isValid() ? layout : defaultPrintPageLayout();
}

QStringList cupsLpArgv(const QString &printerName, int copies, const QString &pdfPath)
{
    QStringList args;
    if (!printerName.isEmpty())
        args << QStringLiteral("-d") << printerName;
    if (copies > 1)
        args << QStringLiteral("-n") << QString::number(copies);
    args << pdfPath;
    return args;
}

void showFamilyInCombo(QFontComboBox *combo, const QString &family)
{
    if (!combo || family.isEmpty())
        return;
    QSignalBlocker block(combo);
    QString pick = family;
    if (QString::compare(family, QStringLiteral("Georgia"), Qt::CaseInsensitive) == 0
        && !QFontDatabase::hasFamily(QStringLiteral("Georgia"))
        && QFontDatabase::hasFamily(QStringLiteral("Gelasio"))) {
        pick = QStringLiteral("Gelasio");
    }
    combo->setCurrentFont(QFont(pick));
    if (QString::compare(combo->currentText(), family, Qt::CaseInsensitive) == 0)
        return;
    if (!combo->isEditable())
        combo->setEditable(true);
    // Keep Scrivener's family name in the closed field when we mapped Georgia → Gelasio.
    combo->setEditText(family);
    if (QString::compare(combo->currentText(), family, Qt::CaseInsensitive) != 0)
        combo->setCurrentText(family);
}

void showSizeInCombo(QComboBox *combo, int pt)
{
    if (!combo || pt <= 0)
        return;
    QSignalBlocker block(combo);
    const QString s = QString::number(pt);
    if (combo->findText(s) < 0) {
        int i = 0;
        for (; i < combo->count(); ++i) {
            if (combo->itemText(i).toInt() > pt)
                break;
        }
        combo->insertItem(i, s);
    }
    combo->setCurrentText(s);
}


QString flattenKindleBody(const QString &html)
{
    QString s = EpubWriter::sanitizeBody(html);
    static const QRegularExpression openH(QStringLiteral("<h[1-6][^>]*>"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression closeH(QStringLiteral("</h[1-6]>"),
                                           QRegularExpression::CaseInsensitiveOption);
    s.replace(openH, QStringLiteral("<p>"));
    s.replace(closeH, QStringLiteral("</p>"));
    return s.trimmed();
}

bool isKindleTitlePageScene(const EpubWriter::Scene &sc)
{
    return sc.title.compare(QStringLiteral("Title Page"), Qt::CaseInsensitive) == 0;
}

// After chapter H1: mark location/date <p><em>…</em></p> lines and the first
// story paragraph with class chapter-open so text-indent stays 0 (h1+p alone
// only clears the first sibling; Prologue/Ch25 put date + story after that).
QString markPaperbackChapterOpenParas(QString body)
{
    static const QRegularExpression pRe(
        QStringLiteral(R"~(^\s*<p(\b[^>]*)>([\s\S]*?)</p>)~"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    static const QRegularExpression emOnlyRe(
        QStringLiteral(R"~(^\s*<(em|i)\b[^>]*>[\s\S]*?</\1>\s*$)~"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression classRe(
        QStringLiteral(R"~(class\s*=\s*"([^"]*)")~"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    for (int n = 0; n < 8; ++n) {
        const QRegularExpressionMatch m = pRe.match(body);
        if (!m.hasMatch() || m.capturedStart() != 0)
            break;
        QString attrs = m.captured(1);
        const QString inner = m.captured(2);
        QString plain = inner;
        plain.remove(tagRe);
        plain.replace(QLatin1String("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
        plain.replace(QLatin1String("&#160;"), QStringLiteral(" "));
        plain = plain.trimmed();
        const bool emOnly = emOnlyRe.match(inner).hasMatch();
        const bool locationLike = plain.isEmpty() || (emOnly && plain.size() <= 64);
        if (!attrs.contains(QStringLiteral("chapter-open"), Qt::CaseInsensitive)) {
            const QRegularExpressionMatch cm = classRe.match(attrs);
            if (cm.hasMatch()) {
                attrs.replace(cm.capturedStart(1), cm.capturedLength(1),
                              cm.captured(1) + QStringLiteral(" chapter-open"));
            } else {
                attrs += QStringLiteral(" class=\"chapter-open\"");
            }
        }
        out += QStringLiteral("<p%1>%2</p>").arg(attrs, inner);
        body = body.mid(m.capturedLength());
        if (!locationLike)
            break;
    }
    return out + body;
}

QString buildPaperbackPdfHtml(const QString &bookTitle,
                              const QVector<EpubWriter::Scene> &scenes,
                              double gutterIn = 0.875,
                              bool frontOnly = false)
{
    Q_UNUSED(gutterIn); // gutter applied via QPageLayout in writePaperbackPdfFile (true mirror)
    QString parts;
    parts += QStringLiteral(
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>%1</title>\n"
        "<style>\n"
        "%2"
        "@page { size: 5in 8in; margin: 0; }\n"
        "html, body, p, div, span, li, td, th {\n"
        "  font-family: QuireRoman, \"Times New Roman\", serif;\n"
        "  font-style: normal !important;\n"
        "  font-weight: 400;\n"
        "  font-size: 10.5pt;\n"
        "  line-height: 1.25;\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "  color: #000;\n"
        "  background: #fff;\n"
        "}\n"
        "em, i {\n"
        "  font-family: QuireItalic, QuireRoman, serif !important;\n"
        "  font-style: normal !important;\n"
        "  font-weight: 400;\n"
        "}\n"
        "strong, b {\n"
        "  font-family: QuireBold, QuireRoman, serif !important;\n"
        "  font-style: normal !important;\n"
        "  font-weight: 700;\n"
        "}\n"
        "em strong, strong em, i b, b i {\n"
        "  font-family: QuireBoldItalic, QuireBold, QuireItalic, QuireRoman, serif !important;\n"
        "  font-style: normal !important;\n"
        "  font-weight: 700;\n"
        "}\n"
        "h1, h1.chapter {\n"
        "  font-family: QuireBold, QuireRoman, \"Times New Roman\", serif;\n"
        "  font-size: 13pt;\n"
        "  font-weight: 700;\n"
        "  font-style: normal !important;\n"
        "  text-align: center;\n"
        "  margin: 0 0 1.1em 0;\n"
        "}\n"
        "h1.chapter {\n"
        "  page-break-before: always;\n"
        "  break-before: page;\n"
        "}\n"
        "h2 {\n"
        "  font-family: QuireBold, QuireRoman, serif;\n"
        "  font-size: 11.5pt;\n"
        "  font-weight: 700;\n"
        "  font-style: normal !important;\n"
        "  text-align: center;\n"
        "  margin: 1em 0 0.7em 0;\n"
        "}\n"
        "p {\n"
        "  margin: 0 0 0.55em 0;\n"
        "  text-indent: 1.2em;\n"
        "  text-align: justify;\n"
        "}\n"
        "p:first-of-type,\n"
        "h1 + p,\n"
        "h2 + p,\n"
        ".scene-break + p,\n"
        "p.scene-break,\n"
        "p.chapter-open,\n"
        "p[style*=\"text-align:center\"],\n"
        "p[style*=\"text-align: center\"],\n"
        "div[style*=\"text-align:center\"],\n"
        "div[style*=\"text-align: center\"] {\n"
        "  text-indent: 0;\n"
        "}\n"
        "p.scene-break {\n"
        "  text-align: center;\n"
        "  margin: 0.85em 0;\n"
        "}\n"
        ".page-break, .pagebreak {\n"
        "  break-after: page;\n"
        "  page-break-after: always;\n"
        "  border: none;\n"
        "  margin: 0;\n"
        "  height: 0;\n"
        "}\n"
        ".blank-page {\n"
        "  min-height: 1px;\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "  border: none;\n"
        "  color: transparent;\n"
        "  visibility: hidden;\n"
        "}\n"
        "section.compile-scene { display: block; }\n"
        "@media print {\n"
        "  html, body { color: #000 !important; background: #fff !important; }\n"
        "}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n")
        .arg(bookTitle.toHtmlEscaped(),
             gelasioPaperbackFontFaceCss());

    static const QRegularExpression emptyEm(
        QStringLiteral("<(em|i)(?:\\s[^>]*)?>\\s*</\\1>"),
        QRegularExpression::CaseInsensitiveOption);

    const QString blankAfterDedication = QStringLiteral(
        "<section class=\"compile-scene blank-leaf\" aria-hidden=\"true\">\n"
        "<div class=\"page-break\"></div>\n"
        "<div class=\"blank-page\">&nbsp;</div>\n"
        "<!-- blank leaf: next h1.chapter page-break-before starts body on following recto -->\n"
        "</section>\n");

    bool needSceneBreak = false;
    bool pendingBlankAfterFront = false;
    bool insertedBlankAfterFront = false;
    auto insertBlankLeaf = [&]() {
        if (insertedBlankAfterFront)
            return;
        parts += blankAfterDedication;
        insertedBlankAfterFront = true;
        pendingBlankAfterFront = false;
    };

    for (const EpubWriter::Scene &sc : scenes) {
        if (frontOnly && !sc.frontMatter)
            continue;
        // After dedication / last Intro front matter: one blank leaf so body PN1 is recto.
        if (!sc.frontMatter && pendingBlankAfterFront)
            insertBlankLeaf();
        QString inner = EpubWriter::headingHtml(sc);
        QString body = EpubWriter::sanitizeBody(sc.bodyHtml);
        body.remove(emptyEm);
        // Drop empty shells left after stripping empty <em></em>.
        static const QRegularExpression emptyP(
            QStringLiteral("<p\\b[^>]*>\\s*</p>"),
            QRegularExpression::CaseInsensitiveOption);
        body.remove(emptyP);
        if (sc.startChapter && !sc.chapterTitle.isEmpty())
            body = markPaperbackChapterOpenParas(body);
        const bool empty = EpubWriter::isVisuallyEmpty(body);
        if (sc.startChapter && !sc.chapterTitle.isEmpty())
            needSceneBreak = false;
        else if (needSceneBreak && !empty && !sc.frontMatter) {
            if (!inner.isEmpty())
                inner += QLatin1Char('\n');
            inner += QStringLiteral("<p class=\"scene-break\">#</p>");
        }
        if (!inner.isEmpty() && !body.isEmpty())
            inner += QLatin1Char('\n');
        inner += body;
        if (inner.trimmed().isEmpty())
            continue;
        parts += QStringLiteral("<section class=\"compile-scene\">\n%1\n</section>\n")
                     .arg(inner);
        if (sc.frontMatter)
            pendingBlankAfterFront = true;
        if (!empty && !sc.frontMatter)
            needSceneBreak = true;
    }
    // frontOnly probe must count the blank so Ghostscript skips PN on it.
    if (pendingBlankAfterFront)
        insertBlankLeaf();
    parts += QStringLiteral("</body></html>\n");
    return parts;
}

bool writeTextFilePath(const QString &path, const QString &text)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = text.toUtf8();
    return f.write(bytes) == bytes.size();
}

bool printWebEnginePdf(QWebEnginePage *page, const QString &pdfPath,
                       const QPageLayout &layout, QString *error)
{
    QFile::remove(pdfPath);
    QEventLoop loop;
    bool pdfDone = false;
    bool pdfSuccess = false;
    QObject::connect(page, &QWebEnginePage::pdfPrintingFinished, &loop,
                     [&](const QString &path, bool success) {
        Q_UNUSED(path);
        pdfDone = true;
        pdfSuccess = success;
        loop.quit();
    });
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    page->printToPdf(pdfPath, layout);
    loop.exec();
    const qint64 bytes = QFileInfo(pdfPath).size();
    if (!pdfDone || !pdfSuccess || bytes < 10000) {
        if (error)
            *error = QStringLiteral("Paperback PDF: printToPdf failed (bytes=%1).")
                         .arg(bytes);
        return false;
    }
    return true;
}

int pdfPageCountWithPdfinfo(const QString &pdfPath)
{
    QProcess proc;
    proc.start(QStringLiteral("pdfinfo"), QStringList{pdfPath});
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0)
        return -1;
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    static const QRegularExpression re(QStringLiteral("^Pages:\\s*(\\d+)"),
                                       QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(out);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

bool mergeMirroredPaperbackPdf(const QString &rectoPdf, const QString &versoPdf,
                               const QString &outPdf, QString *error)
{
    // Odd pages from recto (gutter left); even pages from verso (gutter right).
    const int pages = pdfPageCountWithPdfinfo(rectoPdf);
    const int pagesV = pdfPageCountWithPdfinfo(versoPdf);
    if (pages < 1 || pages != pagesV) {
        if (error)
            *error = QStringLiteral("Paperback PDF: recto/verso page count mismatch (%1 vs %2).")
                         .arg(pages).arg(pagesV);
        return false;
    }

    QTemporaryDir parts;
    if (!parts.isValid()) {
        if (error)
            *error = QStringLiteral("Paperback PDF: could not stage page parts.");
        return false;
    }
    const QString rectoSep = parts.filePath(QStringLiteral("r-%04d.pdf"));
    const QString versoSep = parts.filePath(QStringLiteral("v-%04d.pdf"));

    {
        QProcess sep;
        sep.start(QStringLiteral("pdfseparate"), QStringList{rectoPdf, rectoSep});
        if (!sep.waitForFinished(120000) || sep.exitCode() != 0) {
            if (error)
                *error = QStringLiteral("Paperback PDF: pdfseparate recto failed.");
            return false;
        }
    }
    {
        QProcess sep;
        sep.start(QStringLiteral("pdfseparate"), QStringList{versoPdf, versoSep});
        if (!sep.waitForFinished(120000) || sep.exitCode() != 0) {
            if (error)
                *error = QStringLiteral("Paperback PDF: pdfseparate verso failed.");
            return false;
        }
    }

    QStringList uniteArgs;
    for (int i = 1; i <= pages; ++i) {
        const QString leaf = (i % 2 == 1)
                                 ? QStringLiteral("r-%1.pdf")
                                 : QStringLiteral("v-%1.pdf");
        uniteArgs << parts.filePath(leaf.arg(i, 4, 10, QLatin1Char('0')));
        if (!QFileInfo::exists(uniteArgs.last())) {
            if (error)
                *error = QStringLiteral("Paperback PDF: missing separated page %1.").arg(i);
            return false;
        }
    }
    uniteArgs << outPdf;
    QFile::remove(outPdf);
    QProcess unite;
    unite.start(QStringLiteral("pdfunite"), uniteArgs);
    if (!unite.waitForFinished(180000) || unite.exitCode() != 0 || QFileInfo(outPdf).size() < 10000) {
        if (error)
            *error = QStringLiteral("Paperback PDF: pdfunite mirror merge failed.");
        return false;
    }
    return true;
}

bool stampPaperbackPageNumbers(const QString &inPdf, const QString &outPdf,
                               int frontMatterPages, QString *error)
{
    // WebEngine ignores CSS @page @bottom-center; stamp arabic numbers via Ghostscript.
    // EndPage count is 0-based. Front matter pages stay unnumbered; body starts at 1.
    if (frontMatterPages < 0)
        frontMatterPages = 0;
    QTemporaryDir stage;
    if (!stage.isValid()) {
        if (error)
            *error = QStringLiteral("Paperback PDF: could not stage page-number stamp.");
        return false;
    }
    const QString psPath = stage.filePath(QStringLiteral("stamp_pages.ps"));
    const QString ps = QStringLiteral(
        "/frontPages %1 def\n"
        "<<\n"
        "/EndPage {\n"
        "  exch /pg exch def\n"
        "  /reason exch def\n"
        "  reason 2 eq {\n"
        "    pop false\n"
        "  } {\n"
        "    pg frontPages lt {\n"
        "      true\n"
        "    } {\n"
        "      gsave\n"
        "        /Times-Roman findfont 9 scalefont setfont\n"
        "        0 setgray\n"
        "        /n pg frontPages sub 1 add def\n"
        "        n 10 string cvs /s exch def\n"
        "        s stringwidth pop 360 exch sub 2 div\n"
        "        28 moveto\n"
        "        s show\n"
        "      grestore\n"
        "      true\n"
        "    } ifelse\n"
        "  } ifelse\n"
        "} bind\n"
        ">> setpagedevice\n").arg(frontMatterPages);
    if (!writeTextFilePath(psPath, ps)) {
        if (error)
            *error = QStringLiteral("Paperback PDF: could not write page-number stamp script.");
        return false;
    }
    QFile::remove(outPdf);
    QProcess gs;
    gs.start(QStringLiteral("gs"), QStringList{
        QStringLiteral("-dBATCH"),
        QStringLiteral("-dNOPAUSE"),
        QStringLiteral("-dQUIET"),
        QStringLiteral("-sDEVICE=pdfwrite"),
        QStringLiteral("-dCompatibilityLevel=1.4"),
        QStringLiteral("-dPDFSETTINGS=/printer"),
        QStringLiteral("-sOutputFile=") + outPdf,
        psPath,
        inPdf,
    });
    if (!gs.waitForFinished(300000) || gs.exitCode() != 0
        || QFileInfo(outPdf).size() < 10000) {
        if (error)
            *error = QStringLiteral("Paperback PDF: Ghostscript page-number stamp failed.");
        return false;
    }
    return true;
}

bool writePaperbackPdfFile(const QString &pdfPath, const QString &html, QString *error,
                           double gutterIn = 0.875,
                           const QString &frontHtml = QString())
{
    QTemporaryDir stage;
    if (!stage.isValid()) {
        if (error)
            *error = QStringLiteral("Could not create temp dir for paperback PDF.");
        return false;
    }
    QString fontErr;
    if (!copyGelasioFontsToDir(stage.filePath(QStringLiteral("fonts/gelasio")), &fontErr)) {
        if (error)
            *error = fontErr.isEmpty()
                         ? QStringLiteral("Could not stage Gelasio fonts for PDF.")
                         : fontErr;
        return false;
    }
    const QString htmlPath = stage.filePath(QStringLiteral("print.html"));
    if (!writeTextFilePath(htmlPath, html)) {
        if (error)
            *error = QStringLiteral("Could not write paperback PDF HTML.");
        return false;
    }

    QDir().mkpath(QFileInfo(pdfPath).absolutePath());
    QFile::remove(pdfPath);

    const double outsideIn = 0.5;
    // Print-perfect: inside/gutter 0.875in (locked; no auto-bump).
    const double gutter = gutterIn > 0.0 ? gutterIn : 0.875;

    // Avoid AppImage FONTCONFIG Gelasio (all four faces) winning over @font-face.
    qputenv("FONTCONFIG_FILE", QByteArray());
    qunsetenv("FONTCONFIG_PATH");

    auto loadHtml = [](QWebEnginePage *page, const QString &path, QString *err) -> bool {
        QEventLoop loop;
        bool loadOk = false;
        QObject::connect(page, &QWebEnginePage::loadFinished, &loop,
                         [&](bool ok) {
            loadOk = ok;
            loop.quit();
        });
        QTimer::singleShot(180000, &loop, &QEventLoop::quit);
        page->load(QUrl::fromLocalFile(path));
        loop.exec();
        if (!loadOk) {
            if (err)
                *err = QStringLiteral("Paperback PDF: failed to load staged HTML.");
            return false;
        }
        return true;
    };

    int frontPages = 0;
    if (!frontHtml.trimmed().isEmpty()) {
        const QString frontPath = stage.filePath(QStringLiteral("front.html"));
        if (!writeTextFilePath(frontPath, frontHtml)) {
            if (error)
                *error = QStringLiteral("Could not write front-matter probe HTML.");
            return false;
        }
        QWebEnginePage frontPage;
        QString frontErr;
        if (!loadHtml(&frontPage, frontPath, &frontErr)) {
            if (error)
                *error = frontErr;
            return false;
        }
        const QString frontPdf = stage.filePath(QStringLiteral("front.pdf"));
        if (!printWebEnginePdf(&frontPage, frontPdf,
                               paperbackPdfPageLayout(gutter, outsideIn), error))
            return false;
        frontPages = pdfPageCountWithPdfinfo(frontPdf);
        if (frontPages < 0)
            frontPages = 0;
    }

    QWebEnginePage page;
    QString loadErr;
    if (!loadHtml(&page, htmlPath, &loadErr)) {
        if (error)
            *error = loadErr;
        return false;
    }

    const QString rectoPath = stage.filePath(QStringLiteral("recto.pdf"));
    const QString versoPath = stage.filePath(QStringLiteral("verso.pdf"));
    const QString mergedPath = stage.filePath(QStringLiteral("merged.pdf"));

    // Odd (recto): gutter left / outside right. Even (verso): outside left / gutter right.
    QString printErr;
    if (!printWebEnginePdf(&page, rectoPath, paperbackPdfPageLayout(gutter, outsideIn), &printErr)) {
        if (error)
            *error = printErr;
        return false;
    }
    if (!printWebEnginePdf(&page, versoPath, paperbackPdfPageLayout(outsideIn, gutter), &printErr)) {
        if (error)
            *error = printErr;
        return false;
    }
    if (!mergeMirroredPaperbackPdf(rectoPath, versoPath, mergedPath, error))
        return false;

    // pdfseparate/pdfunite duplicates font subsets per page — recompress with Ghostscript.
    const QString compactPath = stage.filePath(QStringLiteral("compact.pdf"));
    QProcess gs;
    gs.start(QStringLiteral("gs"), QStringList{
        QStringLiteral("-sDEVICE=pdfwrite"),
        QStringLiteral("-dCompatibilityLevel=1.4"),
        QStringLiteral("-dPDFSETTINGS=/printer"),
        QStringLiteral("-dNOPAUSE"),
        QStringLiteral("-dQUIET"),
        QStringLiteral("-dBATCH"),
        QStringLiteral("-sOutputFile=") + compactPath,
        mergedPath,
    });
    const bool gsOk = gs.waitForFinished(300000) && gs.exitCode() == 0
                      && QFileInfo(compactPath).size() > 10000;
    const QString compactSrc = gsOk ? compactPath : mergedPath;

    const QString numberedPath = stage.filePath(QStringLiteral("numbered.pdf"));
    if (!stampPaperbackPageNumbers(compactSrc, numberedPath, frontPages, error))
        return false;

    QFile::remove(pdfPath);
    if (!QFile::copy(numberedPath, pdfPath)) {
        if (error)
            *error = QStringLiteral("Paperback PDF: could not write final PDF.");
        return false;
    }
    return true;
}


QString kindleCompileHtml(const QString &title, const QString &author,
                          const QVector<EpubWriter::Scene> &scenes)
{
    QString o;
    o += QStringLiteral("<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>\n");
    o += QStringLiteral("<p class=\"kindle-title\">%1</p>\n").arg(title.toHtmlEscaped());
    o += QStringLiteral("<p class=\"kindle-author\">%1</p>\n").arg(author.toHtmlEscaped());
    o += QStringLiteral("<div class=\"pagebreak\"></div>\n");

    bool needSceneBreak = false;
    for (const EpubWriter::Scene &sc : scenes) {
        if (isKindleTitlePageScene(sc))
            continue;
        const QString body = flattenKindleBody(sc.bodyHtml);
        const bool empty = EpubWriter::isVisuallyEmpty(body);
        if (sc.startChapter && !sc.chapterTitle.isEmpty()) {
            o += QStringLiteral("<div class=\"pagebreak\"></div>\n");
            o += QStringLiteral("<h1>%1</h1>\n").arg(sc.chapterTitle.toHtmlEscaped());
            needSceneBreak = false;
        } else if (needSceneBreak && !empty) {
            o += QStringLiteral("<p class=\"scene-break\">#</p>\n");
        }
        if (empty)
            continue;
        o += body;
        o += QLatin1Char('\n');
        needSceneBreak = true;
    }
    o += QStringLiteral("</body></html>\n");
    return o;
}

} // namespace

QuireFrame::QuireFrame(QWidget *parent)
    : QMainWindow(parent)
    , m_currentTheme(themeForId(ThemeId::Leather))
{
    setWindowTitle(QStringLiteral("Quire"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/monastery.png")));
    resize(1280, 820);

    createActions();
    createMenus();
    createToolBar();
    createStatusBar();

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setReadOnly(false);
    m_fileModel->setNameFilters({QStringLiteral("*.html")});
    m_fileModel->setNameFilterDisables(false);
    m_fileModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);

    m_sortProxy = new BinderProxy(this);
    if (auto *bp = static_cast<BinderProxy *>(m_sortProxy))
        bp->order = &m_order;
    m_sortProxy->setSourceModel(m_fileModel);
    m_sortProxy->setDynamicSortFilter(true);
    m_sortProxy->sort(0, Qt::AscendingOrder);

    m_treeView = new QTreeView(m_splitter);
    m_treeView->setModel(m_sortProxy);
    m_treeView->setHeaderHidden(true);
    m_treeView->setAnimated(true);
    m_treeView->setExpandsOnDoubleClick(true);
    m_treeView->setRootIsDecorated(true);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *binderDelegate = new BinderDelegate(m_treeView);
    binderDelegate->exclude = &m_exclude;
    m_treeView->setItemDelegate(binderDelegate);
    for (int c = 1; c < 4; ++c)
        m_treeView->setColumnHidden(c, true);
    connect(m_treeView, &QTreeView::clicked, this, &QuireFrame::onTreeClicked);
    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &QuireFrame::onTreeSelectionChanged);
    connect(m_treeView, &QWidget::customContextMenuRequested, this, &QuireFrame::onBinderContextMenu);
    connect(m_fileModel, &QFileSystemModel::fileRenamed, this, &QuireFrame::onFileRenamed);
    connect(m_fileModel, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (auto *bp = static_cast<BinderProxy *>(m_sortProxy))
            bp->invalidate();
        if (!m_treeView || path.isEmpty())
            return;
        if (pathEquals(path, m_projectDir) || pathEquals(path, manuscriptDir())
            || pathEquals(path, notesDir())) {
            const QModelIndex idx = viewIndexForPath(path);
            if (idx.isValid())
                m_treeView->expand(idx);
        }
    });
    m_treeView->addAction(m_renameAction);
    m_treeView->addAction(m_deleteAction);
    m_treeView->addAction(m_moveUpAction);
    m_treeView->addAction(m_moveDownAction);
    m_treeView->addAction(m_includeAction);

    m_editor = new MonasteryEditor(m_splitter);
    m_splitter->addWidget(m_treeView);
    m_splitter->addWidget(m_editor);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 1000});
    setCentralWidget(m_splitter);

    connect(m_editor, &MonasteryEditor::wordCountChanged, this, [this](int) { updateStatus(); });
    connect(m_editor, &MonasteryEditor::dirtyChanged, this, [this](bool) { updateStatus(); });
    connect(m_editor, &MonasteryEditor::selectionFontChanged, this, &QuireFrame::onSelectionFontChanged);
    connect(m_editor->webView()->page(), &QWebEnginePage::pdfPrintingFinished,
            this, &QuireFrame::onPdfPrintingFinished);
    connect(m_editor, &MonasteryEditor::ready, this, [this](bool ok) {
        if (ok)
            applyTheme(m_currentTheme.themeId);
        if (!QCoreApplication::arguments().contains(QStringLiteral("--listen")))
            return;
        if (!ok) {
            std::fprintf(stdout, "editor: monastery-sep1\n");
            std::fprintf(stdout, "compiled: none  ooxml: no  leftover_html: yes  health: fail\n");
            std::fprintf(stdout, "health: fail\n");
            std::fflush(stdout);
            QTimer::singleShot(400, qApp, []() { QCoreApplication::quit(); });
            return;
        }
        runListenProof();
    });

    m_autoSaveTimer = new QTimer(this);
    m_autoSaveTimer->setInterval(15000);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &QuireFrame::onAutoSave);
    m_autoSaveTimer->start();

    QSettings settings(kOrg, kApp);
    const bool listen = QCoreApplication::arguments().contains(QStringLiteral("--listen"));
    const bool compileOneShot = QCoreApplication::arguments().contains(QStringLiteral("--compile"));
    if (!listen && !compileOneShot) {
        const QString last = settings.value(QStringLiteral("lastProject")).toString();
        QString project = last;
        if (project.isEmpty() || !QFileInfo::exists(project + QStringLiteral("/quire.json"))) {
            if (!ensureDefaultProject(&project))
                project.clear();
        }
        if (!project.isEmpty())
            openProject(project);
    }

    applyTheme(themeIdFromString(settings.value(QStringLiteral("theme"), QStringLiteral("leather")).toString()));
}

QuireFrame::~QuireFrame() = default;

void QuireFrame::closeEvent(QCloseEvent *event)
{
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);
    event->accept();
}

void QuireFrame::createActions()
{
    m_newProjectAction = new QAction(QStringLiteral("New Project"), this);
    m_newProjectAction->setShortcut(QKeySequence::New);
    connect(m_newProjectAction, &QAction::triggered, this, &QuireFrame::onNewProject);

    m_openProjectAction = new QAction(QStringLiteral("Open Project"), this);
    m_openProjectAction->setShortcut(QKeySequence::Open);
    connect(m_openProjectAction, &QAction::triggered, this, &QuireFrame::onOpenProject);

    m_saveAction = new QAction(QStringLiteral("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setIcon(QIcon(QStringLiteral(":/icons/save.png")));
    m_saveAction->setToolTip(QStringLiteral("Save"));
    connect(m_saveAction, &QAction::triggered, this, &QuireFrame::onSave);

    m_newChapterAction = new QAction(QStringLiteral("New Chapter"), this);
    m_newChapterAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    m_newChapterAction->setIcon(QIcon(QStringLiteral(":/icons/new.png")));
    m_newChapterAction->setToolTip(QStringLiteral("New Chapter"));
    connect(m_newChapterAction, &QAction::triggered, this, &QuireFrame::onNewChapter);

    m_newFolderAction = new QAction(QStringLiteral("New Folder"), this);
    m_newFolderAction->setIcon(QIcon(QStringLiteral(":/icons/new.png")));
    m_newFolderAction->setToolTip(QStringLiteral("New Folder"));
    connect(m_newFolderAction, &QAction::triggered, this, &QuireFrame::onNewFolder);

    m_newSceneAction = new QAction(QStringLiteral("New Scene"), this);
    m_newSceneAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    m_newSceneAction->setIcon(QIcon(QStringLiteral(":/icons/numbered.png")));
    m_newSceneAction->setToolTip(QStringLiteral("New Scene"));
    connect(m_newSceneAction, &QAction::triggered, this, &QuireFrame::onNewScene);

    m_newNoteAction = new QAction(QStringLiteral("New Note"), this);
    m_newNoteAction->setToolTip(QStringLiteral("New Note"));
    connect(m_newNoteAction, &QAction::triggered, this, &QuireFrame::onNewNote);

    m_importAction = new QAction(QStringLiteral("Import…"), this);
    m_importAction->setToolTip(QStringLiteral("Import as scenes"));
    connect(m_importAction, &QAction::triggered, this, &QuireFrame::onImport);

    m_importScrivenerAction = new QAction(QStringLiteral("Import Scrivener Project…"), this);
    m_importScrivenerAction->setToolTip(QStringLiteral("Copy a Scrivener .scriv binder into a new .qr project"));
    connect(m_importScrivenerAction, &QAction::triggered, this, &QuireFrame::onImportScrivener);

    m_compileAction = new QAction(QStringLiteral("Compile EPUB3 + Kindle DOCX + PDF"), this);
    m_compileAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    m_compileAction->setIcon(QIcon(QStringLiteral(":/icons/justify.png")));
    m_compileAction->setToolTip(QStringLiteral("Compile EPUB3 + Kindle DOCX + manuscript.pdf (5×8 paperback interior)"));
    connect(m_compileAction, &QAction::triggered, this, &QuireFrame::onCompile);

    m_printAction = new QAction(QStringLiteral("&Print…"), this);
    m_printAction->setShortcut(QKeySequence::Print);
    m_printAction->setToolTip(QStringLiteral(
        "Print the book (whole manuscript), or the selected manuscript/Notes folder"));
    connect(m_printAction, &QAction::triggered, this, &QuireFrame::onPrint);

    m_printCurrentSceneAction = new QAction(QStringLiteral("Print Current Scene…"), this);
    m_printCurrentSceneAction->setToolTip(QStringLiteral(
        "Print only the open scene from the editor (unsaved buffer OK)"));
    connect(m_printCurrentSceneAction, &QAction::triggered, this, &QuireFrame::onPrintCurrentScene);

    m_boldAction = new QAction(QStringLiteral("Bold"), this);
    m_boldAction->setCheckable(true);
    m_boldAction->setToolTip(QStringLiteral("Bold"));
    m_boldAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    m_boldAction->setIcon(QIcon(QStringLiteral(":/icons/bold.png")));
    connect(m_boldAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("bold"));
    });

    m_italicAction = new QAction(QStringLiteral("Italic"), this);
    m_italicAction->setCheckable(true);
    m_italicAction->setToolTip(QStringLiteral("Italic"));
    m_italicAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    m_italicAction->setIcon(QIcon(QStringLiteral(":/icons/italic.png")));
    connect(m_italicAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("italic"));
    });

    m_underlineAction = new QAction(QStringLiteral("Underline"), this);
    m_underlineAction->setCheckable(true);
    m_underlineAction->setToolTip(QStringLiteral("Underline"));
    m_underlineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+U")));
    m_underlineAction->setIcon(QIcon(QStringLiteral(":/icons/underline.png")));
    connect(m_underlineAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("underline"));
    });

    QActionGroup *alignGroup = new QActionGroup(this);
    m_alignLeftAction = new QAction(QStringLiteral("Align Left"), this);
    m_alignLeftAction->setCheckable(true);
    m_alignLeftAction->setToolTip(QStringLiteral("Align Left"));
    m_alignLeftAction->setIcon(QIcon(QStringLiteral(":/icons/alignleft.png")));
    connect(m_alignLeftAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("justifyLeft"));
    });
    alignGroup->addAction(m_alignLeftAction);

    m_alignCenterAction = new QAction(QStringLiteral("Align Center"), this);
    m_alignCenterAction->setCheckable(true);
    m_alignCenterAction->setToolTip(QStringLiteral("Align Center"));
    m_alignCenterAction->setIcon(QIcon(QStringLiteral(":/icons/aligncenter.png")));
    connect(m_alignCenterAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("justifyCenter"));
    });
    alignGroup->addAction(m_alignCenterAction);

    m_alignRightAction = new QAction(QStringLiteral("Align Right"), this);
    m_alignRightAction->setCheckable(true);
    m_alignRightAction->setToolTip(QStringLiteral("Align Right"));
    m_alignRightAction->setIcon(QIcon(QStringLiteral(":/icons/alignright.png")));
    connect(m_alignRightAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("justifyRight"));
    });
    alignGroup->addAction(m_alignRightAction);

    m_justifyAction = new QAction(QStringLiteral("Justify"), this);
    m_justifyAction->setCheckable(true);
    m_justifyAction->setToolTip(QStringLiteral("Justify"));
    m_justifyAction->setIcon(QIcon(QStringLiteral(":/icons/justify.png")));
    connect(m_justifyAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("justifyFull"));
    });
    alignGroup->addAction(m_justifyAction);

    m_bulletAction = new QAction(QStringLiteral("Bulleted List"), this);
    m_bulletAction->setToolTip(QStringLiteral("Bulleted List"));
    m_bulletAction->setIcon(QIcon(QStringLiteral(":/icons/bullet.png")));
    connect(m_bulletAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("insertUnorderedList"));
    });

    m_numberAction = new QAction(QStringLiteral("Numbered List"), this);
    m_numberAction->setToolTip(QStringLiteral("Numbered List"));
    m_numberAction->setIcon(QIcon(QStringLiteral(":/icons/numbered.png")));
    connect(m_numberAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("insertOrderedList"));
    });

    m_checklistAction = new QAction(QStringLiteral("Checklist"), this);
    m_checklistAction->setToolTip(QStringLiteral("Checklist"));
    m_checklistAction->setIcon(QIcon(QStringLiteral(":/icons/checklist.png")));
    connect(m_checklistAction, &QAction::triggered, this, &QuireFrame::onChecklist);

    m_undoAction = new QAction(QStringLiteral("Undo"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("undo"));
    });

    m_redoAction = new QAction(QStringLiteral("Redo"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("redo"));
    });

    m_cutAction = new QAction(QStringLiteral("Cu&t"), this);
    m_cutAction->setShortcut(QKeySequence::Cut);
    connect(m_cutAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("cut"));
    });

    m_copyAction = new QAction(QStringLiteral("&Copy"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    connect(m_copyAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("copy"));
    });

    m_pasteAction = new QAction(QStringLiteral("&Paste"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, [this]() {
        if (m_editor) m_editor->execCommand(QStringLiteral("paste"));
    });

    m_findAction = new QAction(QStringLiteral("&Find…"), this);
    m_findAction->setShortcut(QKeySequence::Find);
    m_findAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(m_findAction, &QAction::triggered, this, &QuireFrame::onFind);
    addAction(m_findAction);

    m_pageBreakAction = new QAction(QStringLiteral("Insert Page &Break"), this);
    connect(m_pageBreakAction, &QAction::triggered, this, [this]() {
        if (m_editor)
            m_editor->insertPageBreak();
    });

    m_renameAction = new QAction(QStringLiteral("Rename"), this);
    m_renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    m_renameAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_renameAction, &QAction::triggered, this, &QuireFrame::onRenameItem);

    m_deleteAction = new QAction(QStringLiteral("Delete"), this);
    m_deleteAction->setShortcut(QKeySequence::Delete);
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_deleteAction, &QAction::triggered, this, &QuireFrame::onDeleteItem);

    m_moveUpAction = new QAction(QStringLiteral("Move Up"), this);
    m_moveUpAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    m_moveUpAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_moveUpAction, &QAction::triggered, this, &QuireFrame::onMoveUp);

    m_moveDownAction = new QAction(QStringLiteral("Move Down"), this);
    m_moveDownAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    m_moveDownAction->setShortcutContext(Qt::WindowShortcut);
    connect(m_moveDownAction, &QAction::triggered, this, &QuireFrame::onMoveDown);

    m_includeAction = new QAction(QStringLiteral("Include in Compile"), this);
    m_includeAction->setCheckable(true);
    m_includeAction->setChecked(true);
    m_includeAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_includeAction, &QAction::triggered, this, &QuireFrame::onToggleIncludeInCompile);

    m_projectDetailsAction = new QAction(QStringLiteral("Project Details…"), this);
    connect(m_projectDetailsAction, &QAction::triggered, this, &QuireFrame::onProjectDetails);

    m_focusAction = new QAction(QStringLiteral("Focus Mode"), this);
    m_focusAction->setCheckable(true);
    m_focusAction->setShortcut(QKeySequence(Qt::Key_F11));
    m_focusAction->setShortcutContext(Qt::ApplicationShortcut);
    m_focusAction->setToolTip(QStringLiteral("Hide binder and format toolbar"));
    connect(m_focusAction, &QAction::toggled, this, &QuireFrame::onToggleFocusMode);
    addAction(m_focusAction);

    m_prevSceneAction = new QAction(QStringLiteral("Previous Scene"), this);
    m_prevSceneAction->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageUp),
        QKeySequence(Qt::ALT | Qt::Key_Left)
    });
    m_prevSceneAction->setShortcutContext(Qt::ApplicationShortcut);
    m_prevSceneAction->setToolTip(QStringLiteral("Previous manuscript scene"));
    connect(m_prevSceneAction, &QAction::triggered, this, &QuireFrame::onPreviousScene);
    addAction(m_prevSceneAction);

    m_nextSceneAction = new QAction(QStringLiteral("Next Scene"), this);
    m_nextSceneAction->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_PageDown),
        QKeySequence(Qt::ALT | Qt::Key_Right)
    });
    m_nextSceneAction->setShortcutContext(Qt::ApplicationShortcut);
    m_nextSceneAction->setToolTip(QStringLiteral("Next manuscript scene"));
    connect(m_nextSceneAction, &QAction::triggered, this, &QuireFrame::onNextScene);
    addAction(m_nextSceneAction);

    auto *escFocus = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escFocus->setContext(Qt::ApplicationShortcut);
    connect(escFocus, &QShortcut::activated, this, [this]() {
        if (!m_focusMode)
            return;
        if (QWidget *w = QApplication::activeWindow()) {
            if (w != this)
                return;
        }
        setFocusMode(false);
    });

    m_aboutAction = new QAction(QStringLiteral("About Quire"), this);
    connect(m_aboutAction, &QAction::triggered, this, &QuireFrame::onAbout);

    m_quitAction = new QAction(QStringLiteral("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);
}

void QuireFrame::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(m_newProjectAction);
    fileMenu->addAction(m_openProjectAction);
    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open Recent"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, &QuireFrame::rebuildRecentMenu);
    rebuildRecentMenu();
    fileMenu->addAction(m_saveAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_newChapterAction);
    fileMenu->addAction(m_newSceneAction);
    fileMenu->addAction(m_newNoteAction);
    fileMenu->addAction(m_newFolderAction);
    fileMenu->addAction(m_importAction);
    fileMenu->addAction(m_importScrivenerAction);
    fileMenu->addAction(m_renameAction);
    fileMenu->addAction(m_deleteAction);
    fileMenu->addAction(m_moveUpAction);
    fileMenu->addAction(m_moveDownAction);
    fileMenu->addAction(m_includeAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_projectDetailsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_compileAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_printAction);
    fileMenu->addAction(m_printCurrentSceneAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);
    for (QAction *a : fileMenu->actions())
        a->setIconVisibleInMenu(false);

    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    editMenu->addAction(m_cutAction);
    editMenu->addAction(m_copyAction);
    editMenu->addAction(m_pasteAction);
    editMenu->addSeparator();
    editMenu->addAction(m_findAction);
    editMenu->addSeparator();
    editMenu->addAction(m_boldAction);
    editMenu->addAction(m_italicAction);
    editMenu->addAction(m_underlineAction);
    editMenu->addSeparator();
    editMenu->addAction(m_alignLeftAction);
    editMenu->addAction(m_alignCenterAction);
    editMenu->addAction(m_alignRightAction);
    editMenu->addAction(m_justifyAction);
    editMenu->addSeparator();
    editMenu->addAction(m_bulletAction);
    editMenu->addAction(m_numberAction);
    editMenu->addAction(m_checklistAction);
    editMenu->addSeparator();
    editMenu->addAction(m_pageBreakAction);
    for (QAction *a : editMenu->actions())
        a->setIconVisibleInMenu(false);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->addAction(m_focusAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_prevSceneAction);
    viewMenu->addAction(m_nextSceneAction);

    QMenu *themeMenu = menuBar()->addMenu(QStringLiteral("&Theme"));
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);
    for (const Theme &th : allThemes()) {
        QAction *action = themeMenu->addAction(th.name);
        action->setCheckable(true);
        action->setData(th.id);
        m_themeGroup->addAction(action);
        const ThemeId id = th.themeId;
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (checked)
                applyTheme(id);
        });
    }

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void QuireFrame::createToolBar()
{
    m_formatToolBar = addToolBar(QStringLiteral("Binder"));
    QToolBar *bar = m_formatToolBar;
    bar->setMovable(false);
    bar->setIconSize(QSize(16, 16));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->addAction(m_saveAction);
    bar->addSeparator();
    bar->addAction(m_newChapterAction);
    bar->addAction(m_newSceneAction);
    bar->addSeparator();
    m_fontCombo = new QFontComboBox();
    m_fontCombo->setEditable(true);
    m_fontCombo->setCurrentFont(QFont(QStringLiteral("Gelasio")));
    connect(m_fontCombo, &QComboBox::textActivated, this, &QuireFrame::onFontChanged);
    bar->addWidget(m_fontCombo);
    m_sizeCombo = new QComboBox();
    m_sizeCombo->addItems({QStringLiteral("8"), QStringLiteral("10"), QStringLiteral("12"),
                           QStringLiteral("14"), QStringLiteral("16"), QStringLiteral("18"),
                           QStringLiteral("20"), QStringLiteral("24"), QStringLiteral("28"),
                           QStringLiteral("32")});
    m_sizeCombo->setCurrentText(QStringLiteral("12"));
    connect(m_sizeCombo, &QComboBox::textActivated, this, &QuireFrame::onSizeChanged);
    bar->addWidget(m_sizeCombo);
    bar->addAction(m_boldAction);
    bar->addAction(m_italicAction);
    bar->addAction(m_underlineAction);
    bar->addSeparator();
    bar->addAction(m_alignLeftAction);
    bar->addAction(m_alignCenterAction);
    bar->addAction(m_alignRightAction);
    bar->addAction(m_justifyAction);
    bar->addSeparator();
    bar->addAction(m_bulletAction);
    bar->addAction(m_numberAction);
    bar->addAction(m_checklistAction);
    bar->addSeparator();
    bar->addAction(m_compileAction);
}

void QuireFrame::createStatusBar()
{
    m_dirtyLabel = new QLabel(QStringLiteral("Saved"));
    m_sceneLabel = new QLabel;
    m_wordCountLabel = new QLabel(QStringLiteral("Words: 0 / 0"));
    statusBar()->addWidget(m_dirtyLabel);
    statusBar()->addWidget(m_sceneLabel);
    statusBar()->addPermanentWidget(m_wordCountLabel);
}

QString QuireFrame::defaultManuscriptsRoot() const
{
    // APPIMAGE is the packed image path; applicationDirPath() points inside its mount.
    const QString imagePath = QFile::decodeName(qgetenv("APPIMAGE"));
    if (!imagePath.isEmpty()) {
        const QFileInfo imageInfo(imagePath);
        if (imageInfo.isAbsolute())
            return QDir::cleanPath(imageInfo.absolutePath() + QStringLiteral("/Manuscripts"));
    }

    // Development builds retain the source-tree Manuscripts location.
    const QString beside = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Manuscripts"));
    if (QDir(beside).exists())
        return canonicalOrAbs(beside);
    const QString home = QFile::decodeName(qgetenv("HOME"));
    if (!home.isEmpty())
        return QDir::cleanPath(home + QStringLiteral("/Quire/Manuscripts"));
    return QStringLiteral("Manuscripts");
}

QString QuireFrame::manuscriptDir() const { return m_projectDir + QStringLiteral("/manuscript"); }
QString QuireFrame::notesDir() const { return m_projectDir + QStringLiteral("/notes"); }
QString QuireFrame::compileDir() const { return m_projectDir + QStringLiteral("/compile"); }
QString QuireFrame::autosaveDir() const { return m_projectDir + QStringLiteral("/.autosave"); }
QString QuireFrame::quireJsonPath() const { return m_projectDir + QStringLiteral("/quire.json"); }

bool QuireFrame::writeTextFile(const QString &path, const QString &text) const
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    const QByteArray bytes = text.toUtf8();
    return f.write(bytes) == bytes.size();
}

QString QuireFrame::readTextFile(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

bool QuireFrame::createProjectAt(const QString &projectDir, const QString &title, const QString &author)
{
    QDir().mkpath(projectDir + QStringLiteral("/manuscript/Front Matter"));
    QDir().mkpath(projectDir + QStringLiteral("/manuscript/Chapter 1"));
    QDir().mkpath(projectDir + QStringLiteral("/notes"));
    QDir().mkpath(projectDir + QStringLiteral("/compile"));
    QDir().mkpath(projectDir + QStringLiteral("/.autosave"));

    QJsonObject meta;
    meta.insert(QStringLiteral("title"), title);
    meta.insert(QStringLiteral("author"), author);
    meta.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert(QStringLiteral("quire"), kQuireVersion);
    const QJsonDocument doc(meta);
    if (!writeTextFile(projectDir + QStringLiteral("/quire.json"),
                       QString::fromUtf8(doc.toJson(QJsonDocument::Indented))))
        return false;

    const QString titlePage =
        QStringLiteral("<h1>%1</h1><p>by %2</p><p></p>")
            .arg(title.toHtmlEscaped(), author.toHtmlEscaped());
    const QString scene1 =
        QStringLiteral(
            "<p>The lamp made a small island on the desk. He opened a new binder "
            "and wrote the first sentence of the book he had been carrying for years.</p>"
            "<p>Outside, the city kept its own counsel. Inside, the page was finally willing.</p>");

    writeTextFile(projectDir + QStringLiteral("/manuscript/Front Matter/Title Page.html"), titlePage);
    writeTextFile(projectDir + QStringLiteral("/manuscript/Chapter 1/Scene 1.html"), scene1);
    return true;
}

bool QuireFrame::ensureDefaultProject(QString *outPath)
{
    const QString path = defaultManuscriptsRoot() + QStringLiteral("/Untitled Novel");
    if (!QFileInfo::exists(path + QStringLiteral("/quire.json"))) {
        if (!createProjectAt(path, QStringLiteral("Untitled Novel"), QStringLiteral("Noble Brown")))
            return false;
    }
    if (outPath)
        *outPath = path;
    return true;
}

QStringList QuireFrame::cleanedRecentProjects()
{
    QSettings settings(kOrg, kApp);
    const QStringList recent = settings.value(QStringLiteral("recentProjects")).toStringList();
    QStringList kept;
    for (const QString &raw : recent) {
        if (raw.isEmpty())
            continue;
        const QString p = canonicalOrAbs(raw);
        if (!QFileInfo::exists(p + QStringLiteral("/quire.json")))
            continue;
        bool dup = false;
        for (const QString &k : kept) {
            if (pathEquals(k, p)) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        kept.append(p);
        if (kept.size() >= 8)
            break;
    }
    if (kept != recent)
        settings.setValue(QStringLiteral("recentProjects"), kept);
    return kept;
}

void QuireFrame::rebuildRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    const QStringList recent = cleanedRecentProjects();
    if (recent.isEmpty()) {
        QAction *empty = m_recentMenu->addAction(QStringLiteral("(None)"));
        empty->setEnabled(false);
        return;
    }
    for (const QString &p : recent) {
        QString label = QFileInfo(p).fileName();
        if (label.isEmpty())
            label = p;
        QAction *a = m_recentMenu->addAction(label);
        a->setToolTip(p);
        a->setData(p);
        connect(a, &QAction::triggered, this, [this, p]() {
            openProject(p);
        });
    }
}

void QuireFrame::rememberProject(const QString &projectDir)
{
    QString dir = projectDir;
    if (dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    const QString stored = canonicalOrAbs(dir);

    QSettings settings(kOrg, kApp);
    settings.setValue(QStringLiteral("lastProject"), stored);

    QStringList next;
    if (QFileInfo::exists(stored + QStringLiteral("/quire.json")))
        next.append(stored);
    const QStringList recent = settings.value(QStringLiteral("recentProjects")).toStringList();
    for (const QString &raw : recent) {
        if (raw.isEmpty())
            continue;
        const QString p = canonicalOrAbs(raw);
        if (pathEquals(p, stored))
            continue;
        if (!QFileInfo::exists(p + QStringLiteral("/quire.json")))
            continue;
        next.append(p);
        if (next.size() >= 8)
            break;
    }
    settings.setValue(QStringLiteral("recentProjects"), next);
    rebuildRecentMenu();
}

void QuireFrame::bindTreeToManuscript()
{
    QDir().mkpath(manuscriptDir());
    QDir().mkpath(notesDir());
    const QString root = m_projectDir;
    m_fileModel->setRootPath(root);
    if (auto *bp = static_cast<BinderProxy *>(m_sortProxy)) {
        bp->manuscriptRoot = manuscriptDir();
        bp->projectRoot = root;
        bp->notesRoot = notesDir();
        bp->invalidate();
    }
    if (auto *del = static_cast<BinderDelegate *>(m_treeView->itemDelegate()))
        del->manuscriptRoot = manuscriptDir();
    const QModelIndex src = m_fileModel->index(root);
    m_treeView->setRootIndex(m_sortProxy ? m_sortProxy->mapFromSource(src) : src);
    if (m_sortProxy)
        m_sortProxy->sort(0, Qt::AscendingOrder);
    m_treeView->expandToDepth(1);
}

bool QuireFrame::openProject(const QString &projectDir, bool remember)
{
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    QString dir = projectDir;
    if (dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    if (!QDir(dir).exists())
        return false;

    if (!QFileInfo::exists(dir + QStringLiteral("/quire.json"))) {
        QDir().mkpath(dir + QStringLiteral("/manuscript"));
        QDir().mkpath(dir + QStringLiteral("/notes"));
        QDir().mkpath(dir + QStringLiteral("/compile"));
        QJsonObject meta;
        meta.insert(QStringLiteral("title"), QFileInfo(dir).fileName());
        meta.insert(QStringLiteral("author"), QStringLiteral("Noble Brown"));
        meta.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
        meta.insert(QStringLiteral("quire"), kQuireVersion);
        writeTextFile(dir + QStringLiteral("/quire.json"),
                      QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Indented)));
    }

    m_projectDir = dir;
    const QJsonDocument doc = QJsonDocument::fromJson(readTextFile(quireJsonPath()).toUtf8());
    const QJsonObject obj = doc.object();
    m_projectTitle = obj.value(QStringLiteral("title")).toString(QFileInfo(dir).fileName());
    m_projectAuthor = obj.value(QStringLiteral("author")).toString(QStringLiteral("Noble Brown"));
    loadBinderLists(obj);
    syncOrderFromDisk();
    writeProjectJson();
    if (remember)
        rememberProject(dir);
    bindTreeToManuscript();

    m_currentScenePath.clear();
    const QString scene1 = dir + QStringLiteral("/manuscript/Chapter 1/Scene 1.html");
    if (QFileInfo::exists(scene1)) {
        const QModelIndex idx = viewIndexForPath(scene1);
        if (idx.isValid())
            m_treeView->setCurrentIndex(idx);
        loadScene(scene1);
    }
    updateWindowTitle();
    refreshManuscriptWordCount();
    updateStatus();
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(m_projectTitle), 4000);
    return true;
}

void QuireFrame::onNewProject()
{
    const QString parent = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Parent folder for the new project"),
        defaultManuscriptsRoot());
    if (parent.isEmpty())
        return;
    bool ok = false;
    const QString entered = QInputDialog::getText(
        this, QStringLiteral("New Project"),
        QStringLiteral("Novel title (.qr marks the project folder):"),
        QLineEdit::Normal, QStringLiteral("Untitled Novel"), &ok);
    const QString title = projectTitleFromInput(entered);
    if (!ok || title.isEmpty())
        return;
    const QString path = parent + QLatin1Char('/') + projectFolderName(entered);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             QStringLiteral("A file or project folder already exists there."));
        return;
    }
    if (!createProjectAt(path, title, QStringLiteral("Noble Brown"))) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             QStringLiteral("Could not create the project."));
        return;
    }
    openProject(path);
}

void QuireFrame::onOpenProject()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open Quire project"),
        m_projectDir.isEmpty() ? defaultManuscriptsRoot() : m_projectDir);
    if (dir.isEmpty())
        return;
    openProject(dir);
}

void QuireFrame::onSave()
{
    if (persistCurrentScene(true))
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(sceneTitleFromPath(m_currentScenePath)), 3000);
    else
        statusBar()->showMessage(QStringLiteral("Nothing to save"), 2000);
}

void QuireFrame::onChecklist()
{
    if (m_editor)
        m_editor->insertChecklist();
}

void QuireFrame::onAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About Quire"),
        QStringLiteral(
            "<h2>Quire %1</h2>"
            "<p>Linux-native novel binder. Monastery on the page, a real binder on the left.</p>"
            "<p>Notes live in the binder and never compile. "
            "Binder order and compile include/exclude live in quire.json. "
            "Find walks every manuscript scene in binder order. "
            "Import html/md/txt/docx as scenes. "
            "File → Import Scrivener Project copies a .scriv binder into a .qr; the original is left untouched. "
            "F11 focus mode hides the binder and format toolbar. "
            "Previous/Next Scene (Ctrl+PageUp/PageDown) walk manuscript scenes "
            "without the binder, including in focus mode. "
            "File → Open Recent lists up to eight project folders. "
            "New projects use a .qr suffix to mark Quire project folders; existing projects remain openable. "
            "Use binder operations to preserve order; files moved externally may append. "
            "Compile writes HTML, EPUB3 (manuscript.epub = KDP Kindle upload), "
            "a Word fallback (manuscript.docx), and manuscript.pdf "
            "(5×8 paperback interior — not File→Print).</p>"
            "<p>Sociopathletic · Noble Brown</p>").arg(kQuireVersion));
}

void QuireFrame::onPreviousScene()
{
    navigateManuscriptScene(-1);
}

void QuireFrame::onNextScene()
{
    navigateManuscriptScene(1);
}

void QuireFrame::navigateManuscriptScene(int delta)
{
    if (m_projectDir.isEmpty() || delta == 0)
        return;
    QStringList scenes;
    collectScenes(manuscriptDir(), &scenes, true);
    const int n = scenes.size();
    if (n <= 0)
        return;

    int idx = -1;
    for (int i = 0; i < n; ++i) {
        if (pathEquals(scenes.at(i), m_currentScenePath)) {
            idx = i;
            break;
        }
    }

    QString target;
    if (idx < 0) {
        // Open note (or anything outside manuscript/): Next -> first, Previous -> last.
        target = (delta > 0) ? scenes.first() : scenes.last();
    } else {
        int next = (idx + delta) % n;
        if (next < 0)
            next += n;
        target = scenes.at(next);
    }
    activateSceneForFind(target);
}

void QuireFrame::onToggleFocusMode(bool on)
{
    setFocusMode(on);
}

void QuireFrame::setFocusMode(bool on)
{
    if (m_focusAction) {
        QSignalBlocker blocker(m_focusAction);
        m_focusAction->setChecked(on);
    }
    if (m_focusMode == on)
        return;
    m_focusMode = on;

    const bool listen = QCoreApplication::arguments().contains(QStringLiteral("--listen"));

    if (on) {
        m_savedMaximized = isMaximized();
        m_savedGeometry = saveGeometry();
        if (m_splitter)
            m_savedSplitterSizes = m_splitter->sizes();
        if (m_treeView)
            m_treeView->hide();
        if (m_formatToolBar)
            m_formatToolBar->hide();
        if (!listen && !m_savedMaximized)
            showMaximized();
    } else {
        if (m_treeView)
            m_treeView->show();
        if (m_formatToolBar)
            m_formatToolBar->show();
        if (m_splitter && !m_savedSplitterSizes.isEmpty())
            m_splitter->setSizes(m_savedSplitterSizes);
        if (!listen) {
            if (m_savedMaximized) {
                showMaximized();
            } else {
                showNormal();
                if (!m_savedGeometry.isEmpty())
                    restoreGeometry(m_savedGeometry);
            }
        }
    }
}

bool QuireFrame::isNotesPath(const QString &path) const
{
    return !path.isEmpty() && !m_projectDir.isEmpty() && pathIsUnder(path, notesDir());
}

bool QuireFrame::isManuscriptPath(const QString &path) const
{
    return !path.isEmpty() && !m_projectDir.isEmpty() && pathIsUnder(path, manuscriptDir());
}

bool QuireFrame::isBinderTopFolder(const QString &path) const
{
    if (path.isEmpty() || m_projectDir.isEmpty())
        return false;
    return pathEquals(path, manuscriptDir()) || pathEquals(path, notesDir())
        || pathEquals(path, m_projectDir);
}

QString QuireFrame::currentParentDir() const
{
    const QString root = manuscriptDir();
    const QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid())
        return root;
    const QString path = pathFromView(idx);
    if (!isManuscriptPath(path))
        return root;
    const QFileInfo info(path);
    if (info.isDir())
        return info.absoluteFilePath();
    return info.absolutePath();
}

QString QuireFrame::currentNotesParentDir() const
{
    const QString root = notesDir();
    const QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid())
        return root;
    const QString path = pathFromView(idx);
    if (!isNotesPath(path))
        return root;
    const QFileInfo info(path);
    if (info.isDir())
        return info.absoluteFilePath();
    return info.absolutePath();
}

QString QuireFrame::uniquePath(const QString &dir, const QString &base, const QString &suffix) const
{
    QString candidate = dir + QLatin1Char('/') + base + suffix;
    int n = 2;
    while (QFileInfo::exists(candidate)) {
        candidate = dir + QLatin1Char('/') + base + QStringLiteral(" %1").arg(n) + suffix;
        ++n;
    }
    return candidate;
}

int QuireFrame::nextChapterNumber() const
{
    int maxN = 0;
    const QRegularExpression re(QStringLiteral("^Chapter\\s*(\\d+)"),
                                QRegularExpression::CaseInsensitiveOption);
    const QFileInfoList entries = QDir(manuscriptDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &e : entries) {
        const QRegularExpressionMatch m = re.match(e.fileName());
        if (m.hasMatch())
            maxN = qMax(maxN, m.captured(1).toInt());
    }
    return maxN + 1;
}

void QuireFrame::onNewChapter()
{
    if (m_projectDir.isEmpty())
        return;
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    const QString suggested = QStringLiteral("Chapter %1").arg(nextChapterNumber());
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New Chapter"), QStringLiteral("Chapter name:"),
        QLineEdit::Normal, suggested, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QString chapterPath = uniquePath(manuscriptDir(), name.trimmed(), QString());
    if (!QDir().mkpath(chapterPath)) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not create the chapter."));
        return;
    }

    const QString scenePath = uniquePath(chapterPath, QStringLiteral("Scene 1"), QStringLiteral(".html"));
    if (!writeTextFile(scenePath, QStringLiteral("<p></p>"))) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not create Scene 1."));
        return;
    }

    const QModelIndex chapterIdx = viewIndexForPath(chapterPath);
    if (chapterIdx.isValid())
        m_treeView->expand(chapterIdx);
    const QModelIndex sceneIdx = viewIndexForPath(scenePath);
    if (sceneIdx.isValid())
        m_treeView->setCurrentIndex(sceneIdx);
    appendToOrder(chapterPath);
    appendToOrder(scenePath);
    writeProjectJson();
    loadScene(scenePath);
    statusBar()->showMessage(QStringLiteral("Created %1").arg(QFileInfo(chapterPath).fileName()), 3000);
}

void QuireFrame::onNewFolder()
{
    if (m_projectDir.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New Folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, QStringLiteral("New Folder"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = uniquePath(currentParentDir(), name.trimmed(), QString());
    if (!QDir().mkpath(path)) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not create the folder."));
        return;
    }
    const QModelIndex idx = viewIndexForPath(path);
    if (idx.isValid()) {
        m_treeView->expand(viewIndexForPath(QFileInfo(path).absolutePath()));
        m_treeView->setCurrentIndex(idx);
    }
    appendToOrder(path);
    writeProjectJson();
    statusBar()->showMessage(QStringLiteral("Created folder %1").arg(QFileInfo(path).fileName()), 3000);
}

void QuireFrame::onNewScene()
{
    if (m_projectDir.isEmpty())
        return;
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New Scene"), QStringLiteral("Scene name:"),
        QLineEdit::Normal, QStringLiteral("New Scene"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = uniquePath(currentParentDir(), name.trimmed(), QStringLiteral(".html"));
    const QString body = QStringLiteral("<p></p>");
    if (!writeTextFile(path, body)) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not create the scene."));
        return;
    }
    const QModelIndex idx = viewIndexForPath(path);
    if (idx.isValid()) {
        m_treeView->expand(viewIndexForPath(QFileInfo(path).absolutePath()));
        m_treeView->setCurrentIndex(idx);
    }
    appendToOrder(path);
    writeProjectJson();
    loadScene(path);
    statusBar()->showMessage(QStringLiteral("Created scene %1").arg(QFileInfo(path).completeBaseName()), 3000);
}

void QuireFrame::onNewNote()
{
    if (m_projectDir.isEmpty())
        return;
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    QDir().mkpath(notesDir());
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New Note"), QStringLiteral("Note name:"),
        QLineEdit::Normal, QStringLiteral("New Note"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = uniquePath(currentNotesParentDir(), name.trimmed(), QStringLiteral(".html"));
    if (!writeTextFile(path, QStringLiteral("<p></p>"))) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not create the note."));
        return;
    }
    const QModelIndex idx = viewIndexForPath(path);
    if (idx.isValid()) {
        m_treeView->expand(viewIndexForPath(QFileInfo(path).absolutePath()));
        m_treeView->setCurrentIndex(idx);
    }
    loadScene(path);
    statusBar()->showMessage(QStringLiteral("Created note %1").arg(QFileInfo(path).completeBaseName()), 3000);
}

QString QuireFrame::importSceneFromFile(const QString &sourcePath, const QString &destDir, QString *error)
{
    if (error)
        error->clear();
    if (m_projectDir.isEmpty() || destDir.isEmpty()) {
        if (error)
            *error = QStringLiteral("No project folder.");
        return {};
    }
    if (!isManuscriptPath(destDir)) {
        if (error)
            *error = QStringLiteral("Import only writes to manuscript.");
        return {};
    }
    QString ioErr;
    QString html = DocumentIo::htmlFromFile(sourcePath, &ioErr);
    if (html.isEmpty() && !ioErr.isEmpty()) {
        if (error)
            *error = ioErr;
        return {};
    }
    if (html.isEmpty())
        html = QStringLiteral("<p></p>");

    const QString base = QFileInfo(sourcePath).completeBaseName();
    const QString path = uniquePath(destDir, base, QStringLiteral(".html"));
    if (!writeTextFile(path, html)) {
        if (error)
            *error = QStringLiteral("Could not write the scene.");
        return {};
    }
    appendToOrder(path);
    return path;
}

void QuireFrame::onImport()
{
    if (m_projectDir.isEmpty())
        return;
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import as scenes"),
        m_projectDir.isEmpty() ? defaultManuscriptsRoot() : m_projectDir,
        QStringLiteral("Scenes (*.html *.htm *.md *.markdown *.txt *.docx);;All files (*)"));
    if (files.isEmpty())
        return;

    const QString destDir = currentParentDir();
    QString lastPath;
    int ok = 0;
    QStringList fails;
    for (const QString &src : files) {
        QString err;
        const QString path = importSceneFromFile(src, destDir, &err);
        if (path.isEmpty()) {
            QString line = QFileInfo(src).fileName();
            if (!err.isEmpty())
                line += QStringLiteral(": ") + err;
            fails.append(line);
            continue;
        }
        lastPath = path;
        ++ok;
    }
    writeProjectJson();
    if (!lastPath.isEmpty()) {
        const QModelIndex idx = viewIndexForPath(lastPath);
        if (idx.isValid()) {
            m_treeView->expand(viewIndexForPath(QFileInfo(lastPath).absolutePath()));
            m_treeView->setCurrentIndex(idx);
        }
        loadScene(lastPath);
    }
    if (!fails.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             QStringLiteral("Could not import:\n%1").arg(fails.join(QLatin1Char('\n'))));
    }
    if (ok > 0)
        statusBar()->showMessage(QStringLiteral("Imported %1 scene%2")
                                     .arg(ok)
                                     .arg(ok == 1 ? QString() : QStringLiteral("s")),
                                 4000);
}

void QuireFrame::onImportScrivener()
{
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    const QString scrivStart = QDir::homePath();
    QString scrivPath = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Scrivener project (.scriv folder)"),
        scrivStart);
    if (scrivPath.isEmpty()) {
        const QString scrivx = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Scrivener project (.scrivx)"),
            scrivStart,
            QStringLiteral("Scrivener project (*.scrivx);;All files (*)"));
        if (scrivx.isEmpty())
            return;
        scrivPath = scrivx;
    }

    const QString parent = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Parent folder for the new Quire project"),
        defaultManuscriptsRoot());
    if (parent.isEmpty())
        return;

    ScrivenerImportResult probe;
    // Import into a temporary name first via parent dir — title comes from package.
    // We need the title before creating; run import with parent directory so helper names Title.qr.
    // But importProject with existing dir creates parent/Title.qr. Good.
    // Avoid collision: if Title.qr exists, importer refuses overwrite.
    const QString outHint = parent; // directory → Title.qr inside
    ScrivenerImportResult result;
    const bool ok = ScrivenerImport::importProject(scrivPath, outHint, &result);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             result.error.isEmpty()
                                 ? QStringLiteral("Scrivener import failed.")
                                 : result.error);
        return;
    }

    statusBar()->showMessage(
        QStringLiteral("Imported Scrivener → %1 (%2 scenes, %3 folders, %4 notes)")
            .arg(result.outName)
            .arg(result.scenes)
            .arg(result.folders)
            .arg(result.notes),
        6000);
    openProject(result.outPath);
}


void QuireFrame::collectScenes(const QString &dir, QStringList *out, bool includeExcluded) const
{
    QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    std::sort(entries.begin(), entries.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        return binderLessThan(manuscriptRelative(a.absoluteFilePath()), a.fileName(), a.isDir(),
                              manuscriptRelative(b.absoluteFilePath()), b.fileName(), b.isDir());
    });

    const QString rootCanon = QFileInfo(manuscriptDir()).canonicalFilePath();
    const QString dirCanon = QFileInfo(dir).canonicalFilePath();
    if (!rootCanon.isEmpty() && dirCanon == rootCanon) {
        QFileInfoList front, middle, back;
        for (const QFileInfo &e : entries) {
            const QString key = e.isDir() ? e.fileName() : e.completeBaseName();
            switch (matterKindForName(key)) {
            case MatterKind::Front:
                front.append(e);
                break;
            case MatterKind::Back:
                back.append(e);
                break;
            case MatterKind::Middle:
            default:
                middle.append(e);
                break;
            }
        }
        entries = front + middle + back;
    }

    for (const QFileInfo &e : entries) {
        if (!includeExcluded && isSelfExcluded(e.absoluteFilePath()))
            continue;
        if (e.isDir())
            collectScenes(e.absoluteFilePath(), out, includeExcluded);
        else if (e.suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0)
            out->append(e.absoluteFilePath());
    }
}

QString QuireFrame::wrapSceneHtml(const QString &path, const QString &body) const
{
    const QString title = sceneTitleFromPath(path);
    return QStringLiteral("<!-- %1 -->\n<section data-scene=\"%2\">\n%3\n</section>\n")
        .arg(title.toHtmlEscaped(), QFileInfo(path).fileName().toHtmlEscaped(), body);
}

void QuireFrame::onCompile()
{
    if (m_projectDir.isEmpty())
        return;
    QString err;
    const bool ok = compileToDisk(&err);
    const QString docx = compileDir() + QStringLiteral("/manuscript.docx");
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             err.isEmpty() ? QStringLiteral("Compile failed.") : err);
        if (!QFileInfo::exists(docx))
            return;
    }
    statusBar()->showMessage(
        QStringLiteral("Compiled manuscript.epub (Kindle), manuscript.docx, and manuscript.pdf (5×8 paperback interior)"),
        8000);
    refreshManuscriptWordCount();
    updateStatus();
}

bool QuireFrame::compileToDisk(QString *error)
{
    if (m_projectDir.isEmpty()) {
        if (error)
            *error = QStringLiteral("No project open.");
        return false;
    }
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    syncOrderFromDisk();
    writeProjectJson();

    QStringList scenes;
    collectScenes(manuscriptDir(), &scenes);

    QVector<EpubWriter::Scene> epubScenes;
    epubScenes.reserve(scenes.size());
    QString lastChapter;
    const QString msRoot = manuscriptDir();
    for (const QString &path : scenes) {
        EpubWriter::Scene sc;
        sc.title = sceneTitleFromPath(path);
        {
            const QString raw = readTextFile(path);
            const QString healed = EpubWriter::healBody(raw);
            if (healed != raw)
                writeTextFile(path, healed);
            sc.bodyHtml = sceneBody(healed);
        }
        QString rel = QDir(msRoot).relativeFilePath(QFileInfo(path).absolutePath());
        if (!rel.isEmpty() && rel != QLatin1String("."))
            sc.folderTrail = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        annotateCompileScene(&sc, &lastChapter);
        epubScenes.append(sc);
    }

    const QString fontOutDir = compileDir() + QStringLiteral("/fonts/gelasio");
    if (!copyGelasioFontsToDir(fontOutDir, error))
        return false;

    QString parts;
    parts += QStringLiteral(
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>%1</title>\n"
        "<style>\n"
        "@font-face {\n"
        "  font-family: \"Gelasio\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Regular.ttf\") format(\"truetype\");\n"
        "  font-weight: 400;\n"
        "  font-style: normal;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Gelasio\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Italic.ttf\") format(\"truetype\");\n"
        "  font-weight: 400;\n"
        "  font-style: italic;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Gelasio\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Bold.ttf\") format(\"truetype\");\n"
        "  font-weight: 700;\n"
        "  font-style: normal;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Gelasio\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-BoldItalic.ttf\") format(\"truetype\");\n"
        "  font-weight: 700;\n"
        "  font-style: italic;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Georgia\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Regular.ttf\") format(\"truetype\");\n"
        "  font-weight: 400;\n"
        "  font-style: normal;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Georgia\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Italic.ttf\") format(\"truetype\");\n"
        "  font-weight: 400;\n"
        "  font-style: italic;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Georgia\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-Bold.ttf\") format(\"truetype\");\n"
        "  font-weight: 700;\n"
        "  font-style: normal;\n"
        "}\n"
        "@font-face {\n"
        "  font-family: \"Georgia\";\n"
        "  src: url(\"fonts/gelasio/Gelasio-BoldItalic.ttf\") format(\"truetype\");\n"
        "  font-weight: 700;\n"
        "  font-style: italic;\n"
        "}\n"
        "body {\n"
        "  font-family: Gelasio, Georgia, \"Times New Roman\", serif;\n"
        "  font-size: 1em;\n"
        "  line-height: 1.35;\n"
        "  margin: 1em 1.2em;\n"
        "  color: #111;\n"
        "  background: #fff;\n"
        "}\n"
        "h1 { font-size: 1.6em; font-weight: bold; text-align: center; margin: 2em 0 1.2em 0; }\n"
        "h1.chapter { text-align: center; }\n"
        "h2 { font-size: 1.2em; font-weight: bold; margin: 1.4em 0 0.8em 0; }\n"
        "p { margin: 0 0 0.8em 0; text-indent: 1.2em; }\n"
        "p:first-of-type { text-indent: 0; }\n"
        "p.scene-break, p[style*=\"text-align:center\"] { text-align: center; text-indent: 0; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n").arg(m_projectTitle.toHtmlEscaped());
    for (int i = 0; i < scenes.size(); ++i) {
        const EpubWriter::Scene &sc = epubScenes.at(i);
        QString inner = EpubWriter::headingHtml(sc);
        const QString body = EpubWriter::sanitizeBody(sc.bodyHtml);
        if (!inner.isEmpty() && !body.isEmpty())
            inner += QLatin1Char('\n');
        inner += body;
        parts += wrapSceneHtml(scenes.at(i), inner);
    }
    parts += QStringLiteral("</body></html>\n");

    const QString htmlOut = compileDir() + QStringLiteral("/manuscript.html");
    if (!writeTextFile(htmlOut, parts)) {
        if (error)
            *error = QStringLiteral("Compile failed writing HTML.");
        return false;
    }

    QStringList fails;
    const QString epubOut = compileDir() + QStringLiteral("/manuscript.epub");
    QString epubError;
    if (!EpubWriter::write(epubOut, m_projectTitle, m_projectAuthor, epubScenes, &epubError))
        fails.append(QStringLiteral("EPUB: %1").arg(epubError));

    const QString docxOut = compileDir() + QStringLiteral("/manuscript.docx");
    QString docxError;
    const QString kindle = kindleCompileHtml(m_projectTitle, m_projectAuthor, epubScenes);
    if (!DocumentIo::htmlToKindleDocx(docxOut, kindle, &docxError))
        fails.append(QStringLiteral("DOCX: %1").arg(docxError));

    const QString pdfOut = compileDir() + QStringLiteral("/manuscript.pdf");
    QString pdfError;
    const QString pdfHtml = buildPaperbackPdfHtml(m_projectTitle, epubScenes);
    const QString frontHtml = buildPaperbackPdfHtml(m_projectTitle, epubScenes, 0.875, true);
    if (!writePaperbackPdfFile(pdfOut, pdfHtml, &pdfError, 0.875, frontHtml))
        fails.append(QStringLiteral("PDF: %1").arg(pdfError));

    if (!fails.isEmpty()) {
        if (error)
            *error = fails.join(QStringLiteral("; "));
        return false;
    }
    return true;
}

int QuireFrame::runHeadlessCompile(const QString &projectDir)
{
    if (!openProject(projectDir, false)) {
        std::fprintf(stdout, "compiled: epub=fail xhtml=fail pdf=fail heading1:0\n");
        std::fflush(stdout);
        return 1;
    }

    QString err;
    const bool compiled = compileToDisk(&err);
    const QString epub = compileDir() + QStringLiteral("/manuscript.epub");
    const QString docx = compileDir() + QStringLiteral("/manuscript.docx");
    const QString pdf = compileDir() + QStringLiteral("/manuscript.pdf");
    const bool epubOk = QFileInfo::exists(epub);
    const bool pdfOk = QFileInfo::exists(pdf) && QFileInfo(pdf).size() > 10000;
    bool xhtmlOk = false;
    int h1 = 0;

    if (epubOk) {
        xhtmlOk = true;
        bool sawXhtml = false;
        QProcess list;
        list.start(QStringLiteral("unzip"),
                   {QStringLiteral("-Z"), QStringLiteral("-1"), epub});
        if (!list.waitForFinished(15000) || list.exitCode() != 0)
            xhtmlOk = false;
        const QString listing = QString::fromUtf8(list.readAllStandardOutput());
        const QStringList files = listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &f : files) {
            if (!f.startsWith(QLatin1String("OEBPS/")) || !f.endsWith(QLatin1String(".xhtml")))
                continue;
            sawXhtml = true;
            QProcess uz;
            uz.start(QStringLiteral("unzip"), {QStringLiteral("-p"), epub, f});
            if (!uz.waitForFinished(15000) || uz.exitCode() != 0) {
                xhtmlOk = false;
                break;
            }
            QByteArray data = uz.readAllStandardOutput();
            data.replace("<!DOCTYPE html>", "");
            QXmlStreamReader xml(data);
            while (!xml.atEnd())
                xml.readNext();
            if (xml.hasError()) {
                xhtmlOk = false;
                break;
            }
        }
        if (!sawXhtml)
            xhtmlOk = false;
    }

    if (QFileInfo::exists(docx)) {
        QProcess uz;
        uz.start(QStringLiteral("unzip"),
                 {QStringLiteral("-p"), docx, QStringLiteral("word/document.xml")});
        uz.waitForFinished(8000);
        const QString xml = QString::fromUtf8(uz.readAllStandardOutput());
        h1 = xml.count(QStringLiteral("w:pStyle w:val=\"Heading1\""));
    }

    if (!err.isEmpty())
        std::fprintf(stdout, "compile-error: %s\n", qPrintable(err));
    if (!compiled && err.isEmpty())
        std::fprintf(stdout, "compile-error: compileToDisk failed\n");

    std::fprintf(stdout, "compiled: epub=%s xhtml=%s pdf=%s heading1:%d\n",
                 epubOk ? "ok" : "fail",
                 xhtmlOk ? "ok" : "fail",
                 pdfOk ? "ok" : "fail",
                 h1);

    bool gelasioEmbed = false;
    if (epubOk) {
        QProcess list;
        list.start(QStringLiteral("unzip"),
                   {QStringLiteral("-Z"), QStringLiteral("-1"), epub});
        if (list.waitForFinished(15000) && list.exitCode() == 0) {
            const QString listing = QString::fromUtf8(list.readAllStandardOutput());
            gelasioEmbed =
                listing.contains(QLatin1String("OEBPS/fonts/Gelasio-Regular.ttf"))
                && listing.contains(QLatin1String("OEBPS/fonts/Gelasio-Italic.ttf"))
                && listing.contains(QLatin1String("OEBPS/fonts/Gelasio-Bold.ttf"))
                && listing.contains(QLatin1String("OEBPS/fonts/Gelasio-BoldItalic.ttf"));
        }
    }
    std::fprintf(stdout, "font-embed: gelasio=%s\n", gelasioEmbed ? "ok" : "fail");
    std::fflush(stdout);
    return (compiled && epubOk && gelasioEmbed && pdfOk) ? 0 : 1;
}

int QuireFrame::runPrintProof(const QString &projectDir)
{
    if (!openProject(projectDir, false)) {
        std::fprintf(stdout, "print-scope: manuscript=0  default=0  scene=0  h1=0  health: fail\n");
        std::fflush(stdout);
        return 1;
    }

    // Manuscript root selected → whole book.
    if (m_treeView) {
        const QModelIndex msIdx = viewIndexForPath(manuscriptDir());
        if (msIdx.isValid())
            m_treeView->setCurrentIndex(msIdx);
    }

    QString msLabel;
    const QStringList manuscriptScope = resolvePrintScope(&msLabel);
    const int msCount = manuscriptScope.size();

    // Scene selected (default Print path) must also resolve to the whole manuscript.
    QString defaultLabel;
    int defaultCount = 0;
    if (!manuscriptScope.isEmpty()) {
        if (m_treeView) {
            const QModelIndex oneIdx = viewIndexForPath(manuscriptScope.first());
            if (oneIdx.isValid())
                m_treeView->setCurrentIndex(oneIdx);
        }
        const QStringList defScope = resolvePrintScope(&defaultLabel);
        defaultCount = defScope.size();
    }

    // Print Current Scene is always a single open scene (editor path).
    const int sceneCount = manuscriptScope.isEmpty() ? 0 : 1;

    const QString html = buildPrintHtml(manuscriptScope);
    const QString outPath = QDir::temp().filePath(
        QStringLiteral("quire-print-proof-%1.html").arg(QCoreApplication::applicationPid()));
    writeTextFile(outPath, html);

    const int h1 = html.count(QStringLiteral("<h1"));
    const int sceneBreaks = html.count(QStringLiteral("class=\"print-scene\""));
    const qint64 htmlBytes = QFileInfo(outPath).size();
    const bool hasRelFont = html.contains(QStringLiteral("fonts/gelasio/Gelasio-Regular.ttf"));
    const bool hasDataFont = html.contains(QStringLiteral("data:font"));
    const bool htmlLean = (htmlBytes > 0) && (htmlBytes < 2000000) && hasRelFont && !hasDataFont;
    const bool scopeOk = (msCount >= 30) && (defaultCount >= 30) && (defaultCount == msCount)
                         && (sceneCount == 1) && (h1 >= 30) && (sceneBreaks >= 30);

    std::fprintf(stdout,
                 "print-scope: manuscript=%d  default=%d  scene=%d  h1=%d  breaks=%d  html=%s  bytes=%lld  health: %s\n",
                 msCount, defaultCount, sceneCount, h1, sceneBreaks, qPrintable(outPath),
                 static_cast<long long>(htmlBytes),
                 (scopeOk && htmlLean) ? "ok" : "fail");
    std::fprintf(stdout, "print-label: %s | %s\n", qPrintable(msLabel), qPrintable(defaultLabel));
    std::fprintf(stdout,
                 "print-html: rel-font=%s  data-font=%s  lean=%s\n",
                 hasRelFont ? "ok" : "fail",
                 hasDataFont ? "bad" : "ok",
                 htmlLean ? "ok" : "fail");
    std::fflush(stdout);

    // Prove WebEngine can load the staged print HTML and emit a PDF (the failure Noble hit).
    bool pdfOk = false;
    qint64 pdfBytes = 0;
    QString pdfPath = QDir::temp().filePath(
        QStringLiteral("quire-print-proof-%1.pdf").arg(QCoreApplication::applicationPid()));
    QFile::remove(pdfPath);
    if (scopeOk && htmlLean && !manuscriptScope.isEmpty()) {
        QTemporaryDir stage;
        QString stageErr;
        const QString stagedHtml = stage.isValid()
                                       ? stage.filePath(QStringLiteral("print.html"))
                                       : QString();
        const bool staged = stage.isValid()
                            && copyGelasioFontsToDir(stage.filePath(QStringLiteral("fonts/gelasio")),
                                                     &stageErr)
                            && writeTextFile(stagedHtml, html);
        if (!staged) {
            std::fprintf(stdout, "print-pdf: stage-fail %s\n",
                         qPrintable(stageErr.isEmpty() ? QStringLiteral("temp") : stageErr));
        } else {
            QWebEnginePage page;
            QEventLoop loop;
            bool loadOk = false;
            bool pdfDone = false;
            bool pdfSuccess = false;
            QObject::connect(&page, &QWebEnginePage::loadFinished, &loop,
                             [&](bool ok) {
                loadOk = ok;
                if (!ok) {
                    loop.quit();
                    return;
                }
                page.printToPdf(pdfPath, defaultPrintPageLayout());
            });
            QObject::connect(&page, &QWebEnginePage::pdfPrintingFinished, &loop,
                             [&](const QString &path, bool success) {
                Q_UNUSED(path);
                pdfDone = true;
                pdfSuccess = success;
                loop.quit();
            });
            QTimer::singleShot(120000, &loop, &QEventLoop::quit);
            page.load(QUrl::fromLocalFile(stagedHtml));
            loop.exec();
            pdfBytes = QFileInfo(pdfPath).size();
            pdfOk = loadOk && pdfDone && pdfSuccess && pdfBytes > 100000;
            std::fprintf(stdout,
                         "print-pdf: load=%s  done=%s  success=%s  bytes=%lld  path=%s  health: %s\n",
                         loadOk ? "ok" : "fail",
                         pdfDone ? "ok" : "fail",
                         pdfSuccess ? "ok" : "fail",
                         static_cast<long long>(pdfBytes),
                         qPrintable(pdfPath),
                         pdfOk ? "ok" : "fail");
        }
    } else {
        std::fprintf(stdout, "print-pdf: skipped\n");
    }
    std::fflush(stdout);

    const bool ok = scopeOk && htmlLean && pdfOk;
    return ok ? 0 : 1;
}

void QuireFrame::runListenProof()
{
    {
        const QByteArray faces = qgetenv("QUIRE_GELASIO_FACES");
        const bool hasG = QFontDatabase::hasFamily(QStringLiteral("Gelasio"));
        std::fprintf(stdout, "font: gelasio=%s faces=%s\n",
                     hasG ? "ok" : "fail",
                     faces.isEmpty() ? "?" : faces.constData());
        std::fflush(stdout);
    }

    std::fprintf(stdout, "editor: monastery-sep1\n");
    std::fprintf(stdout, "default-root: %s\n", qPrintable(defaultManuscriptsRoot()));

    const QString suffixParent = QStringLiteral("/tmp/quire-suffix-proof-%1")
                                     .arg(QCoreApplication::applicationPid());
    QDir(suffixParent).removeRecursively();
    const QString suffixTitle = projectTitleFromInput(QStringLiteral("Snowflake"));
    const QString suffixPath = suffixParent + QLatin1Char('/')
                               + projectFolderName(QStringLiteral("Snowflake"));
    const bool suffixCreated = createProjectAt(suffixPath, suffixTitle, QStringLiteral("Noble Brown"));
    const QJsonObject suffixMeta = QJsonDocument::fromJson(
        readTextFile(suffixPath + QStringLiteral("/quire.json")).toUtf8()).object();
    const bool suffixOk = suffixCreated
                          && QFileInfo(suffixPath).fileName() == QLatin1String("Snowflake.qr")
                          && suffixMeta.value(QStringLiteral("title")).toString() == QLatin1String("Snowflake")
                          && projectFolderName(QStringLiteral("Already.QR")) == QLatin1String("Already.QR");
    std::fprintf(stdout, "project-suffix: Snowflake.qr  title=Snowflake  %s\n",
                 suffixOk ? "ok" : "fail");
    std::fflush(stdout);

    const QString root = QStringLiteral("/tmp/quire-listen-%1")
                             .arg(QCoreApplication::applicationPid());
    QDir(root).removeRecursively();
    if (!createProjectAt(root, QStringLiteral("Listen Novel"), QStringLiteral("Noble Brown"))) {
        std::fprintf(stdout, "compiled: none  ooxml: no  leftover_html: yes  health: fail\n");
        std::fprintf(stdout, "health: fail\n");
        std::fflush(stdout);
        QTimer::singleShot(400, qApp, []() { QCoreApplication::quit(); });
        return;
    }
    writeTextFile(root + QStringLiteral("/manuscript/Front Matter/Dedication.html"),
                  QStringLiteral("<p>For the night shift.</p>"));
    writeTextFile(root + QStringLiteral("/manuscript/Chapter 1/Scene 2.html"),
                  QStringLiteral("<p>He <strong>stopped</strong> at the <em>threshold</em>.</p>"));
    writeTextFile(root + QStringLiteral("/manuscript/Chapter 2/Scene 1.html"),
                  QStringLiteral("<p>Dawn found the page still open.</p>"));
    writeTextFile(root + QStringLiteral("/manuscript/Chapter 2/The River.html"),
                  QStringLiteral("<p>Water moved under the ice.</p>"));

    if (!openProject(root, false)) {
        std::fprintf(stdout, "compiled: none  ooxml: no  leftover_html: yes  health: fail\n");
        std::fprintf(stdout, "health: fail\n");
        std::fflush(stdout);
        QTimer::singleShot(400, qApp, []() { QCoreApplication::quit(); });
        return;
    }

    QString err;
    const bool compiled = compileToDisk(&err);
    const QString docx = compileDir() + QStringLiteral("/manuscript.docx");
    bool hasCt = false;
    bool hasDoc = false;
    bool leftover = true;
    QString peekErr;
    const bool peeked = compiled && QFileInfo::exists(docx)
                        && DocumentIo::docxPeek(docx, &hasCt, &hasDoc, &leftover, &peekErr);
    const bool ooxml = peeked && hasCt && hasDoc;
    const bool ok = ooxml && !leftover;

    int h1 = 0;
    int h2 = 0;
    if (QFileInfo::exists(docx)) {
        QProcess uz;
        uz.start(QStringLiteral("unzip"),
                 {QStringLiteral("-p"), docx, QStringLiteral("word/document.xml")});
        uz.waitForFinished(5000);
        const QString xml = QString::fromUtf8(uz.readAllStandardOutput());
        h1 = xml.count(QStringLiteral("w:pStyle w:val=\"Heading1\""));
        h2 = xml.count(QStringLiteral("w:pStyle w:val=\"Heading2\""));
    }

    std::fprintf(stdout, "compiled: %s  ooxml: %s  leftover_html: %s  health: %s\n",
                 ok ? qPrintable(docx) : "none",
                 ooxml ? "yes" : "no",
                 leftover ? "yes" : "no",
                 ok ? "ok" : "fail");
    std::fprintf(stdout, "heading1: %d  heading2: %d\n", h1, h2);
    if (!err.isEmpty())
        std::fprintf(stdout, "compile-error: %s\n", qPrintable(err));
    std::fprintf(stdout, "health: %s\n", ok ? "ok" : "fail");
    std::fflush(stdout);

    listenReportFind(QStringLiteral("threshold"));
    listenReportFind(QStringLiteral("ice"));
    listenReportFind(QStringLiteral("zzzxnotinthebook"));

    const QString chapter2 = root + QStringLiteral("/manuscript/Chapter 2");
    const QString mdSrc = root + QStringLiteral("/quire-import-md.md");
    const QString htmlSrc = root + QStringLiteral("/quire-import-html.html");
    writeTextFile(mdSrc,
                  QStringLiteral("# Not A Chapter\n\nquireimportmdxyz\n"));
    writeTextFile(htmlSrc,
                  QStringLiteral("<h1>Not A Chapter</h1><p>quireimporthtmlxyz</p>"));

    auto reportImport = [&](const char *kind, const QString &src) {
        QString importErr;
        const QString dest = importSceneFromFile(src, chapter2, &importErr);
        const bool iok = !dest.isEmpty() && QFileInfo::exists(dest)
                         && manuscriptRelative(dest).startsWith(QStringLiteral("Chapter 2/"));
        const QString name = QFileInfo(iok ? dest : src).completeBaseName();
        std::fprintf(stdout, "import: %s=Chapter 2/%s  %s\n",
                     kind, qPrintable(name), iok ? "ok" : "fail");
        if (!importErr.isEmpty())
            std::fprintf(stdout, "import-error: %s\n", qPrintable(importErr));
        std::fflush(stdout);
    };
    reportImport("md", mdSrc);
    reportImport("html", htmlSrc);
    writeProjectJson();

    listenReportFind(QStringLiteral("quireimportmdxyz"));
    listenReportFind(QStringLiteral("quireimporthtmlxyz"));

    QString err2;
    compileToDisk(&err2);
    int h1b = 0;
    int h2b = 0;
    if (QFileInfo::exists(docx)) {
        QProcess uz2;
        uz2.start(QStringLiteral("unzip"),
                  {QStringLiteral("-p"), docx, QStringLiteral("word/document.xml")});
        uz2.waitForFinished(5000);
        const QString xml2 = QString::fromUtf8(uz2.readAllStandardOutput());
        h1b = xml2.count(QStringLiteral("w:pStyle w:val=\"Heading1\""));
        h2b = xml2.count(QStringLiteral("w:pStyle w:val=\"Heading2\""));
    }
    std::fprintf(stdout, "heading1: %d  heading2: %d\n", h1b, h2b);
    if (!err2.isEmpty())
        std::fprintf(stdout, "compile-error: %s\n", qPrintable(err2));
    std::fflush(stdout);

    QDir().mkpath(notesDir());
    const QString noteName = QStringLiteral("Research.html");
    const QString notePath = notesDir() + QLatin1Char('/') + noteName;
    const bool noteWrote = writeTextFile(
        notePath, QStringLiteral("<h1>Not A Chapter</h1><p>quirenotexyz</p>"));
    const bool noteOk = noteWrote && QFileInfo::exists(notePath) && isNotesPath(notePath)
                        && !m_order.contains(manuscriptRelative(notePath));
    std::fprintf(stdout, "note: notes/%s %s\n", qPrintable(noteName),
                 noteOk ? "ok" : "fail");
    std::fflush(stdout);

    loadScene(notePath);
    QString err3;
    compileToDisk(&err3);
    int h1c = 0;
    int h2c = 0;
    if (QFileInfo::exists(docx)) {
        QProcess uz3;
        uz3.start(QStringLiteral("unzip"),
                  {QStringLiteral("-p"), docx, QStringLiteral("word/document.xml")});
        uz3.waitForFinished(5000);
        const QString xml3 = QString::fromUtf8(uz3.readAllStandardOutput());
        h1c = xml3.count(QStringLiteral("w:pStyle w:val=\"Heading1\""));
        h2c = xml3.count(QStringLiteral("w:pStyle w:val=\"Heading2\""));
    }
    std::fprintf(stdout, "heading1: %d  heading2: %d\n", h1c, h2c);
    if (!err3.isEmpty())
        std::fprintf(stdout, "compile-error: %s\n", qPrintable(err3));
    std::fflush(stdout);
    listenReportFind(QStringLiteral("quirenotexyz"));

    setFocusMode(true);
    QCoreApplication::processEvents();
    std::fprintf(stdout, "focus: on  binder=%s  toolbar=%s\n",
                 (m_treeView && m_treeView->isHidden()) ? "hidden" : "visible",
                 (m_formatToolBar && m_formatToolBar->isHidden()) ? "hidden" : "visible");
    std::fflush(stdout);
    setFocusMode(false);
    QCoreApplication::processEvents();
    std::fprintf(stdout, "focus: off  binder=%s  toolbar=%s\n",
                 (m_treeView && !m_treeView->isHidden()) ? "visible" : "hidden",
                 (m_formatToolBar && !m_formatToolBar->isHidden()) ? "visible" : "hidden");
    std::fflush(stdout);

    auto sceneListenLabel = [this]() -> QString {
        QString rel = manuscriptRelative(m_currentScenePath);
        if (rel.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
            rel.chop(5);
        return rel;
    };

    const QString firstBody = root + QStringLiteral("/manuscript/Chapter 1/Scene 1.html");
    loadScene(firstBody);
    QCoreApplication::processEvents();
    std::fprintf(stdout, "scene: %s\n", qPrintable(sceneListenLabel()));
    std::fflush(stdout);

    onNextScene();
    QCoreApplication::processEvents();
    std::fprintf(stdout, "scene-next: %s\n", qPrintable(sceneListenLabel()));
    std::fflush(stdout);

    onNextScene();
    QCoreApplication::processEvents();
    std::fprintf(stdout, "scene-next: %s\n", qPrintable(sceneListenLabel()));
    std::fflush(stdout);

    onPreviousScene();
    QCoreApplication::processEvents();
    std::fprintf(stdout, "scene-prev: %s\n", qPrintable(sceneListenLabel()));
    std::fflush(stdout);

    setFocusMode(true);
    QCoreApplication::processEvents();
    const QString beforeFocusNext = m_currentScenePath;
    onNextScene();
    QCoreApplication::processEvents();
    const bool binderHidden = m_treeView && m_treeView->isHidden();
    const bool moved = !pathEquals(m_currentScenePath, beforeFocusNext);
    std::fprintf(stdout, "focus-next: %s  binder=%s\n",
                 (moved && binderHidden) ? "ok" : "fail",
                 binderHidden ? "hidden" : "visible");
    std::fflush(stdout);

    QSettings recentSettings(kOrg, kApp);
    const QString savedLast = recentSettings.value(QStringLiteral("lastProject")).toString();
    const QStringList savedRecent = recentSettings.value(QStringLiteral("recentProjects")).toStringList();
    rememberProject(root);
    const QStringList recents = cleanedRecentProjects();
    recentSettings.setValue(QStringLiteral("lastProject"), savedLast);
    recentSettings.setValue(QStringLiteral("recentProjects"), savedRecent);
    std::fprintf(stdout, "recent: %d\n", int(recents.size()));
    std::fflush(stdout);

    {
        if (m_treeView) {
            const QModelIndex msIdx = viewIndexForPath(manuscriptDir());
            if (msIdx.isValid())
                m_treeView->setCurrentIndex(msIdx);
        }
        QString msLabel;
        const QStringList msScope = resolvePrintScope(&msLabel);
        QString defaultLabel;
        int defaultCount = 0;
        if (!msScope.isEmpty() && m_treeView) {
            const QModelIndex oneIdx = viewIndexForPath(msScope.first());
            if (oneIdx.isValid())
                m_treeView->setCurrentIndex(oneIdx);
            defaultCount = resolvePrintScope(&defaultLabel).size();
        }
        const int sceneCount = msScope.isEmpty() ? 0 : 1;
        const QString html = buildPrintHtml(msScope);
        const int h1 = html.count(QStringLiteral("<h1"));
        const int sceneBreaks = html.count(QStringLiteral("class=\"print-scene\""));
        std::fprintf(stdout,
                     "print-scope: manuscript=%d  default=%d  scene=%d  h1=%d  breaks=%d  health: %s\n",
                     int(msScope.size()), defaultCount, sceneCount, h1, sceneBreaks,
                     (msScope.size() >= 4 && defaultCount == int(msScope.size())
                      && sceneCount == 1 && h1 >= 2) ? "ok" : "fail");
        std::fflush(stdout);
    }

    QTimer::singleShot(400, qApp, []() { QCoreApplication::quit(); });
}

QString QuireFrame::sceneTitleFromPath(const QString &path) const
{
    return QFileInfo(path).completeBaseName();
}

QString QuireFrame::autosavePathForScene(const QString &scenePath) const
{
    const QString root = manuscriptDir();
    QString rel = QDir(root).relativeFilePath(scenePath);
    if (rel.startsWith(QLatin1String("..")))
        rel = QFileInfo(scenePath).fileName();
    return autosaveDir() + QLatin1Char('/') + rel;
}

bool QuireFrame::persistCurrentScene(bool markCleanAfter)
{
    if (m_currentScenePath.isEmpty() || !m_editor)
        return false;

    QString html;
    QEventLoop loop;
    QTimer safety;
    safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);
    m_editor->fetchHtml([&](const QString &h) {
        html = h;
        loop.quit();
    });
    safety.start(2000);
    loop.exec();
    if (html.trimmed().isEmpty())
        html = m_editor->lastGoodHtml();

    if (!writeTextFile(m_currentScenePath, html))
        return false;
    if (markCleanAfter)
        m_editor->markClean();
    refreshManuscriptWordCount();
    updateStatus();
    return true;
}

void QuireFrame::loadScene(const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return;
    m_loadingScene = true;
    m_currentScenePath = path;
    QString html = readTextFile(path);
    const QString healed = EpubWriter::healBody(html);
    if (healed != html) {
        html = healed;
        writeTextFile(path, html);
    }
    if (m_editor) {
        m_editor->setHtml(html);
        m_editor->markClean();
    }
    m_loadingScene = false;
    updateWindowTitle();
    refreshManuscriptWordCount();
    updateStatus();
}

void QuireFrame::onTreeClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const QString path = pathFromView(index);
    if (!QFileInfo(path).isFile())
        return;
    if (path == m_currentScenePath)
        return;
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);
    loadScene(path);
}

void QuireFrame::onTreeSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
    updateBinderActionState();
    if (selected.indexes().isEmpty())
        return;
    onTreeClicked(selected.indexes().first());
}

void QuireFrame::onAutoSave()
{
    if (m_loadingScene || m_projectDir.isEmpty() || m_currentScenePath.isEmpty())
        return;
    if (!m_editor || !m_editor->isDirty())
        return;
    const QString html = m_editor->lastGoodHtml();
    if (html.trimmed().isEmpty())
        return;
    writeTextFile(autosavePathForScene(m_currentScenePath), html);
}

QModelIndex QuireFrame::sourceIndex(const QModelIndex &viewIndex) const
{
    if (!viewIndex.isValid())
        return {};
    if (m_sortProxy && viewIndex.model() == m_sortProxy)
        return m_sortProxy->mapToSource(viewIndex);
    return viewIndex;
}

QString QuireFrame::pathFromView(const QModelIndex &viewIndex) const
{
    const QModelIndex src = sourceIndex(viewIndex);
    if (!src.isValid() || !m_fileModel)
        return {};
    return m_fileModel->filePath(src);
}

QModelIndex QuireFrame::viewIndexForPath(const QString &path) const
{
    if (!m_fileModel || path.isEmpty())
        return {};
    const QModelIndex src = m_fileModel->index(path);
    if (!src.isValid())
        return {};
    return m_sortProxy ? m_sortProxy->mapFromSource(src) : src;
}

bool QuireFrame::writeProjectJson()
{
    if (m_projectDir.isEmpty())
        return false;
    QJsonObject meta;
    const QJsonDocument existing = QJsonDocument::fromJson(readTextFile(quireJsonPath()).toUtf8());
    if (existing.isObject())
        meta = existing.object();
    meta.insert(QStringLiteral("title"), m_projectTitle);
    meta.insert(QStringLiteral("author"), m_projectAuthor);
    if (!meta.contains(QStringLiteral("created")))
        meta.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert(QStringLiteral("quire"), kQuireVersion);
    pruneBinderLists();
    QJsonArray orderArr;
    for (const QString &rel : m_order)
        orderArr.append(rel);
    QJsonArray excludeArr;
    for (const QString &rel : m_exclude)
        excludeArr.append(rel);
    meta.insert(QStringLiteral("order"), orderArr);
    meta.insert(QStringLiteral("exclude"), excludeArr);
    return writeTextFile(quireJsonPath(),
                         QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Indented)));
}

void QuireFrame::clearEditor()
{
    m_currentScenePath.clear();
    if (m_editor) {
        m_editor->setHtml(QStringLiteral("<p></p>"));
        m_editor->markClean();
    }
    m_manuscriptWordCount = 0;
    updateWindowTitle();
    updateStatus();
}

void QuireFrame::onRenameItem()
{
    const QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid())
        return;
    const QString path = pathFromView(idx);
    if (isBinderTopFolder(path))
        return;
    m_treeView->edit(idx);
}

void QuireFrame::onDeleteItem()
{
    const QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid())
        return;
    const QString path = pathFromView(idx);
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    const QString canon = info.canonicalFilePath();
    if (canon.isEmpty())
        return;
    if (isBinderTopFolder(path)) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             QStringLiteral("The Manuscript and Notes folders cannot be deleted."));
        return;
    }
    const QString label = info.isDir() ? info.fileName() : info.completeBaseName();
    const auto reply = QMessageBox::question(
        this, QStringLiteral("Delete"),
        QStringLiteral("Delete “%1”? This cannot be undone.").arg(label),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    const QString openCanon = QFileInfo(m_currentScenePath).canonicalFilePath();
    const bool deletingOpen = !openCanon.isEmpty()
        && (openCanon == canon || openCanon.startsWith(canon + QLatin1Char('/')));

    bool ok = false;
    if (info.isDir())
        ok = QDir(path).removeRecursively();
    else
        ok = QFile::remove(path);

    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Quire"),
                             QStringLiteral("Could not delete %1.").arg(label));
        return;
    }
    removePathFromLists(manuscriptRelative(path));
    writeProjectJson();
    if (deletingOpen)
        clearEditor();
    refreshBinderView();
    statusBar()->showMessage(QStringLiteral("Deleted %1").arg(label), 3000);
}

void QuireFrame::onProjectDetails()
{
    if (m_projectDir.isEmpty())
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Project Details"));
    auto *form = new QFormLayout;
    auto *titleEdit = new QLineEdit(m_projectTitle, &dlg);
    auto *authorEdit = new QLineEdit(m_projectAuthor, &dlg);
    form->addRow(QStringLiteral("Title:"), titleEdit);
    form->addRow(QStringLiteral("Author:"), authorEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString title = titleEdit->text().trimmed();
    const QString author = authorEdit->text().trimmed();
    if (title.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Title cannot be empty."));
        return;
    }
    m_projectTitle = title;
    m_projectAuthor = author;
    if (!writeProjectJson()) {
        QMessageBox::warning(this, QStringLiteral("Quire"), QStringLiteral("Could not write quire.json."));
        return;
    }
    updateWindowTitle();
    statusBar()->showMessage(QStringLiteral("Updated project details"), 3000);
}

void QuireFrame::onBinderContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_treeView->indexAt(pos);
    if (idx.isValid())
        m_treeView->setCurrentIndex(idx);

    QMenu menu(this);
    menu.addAction(m_newChapterAction);
    menu.addAction(m_newSceneAction);
    menu.addAction(m_newNoteAction);
    menu.addAction(m_newFolderAction);
    menu.addAction(m_importAction);
    menu.addSeparator();
    menu.addAction(m_moveUpAction);
    menu.addAction(m_moveDownAction);
    menu.addAction(m_includeAction);
    menu.addSeparator();
    menu.addAction(m_renameAction);
    menu.addAction(m_deleteAction);

    updateBinderActionState();
    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

void QuireFrame::onFileRenamed(const QString &path, const QString &oldName, const QString &newName)
{
    const QString oldPath = path + QLatin1Char('/') + oldName;
    const QString newPath = path + QLatin1Char('/') + newName;
    if (m_currentScenePath == oldPath) {
        m_currentScenePath = newPath;
    } else if (m_currentScenePath.startsWith(oldPath + QLatin1Char('/'))) {
        m_currentScenePath = newPath + m_currentScenePath.mid(oldPath.size());
    }
    rewritePathInLists(manuscriptRelative(oldPath), manuscriptRelative(newPath));
    writeProjectJson();
    refreshBinderView();
    updateWindowTitle();
}

void QuireFrame::onMoveUp()
{
    moveCurrent(-1);
}

void QuireFrame::onMoveDown()
{
    moveCurrent(1);
}

void QuireFrame::onToggleIncludeInCompile()
{
    const QModelIndex idx = m_treeView ? m_treeView->currentIndex() : QModelIndex();
    if (!idx.isValid())
        return;
    const QString path = pathFromView(idx);
    if (!isManuscriptPath(path) || isBinderTopFolder(path))
        return;
    const QString rel = manuscriptRelative(path);
    if (rel.isEmpty() || rel.contains(QLatin1String("..")))
        return;

    const bool include = m_includeAction && m_includeAction->isChecked();
    if (include)
        m_exclude.removeAll(rel);
    else if (!m_exclude.contains(rel))
        m_exclude.append(rel);
    writeProjectJson();
    refreshBinderView();
    refreshManuscriptWordCount();
    updateStatus();
}

QString QuireFrame::manuscriptRelative(const QString &absPath) const
{
    if (absPath.isEmpty() || m_projectDir.isEmpty() || !isManuscriptPath(absPath))
        return {};
    const QString rel = QDir(manuscriptDir()).relativeFilePath(absPath);
    if (rel.isEmpty() || rel == QLatin1String(".") || rel.contains(QLatin1String("..")))
        return {};
    return QDir::fromNativeSeparators(rel);
}

void QuireFrame::loadBinderLists(const QJsonObject &obj)
{
    m_order.clear();
    m_exclude.clear();
    const QJsonArray orderArr = obj.value(QStringLiteral("order")).toArray();
    for (const QJsonValue &v : orderArr) {
        const QString rel = QDir::fromNativeSeparators(v.toString().trimmed());
        if (!rel.isEmpty() && !m_order.contains(rel))
            m_order.append(rel);
    }
    const QJsonArray excludeArr = obj.value(QStringLiteral("exclude")).toArray();
    for (const QJsonValue &v : excludeArr) {
        const QString rel = QDir::fromNativeSeparators(v.toString().trimmed());
        if (!rel.isEmpty() && !m_exclude.contains(rel))
            m_exclude.append(rel);
    }
    pruneBinderLists();
}

void QuireFrame::pruneBinderLists()
{
    if (m_projectDir.isEmpty())
        return;
    const QString root = manuscriptDir();
    auto keep = [&](QStringList *list) {
        QStringList next;
        next.reserve(list->size());
        for (const QString &rel : *list) {
            if (rel.isEmpty() || rel.contains(QLatin1String("..")))
                continue;
            if (QFileInfo::exists(root + QLatin1Char('/') + rel))
                next.append(rel);
        }
        *list = next;
    };
    keep(&m_order);
    keep(&m_exclude);
}

void QuireFrame::refreshBinderView()
{
    if (m_sortProxy) {
        m_sortProxy->invalidate();
        m_sortProxy->sort(0, Qt::AscendingOrder);
    }
    if (m_treeView && m_treeView->viewport())
        m_treeView->viewport()->update();
}

void QuireFrame::rewritePathInLists(const QString &oldRel, const QString &newRel)
{
    if (oldRel.isEmpty() || oldRel == newRel)
        return;
    auto rewrite = [&](QStringList *list) {
        for (QString &rel : *list) {
            if (rel == oldRel)
                rel = newRel;
            else if (rel.startsWith(oldRel + QLatin1Char('/')))
                rel = newRel + rel.mid(oldRel.size());
        }
    };
    rewrite(&m_order);
    rewrite(&m_exclude);
}

void QuireFrame::removePathFromLists(const QString &rel)
{
    if (rel.isEmpty())
        return;
    auto drop = [&](QStringList *list) {
        QStringList next;
        for (const QString &item : *list) {
            if (item == rel || item.startsWith(rel + QLatin1Char('/')))
                continue;
            next.append(item);
        }
        *list = next;
    };
    drop(&m_order);
    drop(&m_exclude);
}

bool QuireFrame::isSelfExcluded(const QString &absPath) const
{
    const QString rel = manuscriptRelative(absPath);
    return !rel.isEmpty() && m_exclude.contains(rel);
}

bool QuireFrame::isCompileExcluded(const QString &absPath) const
{
    QString rel = manuscriptRelative(absPath);
    while (!rel.isEmpty()) {
        if (m_exclude.contains(rel))
            return true;
        const int slash = rel.lastIndexOf(QLatin1Char('/'));
        rel = (slash < 0) ? QString() : rel.left(slash);
    }
    return false;
}

bool QuireFrame::binderLessThan(const QString &relA, const QString &nameA, bool dirA,
                               const QString &relB, const QString &nameB, bool dirB) const
{
    const int idxA = relA.isEmpty() ? -1 : m_order.indexOf(relA);
    const int idxB = relB.isEmpty() ? -1 : m_order.indexOf(relB);
    if (idxA >= 0 && idxB >= 0)
        return idxA < idxB;
    if (idxA >= 0 && idxB < 0)
        return true;
    if (idxA < 0 && idxB >= 0)
        return false;
    if (dirA != dirB)
        return dirA;
    QCollator col;
    col.setNumericMode(true);
    col.setCaseSensitivity(Qt::CaseInsensitive);
    return col.compare(nameA, nameB) < 0;
}

void QuireFrame::moveCurrent(int delta)
{
    if (!m_treeView || delta == 0)
        return;
    const QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid())
        return;
    const QString path = pathFromView(idx);
    if (path.isEmpty() || !isManuscriptPath(path) || isBinderTopFolder(path))
        return;

    const QModelIndex parent = idx.parent();
    const int rows = m_treeView->model()->rowCount(parent);
    const int dest = idx.row() + delta;
    if (dest < 0 || dest >= rows)
        return;

    QStringList sibs;
    sibs.reserve(rows);
    for (int r = 0; r < rows; ++r) {
        const QString rel = manuscriptRelative(pathFromView(m_treeView->model()->index(r, 0, parent)));
        if (!rel.isEmpty())
            sibs.append(rel);
    }
    const QString currentRel = manuscriptRelative(path);
    const int from = sibs.indexOf(currentRel);
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= sibs.size())
        return;
    sibs.swapItemsAt(from, to);

    int insertAt = m_order.size();
    for (const QString &rel : sibs) {
        const int i = m_order.indexOf(rel);
        if (i >= 0 && i < insertAt)
            insertAt = i;
    }
    for (const QString &rel : sibs)
        m_order.removeAll(rel);
    for (int i = 0; i < sibs.size(); ++i)
        m_order.insert(insertAt + i, sibs.at(i));

    writeProjectJson();
    refreshBinderView();
    const QModelIndex restored = viewIndexForPath(path);
    if (restored.isValid())
        m_treeView->setCurrentIndex(restored);
    updateBinderActionState();
}

void QuireFrame::updateBinderActionState()
{
    const QModelIndex idx = m_treeView ? m_treeView->currentIndex() : QModelIndex();
    const QString path = idx.isValid() ? pathFromView(idx) : QString();
    const bool notes = isNotesPath(path);
    const bool manuscript = isManuscriptPath(path);
    const bool top = isBinderTopFolder(path);
    const bool usable = idx.isValid() && !top && (notes || manuscript);
    const bool manuscriptItem = usable && manuscript;
    if (m_renameAction)
        m_renameAction->setEnabled(usable);
    if (m_deleteAction)
        m_deleteAction->setEnabled(usable);
    if (m_includeAction) {
        m_includeAction->setVisible(!notes);
        m_includeAction->setEnabled(manuscriptItem);
        const QSignalBlocker blocker(m_includeAction);
        m_includeAction->setChecked(manuscriptItem && !isSelfExcluded(path));
    }
    int row = idx.row();
    int rows = 0;
    if (idx.isValid() && m_treeView && m_treeView->model())
        rows = m_treeView->model()->rowCount(idx.parent());
    if (m_moveUpAction)
        m_moveUpAction->setEnabled(manuscriptItem && row > 0);
    if (m_moveDownAction)
        m_moveDownAction->setEnabled(manuscriptItem && row >= 0 && row + 1 < rows);
}


void QuireFrame::onSelectionFontChanged(const QString &family, int pt)
{
    if ((m_fontCombo && m_fontCombo->hasFocus()) || (m_sizeCombo && m_sizeCombo->hasFocus()))
        return;
    showFamilyInCombo(m_fontCombo, family);
    showSizeInCombo(m_sizeCombo, pt);
}

void QuireFrame::collectNotesHtml(const QString &dir, QStringList *out) const
{
    if (!out || dir.isEmpty())
        return;
    QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &a, const QFileInfo &b) {
        if (a.isDir() != b.isDir())
            return a.isDir();
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        return col.compare(a.fileName(), b.fileName()) < 0;
    });
    for (const QFileInfo &e : entries) {
        if (e.isDir())
            collectNotesHtml(e.absoluteFilePath(), out);
        else if (e.suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0)
            out->append(e.absoluteFilePath());
    }
}

QStringList QuireFrame::resolvePrintScope(QString *labelOut) const
{
    QStringList paths;
    QString sel;
    if (m_treeView) {
        const QModelIndex idx = m_treeView->currentIndex();
        if (idx.isValid())
            sel = pathFromView(idx);
    }

    auto finish = [&](const QString &label) -> QStringList {
        if (labelOut)
            *labelOut = label;
        return paths;
    };

    auto wholeManuscript = [&]() -> QStringList {
        paths.clear();
        if (!m_projectDir.isEmpty())
            collectScenes(manuscriptDir(), &paths);
        if (paths.isEmpty())
            return finish(QStringLiteral("(empty)"));
        return finish(QStringLiteral("manuscript (%1 scenes)").arg(paths.size()));
    };

    if (!sel.isEmpty() && !m_projectDir.isEmpty()) {
        const QFileInfo fi(sel);
        if (pathEquals(sel, manuscriptDir())) {
            collectScenes(manuscriptDir(), &paths);
            return finish(QStringLiteral("manuscript (%1 scenes)").arg(paths.size()));
        }
        if (fi.isDir() && isManuscriptPath(sel)) {
            collectScenes(sel, &paths);
            return finish(QStringLiteral("%1 (%2 scenes)").arg(fi.fileName()).arg(paths.size()));
        }
        if (pathEquals(sel, notesDir()) || (fi.isDir() && isNotesPath(sel))) {
            collectNotesHtml(sel, &paths);
            const QString name = pathEquals(sel, notesDir())
                                     ? QStringLiteral("notes")
                                     : fi.fileName();
            return finish(QStringLiteral("%1 (%2 notes)").arg(name).arg(paths.size()));
        }
        // Single scene (or other leaf): File→Print prints the book, not the leaf.
        if (fi.isFile()
            && fi.suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0
            && (isManuscriptPath(sel) || isNotesPath(sel))) {
            return wholeManuscript();
        }
    }

    // Nothing useful selected → whole manuscript (not the open editor scene).
    return wholeManuscript();
}

QString QuireFrame::buildPrintHtml(const QStringList &scenePaths) const
{
    QVector<EpubWriter::Scene> epubScenes;
    epubScenes.reserve(scenePaths.size());
    QString lastChapter;
    const QString msRoot = manuscriptDir();
    for (const QString &path : scenePaths) {
        EpubWriter::Scene sc;
        sc.title = sceneTitleFromPath(path);
        const QString raw = readTextFile(path);
        const QString healed = EpubWriter::healBody(raw);
        sc.bodyHtml = sceneBody(healed);
        if (isManuscriptPath(path)) {
            QString rel = QDir(msRoot).relativeFilePath(QFileInfo(path).absolutePath());
            if (!rel.isEmpty() && rel != QLatin1String("."))
                sc.folderTrail = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            annotateCompileScene(&sc, &lastChapter);
        } else {
            // Notes: treat each file as its own titled block without chapter machinery.
            sc.frontMatter = true;
        }
        epubScenes.append(sc);
    }

    QString parts;
    parts += QStringLiteral(
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>%1</title>\n"
        "<style>\n"
        "%2"
        "html, body {\n"
        "  font-family: Gelasio, Georgia, \"Times New Roman\", serif;\n"
        "  font-size: 12pt;\n"
        "  line-height: 1.35;\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "  color: #000;\n"
        "  background: #fff;\n"
        "}\n"
        "body { margin: 0.6in; }\n"
        "h1, h1.chapter { font-size: 1.6em; font-weight: bold; text-align: center; margin: 1.6em 0 1em 0; }\n"
        "h2 { font-size: 1.2em; font-weight: bold; margin: 1.2em 0 0.7em 0; }\n"
        "p { margin: 0 0 0.8em 0; text-indent: 1.2em; }\n"
        "p:first-of-type { text-indent: 0; }\n"
        "p.scene-break, p[style*=\"text-align:center\"] { text-align: center; text-indent: 0; }\n"
        ".print-scene { page-break-before: always; break-before: page; }\n"
        ".print-scene:first-child { page-break-before: auto; break-before: auto; }\n"
        ".page-break, .pagebreak {\n"
        "  break-after: page;\n"
        "  page-break-after: always;\n"
        "  border: none;\n"
        "  margin: 0;\n"
        "  height: 0;\n"
        "}\n"
        "@media print {\n"
        "  html, body { color: #000 !important; background: #fff !important; }\n"
        "}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n")
        .arg(m_projectTitle.toHtmlEscaped(), gelasioPrintFontFaceCss());

    for (int i = 0; i < scenePaths.size(); ++i) {
        const EpubWriter::Scene &sc = epubScenes.at(i);
        QString inner = EpubWriter::headingHtml(sc);
        if (sc.frontMatter && !isManuscriptPath(scenePaths.at(i)) && !sc.title.isEmpty())
            inner = QStringLiteral("<h1 class=\"chapter\">%1</h1>\n").arg(sc.title.toHtmlEscaped());
        const QString body = EpubWriter::sanitizeBody(sc.bodyHtml);
        if (!inner.isEmpty() && !body.isEmpty())
            inner += QLatin1Char('\n');
        inner += body;
        const QString cls = (i == 0)
                                ? QStringLiteral("print-scene first")
                                : QStringLiteral("print-scene");
        parts += QStringLiteral("<section class=\"%1\" data-scene=\"%2\">\n%3\n</section>\n")
                     .arg(cls,
                          QFileInfo(scenePaths.at(i)).fileName().toHtmlEscaped(),
                          inner);
    }
    parts += QStringLiteral("</body></html>\n");
    return parts;
}

void QuireFrame::beginScopedPrint(const QString &html, const QPageLayout &layout,
                                  const QString &outputPdf, bool cupsJob)
{
    if (m_printPage) {
        m_printPage->deleteLater();
        m_printPage = nullptr;
    }
    if (m_printHtmlDir) {
        delete m_printHtmlDir;
        m_printHtmlDir = nullptr;
    }

    auto *dir = new QTemporaryDir();
    if (!dir->isValid()) {
        delete dir;
        statusBar()->showMessage(QStringLiteral("Print failed: temp dir"));
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not create a temporary print directory."));
        m_pendingLpPdf.clear();
        m_pendingLpPrinter.clear();
        m_pendingLpCopies = 1;
        return;
    }
    m_printHtmlDir = dir;

    const QString htmlPath = dir->filePath(QStringLiteral("print.html"));
    QString fontErr;
    if (!copyGelasioFontsToDir(dir->filePath(QStringLiteral("fonts/gelasio")), &fontErr)) {
        delete m_printHtmlDir;
        m_printHtmlDir = nullptr;
        statusBar()->showMessage(QStringLiteral("Print failed: fonts"));
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             fontErr.isEmpty()
                                 ? QStringLiteral("Could not stage Gelasio fonts for printing.")
                                 : fontErr);
        m_pendingLpPdf.clear();
        m_pendingLpPrinter.clear();
        m_pendingLpCopies = 1;
        return;
    }
    if (!writeTextFile(htmlPath, html)) {
        delete m_printHtmlDir;
        m_printHtmlDir = nullptr;
        statusBar()->showMessage(QStringLiteral("Print failed: write HTML"));
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not write the print HTML:\n%1").arg(htmlPath));
        m_pendingLpPdf.clear();
        m_pendingLpPrinter.clear();
        m_pendingLpCopies = 1;
        return;
    }

    auto *page = new QWebEnginePage(this);
    m_printPage = page;
    connect(page, &QWebEnginePage::pdfPrintingFinished,
            this, &QuireFrame::onPdfPrintingFinished);

    Q_UNUSED(cupsJob);
    const qint64 htmlBytes = QFileInfo(htmlPath).size();
    QMetaObject::Connection *conn = new QMetaObject::Connection;
    *conn = QObject::connect(page, &QWebEnginePage::loadFinished, this,
                             [this, page, layout, outputPdf, conn, htmlPath, htmlBytes](bool ok) {
        if (conn) {
            QObject::disconnect(*conn);
            delete conn;
        }
        if (page != m_printPage)
            return;
        if (!ok) {
            statusBar()->showMessage(QStringLiteral("Print failed: could not load document"));
            QMessageBox::warning(
                this, QStringLiteral("Print Failed"),
                QStringLiteral("Could not load the print document.\n%1\nsize=%2 bytes")
                    .arg(htmlPath)
                    .arg(htmlBytes));
            if (m_printPage == page) {
                m_printPage = nullptr;
                page->deleteLater();
            }
            if (m_printHtmlDir) {
                delete m_printHtmlDir;
                m_printHtmlDir = nullptr;
            }
            m_pendingLpPdf.clear();
            m_pendingLpPrinter.clear();
            m_pendingLpCopies = 1;
            return;
        }
        page->printToPdf(outputPdf, layout);
    });

    page->load(QUrl::fromLocalFile(htmlPath));
}

void QuireFrame::onPrint()
{
    if (!m_editor || !m_editor->webView() || !m_editor->webView()->page())
        return;

    QPrinter printer(QPrinter::HighResolution);
    const QString defName = QPrinterInfo::defaultPrinterName();
    if (!defName.isEmpty())
        printer.setPrinterName(defName);
    printer.setPageLayout(defaultPrintPageLayout());

    QPrintDialog dialog(&printer, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);

    QString scopeLabel;
    const QStringList scope = resolvePrintScope(&scopeLabel);
    if (scope.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Print"),
                                 QStringLiteral("Nothing to print. Open a project with manuscript scenes."));
        return;
    }

    const QPageLayout layout = pageLayoutFromPrinter(printer);
    const bool toFile = printer.outputFormat() == QPrinter::PdfFormat
                        || !printer.outputFileName().isEmpty();

    auto statusPrinting = [&]() {
        statusBar()->showMessage(QStringLiteral("Printing %1…").arg(scopeLabel));
    };

    const QString html = buildPrintHtml(scope);
    if (toFile) {
        QString fileName = printer.outputFileName();
        if (fileName.isEmpty())
            return;
        if (!fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
            fileName += QStringLiteral(".pdf");
        beginScopedPrint(html, layout, fileName, false);
        statusPrinting();
        return;
    }

    if (m_printTemp) {
        m_printTemp->deleteLater();
        m_printTemp = nullptr;
    }
    auto *tmp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/quire-print-XXXXXX.pdf"), this);
    tmp->setAutoRemove(true);
    if (!tmp->open()) {
        delete tmp;
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not create a temporary PDF."));
        return;
    }
    const QString tmpPath = tmp->fileName();
    tmp->close();
    m_printTemp = tmp;
    m_pendingLpPdf = tmpPath;
    m_pendingLpPrinter = printer.printerName();
    m_pendingLpCopies = qMax(1, printer.copyCount());
    beginScopedPrint(html, layout, tmpPath, true);
    statusPrinting();
}

void QuireFrame::onPrintCurrentScene()
{
    if (!m_editor || !m_editor->webView() || !m_editor->webView()->page())
        return;

    QPrinter printer(QPrinter::HighResolution);
    const QString defName = QPrinterInfo::defaultPrinterName();
    if (!defName.isEmpty())
        printer.setPrinterName(defName);
    printer.setPageLayout(defaultPrintPageLayout());

    QPrintDialog dialog(&printer, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QPageLayout layout = pageLayoutFromPrinter(printer);
    const bool toFile = printer.outputFormat() == QPrinter::PdfFormat
                        || !printer.outputFileName().isEmpty();

    const QString scopeLabel = m_currentScenePath.isEmpty()
                                   ? QStringLiteral("current scene")
                                   : QStringLiteral("scene: %1").arg(sceneTitleFromPath(m_currentScenePath));
    auto statusPrinting = [&]() {
        statusBar()->showMessage(QStringLiteral("Printing %1…").arg(scopeLabel));
    };

    // Editor page (unsaved buffer OK) — do not persist first.
    if (toFile) {
        QString fileName = printer.outputFileName();
        if (fileName.isEmpty())
            return;
        if (!fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
            fileName += QStringLiteral(".pdf");
        m_editor->webView()->page()->printToPdf(fileName, layout);
        statusPrinting();
        statusBar()->showMessage(QStringLiteral("Printing %1 to file…").arg(scopeLabel));
        return;
    }

    if (m_printTemp) {
        m_printTemp->deleteLater();
        m_printTemp = nullptr;
    }
    auto *tmp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/quire-print-XXXXXX.pdf"), this);
    tmp->setAutoRemove(true);
    if (!tmp->open()) {
        delete tmp;
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not create a temporary PDF."));
        return;
    }
    const QString tmpPath = tmp->fileName();
    tmp->close();
    m_printTemp = tmp;
    m_pendingLpPdf = tmpPath;
    m_pendingLpPrinter = printer.printerName();
    m_pendingLpCopies = qMax(1, printer.copyCount());
    m_editor->webView()->page()->printToPdf(tmpPath, layout);
    statusPrinting();
}

void QuireFrame::onPdfPrintingFinished(const QString &path, bool success)
{
    QWebEnginePage *finishing = qobject_cast<QWebEnginePage *>(sender());
    auto releasePrintPage = [&]() {
        if (finishing && finishing == m_printPage) {
            m_printPage = nullptr;
            finishing->deleteLater();
        }
        if (m_printHtmlDir) {
            delete m_printHtmlDir;
            m_printHtmlDir = nullptr;
        }
    };

    const bool cupsJob = !m_pendingLpPdf.isEmpty() && path == m_pendingLpPdf;
    if (cupsJob) {
        const QString pdf = m_pendingLpPdf;
        const QString printerName = m_pendingLpPrinter;
        const int copies = m_pendingLpCopies;
        m_pendingLpPdf.clear();
        m_pendingLpPrinter.clear();
        m_pendingLpCopies = 1;

        auto cleanupTemp = [this]() {
            if (m_printTemp) {
                m_printTemp->deleteLater();
                m_printTemp = nullptr;
            }
        };

        if (!success) {
            cleanupTemp();
            releasePrintPage();
            QMessageBox::warning(this, QStringLiteral("Print Failed"),
                                 QStringLiteral("Could not render the page for printing."));
            return;
        }

        const QStringList args = cupsLpArgv(printerName, copies, pdf);
        QProcess *lp = new QProcess(this);
        connect(lp, &QProcess::finished, this,
                [this, lp, finishing](int code, QProcess::ExitStatus st) {
            const QByteArray err = lp->readAllStandardError();
            lp->deleteLater();
            if (m_printTemp) {
                m_printTemp->deleteLater();
                m_printTemp = nullptr;
            }
            if (finishing && finishing == m_printPage) {
                m_printPage = nullptr;
                finishing->deleteLater();
            }
            if (m_printHtmlDir) {
                delete m_printHtmlDir;
                m_printHtmlDir = nullptr;
            }
            if (code != 0 || st != QProcess::NormalExit) {
                const QString msg = err.isEmpty()
                    ? QStringLiteral("lp failed")
                    : QString::fromLocal8Bit(err);
                QMessageBox::warning(this, QStringLiteral("Print Failed"), msg);
            } else {
                statusBar()->showMessage(QStringLiteral("Sent to printer"));
            }
        });
        lp->start(QStringLiteral("lp"), args);
        if (!lp->waitForStarted(3000)) {
            QMessageBox::warning(this, QStringLiteral("Print Failed"),
                                 QStringLiteral("Could not start lp."));
            lp->deleteLater();
            cleanupTemp();
            releasePrintPage();
        }
        return;
    }

    if (!success)
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not write the print file:\n") + path);
    else
        statusBar()->showMessage(QStringLiteral("Printed to: ") + path);
    releasePrintPage();
}

void QuireFrame::onFontChanged(const QString &font)
{
    if (m_editor)
        m_editor->applyFontFamily(font);
}

void QuireFrame::onSizeChanged(const QString &size)
{
    bool ok = false;
    const int pt = size.toInt(&ok);
    if (!ok || pt <= 0)
        return;
    if (m_editor)
        m_editor->applyFontSize(pt);
}

void QuireFrame::appendToOrder(const QString &absPath)
{
    if (!isManuscriptPath(absPath))
        return;
    const QString rel = manuscriptRelative(absPath);
    if (rel.isEmpty() || rel.contains(QLatin1String("..")) || m_order.contains(rel))
        return;
    m_order.append(rel);
}

void QuireFrame::syncOrderFromDisk()
{
    if (m_projectDir.isEmpty())
        return;
    pruneBinderLists();
    syncOrderWalk(manuscriptDir());
}

void QuireFrame::syncOrderWalk(const QString &dir)
{
    QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QFileInfoList missing;
    for (const QFileInfo &e : entries) {
        if (e.isFile() && e.suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) != 0)
            continue;
        const QString rel = manuscriptRelative(e.absoluteFilePath());
        if (rel.isEmpty() || m_order.contains(rel))
            continue;
        missing.append(e);
    }
    std::sort(missing.begin(), missing.end(), [](const QFileInfo &a, const QFileInfo &b) {
        const QDateTime ma = a.lastModified();
        const QDateTime mb = b.lastModified();
        if (ma != mb)
            return ma < mb;
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        return col.compare(a.fileName(), b.fileName()) < 0;
    });
    for (const QFileInfo &e : missing)
        m_order.append(manuscriptRelative(e.absoluteFilePath()));
    for (const QFileInfo &e : entries) {
        if (e.isDir())
            syncOrderWalk(e.absoluteFilePath());
    }
}

void QuireFrame::refreshManuscriptWordCount()
{
    if (m_projectDir.isEmpty()) {
        m_manuscriptWordCount = 0;
        return;
    }
    QStringList scenes;
    collectScenes(manuscriptDir(), &scenes);
    int total = 0;
    const QString dirtyHtml = (m_editor && m_editor->isDirty()) ? m_editor->lastGoodHtml() : QString();
    for (const QString &path : scenes) {
        QString html;
        if (path == m_currentScenePath && m_editor && m_editor->isDirty()
            && !dirtyHtml.trimmed().isEmpty()) {
            html = dirtyHtml;
        } else {
            html = readTextFile(path);
        }
        total += countWordsInHtml(html);
    }
    m_manuscriptWordCount = total;
}

void QuireFrame::updateStatus()
{
    const int words = m_editor ? m_editor->getWordCount() : 0;
    const QLocale loc;
    m_wordCountLabel->setText(QStringLiteral("Words: %1 / %2")
                                  .arg(loc.toString(words), loc.toString(m_manuscriptWordCount)));
    const bool dirty = m_editor && m_editor->isDirty();
    m_dirtyLabel->setText(dirty ? QStringLiteral("Modified") : QStringLiteral("Saved"));
    if (m_sceneLabel) {
        if (m_currentScenePath.isEmpty())
            m_sceneLabel->clear();
        else
            m_sceneLabel->setText(QStringLiteral("Scene: %1").arg(sceneBinderLabel(m_currentScenePath)));
    }
    updateWindowTitle();
}

void QuireFrame::updateWindowTitle()
{
    QString t = QStringLiteral("Quire");
    if (!m_projectTitle.isEmpty())
        t += QStringLiteral(" — %1").arg(m_projectTitle);
    if (!m_currentScenePath.isEmpty())
        t += QStringLiteral(" — %1").arg(sceneTitleFromPath(m_currentScenePath));
    if (m_editor && m_editor->isDirty())
        t += QStringLiteral(" *");
    setWindowTitle(t);
}

void QuireFrame::applyUiFont(const Theme &theme)
{
    QFont ui;
    if (theme.themeId == ThemeId::WordPerfect)
        ui.setFamilies({"IBM Plex Mono", "Fixed", "Courier New", "DejaVu Sans Mono", "sans-serif"});
    else if (theme.themeId == ThemeId::Leather)
        ui.setFamilies({"Gelasio", "Georgia", "Noto Serif", "serif"});
    else
        ui.setFamilies({"Courier New", "Liberation Mono", "DejaVu Sans Mono", "monospace"});
    ui.setPointSize(10);
    setFont(ui);
    menuBar()->setFont(ui);
    if (statusBar()) {
        QFont status = ui;
        status.setPointSize(8);
        statusBar()->setFont(status);
    }
}


QString QuireFrame::sceneBinderLabel(const QString &path) const
{
    if (isNotesPath(path)) {
        QString rel = QDir(notesDir()).relativeFilePath(path);
        if (rel.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
            rel.chop(5);
        rel.replace(QLatin1Char('/'), QStringLiteral(" / "));
        if (rel.isEmpty() || rel == QLatin1String("."))
            return sceneTitleFromPath(path);
        return QStringLiteral("Notes / %1").arg(rel);
    }
    QString rel = manuscriptRelative(path);
    if (rel.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
        rel.chop(5);
    rel.replace(QLatin1Char('/'), QStringLiteral(" / "));
    if (rel.isEmpty())
        return sceneTitleFromPath(path);
    return rel;
}

QString QuireFrame::sceneHtmlForFind(const QString &path) const
{
    if (path == m_currentScenePath && m_editor && m_editor->isDirty()) {
        const QString dirty = m_editor->lastGoodHtml();
        if (!dirty.trimmed().isEmpty())
            return dirty;
    }
    return readTextFile(path);
}

QString QuireFrame::nextSceneWithNeedle(const QString &needle, bool backward) const
{
    // Walk ALL manuscript .html in binder order, including excluded, so
    // research text is still findable. This scene is skipped; caller already
    // searched the open buffer.
    if (needle.isEmpty() || m_projectDir.isEmpty())
        return {};
    QStringList scenes;
    collectScenes(manuscriptDir(), &scenes, true);
    const int n = scenes.size();
    if (n <= 0)
        return {};
    int start = scenes.indexOf(m_currentScenePath);
    if (start < 0)
        start = backward ? 0 : n - 1;
    for (int i = 1; i < n; ++i) {
        const int idx = backward ? (start - i + n) % n : (start + i) % n;
        const QString &path = scenes.at(idx);
        if (path == m_currentScenePath)
            continue;
        if (htmlContainsNeedle(sceneHtmlForFind(path), needle))
            return path;
    }
    return {};
}

QString QuireFrame::firstSceneWithNeedle(const QString &needle) const
{
    if (needle.isEmpty() || m_projectDir.isEmpty())
        return {};
    QStringList scenes;
    collectScenes(manuscriptDir(), &scenes, true);
    for (const QString &path : scenes) {
        if (htmlContainsNeedle(sceneHtmlForFind(path), needle))
            return path;
    }
    return {};
}

void QuireFrame::activateSceneForFind(const QString &path)
{
    if (path.isEmpty() || path == m_currentScenePath)
        return;
    // Same dirty policy as a binder click: persist then load. No new autosave rules.
    if (m_editor && m_editor->isDirty())
        persistCurrentScene(true);
    const QModelIndex idx = viewIndexForPath(path);
    if (idx.isValid())
        m_treeView->setCurrentIndex(idx);
    if (m_currentScenePath != path)
        loadScene(path);
}

void QuireFrame::listenReportFind(const QString &query)
{
    const QString hit = firstSceneWithNeedle(query);
    if (hit.isEmpty()) {
        std::fprintf(stdout, "find: query=%s  hit: no\n", qPrintable(query));
        std::fflush(stdout);
        return;
    }
    activateSceneForFind(hit);
    QString rel = manuscriptRelative(hit);
    if (rel.endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
        rel.chop(5);
    std::fprintf(stdout, "find: query=%s  scene=%s  hit: yes\n",
                 qPrintable(query), qPrintable(rel));
    std::fflush(stdout);
}

void QuireFrame::setupFindDialog()
{
    if (m_findDialog)
        return;

    m_findDialog = new QDialog(this);
    m_findDialog->setWindowTitle(QStringLiteral("Find"));
    m_findDialog->setModal(false);
    auto *layout = new QVBoxLayout(m_findDialog);
    layout->addWidget(new QLabel(QStringLiteral("Find:"), m_findDialog));
    m_findEdit = new QLineEdit(m_findDialog);
    layout->addWidget(m_findEdit);

    m_findManuscriptCheck = new QCheckBox(QStringLiteral("Manuscript"), m_findDialog);
    m_findManuscriptCheck->setChecked(true);
    m_findManuscriptCheck->setToolTip(QStringLiteral(
        "Search every manuscript scene in binder order, including excluded. "
        "Uncheck to search only this scene."));
    layout->addWidget(m_findManuscriptCheck);

    auto *buttons = new QHBoxLayout;
    auto *nextBtn = new QPushButton(QStringLiteral("Next"), m_findDialog);
    auto *prevBtn = new QPushButton(QStringLiteral("Previous"), m_findDialog);
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), m_findDialog);
    buttons->addWidget(prevBtn);
    buttons->addWidget(nextBtn);
    buttons->addWidget(closeBtn);
    layout->addLayout(buttons);

    connect(nextBtn, &QPushButton::clicked, this, &QuireFrame::onFindNext);
    connect(prevBtn, &QPushButton::clicked, this, &QuireFrame::onFindPrevious);
    connect(closeBtn, &QPushButton::clicked, m_findDialog, &QDialog::hide);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &QuireFrame::onFindNext);
}

void QuireFrame::runFind(bool backward)
{
    if (!m_findEdit || !m_editor || !m_editor->webView() || !m_editor->webView()->page())
        return;
    const QString needle = m_findEdit->text();
    if (needle.isEmpty())
        return;
    QWebEnginePage::FindFlags flags{};
    if (backward)
        flags |= QWebEnginePage::FindBackward;
    QWebEnginePage *page = m_editor->webView()->page();
    const bool manuscript = m_findManuscriptCheck && m_findManuscriptCheck->isChecked();
    if (!manuscript) {
        page->findText(needle, flags);
        return;
    }

    const int oldOff = runJsInt(page, kFindSelOffsetJs, 0);
    bool found = false;
    bool done = false;
    QEventLoop loop;
    QTimer safety;
    safety.setSingleShot(true);
    QObject::connect(&safety, &QTimer::timeout, &loop, &QEventLoop::quit);
    page->findText(needle, flags, [&](const QWebEngineFindTextResult &r) {
        found = r.numberOfMatches() > 0;
        done = true;
        loop.quit();
    });
    if (!done) {
        safety.start(2000);
        loop.exec();
    }
    const int newOff = runJsInt(page, kFindSelOffsetJs, oldOff);
    const bool sameHunt = (m_findNeedle == needle && m_findScene == m_currentScenePath && m_findHadHit);
    bool wrapped = false;
    if (found && sameHunt) {
        if (!backward && newOff <= oldOff)
            wrapped = true;
        else if (backward && newOff >= oldOff)
            wrapped = true;
    }

    auto highlightLater = [this, needle, flags](const QString &hit) {
        QTimer::singleShot(160, this, [this, needle, flags, hit]() {
            if (!m_editor || !m_editor->webView() || !m_editor->webView()->page())
                return;
            m_editor->webView()->page()->findText(needle, flags, [this](const QWebEngineFindTextResult &r) {
                if (r.numberOfMatches() > 0)
                    m_findHadHit = true;
            });
            statusBar()->showMessage(QStringLiteral("Found in %1").arg(sceneBinderLabel(hit)), 4000);
        });
    };

    if (found && !wrapped) {
        m_findNeedle = needle;
        m_findScene = m_currentScenePath;
        m_findHadHit = true;
        statusBar()->showMessage(
            QStringLiteral("Found in %1").arg(sceneBinderLabel(m_currentScenePath)), 4000);
        return;
    }

    const QString hit = nextSceneWithNeedle(needle, backward);
    if (hit.isEmpty()) {
        if (found) {
            m_findNeedle = needle;
            m_findScene = m_currentScenePath;
            m_findHadHit = true;
            statusBar()->showMessage(
                QStringLiteral("Found in %1").arg(sceneBinderLabel(m_currentScenePath)), 4000);
            return;
        }
        m_findHadHit = false;
        statusBar()->showMessage(QStringLiteral("Not found"), 3000);
        return;
    }

    m_findNeedle = needle;
    m_findScene = hit;
    m_findHadHit = false;
    activateSceneForFind(hit);
    highlightLater(hit);
}

void QuireFrame::onFind()
{
    setupFindDialog();
    m_findDialog->show();
    m_findDialog->raise();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void QuireFrame::onFindNext()
{
    runFind(false);
}

void QuireFrame::onFindPrevious()
{
    runFind(true);
}

void QuireFrame::colorizeToolbarIcons(const Theme &theme)
{
    const QColor tint(theme.accent);
    auto tinted = [tint](const QString &path) {
        QPixmap src(path);
        if (src.isNull())
            return QIcon();
        QPixmap dest(src.size());
        dest.fill(Qt::transparent);
        QPainter p(&dest);
        p.drawPixmap(0, 0, src);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(dest.rect(), tint);
        p.end();
        return QIcon(dest);
    };
    m_saveAction->setIcon(tinted(QStringLiteral(":/icons/save.png")));
    m_newChapterAction->setIcon(tinted(QStringLiteral(":/icons/new.png")));
    m_newFolderAction->setIcon(tinted(QStringLiteral(":/icons/new.png")));
    m_newSceneAction->setIcon(tinted(QStringLiteral(":/icons/numbered.png")));
    m_boldAction->setIcon(tinted(QStringLiteral(":/icons/bold.png")));
    m_italicAction->setIcon(tinted(QStringLiteral(":/icons/italic.png")));
    m_underlineAction->setIcon(tinted(QStringLiteral(":/icons/underline.png")));
    m_alignLeftAction->setIcon(tinted(QStringLiteral(":/icons/alignleft.png")));
    m_alignCenterAction->setIcon(tinted(QStringLiteral(":/icons/aligncenter.png")));
    m_alignRightAction->setIcon(tinted(QStringLiteral(":/icons/alignright.png")));
    m_justifyAction->setIcon(tinted(QStringLiteral(":/icons/justify.png")));
    m_bulletAction->setIcon(tinted(QStringLiteral(":/icons/bullet.png")));
    m_numberAction->setIcon(tinted(QStringLiteral(":/icons/numbered.png")));
    m_checklistAction->setIcon(tinted(QStringLiteral(":/icons/checklist.png")));
    m_compileAction->setIcon(tinted(QStringLiteral(":/icons/justify.png")));
}

void QuireFrame::applyBinderStyle(const Theme &theme)
{
    const QString treeFg = theme.pageAsObject ? theme.pageBg : theme.textOnChrome;
    if (m_treeView) {
        m_treeView->setStyleSheet(QStringLiteral(
            "QTreeView, QTreeView::item, QTreeView::branch {"
            "  color: %1; background-color: %2;"
            "  selection-background-color: %3; selection-color: %4;"
            "}"
            "QTreeView::item:selected, QTreeView::branch:selected {"
            "  background-color: %3; color: %4;"
            "}"
            "QTreeView QLineEdit {"
            "  color: %1; background-color: %5;"
            "  selection-background-color: %3; selection-color: %4;"
            "}")
            .arg(treeFg, theme.chromeBg, theme.chromeHi,
                 theme.pageAsObject ? theme.pageBg : theme.accent, theme.chromeLo));
    }
}

void QuireFrame::applyTheme(ThemeId id)
{
    const Theme t = themeForId(id);
    m_currentTheme = t;
    applyUiFont(t);

    const QString chromeFg = t.pageAsObject ? t.pageBg : t.textOnChrome;

    menuBar()->setStyleSheet(QStringLiteral(
        "QMenuBar { background-color: %1; color: %2; }"
        "QMenuBar::item { background-color: transparent; color: %2; padding: 4px 8px; }"
        "QMenuBar::item:selected { background-color: %3; color: %4; }"
        "QMenu { background-color: %5; color: %6; border: 1px solid %7; }"
        "QMenu::item { background-color: transparent; color: %6; }"
        "QMenu::item:selected { background-color: %3; color: %4; }"
        "QMenu::separator { height: 1px; background: %7; }")
        .arg(t.menuBarBg, t.menuBarText, t.menuSelectedBg, t.menuSelectedFg,
             t.themeId == ThemeId::WordPerfect ? t.menuBarBg : t.chromeBg,
             t.themeId == ThemeId::WordPerfect ? t.menuBarText : t.textOnChrome,
             t.chromeLo));

    if (QToolBar *bar = findChild<QToolBar *>()) {
        bar->setStyleSheet(QStringLiteral(
            "QToolBar {"
            "  background-color: %1;"
            "  border: none;"
            "  padding: 4px 0;"
            "}"
            "QToolButton { background-color: transparent; border: none; padding: 2px; }"
            "QToolButton:hover { background-color: %2; border-radius: 2px; }"
            "QToolButton:pressed { background-color: %3; }"
            "QComboBox { background-color: %3; color: %4; border: 1px solid %2; border-radius: 2px; padding: 2px; min-width: 60px; }"
            "QComboBox:hover { background-color: %2; }"
            "QComboBox::drop-down { border: none; background-color: %3; }"
            "QComboBox::down-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 4px solid %4; margin-right: 4px; }"
            "QComboBox QAbstractItemView { background-color: %5; color: %4; border: 1px solid %3; selection-background-color: %2; }")
            .arg(t.chromeMid, t.chromeHi, t.chromeLo, chromeFg, t.chromeBg));
    }

    statusBar()->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                   .arg(t.chromeBg, t.textOnChrome));
    if (m_wordCountLabel)
        m_wordCountLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.textOnChrome));
    if (m_dirtyLabel)
        m_dirtyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.textOnChrome));
    if (m_sceneLabel)
        m_sceneLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.textOnChrome));

    applyBinderStyle(t);

    const QString menuBg = (t.themeId == ThemeId::WordPerfect) ? t.menuBarBg : t.chromeMid;
    const QString menuFg = (t.themeId == ThemeId::WordPerfect) ? t.menuBarText : t.textOnChrome;
    QString dialogCss = QStringLiteral(
        "QMainWindow { background-color: __BG__; }"
        "QMessageBox { background-color: __BG__; color: __FG__; }"
        "QMessageBox QLabel { color: __FG__; }"
        "QMessageBox QPushButton { background-color: __MID__; color: __FG__; border: 1px solid __BG__; padding: 5px; }"
        "QMessageBox QPushButton:hover { background-color: __HI__; }"
        "QFileDialog { background-color: __BG__; color: __FG__; }"
        "QFileDialog QLabel, QFileDialog QLineEdit, QFileDialog QTreeView, QFileDialog QListView, QFileDialog QComboBox, QFileDialog QHeaderView::section { color: __FG__; background-color: __BG__; }"
        "QFileDialog QPushButton { background-color: __MID__; color: __FG__; border: 1px solid __BG__; padding: 4px 8px; }"
        "QInputDialog, QDialog { background-color: __BG__; color: __FG__; }"
        "QInputDialog QLabel { color: __FG__; }"
        "QInputDialog QLineEdit { color: __CHROMEFG__; background-color: __LO__; }"
        "QInputDialog QPushButton { background-color: __MID__; color: __FG__; }"
        "QMenu { background-color: __MENUBG__; color: __MENUFG__; border: 1px solid __BG__; }"
        "QMenu::item:selected { background-color: __SELBG__; color: __SELFG__; }");
    dialogCss.replace(QStringLiteral("__BG__"), t.chromeBg);
    dialogCss.replace(QStringLiteral("__FG__"), t.textOnChrome);
    dialogCss.replace(QStringLiteral("__MID__"), t.chromeMid);
    dialogCss.replace(QStringLiteral("__HI__"), t.chromeHi);
    dialogCss.replace(QStringLiteral("__LO__"), t.chromeLo);
    dialogCss.replace(QStringLiteral("__MENUBG__"), menuBg);
    dialogCss.replace(QStringLiteral("__MENUFG__"), menuFg);
    dialogCss.replace(QStringLiteral("__SELBG__"), t.menuSelectedBg);
    dialogCss.replace(QStringLiteral("__SELFG__"), t.menuSelectedFg);
    dialogCss.replace(QStringLiteral("__CHROMEFG__"), chromeFg);
    qApp->setStyleSheet(dialogCss);

    colorizeToolbarIcons(t);

    if (m_themeGroup) {
        const QSignalBlocker blocker(m_themeGroup);
        for (QAction *action : m_themeGroup->actions())
            action->setChecked(action->data().toString() == t.id);
    }

    if (m_editor)
        m_editor->applyTheme(t);

    QSettings settings(kOrg, kApp);
    settings.setValue(QStringLiteral("theme"), t.id);
}

