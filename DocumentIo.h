#ifndef DOCUMENTIO_H
#define DOCUMENTIO_H

#include <QString>

class DocumentIo {
public:
    static QString htmlFromFile(const QString &path, QString *error);
    static bool writeFromHtml(const QString &path, const QString &html, QString *error);

    static QString markdownToHtml(const QString &markdown);
    static QString htmlToMarkdown(const QString &html);

    static QString docxToHtml(const QString &path, QString *error);
    static bool htmlToDocx(const QString &path, const QString &html, QString *error);
    static bool htmlToKindleDocx(const QString &path, const QString &html, QString *error);

    static QString detectFormatName(const QString &path);
    static bool docxLooksHealthy(const QString &path, QString *error);
    static bool docxPeek(const QString &path,
                         bool *hasContentTypes,
                         bool *hasDocumentXml,
                         bool *leftoverHtml,
                         QString *error);
};

#endif // DOCUMENTIO_H
