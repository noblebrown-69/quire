#ifndef SCRIVENERIMPORT_H
#define SCRIVENERIMPORT_H

#include <QString>

struct ScrivenerImportResult {
    QString inName;
    QString outName;
    QString outPath;
    QString title;
    int scenes = 0;
    int folders = 0;
    int notes = 0;
    QString error;
};

class ScrivenerImport {
public:
    // Copy a Scrivener .scriv (or .scrivx) binder into a new Quire .qr project.
    // Never writes into the source tree. Fails if outQrPath already exists.
    static bool importProject(const QString &scrivPath,
                              const QString &outQrPath,
                              ScrivenerImportResult *result = nullptr);

    static QString rtfToSimpleHtml(const QString &rtf);
};

#endif // SCRIVENERIMPORT_H
