#ifndef QUIREFRAME_H
#define QUIREFRAME_H

#include <QMainWindow>
#include <QModelIndex>
#include <QItemSelection>
#include <QPoint>
#include <QStringList>
#include <QByteArray>
#include <QList>
#include "Theme.h"

class MonasteryEditor;
class QFileSystemModel;
class QSortFilterProxyModel;
class QTreeView;
class QSplitter;
class QLabel;
class QAction;
class QActionGroup;
class QTimer;
class QJsonObject;
class QDialog;
class QLineEdit;
class QFontComboBox;
class QComboBox;
class QCheckBox;
class QTemporaryFile;
class QToolBar;
class QMenu;

class QuireFrame : public QMainWindow {
    Q_OBJECT

public:
    explicit QuireFrame(QWidget *parent = nullptr);
    ~QuireFrame() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onNewProject();
    void onOpenProject();
    void onSave();
    void onNewChapter();
    void onNewFolder();
    void onNewScene();
    void onNewNote();
    void onImport();
    void onImportScrivener();
    void onFind();
    void onFindNext();
    void onFindPrevious();
    void onRenameItem();
    void onDeleteItem();
    void onMoveUp();
    void onMoveDown();
    void onToggleIncludeInCompile();
    void onProjectDetails();
    void onCompile();
    bool compileToDisk(QString *error);
    void runListenProof();
    void onChecklist();
    void onAbout();
    void onToggleFocusMode(bool on);
    void onPreviousScene();
    void onNextScene();
    void onFontChanged(const QString &font);
    void onSizeChanged(const QString &size);
    void onSelectionFontChanged(const QString &family, int pt);
    void onPrint();
    void onPdfPrintingFinished(const QString &path, bool success);
    void onAutoSave();
    void onTreeClicked(const QModelIndex &index);
    void onTreeSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void onBinderContextMenu(const QPoint &pos);
    void onFileRenamed(const QString &path, const QString &oldName, const QString &newName);
    void applyTheme(ThemeId id);

private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createStatusBar();
    void setFocusMode(bool on);
    void navigateManuscriptScene(int delta);
    void applyUiFont(const Theme &theme);
    void colorizeToolbarIcons(const Theme &theme);
    void applyBinderStyle(const Theme &theme);

    bool openProject(const QString &projectDir, bool remember = true);
    bool createProjectAt(const QString &projectDir, const QString &title, const QString &author);
    bool ensureDefaultProject(QString *outPath);
    void rememberProject(const QString &projectDir);
    QStringList cleanedRecentProjects();
    void rebuildRecentMenu();
    void bindTreeToManuscript();
    void updateStatus();
    void refreshManuscriptWordCount();
    void updateWindowTitle();
    bool writeProjectJson();

    QString manuscriptDir() const;
    QString notesDir() const;
    QString compileDir() const;
    QString autosaveDir() const;
    QString quireJsonPath() const;
    QString defaultManuscriptsRoot() const;

    QModelIndex sourceIndex(const QModelIndex &viewIndex) const;
    QString pathFromView(const QModelIndex &viewIndex) const;
    QModelIndex viewIndexForPath(const QString &path) const;

    QString currentParentDir() const;
    QString currentNotesParentDir() const;
    bool isNotesPath(const QString &path) const;
    bool isManuscriptPath(const QString &path) const;
    bool isBinderTopFolder(const QString &path) const;
    QString uniquePath(const QString &dir, const QString &base, const QString &suffix) const;
    QString sceneTitleFromPath(const QString &path) const;
    int nextChapterNumber() const;
    void setupFindDialog();
    void runFind(bool backward);

