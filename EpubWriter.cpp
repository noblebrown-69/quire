#include "EpubWriter.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>
#include <cstring>

#include <minizip/zip.h>

namespace {

QString xmlEscape(const QString &s)
{
    QString o = s;
    o.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    o.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    o.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    o.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    o.replace(QLatin1Char('\''), QLatin1String("&apos;"));
    return o;
}

QString toXhtmlFragment(QString html)
{
    static const QRegularExpression voidOpen(
        QStringLiteral("<(br|hr|img|meta|link|input|col|area|base|embed|source|wbr|param|track)(\\s[^>]*)?>"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(html.size() + 16);
    int pos = 0;
    auto it = voidOpen.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(pos, m.capturedStart() - pos);
        QString tag = m.captured();
        if (!tag.trimmed().endsWith(QLatin1String("/>"))) {
            if (tag.endsWith(QLatin1Char('>')))
                tag.chop(1);
            tag = tag.trimmed() + QLatin1String("/>");
        }
        out += tag;
        pos = m.capturedEnd();
    }
    out += html.mid(pos);
    return out.trimmed();
}

QByteArray utf8(const QString &s)
{
    return s.toUtf8();
}

bool addZipEntry(zipFile zf, const char *name, const QByteArray &data, bool store)
{
    zip_fileinfo zi;
    std::memset(&zi, 0, sizeof(zi));
    const int method = store ? 0 : Z_DEFLATED;
    const int level = store ? 0 : Z_DEFAULT_COMPRESSION;
    if (zipOpenNewFileInZip(zf, name, &zi, nullptr, 0, nullptr, 0, nullptr, method, level) != ZIP_OK)
        return false;
    if (!data.isEmpty()) {
        if (zipWriteInFileInZip(zf, data.constData(), static_cast<unsigned>(data.size())) != ZIP_OK) {
            zipCloseFileInZip(zf);
            return false;
        }
    }
    return zipCloseFileInZip(zf) == ZIP_OK;
}

QString sceneHref(int i)
{
    return QStringLiteral("scene-%1.xhtml").arg(i, 4, 10, QLatin1Char('0'));
}

QString wrapXhtml(const EpubWriter::Scene &scene)
{
    const QString heading = EpubWriter::headingHtml(scene);
    QString frag = toXhtmlFragment(EpubWriter::sanitizeBody(scene.bodyHtml));
    QString inner = heading;
    if (!inner.isEmpty() && !frag.isEmpty())
        inner += QLatin1Char('\n');
    inner += frag;
    if (inner.trimmed().isEmpty())
        inner = QStringLiteral("<p></p>");

    QString pageTitle = scene.title;
    if (scene.startChapter && !scene.chapterTitle.isEmpty())
        pageTitle = scene.chapterTitle;
    else if (scene.frontMatter && !scene.title.isEmpty())
        pageTitle = scene.title;
    else if (!scene.chapterTitle.isEmpty())
        pageTitle = scene.chapterTitle;

    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE html>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\"/>\n"
        "  <title>%1</title>\n"
        "  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
        "</head>\n"
        "<body>\n"
        "  <section>\n"
        "    %2\n"
        "  </section>\n"
        "</body>\n"
        "</html>\n")
        .arg(xmlEscape(pageTitle), inner);
}

const char *kCss =
    "@font-face {\n"
    "  font-family: \"Gelasio\";\n"
    "  src: url(\"fonts/Gelasio-Regular.ttf\") format(\"truetype\");\n"
    "  font-weight: 400;\n"
    "  font-style: normal;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Gelasio\";\n"
    "  src: url(\"fonts/Gelasio-Italic.ttf\") format(\"truetype\");\n"
    "  font-weight: 400;\n"
    "  font-style: italic;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Gelasio\";\n"
    "  src: url(\"fonts/Gelasio-Bold.ttf\") format(\"truetype\");\n"
    "  font-weight: 700;\n"
    "  font-style: normal;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Gelasio\";\n"
    "  src: url(\"fonts/Gelasio-BoldItalic.ttf\") format(\"truetype\");\n"
    "  font-weight: 700;\n"
    "  font-style: italic;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Georgia\";\n"
    "  src: url(\"fonts/Gelasio-Regular.ttf\") format(\"truetype\");\n"
    "  font-weight: 400;\n"
    "  font-style: normal;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Georgia\";\n"
    "  src: url(\"fonts/Gelasio-Italic.ttf\") format(\"truetype\");\n"
    "  font-weight: 400;\n"
    "  font-style: italic;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Georgia\";\n"
    "  src: url(\"fonts/Gelasio-Bold.ttf\") format(\"truetype\");\n"
    "  font-weight: 700;\n"
    "  font-style: normal;\n"
    "}\n"
    "@font-face {\n"
    "  font-family: \"Georgia\";\n"
    "  src: url(\"fonts/Gelasio-BoldItalic.ttf\") format(\"truetype\");\n"
    "  font-weight: 700;\n"
    "  font-style: italic;\n"
    "}\n"
    "body {\n"
    "  font-family: Gelasio, Georgia, \"Times New Roman\", serif;\n"
    "  font-size: 1em;\n"
    "  line-height: 1.3;\n"
    "  margin: 1em 1.2em;\n"
    "  color: #111;\n"
    "  background: #fff;\n"
    "}\n"
    "h1 {\n"
    "  font-size: 1.6em;\n"
    "  font-weight: bold;\n"
    "  text-align: center;\n"
    "  margin: 2em 0 1.2em 0;\n"
    "}\n"
    "h1.chapter {\n"
    "  page-break-before: always;\n"
    "  text-align: center;\n"
    "}\n"
    "h2 {\n"
    "  font-size: 1.2em;\n"
    "  font-weight: bold;\n"
    "  margin: 1.4em 0 0.8em 0;\n"
    "}\n"
    "p { margin: 0 0 0.8em 0; text-indent: 1.2em; }\n"
    "p:first-of-type { text-indent: 0; }\n"
    "p.scene-break, p[style*=\"text-align:center\"] {\n"
    "  text-align: center;\n"
    "  text-indent: 0;\n"
    "}\n"
    "nav ol { list-style: none; padding-left: 1em; }\n"
    "nav a { text-decoration: none; color: inherit; }\n";

QString containerXml()
{
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <rootfiles>\n"
        "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
        "  </rootfiles>\n"
        "</container>\n");
}

