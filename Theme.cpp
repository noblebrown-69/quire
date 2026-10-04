#include "Theme.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>

static Theme makeLeather()
{
    Theme t;
    t.themeId = ThemeId::Leather;
    t.id = QStringLiteral("leather");
    t.name = QStringLiteral("Leather");
    t.chromeBg = QStringLiteral("#3C2F2F");
    t.chromeMid = QStringLiteral("#6F5A4A");
    t.chromeHi = QStringLiteral("#8B7355");
    t.chromeLo = QStringLiteral("#5C4A3F");
    t.accent = QStringLiteral("#D4AF37");
    t.textOnChrome = QStringLiteral("#D4AF37");
    t.menuBarBg = t.chromeBg;
    t.menuBarText = t.textOnChrome;
    t.menuSelectedBg = t.chromeLo;
    t.menuSelectedFg = QStringLiteral("#F5E8C7");
    t.pageBg = QStringLiteral("#F5E8C7");
    t.pageText = QStringLiteral("#1a1a1a");
    t.desk = QStringLiteral("#3C2F2F");
    t.selectionBg = QStringLiteral("#D4AF37");
    t.selectionFg = QStringLiteral("#000000");
    t.uiFont = QStringLiteral("Gelasio");
    t.editorFont = QStringLiteral("Georgia, Gelasio, \"Noto Serif\", serif");
    t.pageAsObject = true;
    return t;
}

static Theme makeWordPerfect()
{
    Theme t;
    t.themeId = ThemeId::WordPerfect;
    t.id = QStringLiteral("wordperfect");
    t.name = QStringLiteral("WordPerfect");
    t.chromeBg = QStringLiteral("#000080");
    t.chromeMid = QStringLiteral("#0000AA");
    t.chromeHi = QStringLiteral("#0000FF");
    t.chromeLo = QStringLiteral("#000055");
    t.accent = QStringLiteral("#FFFF55");
    t.textOnChrome = QStringLiteral("#55FFFF");
    t.menuBarBg = QStringLiteral("#C0C0C0");
    t.menuBarText = QStringLiteral("#000000");
    t.menuSelectedBg = QStringLiteral("#0000AA");
    t.menuSelectedFg = QStringLiteral("#FFFFFF");
    t.pageBg = QStringLiteral("#0000AA");
    t.pageText = QStringLiteral("#FFFFFF");
    t.desk = QStringLiteral("#0000AA");
    t.selectionBg = QStringLiteral("#FFFF55");
    t.selectionFg = QStringLiteral("#0000AA");
    t.uiFont = QStringLiteral("IBM Plex Mono");
    t.editorFont = QStringLiteral("\"Courier New\", \"IBM Plex Mono\", monospace");
    t.pageAsObject = false;
    return t;
}

static Theme makeMatrix()
{
    Theme t;
    t.themeId = ThemeId::Matrix;
    t.id = QStringLiteral("matrix");
    t.name = QStringLiteral("Matrix");
    t.chromeBg = QStringLiteral("#050805");
    t.chromeMid = QStringLiteral("#0B1A0B");
    t.chromeHi = QStringLiteral("#003B00");
    t.chromeLo = QStringLiteral("#001A00");
    t.accent = QStringLiteral("#00FF41");
    t.textOnChrome = QStringLiteral("#00FF41");
    t.menuBarBg = t.chromeBg;
    t.menuBarText = t.textOnChrome;
    t.menuSelectedBg = t.chromeHi;
    t.menuSelectedFg = t.accent;
    t.pageBg = QStringLiteral("#000000");
    t.pageText = QStringLiteral("#00FF41");
    t.desk = QStringLiteral("#000000");
    t.selectionBg = QStringLiteral("#00FF41");
    t.selectionFg = QStringLiteral("#003B00");
    t.uiFont = QStringLiteral("Courier New");
    t.editorFont = QStringLiteral("\"Courier New\", \"Liberation Mono\", monospace");
    t.pageAsObject = false;
    return t;
}

