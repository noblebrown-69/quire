#include "ScrivenerImport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QXmlStreamReader>

namespace {

const QString kQuireVersion = QStringLiteral("0.3.17");

QString projectFolderName(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive))
        return trimmed;
    return trimmed + QStringLiteral(".qr");
}

QString sanitizeTitle(QString title)
{
    title = title.trimmed();
    title.replace(QLatin1Char('/'), QLatin1Char('-'));
    title.replace(QLatin1Char('\\'), QLatin1Char('-'));
    title.replace(QLatin1Char(':'), QLatin1Char('-'));
    while (title.contains(QLatin1String("  ")))
        title.replace(QLatin1String("  "), QLatin1String(" "));
    title = title.trimmed();
    if (title.isEmpty())
        title = QStringLiteral("Untitled");
    return title;
}

bool writeTextFile(const QString &path, const QString &text)
{
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    return f.write(bytes) == bytes.size();
}

QString uniqueChildPath(const QString &dir, const QString &baseName, const QString &suffix)
{
    QString candidate = dir + QLatin1Char('/') + baseName + suffix;
    if (!QFileInfo::exists(candidate))
        return candidate;
    for (int i = 2; i < 10000; ++i) {
        candidate = dir + QLatin1Char('/') + baseName + QLatin1Char(' ')
                    + QString::number(i) + suffix;
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return dir + QLatin1Char('/') + baseName + QStringLiteral(" dup") + suffix;
}

QString uniqueDirPath(const QString &parent, const QString &baseName)
{
    QString candidate = parent + QLatin1Char('/') + baseName;
    if (!QFileInfo::exists(candidate))
        return candidate;
    for (int i = 2; i < 10000; ++i) {
        candidate = parent + QLatin1Char('/') + baseName + QLatin1Char(' ')
                    + QString::number(i);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return parent + QLatin1Char('/') + baseName + QStringLiteral(" dup");
}

bool readFileBytes(const QString &path, QByteArray *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    *out = f.readAll();
    return true;
}

QString decodeRtfBytes(const QByteArray &bytes)
{
    // Prefer UTF-8; fall back to Latin-1 for older Windows RTF.
    const QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar::ReplacementCharacter))
        return utf8;
    return QString::fromLatin1(bytes);
}