    bool persistCurrentScene(bool markCleanAfter);
    bool writeTextFile(const QString &path, const QString &text) const;
    QString readTextFile(const QString &path) const;
    void loadScene(const QString &path);
    void clearEditor();
    void collectScenes(const QString &dir, QStringList *out, bool includeExcluded = false) const;
    QString sceneBinderLabel(const QString &path) const;
    QString sceneHtmlForFind(const QString &path) const;
    QString nextSceneWithNeedle(const QString &needle, bool backward) const;
    QString firstSceneWithNeedle(const QString &needle) const;
    void activateSceneForFind(const QString &path);
    void listenReportFind(const QString &query);
    QString wrapSceneHtml(const QString &path, const QString &body) const;
    QString autosavePathForScene(const QString &scenePath) const;

    QString manuscriptRelative(const QString &absPath) const;
    void loadBinderLists(const QJsonObject &obj);
    void pruneBinderLists();
    void refreshBinderView();
    void rewritePathInLists(const QString &oldRel, const QString &newRel);
    void removePathFromLists(const QString &rel);
    bool isSelfExcluded(const QString &absPath) const;
    bool isCompileExcluded(const QString &absPath) const;
    bool binderLessThan(const QString &relA, const QString &nameA, bool dirA,
                        const QString &relB, const QString &nameB, bool dirB) const;
    void moveCurrent(int delta);
    void updateBinderActionState();
    void appendToOrder(const QString &absPath);
    QString importSceneFromFile(const QString &sourcePath, const QString &destDir, QString *error);
    void syncOrderFromDisk();
    void syncOrderWalk(const QString &dir);

    MonasteryEditor *m_editor = nullptr;
    QFileSystemModel *m_fileModel = nullptr;
    QSortFilterProxyModel *m_sortProxy = nullptr;
    QTreeView *m_treeView = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_wordCountLabel = nullptr;
    QLabel *m_sceneLabel = nullptr;
    QLabel *m_dirtyLabel = nullptr;
    int m_manuscriptWordCount = 0;
    QActionGroup *m_themeGroup = nullptr;
    Theme m_currentTheme;

    QAction *m_newProjectAction = nullptr;
    QAction *m_openProjectAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_newChapterAction = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_newSceneAction = nullptr;
    QAction *m_newNoteAction = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_importScrivenerAction = nullptr;
    QAction *m_boldAction = nullptr;
    QAction *m_italicAction = nullptr;
    QAction *m_underlineAction = nullptr;
    QAction *m_alignLeftAction = nullptr;
    QAction *m_alignCenterAction = nullptr;
    QAction *m_alignRightAction = nullptr;
    QAction *m_justifyAction = nullptr;
    QAction *m_bulletAction = nullptr;
    QAction *m_numberAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_pageBreakAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
    QAction *m_includeAction = nullptr;
    QAction *m_projectDetailsAction = nullptr;
    QAction *m_compileAction = nullptr;
    QAction *m_checklistAction = nullptr;
    QAction *m_aboutAction = nullptr;
    QAction *m_quitAction = nullptr;
    QAction *m_printAction = nullptr;
    QAction *m_focusAction = nullptr;
    QAction *m_prevSceneAction = nullptr;
    QAction *m_nextSceneAction = nullptr;
    QToolBar *m_formatToolBar = nullptr;
    bool m_focusMode = false;
    QByteArray m_savedGeometry;
    QList<int> m_savedSplitterSizes;
    bool m_savedMaximized = false;
    QFontComboBox *m_fontCombo = nullptr;
    QComboBox *m_sizeCombo = nullptr;
    QTemporaryFile *m_printTemp = nullptr;
    QString m_pendingLpPdf;
    QString m_pendingLpPrinter;
    int m_pendingLpCopies = 1;

    QTimer *m_autoSaveTimer = nullptr;
    QDialog *m_findDialog = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QCheckBox *m_findManuscriptCheck = nullptr;
    QString m_findNeedle;
    QString m_findScene;
    bool m_findHadHit = false;

    QString m_projectDir;
    QString m_projectTitle;
    QString m_projectAuthor;
    QString m_currentScenePath;
    QStringList m_order;
    QStringList m_exclude;
    bool m_loadingScene = false;
};

#endif // QUIREFRAME_H