static Theme makeAmber()
{
    Theme t;
    t.themeId = ThemeId::Amber;
    t.id = QStringLiteral("amber");
    t.name = QStringLiteral("Amber");
    t.chromeBg = QStringLiteral("#140E00");
    t.chromeMid = QStringLiteral("#2A1C00");
    t.chromeHi = QStringLiteral("#4A3000");
    t.chromeLo = QStringLiteral("#1A1200");
    t.accent = QStringLiteral("#FFB000");
    t.textOnChrome = QStringLiteral("#FFB000");
    t.menuBarBg = t.chromeBg;
    t.menuBarText = t.textOnChrome;
    t.menuSelectedBg = t.chromeHi;
    t.menuSelectedFg = t.accent;
    t.pageBg = QStringLiteral("#000000");
    t.pageText = QStringLiteral("#FFB000");
    t.desk = QStringLiteral("#000000");
    t.selectionBg = QStringLiteral("#FFB000");
    t.selectionFg = QStringLiteral("#4A3000");
    t.uiFont = QStringLiteral("Courier New");
    t.editorFont = QStringLiteral("\"Courier New\", \"Liberation Mono\", monospace");
    t.pageAsObject = false;
    return t;
}

// BushidOS Neon: exact palette from BushidOS src/gtk3/_colors-neon.scss and Techo bushidos-neon.xml.
static Theme makeNeon()
{
    Theme t;
    t.themeId = ThemeId::Neon;
    t.id = QStringLiteral("neon");
    t.name = QStringLiteral("BushidOS Neon");
    t.chromeBg = QStringLiteral("#0d0b14");      // obsidian (header/title bar)
    t.chromeMid = QStringLiteral("#17141f");     // window
    t.chromeHi = QStringLiteral("#2e2842");      // hover
    t.chromeLo = QStringLiteral("#15121f");      // glass
    t.accent = QStringLiteral("#00e5ff");        // cyan (toolbar icon tint)
    t.textOnChrome = QStringLiteral("#e2e0f0");
    t.menuBarBg = t.chromeBg;
    t.menuBarText = t.textOnChrome;
    t.menuSelectedBg = QStringLiteral("#c0176e"); // magenta_deep
    t.menuSelectedFg = QStringLiteral("#ffffff");
    t.pageBg = QStringLiteral("#0f0d18");        // base
    t.pageText = QStringLiteral("#ecebf7");
    t.desk = QStringLiteral("#0f0d18");
    t.selectionBg = QStringLiteral("#c0176e");
    t.selectionFg = QStringLiteral("#ffffff");
    t.uiFont = QStringLiteral("Noto Sans");
    t.editorFont = QStringLiteral("\"Noto Serif\", Georgia, serif");
    t.pageAsObject = false;                      // flat page, like Techo
    return t;
}

QVector<Theme> allThemes()
{
    return {makeLeather(), makeWordPerfect(), makeMatrix(), makeAmber(), makeNeon()};
}

Theme themeForId(ThemeId id)
{
    switch (id) {
    case ThemeId::WordPerfect:
        return makeWordPerfect();
    case ThemeId::Matrix:
        return makeMatrix();
    case ThemeId::Amber:
        return makeAmber();
    case ThemeId::Neon:
        return makeNeon();
    case ThemeId::Leather:
    default:
        return makeLeather();
    }
}

ThemeId themeIdFromString(const QString &id)
{
    if (id == QLatin1String("wordperfect"))
        return ThemeId::WordPerfect;
    if (id == QLatin1String("matrix"))
        return ThemeId::Matrix;
    if (id == QLatin1String("amber"))
        return ThemeId::Amber;
    if (id == QLatin1String("neon"))
        return ThemeId::Neon;
    return ThemeId::Leather;
}

Theme themeFromSettingsId(const QString &id)
{
    return themeForId(themeIdFromString(id));
}

