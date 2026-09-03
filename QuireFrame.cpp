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
#include <QWebEngineFindTextResult>
#include <QPrinter>
#include <QPrintDialog>
#include <QPrinterInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QMarginsF>
#include <QProcess>
#include <QTemporaryFile>
#include <QSignalBlocker>
#include <QShortcut>
#include <cstdio>

namespace {

const QString kQuireVersion = QStringLiteral("0.3.13");

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
        QStringLiteral("Dedication"),
        QStringLiteral("Copyright"),
        QStringLiteral("Preface"),
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


QPageLayout defaultPrintPageLayout()
{
    return QPageLayout(QPageSize(QPageSize::Letter),
                       QPageLayout::Portrait,
                       QMarginsF(0.75, 0.75, 0.75, 0.75),
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
    combo->setCurrentFont(QFont(family));
    if (QString::compare(combo->currentText(), family, Qt::CaseInsensitive) == 0)
        return;
    if (!combo->isEditable())
        combo->setEditable(true);
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
            o += QStringLiteral("<p class=\"scenebreak\">#</p>\n");
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
    if (!listen) {
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

    m_compileAction = new QAction(QStringLiteral("Compile EPUB3 + Kindle DOCX"), this);
    m_compileAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    m_compileAction->setIcon(QIcon(QStringLiteral(":/icons/justify.png")));
    m_compileAction->setToolTip(QStringLiteral("Compile EPUB3 + Kindle DOCX"));
    connect(m_compileAction, &QAction::triggered, this, &QuireFrame::onCompile);

    m_printAction = new QAction(QStringLiteral("&Print…"), this);
    m_printAction->setShortcut(QKeySequence::Print);
    connect(m_printAction, &QAction::triggered, this, &QuireFrame::onPrint);

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
            m_editor->execCommand(QStringLiteral("insertHTML"),
                QStringLiteral("<div class=\"page-break\" style=\"page-break-after: always; border: none; border-top: 1px dashed #8B7355; margin: 30px 0;\"></div>"));
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
    m_fontCombo->setCurrentFont(QFont(QStringLiteral("Noto Serif")));
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
            "and a Word fallback (manuscript.docx).</p>"
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
        QStringLiteral("Compiled manuscript.epub (EPUB3 = KDP Kindle upload) and manuscript.docx (Word fallback)"),
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
        sc.bodyHtml = sceneBody(readTextFile(path));
        QString rel = QDir(msRoot).relativeFilePath(QFileInfo(path).absolutePath());
        if (!rel.isEmpty() && rel != QLatin1String("."))
            sc.folderTrail = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        annotateCompileScene(&sc, &lastChapter);
        epubScenes.append(sc);
    }

    QString parts;
    parts += QStringLiteral(
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
        "<title>%1</title></head><body>\n").arg(m_projectTitle.toHtmlEscaped());
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

    if (!fails.isEmpty()) {
        if (error)
            *error = fails.join(QStringLiteral("; "));
        return false;
    }
    return true;
}

void QuireFrame::runListenProof()
{
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
    const QString html = readTextFile(path);
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

    const QPageLayout layout = pageLayoutFromPrinter(printer);
    const bool toFile = printer.outputFormat() == QPrinter::PdfFormat
                        || !printer.outputFileName().isEmpty();
    if (toFile) {
        QString fileName = printer.outputFileName();
        if (fileName.isEmpty())
            return;
        if (!fileName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
            fileName += QStringLiteral(".pdf");
        m_editor->webView()->page()->printToPdf(fileName, layout);
        statusBar()->showMessage(QStringLiteral("Printing to file: ") + fileName);
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
    statusBar()->showMessage(QStringLiteral("Printing..."));
}

void QuireFrame::onPdfPrintingFinished(const QString &path, bool success)
{
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
            QMessageBox::warning(this, QStringLiteral("Print Failed"),
                                 QStringLiteral("Could not render the page for printing."));
            return;
        }

        const QStringList args = cupsLpArgv(printerName, copies, pdf);
        QProcess *lp = new QProcess(this);
        connect(lp, &QProcess::finished, this,
                [this, lp](int code, QProcess::ExitStatus st) {
            const QByteArray err = lp->readAllStandardError();
            lp->deleteLater();
            if (m_printTemp) {
                m_printTemp->deleteLater();
                m_printTemp = nullptr;
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
        }
        return;
    }

    if (!success)
        QMessageBox::warning(this, QStringLiteral("Print Failed"),
                             QStringLiteral("Could not write the print file:\n") + path);
    else
        statusBar()->showMessage(QStringLiteral("Printed to: ") + path);
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
        ui.setFamilies({"Noto Serif", "Georgia", "serif"});
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