int skipRtfGroup(const QString &rtf, int i)
{
    // i points at '{'; depth starts at 1.
    int depth = 0;
    const int n = rtf.size();
    while (i < n) {
        const QChar c = rtf.at(i);
        if (c == QLatin1Char('\\') && i + 1 < n) {
            const QChar n1 = rtf.at(i + 1);
            if (n1 == QLatin1Char('\\') || n1 == QLatin1Char('{') || n1 == QLatin1Char('}')) {
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (c == QLatin1Char('{')) {
            ++depth;
            ++i;
            continue;
        }
        if (c == QLatin1Char('}')) {
            ++i;
            --depth;
            if (depth <= 0)
                return i;
            continue;
        }
        ++i;
    }
    return n;
}

bool isDestinedSkipControl(const QString &word)
{
    static const QSet<QString> skip = {
        QStringLiteral("fonttbl"), QStringLiteral("colortbl"), QStringLiteral("stylesheet"),
        QStringLiteral("info"), QStringLiteral("listtable"), QStringLiteral("listoverridetable"),
        QStringLiteral("header"), QStringLiteral("footer"), QStringLiteral("headerl"),
        QStringLiteral("headerr"), QStringLiteral("footerl"), QStringLiteral("footerr"),
        QStringLiteral("footnote"), QStringLiteral("ftnsep"), QStringLiteral("ftnsepc"),
        QStringLiteral("aftnsep"), QStringLiteral("aftnsepc"), QStringLiteral("object"),
        QStringLiteral("pict"), QStringLiteral("nonshppict"), QStringLiteral("shppict"),
        QStringLiteral("datafield"), QStringLiteral("field"), QStringLiteral("fldinst"),
        QStringLiteral("xmlnstbl"), QStringLiteral("rsidtbl"), QStringLiteral("generator"),
    };
    return skip.contains(word);
}

QChar fromRtfAnsiByte(int code)
{
    static const ushort kMap[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
    };
    if (code < 0)
        code += 256;
    if (code >= 0x80 && code <= 0x9F) {
        const ushort mapped = kMap[code - 0x80];
        if (mapped)
            return QChar(mapped);
        return QChar();
    }
    return QChar(code & 0xFF);
}

} // namespace

QString ScrivenerImport::rtfToSimpleHtml(const QString &rtf)
{
    if (rtf.trimmed().isEmpty())
        return QStringLiteral("<p></p>");

    QString body;
    body.reserve(rtf.size());
    bool bold = false;
    bool italic = false;
    bool inParagraph = false;
    int unicodeFallbackLen = 1;
    QString paraAlign; // empty = left; "center"/"right"/"justify"
    auto ensureP = [&]() {
        if (!inParagraph) {
            if (paraAlign.isEmpty())
                body += QStringLiteral("<p>");
            else
                body += QStringLiteral("<p style=\"text-align:%1\">").arg(paraAlign);
            inParagraph = true;
            if (bold)
                body += QStringLiteral("<strong>");
            if (italic)
                body += QStringLiteral("<em>");
        }
    };
    auto closeP = [&]() {
        if (inParagraph) {
            if (italic)
                body += QStringLiteral("</em>");
            if (bold)
                body += QStringLiteral("</strong>");
            body += QStringLiteral("</p>");
            inParagraph = false;
        }
    };
    auto appendText = [&](const QString &t) {
        if (t.isEmpty())
            return;
        ensureP();
        QString esc;
        esc.reserve(t.size());
        for (const QChar ch : t) {
            if (ch == QLatin1Char('&'))
                esc += QStringLiteral("&amp;");
            else if (ch == QLatin1Char('<'))
                esc += QStringLiteral("&lt;");
            else if (ch == QLatin1Char('>'))
                esc += QStringLiteral("&gt;");
            else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'))
                continue;
            else
                esc += ch;
        }
        body += esc;
    };

    const int n = rtf.size();
    int i = 0;
    while (i < n) {
        const QChar c = rtf.at(i);
        if (c == QLatin1Char('{')) {
            // Peek group destination
            int j = i + 1;
            while (j < n && rtf.at(j).isSpace())
                ++j;
            if (j < n && rtf.at(j) == QLatin1Char('\\')) {
                ++j;
                if (j < n && rtf.at(j) == QLatin1Char('*')) {
                    // skip {\* ... }
                    i = skipRtfGroup(rtf, i);
                    continue;
                }
                QString word;
                while (j < n && rtf.at(j).isLetter()) {
                    word += rtf.at(j);
                    ++j;
                }
                if (isDestinedSkipControl(word)) {
                    i = skipRtfGroup(rtf, i);
                    continue;
                }
            }
            ++i;
            continue;
        }
        if (c == QLatin1Char('}')) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= n)
                break;
            const QChar n1 = rtf.at(i + 1);
            if (n1 == QLatin1Char('\\') || n1 == QLatin1Char('{') || n1 == QLatin1Char('}')) {
                appendText(QString(n1));
                i += 2;
                continue;
            }
            if (n1 == QLatin1Char('\'')) {
                if (i + 3 < n) {
                    bool ok = false;
                    const int code = QString(rtf.mid(i + 2, 2)).toInt(&ok, 16);
                    if (ok) {
                        const QChar mapped = fromRtfAnsiByte(code);
                        if (!mapped.isNull())
                            appendText(QString(mapped));
                    }
                }
                i += 4;
                continue;
            }
            if (n1 == QLatin1Char('\n') || n1 == QLatin1Char('\r')) {
                // escaped newline = space
                appendText(QStringLiteral(" "));
                i += 2;
                continue;
            }
            // control word
            int j = i + 1;
            QString word;
            while (j < n && rtf.at(j).isLetter()) {
                word += rtf.at(j);
                ++j;
            }
            bool neg = false;
            if (j < n && rtf.at(j) == QLatin1Char('-')) {
                neg = true;
                ++j;
            }
            int num = 0;
            bool hasNum = false;
            while (j < n && rtf.at(j).isDigit()) {
                hasNum = true;
                num = num * 10 + (rtf.at(j).unicode() - '0');
                ++j;
            }
            if (neg)
                num = -num;
            if (j < n && rtf.at(j) == QLatin1Char(' '))
                ++j;
            i = j;

            if (word == QLatin1String("pard")) {
                closeP();
                paraAlign.clear();
                continue;
            }
            if (word == QLatin1String("par") || word == QLatin1String("line")) {
                closeP();
                continue;
            }
            if (word == QLatin1String("qc")) {
                paraAlign = QStringLiteral("center");
                continue;
            }
            if (word == QLatin1String("ql")) {
                paraAlign.clear();
                continue;
            }
            if (word == QLatin1String("qr")) {
                paraAlign = QStringLiteral("right");
                continue;
            }
            if (word == QLatin1String("qj")) {
                paraAlign = QStringLiteral("justify");
                continue;
            }
            if (word == QLatin1String("b")) {
                ensureP();
                if (hasNum && num == 0) {
                    if (bold) {
                        body += QStringLiteral("</strong>");
                        bold = false;
                    }
                } else if (!bold) {
                    body += QStringLiteral("<strong>");
                    bold = true;
                }
                continue;
            }
            if (word == QLatin1String("i")) {
                ensureP();
                if (hasNum && num == 0) {
                    if (italic) {
                        body += QStringLiteral("</em>");
                        italic = false;
                    }
                } else if (!italic) {
                    body += QStringLiteral("<em>");
                    italic = true;
                }
                continue;
            }
            if (word == QLatin1String("uc") && hasNum) {
                unicodeFallbackLen = num < 0 ? 0 : num;
                continue;
            }
            if (word == QLatin1String("u") && hasNum) {
                int cp = num;
                if (cp < 0)
                    cp += 65536;
                int skip = unicodeFallbackLen;
                while (skip > 0 && i < n) {
                    if (i + 3 < n && rtf.at(i) == QLatin1Char('\\')
                        && rtf.at(i + 1) == QLatin1Char('\'')) {
                        i += 4;
                        --skip;
                        continue;
                    }
                    const QChar nxt = rtf.at(i);
                    if (nxt == QLatin1Char('\\') || nxt == QLatin1Char('{')
                        || nxt == QLatin1Char('}'))
                        break;
                    if (nxt == QLatin1Char('\n') || nxt == QLatin1Char('\r'))
                        break;
                    ++i;
                    --skip;
                }
                if (cp > 0)
                    appendText(QString(QChar(cp)));
                continue;
            }
            if (word == QLatin1String("tab")) {
                appendText(QStringLiteral("\t"));
                continue;
            }
            if (word == QLatin1String("emdash")) {
                appendText(QStringLiteral("—"));
                continue;
            }
            if (word == QLatin1String("endash")) {
                appendText(QStringLiteral("–"));
                continue;
            }
            if (word == QLatin1String("lquote") || word == QLatin1String("rquote")) {
                appendText(word.startsWith(QLatin1Char('l')) ? QStringLiteral("‘")
                                                             : QStringLiteral("’"));
                continue;
            }
            if (word == QLatin1String("ldblquote") || word == QLatin1String("rdblquote")) {
                appendText(word.startsWith(QLatin1Char('l')) ? QStringLiteral("“")
                                                             : QStringLiteral("”"));
                continue;
            }
            if (word == QLatin1String("bullet")) {
                appendText(QStringLiteral("•"));
                continue;
            }
            // ignore other controls
            continue;
        }
        // plain
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            ++i;
            continue;
        }
        appendText(QString(c));
        ++i;
    }
    closeP();
    if (body.trimmed().isEmpty())
        return QStringLiteral("<p></p>");
    return body;
}

