#include "EpubWriter.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
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
    "body {\n"
    "  font-family: Georgia, \"Times New Roman\", serif;\n"
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
        "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n");
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
            if (!roots.isEmpty()) {
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

QString EpubWriter::sanitizeBody(const QString &html)
{
    QString s = html;
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
    return s.trimmed();
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