QString bushidosGtkThemeName()
{
    const QString home = QDir::homePath();
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (desktop.contains(QLatin1String("XFCE"), Qt::CaseInsensitive)) {
        // XFCE keeps the real theme in xfconf (gsettings is often stale there).
        QFile xs(home + QStringLiteral("/.config/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml"));
        if (xs.open(QIODevice::ReadOnly)) {
            static const QRegularExpression re(
                QStringLiteral("<property name=\"ThemeName\" type=\"string\" value=\"([^\"]*)\""));
            const QRegularExpressionMatch m = re.match(QString::fromUtf8(xs.readAll()));
            if (m.hasMatch())
                return m.captured(1);
        }
    }
    // GNOME/Cinnamon: gsettings. Strip LD_LIBRARY_PATH so AppImage libs don't leak into the host tool.
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("gsettings"),
            {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("gtk-theme")});
    if (p.waitForFinished(1500) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
        QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        out.remove(QLatin1Char('\''));
        if (!out.isEmpty())
            return out;
    }
    QSettings ini(home + QStringLiteral("/.config/gtk-3.0/settings.ini"), QSettings::IniFormat);
    return ini.value(QStringLiteral("Settings/gtk-theme-name")).toString();
}

bool bushidosNeonActive()
{
    return bushidosGtkThemeName().startsWith(QLatin1String("BushidOS-Neon"), Qt::CaseInsensitive);
}

// Neon-only widget styling. Appended after the per-theme rules, so it wins on equal specificity.
QString neonExtraQss()
{
    return QStringLiteral(
        "QScrollBar:vertical { background: #121019; width: 12px; margin: 0; border: none; }"
        "QScrollBar:horizontal { background: #121019; height: 12px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background: #4a4262; min-height: 24px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: #4a4262; min-width: 24px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:hover { background: #298ba9; }"
        "QScrollBar::handle:pressed { background: #00e5ff; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }"
        "QTabWidget::pane { border: 1px solid #3a3350; background: #17141f; }"
        "QTabBar::tab { background: #15121f; color: #7c7a87; border: 1px solid #3a3350; border-bottom: none; padding: 5px 12px; margin-right: 2px; }"
        "QTabBar::tab:selected { background: #17141f; color: #ecebf7; border-bottom: 2px solid #ff2e88; }"
        "QTabBar::tab:hover { color: #00e5ff; }"
        "QTreeView, QListView, QTableView { background: #0f0d18; color: #ecebf7; border: 1px solid #3a3350;"
        " selection-background-color: #c0176e; selection-color: #ffffff; outline: 0; }"
        "QTreeView::item:hover, QListView::item:hover { background: #15121f; color: #00e5ff; }"
        "QTreeView::item:selected, QListView::item:selected { background: #c0176e; color: #ffffff; }"
        "QHeaderView::section { background: #17141f; color: #e2e0f0; border: none; border-right: 1px solid #3a3350; padding: 4px; }"
        "QToolTip { background: #15121f; color: #ecebf7; border: 1px solid #00e5ff; padding: 4px; }"
        "QPushButton { background: #15121f; color: #e2e0f0; border: 1px solid #3a3350; border-radius: 4px; padding: 5px 12px; }"
        "QPushButton:hover { border-color: #00e5ff; color: #00e5ff; }"
        "QPushButton:pressed, QPushButton:default { border-color: #ff2e88; }"
        "QLineEdit, QTextEdit, QSpinBox, QComboBox { background: #0f0d18; color: #ecebf7; border: 1px solid #3a3350;"
        " border-radius: 4px; padding: 4px; selection-background-color: #c0176e; selection-color: #ffffff; }"
        "QLineEdit:focus, QTextEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #00e5ff; }"
        "QComboBox QAbstractItemView { background: #15121f; color: #ecebf7; selection-background-color: #c0176e; selection-color: #ffffff; }"
        "QCheckBox, QRadioButton, QLabel, QGroupBox { color: #e2e0f0; }"
        "QGroupBox { border: 1px solid #3a3350; margin-top: 8px; }"
        "QSplitter::handle { background: #3a3350; }"
        "QMenu { background: #15121f; color: #e2e0f0; border: 1px solid #3a3350; }"
        "QMenu::item:selected { background: #c0176e; color: #ffffff; }"
        "QMenu::separator { height: 1px; background: #3a3350; }"
        "QDialog, QMessageBox, QFileDialog { background: #17141f; color: #e2e0f0; }");
}