namespace {

struct BinderItem {
    QString type;
    QString id; // numeric or UUID
    QString title;
    QString includeInCompile; // Yes/No/empty
    QString fileExtension;
    QList<BinderItem> children;
};

struct ImportState {
    QString packageRoot;
    bool uuidLayout = false;
    QStringList order;
    QStringList exclude;
    int scenes = 0;
    int folders = 0;
    int notes = 0;
};

QString localTag(const QStringView &name)
{
    const int idx = name.lastIndexOf(QLatin1Char('}'));
    if (idx >= 0)
        return name.mid(idx + 1).toString();
    return name.toString();
}

BinderItem parseBinderItem(QXmlStreamReader &xml)
{
    BinderItem item;
    const auto attrs = xml.attributes();
    item.type = attrs.value(QStringLiteral("Type")).toString();
    item.id = attrs.value(QStringLiteral("UUID")).toString();
    if (item.id.isEmpty())
        item.id = attrs.value(QStringLiteral("ID")).toString();

    while (xml.readNextStartElement()) {
        const QString tag = localTag(xml.name());
        if (tag == QLatin1String("Title")) {
            item.title = xml.readElementText();
        } else if (tag == QLatin1String("MetaData")) {
            while (xml.readNextStartElement()) {
                const QString mt = localTag(xml.name());
                if (mt == QLatin1String("IncludeInCompile"))
                    item.includeInCompile = xml.readElementText().trimmed();
                else if (mt == QLatin1String("FileExtension"))
                    item.fileExtension = xml.readElementText().trimmed();
                else
                    xml.skipCurrentElement();
            }
        } else if (tag == QLatin1String("Children")) {
            while (xml.readNextStartElement()) {
                if (localTag(xml.name()) == QLatin1String("BinderItem"))
                    item.children.append(parseBinderItem(xml));
                else
                    xml.skipCurrentElement();
            }
        } else {
            xml.skipCurrentElement();
        }
    }
    return item;
}

bool loadBinder(const QString &scrivxPath, QList<BinderItem> *roots, QString *projectTitle,
                bool *uuidLayout, QString *error)
{
    QFile f(scrivxPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not open %1").arg(scrivxPath);
        return false;
    }
    QXmlStreamReader xml(&f);
    QList<BinderItem> binderRoots;
    QString title;
    bool uuid = false;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        const QString tag = localTag(xml.name());
        if (tag == QLatin1String("ScrivenerProject")) {
            const QString ver = xml.attributes().value(QStringLiteral("Version")).toString();
            uuid = ver.startsWith(QLatin1Char('2')) || ver.startsWith(QLatin1String("3"));
        } else if (tag == QLatin1String("ProjectTitle")) {
            if (title.isEmpty())
                title = xml.readElementText().trimmed();
        } else if (tag == QLatin1String("ProjectProperties")) {
            while (xml.readNextStartElement()) {
                const QString pt = localTag(xml.name());
                if ((pt == QLatin1String("ProjectTitle") || pt == QLatin1String("Title"))
                    && title.isEmpty())
                    title = xml.readElementText().trimmed();
                else
                    xml.skipCurrentElement();
            }
        } else if (tag == QLatin1String("Binder")) {
            while (xml.readNextStartElement()) {
                if (localTag(xml.name()) == QLatin1String("BinderItem"))
                    binderRoots.append(parseBinderItem(xml));
                else
                    xml.skipCurrentElement();
            }
        }
    }
    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("XML error: %1").arg(xml.errorString());
        return false;
    }
    if (binderRoots.isEmpty()) {
        if (error)
            *error = QStringLiteral("No binder items found");
        return false;
    }
    *roots = binderRoots;
    if (projectTitle)
        *projectTitle = title;
    if (uuidLayout)
        *uuidLayout = uuid;
    return true;
}