QString contentOpf(const QString &title, const QString &author,
                   const QString &uid, int sceneCount)
{
    const QString modified = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
    QString manifest;
    QString spine;
    manifest += QStringLiteral(
        "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
        "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n"
        "    <item id=\"font-gelasio-regular\" href=\"fonts/Gelasio-Regular.ttf\" media-type=\"font/ttf\"/>\n"
        "    <item id=\"font-gelasio-italic\" href=\"fonts/Gelasio-Italic.ttf\" media-type=\"font/ttf\"/>\n"
        "    <item id=\"font-gelasio-bold\" href=\"fonts/Gelasio-Bold.ttf\" media-type=\"font/ttf\"/>\n"
        "    <item id=\"font-gelasio-bolditalic\" href=\"fonts/Gelasio-BoldItalic.ttf\" media-type=\"font/ttf\"/>\n"
        "    <item id=\"font-gelasio-ofl\" href=\"fonts/OFL.txt\" media-type=\"text/plain\"/>\n");
    for (int i = 0; i < sceneCount; ++i) {
        const QString id = QStringLiteral("scene-%1").arg(i + 1);
        manifest += QStringLiteral("    <item id=\"%1\" href=\"%2\" media-type=\"application/xhtml+xml\"/>\n")
                        .arg(id, sceneHref(i + 1));
        spine += QStringLiteral("    <itemref idref=\"%1\"/>\n").arg(id);
    }
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"en\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:identifier id=\"bookid\">urn:uuid:%1</dc:identifier>\n"
        "    <dc:title>%2</dc:title>\n"
        "    <dc:creator>%3</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        "    <meta property=\"dcterms:modified\">%4</meta>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "%5"
        "  </manifest>\n"
        "  <spine>\n"
        "%6"
        "  </spine>\n"
        "</package>\n")
        .arg(uid, xmlEscape(title), xmlEscape(author.isEmpty() ? QStringLiteral("Unknown") : author),
             modified, manifest, spine);
}

