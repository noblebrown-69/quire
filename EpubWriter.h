#ifndef EPUBWRITER_H
#define EPUBWRITER_H

#include <QString>
#include <QStringList>
#include <QVector>

class EpubWriter {
public:
    struct Scene {
        QStringList folderTrail;
        QString title;
        QString bodyHtml;
        QString chapterTitle;
        bool frontMatter = false;
        bool startChapter = false;
    };

    static QString sanitizeBody(const QString &html);
    static QString headingHtml(const Scene &scene);
    static bool isGenericSceneTitle(const QString &title);
    static bool isVisuallyEmpty(const QString &html);

    static bool write(const QString &epubPath,
                      const QString &title,
                      const QString &author,
                      const QVector<Scene> &scenes,
                      QString *errorOut = nullptr);
};

#endif // EPUBWRITER_H