QString resolveContentPath(const ImportState &st, const BinderItem &item)
{
    if (item.id.isEmpty())
        return {};
    if (st.uuidLayout || item.id.contains(QLatin1Char('-'))) {
        const QString base = st.packageRoot + QStringLiteral("/Files/Data/") + item.id;
        const QString rtf = base + QStringLiteral("/content.rtf");
        if (QFileInfo::exists(rtf))
            return rtf;
        const QString fodt = base + QStringLiteral("/content.fodt");
        if (QFileInfo::exists(fodt))
            return fodt; // caller treats non-rtf as empty
        return rtf; // missing ok
    }
    return st.packageRoot + QStringLiteral("/Files/Docs/") + item.id + QStringLiteral(".rtf");
}

QString resolveNotesPath(const ImportState &st, const BinderItem &item)
{
    if (item.id.isEmpty())
        return {};
    if (st.uuidLayout || item.id.contains(QLatin1Char('-'))) {
        return st.packageRoot + QStringLiteral("/Files/Data/") + item.id
               + QStringLiteral("/notes.rtf");
    }
    return st.packageRoot + QStringLiteral("/Files/Docs/") + item.id + QStringLiteral("_notes.rtf");
}

QString loadBodyHtml(const ImportState &st, const BinderItem &item)
{
    const QString path = resolveContentPath(st, item);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QStringLiteral("<p></p>");
    if (!path.endsWith(QLatin1String(".rtf"), Qt::CaseInsensitive))
        return QStringLiteral("<p></p>");
    QByteArray bytes;
    if (!readFileBytes(path, &bytes))
        return QStringLiteral("<p></p>");
    return ScrivenerImport::rtfToSimpleHtml(decodeRtfBytes(bytes));
}