struct NavNode {
    QString title;
    QString href;
    QVector<NavNode> kids;
};

QString navOl(const QVector<NavNode> &nodes, int indent)
{
    const QString pad(indent, QLatin1Char(' '));
    QString s = pad + QStringLiteral("<ol>\n");
    for (const NavNode &n : nodes) {
        s += pad + QStringLiteral("  <li>");
        if (!n.href.isEmpty())
            s += QStringLiteral("<a href=\"%1\">%2</a>").arg(xmlEscape(n.href), xmlEscape(n.title));
        else
            s += QStringLiteral("<a href=\"#\">%1</a>").arg(xmlEscape(n.title));
        if (!n.kids.isEmpty()) {
            s += QLatin1Char('\n');
            s += navOl(n.kids, indent + 4);
            s += pad + QStringLiteral("  ");
        }
        s += QStringLiteral("</li>\n");
    }
    s += pad + QStringLiteral("</ol>\n");
    return s;
}

QString navXhtml(const QString &bookTitle, const QVector<EpubWriter::Scene> &scenes)
{
    QVector<NavNode> roots;
    for (int i = 0; i < scenes.size(); ++i) {
        const EpubWriter::Scene &sc = scenes[i];
        const QString href = sceneHref(i + 1);
        if (sc.frontMatter) {
            if (EpubWriter::isVisuallyEmpty(sc.bodyHtml))
                continue;
            NavNode leaf;
            leaf.title = sc.title.isEmpty() ? bookTitle : sc.title;
            leaf.href = href;
            roots.append(leaf);
            continue;
        }
        if (!sc.chapterTitle.isEmpty()) {
            if (sc.startChapter) {
                NavNode ch;
                ch.title = sc.chapterTitle;
                ch.href = href;
                roots.append(ch);
            }
            const bool duplicateLeaf = sc.startChapter
                && !sc.title.isEmpty()
                && sc.title.compare(sc.chapterTitle, Qt::CaseInsensitive) == 0;
            if (!roots.isEmpty() && !duplicateLeaf) {
                NavNode kid;
                kid.title = sc.title.isEmpty() ? sc.chapterTitle : sc.title;
                kid.href = href;
                roots.last().kids.append(kid);
            }
            continue;
        }
        if (EpubWriter::isVisuallyEmpty(sc.bodyHtml) && sc.title.isEmpty())
            continue;
        NavNode leaf;
        leaf.title = sc.title.isEmpty() ? bookTitle : sc.title;
        leaf.href = href;
        roots.append(leaf);
    }
    if (roots.isEmpty()) {
        NavNode empty;
        empty.title = bookTitle;
        empty.href = sceneHref(1);
        roots.append(empty);
    }
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE html>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"en\" lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\"/>\n"
        "  <title>Contents</title>\n"
        "  <link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n"
        "</head>\n"
        "<body>\n"
        "  <nav epub:type=\"toc\" id=\"toc\" role=\"doc-toc\">\n"
        "    <h1>Contents</h1>\n"
        "%1"
        "  </nav>\n"
        "</body>\n"
        "</html>\n")
        .arg(navOl(roots, 4));
}


