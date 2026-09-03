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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fprintf(stdout, "Usage: Quire [--listen]\n");
            std::fprintf(stdout, "  --listen   load editor, compile Kindle DOCX fixture, print health, quit\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--listen") == 0)
            listen = true;
    }

    setupWebEnginePrefix();

    if (listen) {
        const QByteArray we = qgetenv("QTWEBENGINEPROCESS_PATH");
        std::fprintf(stdout, "webengine: %s\n", we.isEmpty() ? "(unset)" : we.constData());
        std::fflush(stdout);
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Quire"));
    app.setApplicationVersion(QStringLiteral("0.3.12"));
    app.setOrganizationName(QStringLiteral("Sociopathletic"));
    app.setOrganizationDomain(QStringLiteral("sociopathletic.com"));

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