void maybeWriteInspectorNote(ImportState *st, const QString &notesRoot, const BinderItem &item)
{
    const QString npath = resolveNotesPath(*st, item);
    if (npath.isEmpty() || !QFileInfo::exists(npath))
        return;
    QByteArray bytes;
    if (!readFileBytes(npath, &bytes))
        return;
    const QString html = ScrivenerImport::rtfToSimpleHtml(decodeRtfBytes(bytes));
    if (html == QLatin1String("<p></p>"))
        return;
    const QString base = sanitizeTitle(item.title.isEmpty() ? QStringLiteral("Note") : item.title)
                         + QStringLiteral(" (notes)");
    const QString out = uniqueChildPath(notesRoot, base, QStringLiteral(".html"));
    if (writeTextFile(out, html))
        ++st->notes;
}

bool includeNo(const BinderItem &item)
{
    const QString v = item.includeInCompile.trimmed().toLower();
    return v == QLatin1String("no") || v == QLatin1String("false") || v == QLatin1String("0");
}

QString relUnder(const QString &root, const QString &abs)
{
    const QString rel = QDir(root).relativeFilePath(abs);
    return QDir::fromNativeSeparators(rel);
}

void markExclude(ImportState *st, const QString &manuscriptRoot, const QString &absPath,
                 const BinderItem &item)
{
    if (!includeNo(item))
        return;
    const QString rel = relUnder(manuscriptRoot, absPath);
    if (!rel.isEmpty() && !rel.startsWith(QLatin1String("..")) && !st->exclude.contains(rel))
        st->exclude.append(rel);
}

void emitTextAsScene(ImportState *st, const QString &destDir, const QString &manuscriptRoot,
                     const BinderItem &item, const QString &notesRoot)
{
    const QString base = sanitizeTitle(item.title);
    const QString out = uniqueChildPath(destDir, base, QStringLiteral(".html"));
    writeTextFile(out, loadBodyHtml(*st, item));
    ++st->scenes;
    const QString rel = relUnder(manuscriptRoot, out);
    if (!rel.isEmpty() && !st->order.contains(rel))
        st->order.append(rel);
    markExclude(st, manuscriptRoot, out, item);
    maybeWriteInspectorNote(st, notesRoot, item);
}

void walkNotes(ImportState *st, const QString &destDir, const BinderItem &item,
               const QString &notesRoot);

void walkDraft(ImportState *st, const QString &destDir, const QString &manuscriptRoot,
               const BinderItem &item, const QString &notesRoot)
{
    const QString type = item.type;

    if (type == QLatin1String("Folder")) {
        const QString folder = uniqueDirPath(destDir, sanitizeTitle(item.title));
        QDir().mkpath(folder);
        ++st->folders;
        const QString rel = relUnder(manuscriptRoot, folder);
        if (!rel.isEmpty() && !st->order.contains(rel))
            st->order.append(rel);
        markExclude(st, manuscriptRoot, folder, item);
        for (const BinderItem &ch : item.children)
            walkDraft(st, folder, manuscriptRoot, ch, notesRoot);
        maybeWriteInspectorNote(st, notesRoot, item);
        return;
    }

    if (type == QLatin1String("Text")) {
        if (!item.children.isEmpty()) {
            // Document-with-children → folder + body scene + nested items
            const QString folder = uniqueDirPath(destDir, sanitizeTitle(item.title));
            QDir().mkpath(folder);
            ++st->folders;
            const QString relFolder = relUnder(manuscriptRoot, folder);
            if (!relFolder.isEmpty() && !st->order.contains(relFolder))
                st->order.append(relFolder);
            markExclude(st, manuscriptRoot, folder, item);
            emitTextAsScene(st, folder, manuscriptRoot, item, notesRoot);
            for (const BinderItem &ch : item.children)
                walkDraft(st, folder, manuscriptRoot, ch, notesRoot);
            return;
        }
        emitTextAsScene(st, destDir, manuscriptRoot, item, notesRoot);
        return;
    }

    // Skip binary / unknown under draft (Image/PDF/Media)
}