QString rebalanceInlineAcrossParagraphs(const QString &html)
{
    static const QStringList inlineNames{
        QStringLiteral("em"), QStringLiteral("i"),
        QStringLiteral("strong"), QStringLiteral("b"),
    };
    static const QStringList voidNames{
        QStringLiteral("br"), QStringLiteral("hr"), QStringLiteral("img"),
        QStringLiteral("meta"), QStringLiteral("link"), QStringLiteral("input"),
        QStringLiteral("col"), QStringLiteral("area"), QStringLiteral("base"),
        QStringLiteral("embed"), QStringLiteral("source"), QStringLiteral("wbr"),
        QStringLiteral("param"), QStringLiteral("track"),
    };
    auto isInline = [&](const QString &n) {
        return inlineNames.contains(n, Qt::CaseInsensitive);
    };
    auto isVoid = [&](const QString &n) {
        return voidNames.contains(n, Qt::CaseInsensitive);
    };

    QString out;
    out.reserve(html.size() + 64);
    QStringList stack;
    bool inP = false;

    auto emitCloseInlines = [&]() {
        for (int i = stack.size() - 1; i >= 0; --i)
            out += QLatin1String("</") + stack.at(i) + QLatin1Char('>');
    };
    auto emitOpenInlines = [&]() {
        for (const QString &n : stack)
            out += QLatin1Char('<') + n + QLatin1Char('>');
    };

    const int n = html.size();
    int i = 0;
    while (i < n) {
        if (html.at(i) != QLatin1Char('<')) {
            int j = html.indexOf(QLatin1Char('<'), i);
            if (j < 0)
                j = n;
            out += html.mid(i, j - i);
            i = j;
            continue;
        }
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            const int end = html.indexOf(QLatin1String("-->"), i + 4);
            if (end < 0) {
                out += html.mid(i);
                break;
            }
            out += html.mid(i, end + 3 - i);
            i = end + 3;
            continue;
        }
        const int gt = html.indexOf(QLatin1Char('>'), i + 1);
        if (gt < 0) {
            out += html.mid(i);
            break;
        }
        const QString raw = html.mid(i, gt + 1 - i);
        i = gt + 1;

        int k = 1;
        bool closing = false;
        if (k < raw.size() && raw.at(k) == QLatin1Char('/')) {
            closing = true;
            ++k;
        }
        while (k < raw.size() && raw.at(k).isSpace())
            ++k;
        const int nameStart = k;
        while (k < raw.size() && (raw.at(k).isLetterOrNumber() || raw.at(k) == QLatin1Char('-')
                                  || raw.at(k) == QLatin1Char(':')))
            ++k;
        const QString name = raw.mid(nameStart, k - nameStart).toLower();
        const bool selfClose = raw.trimmed().endsWith(QLatin1String("/>")) || isVoid(name);

        if (name.isEmpty() || selfClose) {
            out += raw;
            continue;
        }
        if (name == QLatin1String("p")) {
            if (closing) {
                if (inP)
                    emitCloseInlines();
                out += raw;
                inP = false;
            } else {
                if (inP) {
                    emitCloseInlines();
                    out += QStringLiteral("</p>");
                    inP = false;
                }
                out += raw;
                emitOpenInlines();
                inP = true;
            }
            continue;
        }
        if (isInline(name)) {
            if (closing) {
                const int idx = stack.lastIndexOf(name);
                if (idx < 0)
                    continue;
                if (inP) {
                    for (int s = stack.size() - 1; s > idx; --s)
                        out += QLatin1String("</") + stack.at(s) + QLatin1Char('>');
                    out += QLatin1String("</") + name + QLatin1Char('>');
                }
                while (stack.size() > idx)
                    stack.removeLast();
            } else {
                out += raw;
                stack.append(name);
            }
            continue;
        }
        out += raw;
    }
    if (inP) {
        emitCloseInlines();
        out += QStringLiteral("</p>");
    }
    return out;
}

ushort mapCp1252C1(ushort u)
{
    static const ushort kMap[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
    };
    if (u < 0x80 || u > 0x9F)
        return u;
    return kMap[u - 0x80];
}


