#include "Theme.h"

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
    t.uiFont = QStringLiteral("Noto Serif");
    t.editorFont = QStringLiteral("Georgia, \"Noto Serif\", serif");
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

QVector<Theme> allThemes()
{
    return {makeLeather(), makeWordPerfect(), makeMatrix(), makeAmber()};
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
    return ThemeId::Leather;
}

Theme themeFromSettingsId(const QString &id)
{
    return themeForId(themeIdFromString(id));
}
