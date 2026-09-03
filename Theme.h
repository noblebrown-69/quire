#ifndef THEME_H
#define THEME_H

#include <QString>
#include <QVector>

enum class ThemeId {
    Leather,
    WordPerfect,
    Matrix,
    Amber
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

#endif // THEME_H