void walkNotes(ImportState *st, const QString &destDir, const BinderItem &item,
               const QString &notesRoot)
{
    const QString type = item.type;
    if (type == QLatin1String("Folder") || type == QLatin1String("ResearchFolder")) {
        const QString folder = uniqueDirPath(destDir, sanitizeTitle(item.title.isEmpty()
                                                                       ? QStringLiteral("Notes")
                                                                       : item.title));
        QDir().mkpath(folder);
        for (const BinderItem &ch : item.children)
            walkNotes(st, folder, ch, notesRoot);
        maybeWriteInspectorNote(st, notesRoot, item);
        return;
    }
    if (type == QLatin1String("Text")) {
        if (!item.children.isEmpty()) {
            const QString folder = uniqueDirPath(destDir, sanitizeTitle(item.title));
            QDir().mkpath(folder);
            const QString base = sanitizeTitle(item.title);
            const QString out = uniqueChildPath(folder, base, QStringLiteral(".html"));
            writeTextFile(out, loadBodyHtml(*st, item));
            ++st->notes;
            maybeWriteInspectorNote(st, notesRoot, item);
            for (const BinderItem &ch : item.children)
                walkNotes(st, folder, ch, notesRoot);
            return;
        }
        const QString base = sanitizeTitle(item.title);
        const QString out = uniqueChildPath(destDir, base, QStringLiteral(".html"));
        writeTextFile(out, loadBodyHtml(*st, item));
        ++st->notes;
        maybeWriteInspectorNote(st, notesRoot, item);
        return;
    }
    // Image/PDF/Media under notes: skip binaries in v1
}

void processTopLevel(ImportState *st, const QString &manuscriptRoot, const QString &notesRoot,
                     const BinderItem &item)
{
    const QString type = item.type;
    if (type == QLatin1String("TrashFolder"))
        return;

    if (type == QLatin1String("DraftFolder")) {
        for (const BinderItem &ch : item.children)
            walkDraft(st, manuscriptRoot, manuscriptRoot, ch, notesRoot);
        return;
    }

    if (type == QLatin1String("ResearchFolder")) {
        // Research root itself is notes/; children go under notes/
        for (const BinderItem &ch : item.children)
            walkNotes(st, notesRoot, ch, notesRoot);
        return;
    }

    // Other top-level Text/Folder → notes/
    walkNotes(st, notesRoot, item, notesRoot);
}

QString findScrivx(const QString &inputPath, QString *packageRoot, QString *error)
{
    QFileInfo info(inputPath);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("Path does not exist");
        return {};
    }
    if (info.isFile() && info.suffix().compare(QLatin1String("scrivx"), Qt::CaseInsensitive) == 0) {
        *packageRoot = info.absolutePath();
        return info.absoluteFilePath();
    }
    QString root = info.absoluteFilePath();
    if (info.isFile()) {
        if (error)
            *error = QStringLiteral("Expected a .scriv folder or .scrivx file");
        return {};
    }
    *packageRoot = root;
    QDir dir(root);
    const QStringList hits = dir.entryList({QStringLiteral("*.scrivx")}, QDir::Files);
    if (hits.isEmpty()) {
        if (error)
            *error = QStringLiteral("No .scrivx found in package");
        return {};
    }
    return dir.absoluteFilePath(hits.first());
}

} // namespace