QString centerSceneBreakParagraphs(const QString &html)
{
    // Heal Scrivener-style separators that lost \\qc: >>--->, #, ***, ••• alone in a <p>.
    static const QRegularExpression paraRe(
        QStringLiteral("<p(\\s[^>]*)?>([\\s\\S]*?)</p>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    QString out;
    out.reserve(html.size() + 32);
    int pos = 0;
    auto it = paraRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(pos, m.capturedStart() - pos);
        const QString attrs = m.captured(1);
        QString inner = m.captured(2);
        pos = m.capturedEnd();

        if (attrs.contains(QStringLiteral("text-align"), Qt::CaseInsensitive)
            || attrs.contains(QStringLiteral("scene-break"), Qt::CaseInsensitive)) {
            out += m.captured(0);
            continue;
        }

        QString plain = inner;
        plain.replace(tagRe, QString());
        plain.replace(QStringLiteral("&gt;"), QStringLiteral(">"), Qt::CaseInsensitive);
        plain.replace(QStringLiteral("&lt;"), QStringLiteral("<"), Qt::CaseInsensitive);
        plain.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
        plain.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
        plain.replace(QChar(0x2003), QLatin1Char(' '));
        plain = plain.trimmed();

        const bool isBreak =
            plain == QLatin1String(">>--->")
            || plain == QLatin1String("#")
            || plain == QLatin1String("***")
            || plain == QLatin1String("* * *")
            || plain == QLatin1String("•••")
            || plain == QString::fromUtf8("• • •");
        if (!isBreak) {
            out += m.captured(0);
            continue;
        }

        QString a = attrs;
        if (a.isEmpty())
            a = QStringLiteral(" class=\"scene-break\" style=\"text-align:center\"");
        else
            a += QStringLiteral(" class=\"scene-break\" style=\"text-align:center\"");
        out += QStringLiteral("<p%1>%2</p>").arg(a, inner);
    }
    out += html.mid(pos);
    return out;
}

QString repairCp1252C1(const QString &s)
{
    QString o;
    o.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const ushort u = s.at(i).unicode();
        if (u < 0x80 || u > 0x9F) {
            o += s.at(i);
            continue;
        }
        const ushort mapped = mapCp1252C1(u);
        if (mapped == 0)
            continue;
        if (!o.isEmpty() && o.at(o.size() - 1).unicode() == mapped)
            continue;
        o += QChar(mapped);
    }
    return o;
}

} // namespace

