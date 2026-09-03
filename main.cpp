#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QList>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include "QuireFrame.h"
#include "ScrivenerImport.h"

static bool isExistingFile(const QByteArray &path)
{
    return !path.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(path))
           && QFileInfo(QString::fromLocal8Bit(path)).isFile();
}

static bool isExistingDir(const QByteArray &path)
{
    return !path.isEmpty() && QFileInfo(QString::fromLocal8Bit(path)).isDir();
}

static QByteArray firstExistingFile(const QList<QByteArray> &paths)
{
    for (const QByteArray &p : paths) {
        if (isExistingFile(p))
            return p;
    }
    return {};
}

static QByteArray firstExistingDir(const QList<QByteArray> &paths)
{
    for (const QByteArray &p : paths) {
        if (isExistingDir(p))
            return p;
    }
    return {};
}

static void setupWebEnginePrefix()
{
    const QByteArray appdir = qgetenv("APPDIR");
    if (!appdir.isEmpty()) {
        const QByteArray process = firstExistingFile({
            appdir + "/usr/lib/qt6/libexec/QtWebEngineProcess",
            appdir + "/usr/bin/QtWebEngineProcess",
        });
        if (!process.isEmpty()) {
            const QByteArray resources = firstExistingDir({
                appdir + "/usr/share/qt6/resources",
                appdir + "/usr/lib/qt6/resources",
                appdir + "/usr/resources",
            });
            qputenv("QTWEBENGINEPROCESS_PATH", process);
            if (!resources.isEmpty())
                qputenv("QTWEBENGINE_RESOURCES_PATH", resources);
            qputenv("QTWEBENGINE_LOCALES_PATH",
                    appdir + "/usr/share/qt6/translations/qtwebengine_locales");
            qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
            return;
        }
    }

    const QByteArray already = qgetenv("QTWEBENGINEPROCESS_PATH");
    if (isExistingFile(already))
        return;

    const QByteArray home = qgetenv("HOME");
    if (home.isEmpty())
        return;

    const QByteArray prefix = home + "/.local/opt/qt6-webengine/usr";
    const QByteArray process = prefix + "/lib/qt6/libexec/QtWebEngineProcess";
    if (!isExistingFile(process))
        return;

    qputenv("QTWEBENGINEPROCESS_PATH", process);
    qputenv("QTWEBENGINE_RESOURCES_PATH", prefix + "/share/qt6/resources");
    qputenv("QTWEBENGINE_LOCALES_PATH", prefix + "/share/qt6/translations/qtwebengine_locales");
    qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");

    const QByteArray extraLib = prefix + "/lib/x86_64-linux-gnu";
    const QByteArray old = qgetenv("LD_LIBRARY_PATH");
    qputenv("LD_LIBRARY_PATH", old.isEmpty() ? extraLib : extraLib + ":" + old);
}

int main(int argc, char *argv[])
{
    bool listen = false;
    bool importScriv = false;
    QByteArray importIn;
    QByteArray importOut;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fprintf(stdout, "Usage: Quire [--listen] [--import-scriv IN.scriv OUT.qr]\n");
            std::fprintf(stdout, "  --listen   load editor, compile Kindle DOCX fixture, print health, quit\n");
            std::fprintf(stdout, "  --import-scriv IN.scriv OUT.qr   copy Scrivener binder into a new .qr and exit\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--listen") == 0) {
            listen = true;
            continue;
        }
        if (std::strcmp(argv[i], "--import-scriv") == 0) {
            if (i + 2 >= argc) {
                std::fprintf(stderr, "Quire: --import-scriv requires IN.scriv and OUT.qr\n");
                return 1;
            }
            importScriv = true;
            importIn = argv[++i];
            importOut = argv[++i];
            continue;
        }
    }

    setupWebEnginePrefix();

    if (listen && !importScriv) {
        const QByteArray we = qgetenv("QTWEBENGINEPROCESS_PATH");
        std::fprintf(stdout, "webengine: %s\n", we.isEmpty() ? "(unset)" : we.constData());
        std::fflush(stdout);
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Quire"));
    app.setApplicationVersion(QStringLiteral("0.3.13"));
    app.setOrganizationName(QStringLiteral("Sociopathletic"));
    app.setOrganizationDomain(QStringLiteral("sociopathletic.com"));

    if (importScriv) {
        ScrivenerImportResult result;
        const QString inPath = QString::fromLocal8Bit(importIn);
        const QString outPath = QString::fromLocal8Bit(importOut);
        const bool ok = ScrivenerImport::importProject(inPath, outPath, &result);
        const QString inName = result.inName.isEmpty() ? QFileInfo(inPath).fileName() : result.inName;
        const QString outName = result.outName.isEmpty() ? QFileInfo(outPath).fileName() : result.outName;
        std::fprintf(stdout,
                     "import-scriv: in=%s  out=%s  scenes=%d  folders=%d  notes=%d  health: %s\n",
                     qPrintable(inName), qPrintable(outName), result.scenes, result.folders,
                     result.notes, ok ? "ok" : "fail");
        if (!result.error.isEmpty())
            std::fprintf(stderr, "import-scriv-error: %s\n", qPrintable(result.error));
        std::fflush(stdout);
        std::fflush(stderr);
        return ok ? 0 : 1;
    }

    QuireFrame frame;
    frame.show();
    frame.raise();
    frame.activateWindow();

    if (listen) {
        QTimer::singleShot(30000, &app, []() {
            std::fprintf(stdout, "health: timeout\n");
            std::fflush(stdout);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}