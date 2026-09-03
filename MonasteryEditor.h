#ifndef MONASTERYEDITOR_H
#define MONASTERYEDITOR_H

#include <QWidget>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QTimer>
#include <functional>
#include "Theme.h"

class MonasteryEditor : public QWidget {
    Q_OBJECT
public:
    explicit MonasteryEditor(QWidget *parent = nullptr);
    void execCommand(const QString &cmd, const QString &value = QString());
    void insertChecklist();
    QString getHtml();
    void setHtml(const QString &html);
    int getWordCount();
    void refreshHighlighter();

    QWebEngineView* webView() const { return m_webView; }

    void fetchHtml(const std::function<void(const QString &)> &callback);
    void requestWordCount(const std::function<void(int)> &callback);
    bool queryDirtyNow();
    void markClean();
    void markDirty();
    bool isDirty() const { return m_dirty; }
    QString lastGoodHtml() const { return m_cachedHtml; }
    void applyFontSize(int pointSize);
    void applyFontFamily(const QString &name);
    void applyTheme(const Theme &theme);
    void requestHeadingFont(const std::function<void(const QString &family, int pt,
                                                     const QString &caretFamily, int caretPt,
                                                     bool found)> &callback);

signals:
    void wordCountChanged(int count);
    void dirtyChanged(bool dirty);
    void ready(bool ok);
    void selectionFontChanged(const QString &family, int pt);

private:
    void considerHtmlCache(const QString &html);
    void pollEditorState();
    void pollSelectionFont();

    QWebEngineView *m_webView;
    QTimer *m_pollTimer;
    int m_cachedWordCount = 0;
    QString m_cachedHtml;
    QString m_cachedSelFamily;
    int m_cachedSelPt = 0;
    bool m_isLoaded = false;
    bool m_dirty = false;
};

#endif // MONASTERYEDITOR_H