bool ScrivenerImport::importProject(const QString &scrivPath, const QString &outQrPath,
                                    ScrivenerImportResult *result)
{
    ScrivenerImportResult local;
    ScrivenerImportResult *r = result ? result : &local;

    QString packageRoot;
    QString err;
    const QString scrivx = findScrivx(scrivPath, &packageRoot, &err);
    if (scrivx.isEmpty()) {
        r->error = err;
        return false;
    }

    QString desiredOut = outQrPath.trimmed();
    if (desiredOut.endsWith(QLatin1Char('/')))
        desiredOut.chop(1);
    // If caller passed a parent directory, build Title.qr inside it later after title known.
    const bool outIsExistingDir = QFileInfo(desiredOut).isDir()
                                  && !desiredOut.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive);

    QList<BinderItem> roots;
    QString xmlTitle;
    bool uuidLayout = false;
    if (!loadBinder(scrivx, &roots, &xmlTitle, &uuidLayout, &err)) {
        r->error = err;
        return false;
    }

    QString title = xmlTitle.trimmed();
    if (title.isEmpty()) {
        QString folderName = QFileInfo(packageRoot).fileName();
        if (folderName.endsWith(QLatin1String(".scriv"), Qt::CaseInsensitive))
            folderName.chop(6);
        title = folderName.trimmed();
    }
    if (title.isEmpty())
        title = QStringLiteral("Imported");

    QString outPath = desiredOut;
    if (outIsExistingDir || desiredOut.isEmpty()) {
        const QString parent = desiredOut.isEmpty() ? QDir::tempPath() : desiredOut;
        outPath = parent + QLatin1Char('/') + projectFolderName(title);
    } else {
        outPath = QFileInfo(desiredOut).isDir() && desiredOut.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive)
                      ? desiredOut
                      : (desiredOut.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive)
                             ? desiredOut
                             : projectFolderName(desiredOut));
        // If relative bare name without parent, keep as given when it ends with .qr
        if (!QFileInfo(outPath).isAbsolute())
            outPath = QDir::current().absoluteFilePath(outPath);
    }

    // Normalize via projectFolderName when path ends without .qr and is not an existing dir case
    if (!outPath.endsWith(QLatin1String(".qr"), Qt::CaseInsensitive))
        outPath = projectFolderName(outPath);

    r->title = title;
    r->inName = QFileInfo(packageRoot).fileName();
    r->outPath = outPath;
    r->outName = QFileInfo(outPath).fileName();

    if (QFileInfo::exists(outPath)) {
        r->error = QStringLiteral("Refusing to overwrite existing path: %1").arg(outPath);
        return false;
    }

    // Never write into the source package.
    const QString pkgCanon = QFileInfo(packageRoot).canonicalFilePath();
    const QString outParent = QFileInfo(outPath).absolutePath();
    if (!pkgCanon.isEmpty()
        && QFileInfo(outPath).absoluteFilePath().startsWith(pkgCanon + QLatin1Char('/'))) {
        r->error = QStringLiteral("Refusing to write inside the Scrivener package");
        return false;
    }
    Q_UNUSED(outParent);

    const QString manuscriptRoot = outPath + QStringLiteral("/manuscript");
    const QString notesRoot = outPath + QStringLiteral("/notes");
    QDir().mkpath(manuscriptRoot);
    QDir().mkpath(notesRoot);
    QDir().mkpath(outPath + QStringLiteral("/compile"));
    QDir().mkpath(outPath + QStringLiteral("/.autosave"));

    ImportState st;
    st.packageRoot = packageRoot;
    st.uuidLayout = uuidLayout
                    || QDir(packageRoot + QStringLiteral("/Files/Data")).exists();

    for (const BinderItem &item : roots)
        processTopLevel(&st, manuscriptRoot, notesRoot, item);

    QJsonObject meta;
    meta.insert(QStringLiteral("title"), title);
    meta.insert(QStringLiteral("author"), QStringLiteral("Noble Brown"));
    meta.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    meta.insert(QStringLiteral("quire"), kQuireVersion);
    QJsonArray orderArr;
    for (const QString &rel : st.order)
        orderArr.append(rel);
    QJsonArray excludeArr;
    for (const QString &rel : st.exclude)
        excludeArr.append(rel);
    meta.insert(QStringLiteral("order"), orderArr);
    meta.insert(QStringLiteral("exclude"), excludeArr);

    if (!writeTextFile(outPath + QStringLiteral("/quire.json"),
                       QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Indented)))) {
        r->error = QStringLiteral("Could not write quire.json");
        return false;
    }

    r->scenes = st.scenes;
    r->folders = st.folders;
    r->notes = st.notes;
    return true;
}