bool EpubWriter::isGenericSceneTitle(const QString &title)
{
    const QString t = title.trimmed();
    if (t.isEmpty())
        return true;
    static const QRegularExpression re(QStringLiteral("^Scene\\s*\\d+$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(t).hasMatch();
}

QString EpubWriter::healBody(const QString &html)
{
    return centerSceneBreakParagraphs(repairCp1252C1(html));
}

QString EpubWriter::sanitizeBody(const QString &html)
{
    QString s = healBody(html);
    static const QRegularExpression emptyP(
        QStringLiteral("<p\\b[^>]*>\\s*(?:<br\\b[^>]*/?>\\s*)?</p>"),
        QRegularExpression::CaseInsensitiveOption);
    s.replace(emptyP, QString());
    s = s.trimmed();
    if (s.isEmpty())
        return s;

    const int firstTag = s.indexOf(QLatin1Char('<'));
    if (firstTag < 0)
        return QStringLiteral("<p>%1</p>").arg(s);

    if (firstTag > 0) {
        const QString lead = s.left(firstTag);
        if (!lead.trimmed().isEmpty())
            s = QStringLiteral("<p>%1</p>").arg(lead.trimmed()) + s.mid(firstTag);
        else
            s = s.mid(firstTag);
    }

    const int lastClose = s.lastIndexOf(QLatin1Char('>'));
    if (lastClose >= 0 && lastClose + 1 < s.size()) {
        const QString trail = s.mid(lastClose + 1);
        if (!trail.trimmed().isEmpty())
            s = s.left(lastClose + 1) + QStringLiteral("<p>%1</p>").arg(trail.trimmed());
        else
            s = s.left(lastClose + 1);
    }
    return rebalanceInlineAcrossParagraphs(s.trimmed());
}

QString EpubWriter::headingHtml(const Scene &scene)
{
    if (scene.frontMatter)
        return QString();
    if (scene.startChapter && !scene.chapterTitle.isEmpty())
        return QStringLiteral("<h1 class=\"chapter\">%1</h1>")
            .arg(xmlEscape(scene.chapterTitle));
    if (!EpubWriter::isGenericSceneTitle(scene.title))
        return QStringLiteral("<h2>%1</h2>").arg(xmlEscape(scene.title));
    return QString();
}

bool EpubWriter::isVisuallyEmpty(const QString &html)
{
    QString s = sanitizeBody(html);
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    s.replace(tags, QString());
    s.replace(QLatin1String("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
    s.replace(QLatin1String("&#160;"), QStringLiteral(" "));
    return s.trimmed().isEmpty();
}

bool EpubWriter::write(const QString &epubPath,
                       const QString &title,
                       const QString &author,
                       const QVector<Scene> &scenes,
                       QString *errorOut)
{
    QFileInfo info(epubPath);
    QDir().mkpath(info.absolutePath());
    if (QFileInfo::exists(epubPath) && !QFile::remove(epubPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not replace existing EPUB.");
        return false;
    }

    zipFile zf = zipOpen(epubPath.toLocal8Bit().constData(), APPEND_STATUS_CREATE);
    if (!zf) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not create EPUB zip.");
        return false;
    }

    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto fail = [&](const QString &msg) {
        zipClose(zf, nullptr);
        QFile::remove(epubPath);
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    const QByteArray mime = QByteArrayLiteral("application/epub+zip");
    if (!addZipEntry(zf, "mimetype", mime, true))
        return fail(QStringLiteral("Failed to write mimetype."));
    if (!addZipEntry(zf, "META-INF/container.xml", utf8(containerXml()), false))
        return fail(QStringLiteral("Failed to write container.xml."));

    QVector<Scene> use = scenes;
    if (use.isEmpty()) {
        Scene blank;
        blank.title = title.isEmpty() ? QStringLiteral("Manuscript") : title;
        blank.bodyHtml = QStringLiteral("<p></p>");
        blank.frontMatter = true;
        use.append(blank);
    }

    if (!addZipEntry(zf, "OEBPS/content.opf",
                     utf8(contentOpf(title, author, uid, use.size())), false))
        return fail(QStringLiteral("Failed to write content.opf."));
    if (!addZipEntry(zf, "OEBPS/nav.xhtml", utf8(navXhtml(title, use)), false))
        return fail(QStringLiteral("Failed to write nav.xhtml."));
    if (!addZipEntry(zf, "OEBPS/style.css", QByteArray(kCss), false))
        return fail(QStringLiteral("Failed to write style.css."));

    const QStringList fontFiles = {
        QStringLiteral("Gelasio-Regular.ttf"),
        QStringLiteral("Gelasio-Italic.ttf"),
        QStringLiteral("Gelasio-Bold.ttf"),
        QStringLiteral("Gelasio-BoldItalic.ttf"),
        QStringLiteral("OFL.txt"),
    };
    for (const QString &name : fontFiles) {
        QFile in(QStringLiteral(":/fonts/gelasio/") + name);
        if (!in.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Missing bundled font resource: %1").arg(name));
        const QByteArray bytes = in.readAll();
        const QByteArray zipName = QStringLiteral("OEBPS/fonts/%1").arg(name).toUtf8();
        if (!addZipEntry(zf, zipName.constData(), bytes, false))
            return fail(QStringLiteral("Failed to write font %1.").arg(name));
    }

    for (int i = 0; i < use.size(); ++i) {
        const QByteArray xhtml = utf8(wrapXhtml(use[i]));
        const QByteArray name = QStringLiteral("OEBPS/%1").arg(sceneHref(i + 1)).toUtf8();
        if (!addZipEntry(zf, name.constData(), xhtml, false))
            return fail(QStringLiteral("Failed to write %1.").arg(sceneHref(i + 1)));
    }

    if (zipClose(zf, nullptr) != ZIP_OK) {
        QFile::remove(epubPath);
        if (errorOut)
            *errorOut = QStringLiteral("Failed to close EPUB.");
        return false;
    }
    return true;
}
