#ifndef THEME_H
#define THEME_H

#include <QString>
#include <QVector>

enum class ThemeId {
    Leather,
    WordPerfect,
    Matrix,
    Amber,
    Neon
};

struct Theme {
    ThemeId themeId = ThemeId::Leather;
    QString id;
    QString name;
    QString chromeBg;
    QString chromeMid;
    QString chromeHi;
    QString chromeLo;
    QString accent;
    QString textOnChrome;
    QString menuBarBg;
    QString menuBarText;
    QString menuSelectedBg;
    QString menuSelectedFg;
    QString pageBg;
    QString pageText;
    QString desk;
    QString selectionBg;
    QString selectionFg;
    QString uiFont;
    QString editorFont;
    bool pageAsObject = true;
};

Theme themeForId(ThemeId id);
Theme themeFromSettingsId(const QString &id);
ThemeId themeIdFromString(const QString &id);
QVector<Theme> allThemes();

// BushidOS integration: GTK theme name of the desktop (xfconf on XFCE, else gsettings / settings.ini),
// whether it is the BushidOS Neon theme, and the extra Neon-only Qt stylesheet.
QString bushidosGtkThemeName();
bool bushidosNeonActive();
QString neonExtraQss();

#endif // THEME_H
