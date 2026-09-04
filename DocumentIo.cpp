#include "DocumentIo.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstring>

#include <minizip/unzip.h>
#include <minizip/zip.h>

namespace {

struct InlineRun {
    QString text;
    bool bold = false;
    bool italic = false;
    QString fontFamily;
    QString fontSizePt;
};

struct TableCell {
    QVector<InlineRun> runs;
};

struct TableRow {
    QVector<TableCell> cells;
    bool header = false;
};

struct Block {
    enum Type { Paragraph, H1, H2, H3, CheckOpen, CheckDone, Table, PageBreak, Title, Author, SceneBreak };
    Type type = Paragraph;
    QVector<InlineRun> runs;
    QVector<TableRow> rows;
};

struct StyleInfo {
    QString basedOn;
    QString fontFamily;
    QString fontSizePt;
};

struct InlineState {
    bool bold = false;
    bool italic = false;
    QString fontFamily;
    QString fontSizePt;
};

QString xmlEscape(QString s)
{
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    s.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    s.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    return s;
}

QString htmlEscape(QString s)
{
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return s;
}

QString decodeEntities(QString s)
{
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&#160;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&#8203;"), QString());
    return s;
}

QString collapseWs(const QString &s)
{
    QString o;
    o.reserve(s.size());
    bool prevSpace = false;
    for (QChar c : s) {
        if (c.isSpace()) {
            if (!prevSpace)
                o.append(QLatin1Char(' '));
            prevSpace = true;
        } else {
            o.append(c);
            prevSpace = false;
        }
    }
    return o;
}

QString stripControl(const QString &s)
{
    QString o;
    o.reserve(s.size());
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u == 9 || u == 10 || u == 13 || u >= 32)
            o.append(c);
    }
    return o;
}

QString extOf(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

QString extractBody(const QString &html)
{
    const QRegularExpression re(QStringLiteral("<body[^>]*>(.*)</body>"),
                                QRegularExpression::CaseInsensitiveOption
                                    | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(html);
    if (m.hasMatch())
        return m.captured(1);
    return html;
}

QString runsToPlain(const QVector<InlineRun> &runs)
{
    QString s;
    for (const InlineRun &r : runs)
        s += r.text;
    return s.trimmed();
}

QString toCssFontFamily(const QString &raw)
{
    QString t = raw;
    t.replace(QLatin1Char(';'), QLatin1Char(','));
    QStringList parts;
    const QStringList bits = t.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString p : bits) {
        p = p.trimmed();
        p.remove(QLatin1Char('\''));
        p.remove(QLatin1Char('"'));
        p = p.trimmed();
        if (p.isEmpty())
            continue;
        parts << p;
    }
    return parts.join(QStringLiteral(", "));
}

QString halfPointsToPt(const QString &hp)
{
    bool ok = false;
    const int v = hp.trimmed().toInt(&ok);
    if (!ok || v <= 0)
        return QString();
    if (v % 2 == 0)
        return QString::number(v / 2);
    return QString::number(v / 2.0, 'f', 1);
}

int ptToHalfPoints(const QString &pt)
{
    bool ok = false;
    const double v = pt.trimmed().toDouble(&ok);
    if (!ok || v <= 0)
        return 0;
    return qRound(v * 2.0);
}

QString formatPt(double v)
{
    if (v <= 0)
        return QString();
    const double r = qRound(v);
    if (qAbs(v - r) < 0.05)
        return QString::number(int(r));
    return QString::number(v, 'f', 1);
}

bool sameRunStyle(const InlineRun &a, const InlineRun &b)
{
    return a.bold == b.bold && a.italic == b.italic
        && a.fontFamily == b.fontFamily && a.fontSizePt == b.fontSizePt;
}

QString runsToHtml(const QVector<InlineRun> &runs)
{
    QString s;
    for (const InlineRun &r : runs) {
        QString t = htmlEscape(r.text);
        if (t.isEmpty())
            continue;
        if (r.bold && r.italic)
            t = QStringLiteral("<strong><em>") + t + QStringLiteral("</em></strong>");
        else if (r.bold)
            t = QStringLiteral("<strong>") + t + QStringLiteral("</strong>");
        else if (r.italic)
            t = QStringLiteral("<em>") + t + QStringLiteral("</em>");
        QString style;
        if (!r.fontFamily.isEmpty())
            style += QStringLiteral("font-family:") + toCssFontFamily(r.fontFamily) + QLatin1Char(';');
        if (!r.fontSizePt.isEmpty())
            style += QStringLiteral("font-size:") + r.fontSizePt + QStringLiteral("pt;");
        if (!style.isEmpty()) {
            style.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
            t = QStringLiteral("<span style=\"") + style + QStringLiteral("\">") + t + QStringLiteral("</span>");
        }
        s += t;
    }
    return s;
}

QString runsToMarkdown(const QVector<InlineRun> &runs)
{
    QString s;
    for (const InlineRun &r : runs) {
        QString t = r.text;
        if (t.isEmpty())
            continue;
        if (r.bold && r.italic)
            s += QStringLiteral("***") + t + QStringLiteral("***");
        else if (r.bold)
            s += QStringLiteral("**") + t + QStringLiteral("**");
        else if (r.italic)
            s += QStringLiteral("*") + t + QStringLiteral("*");
        else
            s += t;
    }
    return s;
}

QString inlineMarkdownToHtml(const QString &in)
{
    QString out;
    const int n = in.size();
    int i = 0;
    while (i < n) {
        if (i + 1 < n && in.at(i) == QLatin1Char('*') && in.at(i + 1) == QLatin1Char('*')) {
            const int close = in.indexOf(QStringLiteral("**"), i + 2);
            if (close > i) {
                out += QStringLiteral("<strong>") + htmlEscape(in.mid(i + 2, close - i - 2))
                       + QStringLiteral("</strong>");
                i = close + 2;
                continue;
            }
        }
        if (in.at(i) == QLatin1Char('*')) {
            const int close = in.indexOf(QLatin1Char('*'), i + 1);
            if (close > i) {
                out += QStringLiteral("<em>") + htmlEscape(in.mid(i + 1, close - i - 1))
                       + QStringLiteral("</em>");
                i = close + 1;
                continue;
            }
        }
        const int start = i;
        while (i < n && in.at(i) != QLatin1Char('*'))
            ++i;
        out += htmlEscape(in.mid(start, i - start));
    }
    return out;
}

QString tagNameOf(const QString &raw)
{
    QString t = raw.trimmed();
    if (t.startsWith(QLatin1Char('/')))
        t = t.mid(1);
    if (t.endsWith(QLatin1Char('/')))
        t.chop(1);
    t = t.trimmed();
    int sp = -1;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c.isSpace() || c == QLatin1Char('/')) {
            sp = i;
            break;
        }
    }
    return (sp < 0 ? t : t.left(sp)).toLower();
}

bool tagIsClose(const QString &raw)
{
    return raw.trimmed().startsWith(QLatin1Char('/'));
}

bool tagSelfClose(const QString &raw, const QString &name)
{
    const QString t = raw.trimmed();
    if (t.endsWith(QLatin1Char('/')))
        return true;
    return name == QLatin1String("br") || name == QLatin1String("hr")
        || name == QLatin1String("input") || name == QLatin1String("img")
        || name == QLatin1String("meta") || name == QLatin1String("link")
        || name == QLatin1String("col");
}

QString attrValue(const QString &raw, const QString &key)
{
    const QRegularExpression dq(QStringLiteral("\\b") + key + QStringLiteral("\\s*=\\s*\"([^\"]*)\""),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = dq.match(raw);
    if (m.hasMatch())
        return decodeEntities(m.captured(1));
    const QRegularExpression sq(QStringLiteral("\\b") + key + QStringLiteral("\\s*=\\s*'([^']*)'"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m1 = sq.match(raw);
    if (m1.hasMatch())
        return decodeEntities(m1.captured(1));
    const QRegularExpression re2(QStringLiteral("\\b") + key + QStringLiteral("\\s*=\\s*([^\\s>]+)"),
                                 QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m2 = re2.match(raw);
    if (m2.hasMatch())
        return decodeEntities(m2.captured(1));
    return QString();
}

bool hasBareAttr(const QString &raw, const QString &key)
{
    const QRegularExpression re(QStringLiteral("(?:^|[\\s/])") + key
                                    + QStringLiteral("(?=[\\s=/>]|$)"),
                                QRegularExpression::CaseInsensitiveOption);
    return re.match(raw).hasMatch();
}

void applyCssStyle(const QString &style, InlineState &st)
{
    const QRegularExpression famRe(QStringLiteral("font-family\\s*:\\s*([^;]+)"),
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch fm = famRe.match(style);
    if (fm.hasMatch()) {
        QString f = fm.captured(1).trimmed();
        f.replace(QLatin1Char('\''), QString());
        f.replace(QLatin1Char('"'), QString());
        f = collapseWs(f).trimmed();
        if (!f.isEmpty())
            st.fontFamily = f;
    }
    const QRegularExpression szRe(QStringLiteral("font-size\\s*:\\s*([0-9.]+)\\s*(pt|px|em)?"),
                                  QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch sm = szRe.match(style);
    if (sm.hasMatch()) {
        double v = sm.captured(1).toDouble();
        const QString unit = sm.captured(2).toLower();
        if (unit == QLatin1String("px"))
            v = v * 72.0 / 96.0;
        else if (unit == QLatin1String("em"))
            v = v * 12.0;
        st.fontSizePt = formatPt(v);
    }
    const QString low = style.toLower();
    if (low.contains(QLatin1String("font-weight"))) {
        if (low.contains(QLatin1String("bold")) || low.contains(QLatin1String("font-weight:700"))
            || low.contains(QLatin1String("font-weight: 700")))
            st.bold = true;
        else if (low.contains(QLatin1String("normal")) || low.contains(QLatin1String("font-weight:400"))
                 || low.contains(QLatin1String("font-weight: 400")))
            st.bold = false;
    }
    if (low.contains(QLatin1String("font-style"))) {
        if (low.contains(QLatin1String("italic")) || low.contains(QLatin1String("oblique")))
            st.italic = true;
        else if (low.contains(QLatin1String("normal")))
            st.italic = false;
    }
}

void flushRun(QVector<InlineRun> &runs, QString &buf, const InlineState &st)
{
    const QString t = buf;
    buf.clear();
    if (t.isEmpty())
        return;
    InlineRun r;
    r.text = t;
    r.bold = st.bold;
    r.italic = st.italic;
    r.fontFamily = st.fontFamily;
    r.fontSizePt = st.fontSizePt;
    if (!runs.isEmpty() && sameRunStyle(runs.last(), r))
        runs.last().text += r.text;
    else
        runs.append(r);
}

void tidyRuns(QVector<InlineRun> &runs)
{
    for (InlineRun &r : runs)
        r.text = collapseWs(r.text);
    while (!runs.isEmpty() && runs.first().text.startsWith(QLatin1Char(' ')))
        runs.first().text.remove(0, 1);
    while (!runs.isEmpty() && runs.last().text.endsWith(QLatin1Char(' ')))
        runs.last().text.chop(1);
    while (!runs.isEmpty() && runs.first().text.isEmpty())
        runs.removeFirst();
    while (!runs.isEmpty() && runs.last().text.isEmpty())
        runs.removeLast();
}

void flushBlock(QVector<Block> &blocks, QVector<InlineRun> &runs, QString &buf,
                const InlineState &st, Block::Type type)
{
    flushRun(runs, buf, st);
    tidyRuns(runs);
    if (runs.isEmpty() && type != Block::CheckOpen && type != Block::CheckDone && type != Block::PageBreak)
        return;
    Block b;
    b.type = type;
    b.runs = runs;
    blocks.append(b);
    runs.clear();
}

QVector<InlineRun> parseInlineFragment(const QString &html)
{
    QVector<InlineRun> runs;
    QString buf;
    InlineState st;
    QVector<InlineState> stack;
    bool skip = false;
    int i = 0;
    const int n = html.size();
    while (i < n) {
        if (html.at(i) != QLatin1Char('<')) {
            int j = html.indexOf(QLatin1Char('<'), i);
            if (j < 0)
                j = n;
            if (!skip)
                buf += decodeEntities(html.mid(i, j - i));
            i = j;
            continue;
        }
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            const int end = html.indexOf(QStringLiteral("-->"), i + 4);
            i = (end < 0) ? n : end + 3;
            continue;
        }
        const int gt = html.indexOf(QLatin1Char('>'), i + 1);
        if (gt < 0)
            break;
        const QString raw = html.mid(i + 1, gt - i - 1);
        const QString name = tagNameOf(raw);
        const bool closing = tagIsClose(raw);
        i = gt + 1;
        if (name == QLatin1String("script") || name == QLatin1String("style")) {
            skip = !closing;
            continue;
        }
        if (skip)
            continue;
        if (name == QLatin1String("br") || name == QLatin1String("p") || name == QLatin1String("div")
            || name == QLatin1String("h1") || name == QLatin1String("h2") || name == QLatin1String("h3")
            || name == QLatin1String("h4") || name == QLatin1String("li")) {
            buf += QLatin1Char(' ');
            continue;
        }
        if (name == QLatin1String("b") || name == QLatin1String("strong")) {
            flushRun(runs, buf, st);
            st.bold = !closing;
            continue;
        }
        if (name == QLatin1String("i") || name == QLatin1String("em")) {
            flushRun(runs, buf, st);
            st.italic = !closing;
            continue;
        }
        if (name == QLatin1String("span") || name == QLatin1String("font")) {
            if (closing) {
                flushRun(runs, buf, st);
                if (!stack.isEmpty())
                    st = stack.takeLast();
            } else {
                flushRun(runs, buf, st);
                stack.append(st);
                const QString style = attrValue(raw, QStringLiteral("style"));
                if (!style.isEmpty())
                    applyCssStyle(style, st);
                const QString face = attrValue(raw, QStringLiteral("face"));
                if (!face.isEmpty())
                    st.fontFamily = face.trimmed();
            }
            continue;
        }
        Q_UNUSED(tagSelfClose(raw, name));
    }
    flushRun(runs, buf, st);
    tidyRuns(runs);
    return runs;
}

int findCloseTag(const QString &html, int from, const QString &close)
{
    return html.indexOf(close, from, Qt::CaseInsensitive);
}

Block parseHtmlTable(const QString &inner)
{
    Block b;
    b.type = Block::Table;
    int i = 0;
    const int n = inner.size();
    TableRow row;
    bool inRow = false;
    bool firstRow = true;
    auto finishRow = [&]() {
        if (!inRow)
            return;
        if (firstRow)
            row.header = true;
        firstRow = false;
        if (!row.cells.isEmpty())
            b.rows.append(row);
        row = TableRow();
        inRow = false;
    };
    while (i < n) {
        const int lt = inner.indexOf(QLatin1Char('<'), i);
        if (lt < 0)
            break;
        const int gt = inner.indexOf(QLatin1Char('>'), lt + 1);
        if (gt < 0)
            break;
        const QString raw = inner.mid(lt + 1, gt - lt - 1);
        const QString name = tagNameOf(raw);
        const bool closing = tagIsClose(raw);
        i = gt + 1;
        if (name == QLatin1String("tr")) {
            if (closing)
                finishRow();
            else {
                finishRow();
                inRow = true;
                row = TableRow();
            }
            continue;
        }
        if (name == QLatin1String("th") || name == QLatin1String("td")) {
            if (closing)
                continue;
            if (!inRow) {
                inRow = true;
                row = TableRow();
            }
            if (name == QLatin1String("th"))
                row.header = true;
            const QString close = (name == QLatin1String("th"))
                                      ? QStringLiteral("</th>")
                                      : QStringLiteral("</td>");
            const int end = findCloseTag(inner, i, close);
            const QString cellHtml = (end < 0) ? inner.mid(i) : inner.mid(i, end - i);
            TableCell cell;
            cell.runs = parseInlineFragment(cellHtml);
            row.cells.append(cell);
            i = (end < 0) ? n : end + close.size();
            continue;
        }
    }
    finishRow();
    return b;
}

QVector<Block> parseHtmlBlocks(QString html)
{
    html = extractBody(html);
    QVector<Block> blocks;
    QVector<InlineRun> runs;
    QString buf;
    InlineState st;
    QVector<InlineState> stack;
    Block::Type type = Block::Paragraph;
    bool inChecklist = false;
    bool skip = false;
    int i = 0;
    const int n = html.size();
    while (i < n) {
        if (html.at(i) != QLatin1Char('<')) {
            int j = html.indexOf(QLatin1Char('<'), i);
            if (j < 0)
                j = n;
            if (!skip)
                buf += decodeEntities(html.mid(i, j - i));
            i = j;
            continue;
        }
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            const int end = html.indexOf(QStringLiteral("-->"), i + 4);
            i = (end < 0) ? n : end + 3;
            continue;
        }
        const int gt = html.indexOf(QLatin1Char('>'), i + 1);
        if (gt < 0)
            break;
        const QString raw = html.mid(i + 1, gt - i - 1);
        const QString name = tagNameOf(raw);
        const bool closing = tagIsClose(raw);
        i = gt + 1;

        if (name == QLatin1String("script") || name == QLatin1String("style")) {
            skip = !closing;
            continue;
        }
        if (skip)
            continue;

        if (name == QLatin1String("table")) {
            if (!closing) {
                flushBlock(blocks, runs, buf, st, type);
                type = Block::Paragraph;
                const int end = findCloseTag(html, i, QStringLiteral("</table>"));
                const QString inner = (end < 0) ? html.mid(i) : html.mid(i, end - i);
                Block tb = parseHtmlTable(inner);
                if (!tb.rows.isEmpty())
                    blocks.append(tb);
                i = (end < 0) ? n : end + 8;
            }
            continue;
        }
        if (name == QLatin1String("br")) {
            buf += QLatin1Char(' ');
            continue;
        }
        if (name == QLatin1String("input")) {
            const QString itype = attrValue(raw, QStringLiteral("type")).toLower();
            if (itype == QLatin1String("checkbox") || itype.isEmpty()) {
                if (hasBareAttr(raw, QStringLiteral("checked")))
                    type = Block::CheckDone;
            }
            continue;
        }
        if (name == QLatin1String("b") || name == QLatin1String("strong")) {
            flushRun(runs, buf, st);
            st.bold = !closing;
            continue;
        }
        if (name == QLatin1String("i") || name == QLatin1String("em")) {
            flushRun(runs, buf, st);
            st.italic = !closing;
            continue;
        }
        if (name == QLatin1String("span") || name == QLatin1String("font")) {
            if (closing) {
                flushRun(runs, buf, st);
                if (!stack.isEmpty())
                    st = stack.takeLast();
            } else {
                flushRun(runs, buf, st);
                stack.append(st);
                const QString style = attrValue(raw, QStringLiteral("style"));
                if (!style.isEmpty())
                    applyCssStyle(style, st);
                const QString face = attrValue(raw, QStringLiteral("face"));
                if (!face.isEmpty())
                    st.fontFamily = face.trimmed();
            }
            continue;
        }
        if (name == QLatin1String("ul")) {
            if (!closing) {
                const QString cls = attrValue(raw, QStringLiteral("class")).toLower();
                inChecklist = cls.contains(QLatin1String("checklist"));
            } else {
                flushBlock(blocks, runs, buf, st, type);
                inChecklist = false;
                type = Block::Paragraph;
            }
            continue;
        }
        if (name == QLatin1String("li")) {
            if (closing) {
                flushBlock(blocks, runs, buf, st, type);
                type = Block::Paragraph;
            } else {
                flushBlock(blocks, runs, buf, st, type);
                const QString cls = attrValue(raw, QStringLiteral("class")).toLower();
                if (inChecklist || cls.contains(QLatin1String("task"))) {
                    type = cls.contains(QLatin1String("done")) ? Block::CheckDone : Block::CheckOpen;
                } else {
                    type = Block::Paragraph;
                }
            }
            continue;
        }
        if (name == QLatin1String("h1") || name == QLatin1String("h2") || name == QLatin1String("h3")
            || name == QLatin1String("p") || name == QLatin1String("div")
            || name == QLatin1String("h4") || name == QLatin1String("h5") || name == QLatin1String("h6")) {
            if (closing) {
                flushBlock(blocks, runs, buf, st, type);
                type = Block::Paragraph;
            } else {
                flushBlock(blocks, runs, buf, st, type);
                if (name == QLatin1String("h1"))
                    type = Block::H1;
                else if (name == QLatin1String("h2"))
                    type = Block::H2;
                else if (name == QLatin1String("h3"))
                    type = Block::H3;
                else
                    type = Block::Paragraph;
            }
            continue;
        }
        Q_UNUSED(tagSelfClose(raw, name));
    }
    flushBlock(blocks, runs, buf, st, type);
    return blocks;
}

bool classHas(const QString &cls, const QString &token)
{
    const QStringList parts = cls.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (p.compare(token, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void flattenTableToParagraphs(QVector<Block> &blocks, const Block &table)
{
    for (const TableRow &row : table.rows) {
        for (const TableCell &cell : row.cells) {
            if (runsToPlain(cell.runs).isEmpty())
                continue;
            Block p;
            p.type = Block::Paragraph;
            p.runs = cell.runs;
            blocks.append(p);
        }
    }
}

QVector<Block> parseKindleHtmlBlocks(QString html)
{
    html = extractBody(html);
    QVector<Block> blocks;
    QVector<InlineRun> runs;
    QString buf;
    InlineState st;
    QVector<InlineState> stack;
    Block::Type type = Block::Paragraph;
    bool skip = false;
    int i = 0;
    const int n = html.size();
    while (i < n) {
        if (html.at(i) != QLatin1Char('<')) {
            int j = html.indexOf(QLatin1Char('<'), i);
            if (j < 0)
                j = n;
            if (!skip)
                buf += decodeEntities(html.mid(i, j - i));
            i = j;
            continue;
        }
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            const int end = html.indexOf(QStringLiteral("-->"), i + 4);
            i = (end < 0) ? n : end + 3;
            continue;
        }
        const int gt = html.indexOf(QLatin1Char('>'), i + 1);
        if (gt < 0)
            break;
        const QString raw = html.mid(i + 1, gt - i - 1);
        const QString name = tagNameOf(raw);
        const bool closing = tagIsClose(raw);
        i = gt + 1;

        if (name == QLatin1String("script") || name == QLatin1String("style")) {
            skip = !closing;
            continue;
        }
        if (skip)
            continue;

        const QString cls = attrValue(raw, QStringLiteral("class"));
        if (!closing && classHas(cls, QStringLiteral("pagebreak"))) {
            flushBlock(blocks, runs, buf, st, type);
            Block b;
            b.type = Block::PageBreak;
            blocks.append(b);
            type = Block::Paragraph;
            continue;
        }

        if (name == QLatin1String("table")) {
            if (!closing) {
                flushBlock(blocks, runs, buf, st, type);
                type = Block::Paragraph;
                const int end = findCloseTag(html, i, QStringLiteral("</table>"));
                const QString inner = (end < 0) ? html.mid(i) : html.mid(i, end - i);
                flattenTableToParagraphs(blocks, parseHtmlTable(inner));
                i = (end < 0) ? n : end + 8;
            }
            continue;
        }
        if (name == QLatin1String("br")) {
            buf += QLatin1Char(' ');
            continue;
        }
        if (name == QLatin1String("b") || name == QLatin1String("strong")) {
            flushRun(runs, buf, st);
            st.bold = !closing;
            continue;
        }
        if (name == QLatin1String("i") || name == QLatin1String("em")) {
            flushRun(runs, buf, st);
            st.italic = !closing;
            continue;
        }
        if (name == QLatin1String("span") || name == QLatin1String("font")) {
            if (closing) {
                flushRun(runs, buf, st);
                if (!stack.isEmpty())
                    st = stack.takeLast();
            } else {
                flushRun(runs, buf, st);
                stack.append(st);
                const QString style = attrValue(raw, QStringLiteral("style"));
                if (!style.isEmpty())
                    applyCssStyle(style, st);
                const QString face = attrValue(raw, QStringLiteral("face"));
                if (!face.isEmpty())
                    st.fontFamily = face.trimmed();
            }
            continue;
        }
        if (name == QLatin1String("h1") || name == QLatin1String("h2") || name == QLatin1String("h3")
            || name == QLatin1String("h4") || name == QLatin1String("h5") || name == QLatin1String("h6")
            || name == QLatin1String("p") || name == QLatin1String("div")
            || name == QLatin1String("li") || name == QLatin1String("ul") || name == QLatin1String("ol")) {
            if (closing) {
                flushBlock(blocks, runs, buf, st, type);
                type = Block::Paragraph;
            } else {
                flushBlock(blocks, runs, buf, st, type);
                if (name == QLatin1String("h1"))
                    type = Block::H1;
                else if (classHas(cls, QStringLiteral("kindle-title")))
                    type = Block::Title;
                else if (classHas(cls, QStringLiteral("kindle-author")))
                    type = Block::Author;
                else if (classHas(cls, QStringLiteral("scenebreak"))
                         || classHas(cls, QStringLiteral("scene-break")))
                    type = Block::SceneBreak;
                else
                    type = Block::Paragraph;
            }
            continue;
        }
        Q_UNUSED(tagSelfClose(raw, name));
    }
    flushBlock(blocks, runs, buf, st, type);
    return blocks;
}


QString blocksToHtml(const QVector<Block> &blocks)
{
    QString out;
    bool inCheck = false;
    auto closeCheck = [&]() {
        if (inCheck) {
            out += QStringLiteral("</ul>");
            inCheck = false;
        }
    };
    for (const Block &b : blocks) {
        if (b.type == Block::Table) {
            closeCheck();
            out += QStringLiteral("<table>");
            for (const TableRow &row : b.rows) {
                out += QStringLiteral("<tr>");
                const QString tag = row.header ? QStringLiteral("th") : QStringLiteral("td");
                for (const TableCell &cell : row.cells) {
                    out += QLatin1Char('<') + tag + QLatin1Char('>');
                    out += runsToHtml(cell.runs);
                    out += QStringLiteral("</") + tag + QLatin1Char('>');
                }
                out += QStringLiteral("</tr>");
            }
            out += QStringLiteral("</table>");
            continue;
        }
        const bool isCheck = (b.type == Block::CheckOpen || b.type == Block::CheckDone);
        if (isCheck) {
            if (!inCheck) {
                out += QStringLiteral("<ul class=\"checklist\">");
                inCheck = true;
            }
            const bool done = (b.type == Block::CheckDone);
            QString inner = runsToHtml(b.runs);
            if (inner.isEmpty())
                inner = QStringLiteral("&#8203;");
            out += QStringLiteral("<li class=\"task");
            if (done)
                out += QStringLiteral(" done");
            out += QStringLiteral("\"><input type=\"checkbox\"");
            if (done)
                out += QStringLiteral(" checked");
            out += QStringLiteral(" contenteditable=\"false\"><span>");
            out += inner;
            out += QStringLiteral("</span></li>");
            continue;
        }
        closeCheck();
        const QString inner = runsToHtml(b.runs);
        if (b.type == Block::H1)
            out += QStringLiteral("<h1>") + inner + QStringLiteral("</h1>");
        else if (b.type == Block::H2)
            out += QStringLiteral("<h2>") + inner + QStringLiteral("</h2>");
        else if (b.type == Block::H3)
            out += QStringLiteral("<h3>") + inner + QStringLiteral("</h3>");
        else
            out += QStringLiteral("<p>") + inner + QStringLiteral("</p>");
    }
    closeCheck();
    if (out.isEmpty())
        out = QStringLiteral("<p></p>");
    return out;
}

QString blocksToMarkdown(const QVector<Block> &blocks)
{
    QStringList lines;
    for (const Block &b : blocks) {
        if (b.type == Block::Table) {
            bool first = true;
            for (const TableRow &row : b.rows) {
                QStringList cells;
                for (const TableCell &cell : row.cells) {
                    QString t = runsToMarkdown(cell.runs).trimmed();
                    t.replace(QLatin1Char('|'), QStringLiteral("\\|"));
                    cells << t;
                }
                lines << (QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
                if (first && row.header) {
                    QStringList sep;
                    for (int i = 0; i < row.cells.size(); ++i)
                        sep << QStringLiteral("---");
                    lines << (QStringLiteral("| ") + sep.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
                }
                first = false;
            }
            continue;
        }
        const QString inner = runsToMarkdown(b.runs).trimmed();
        if (b.type == Block::H1)
            lines << (QStringLiteral("# ") + inner);
        else if (b.type == Block::H2)
            lines << (QStringLiteral("## ") + inner);
        else if (b.type == Block::H3)
            lines << (QStringLiteral("### ") + inner);
        else if (b.type == Block::CheckOpen)
            lines << (QStringLiteral("- [ ] ") + inner);
        else if (b.type == Block::CheckDone)
            lines << (QStringLiteral("- [x] ") + inner);
        else
            lines << inner;
    }
    QString md;
    for (int i = 0; i < lines.size(); ++i) {
        const bool check = lines.at(i).startsWith(QLatin1String("- ["));
        const bool prevCheck = (i > 0 && lines.at(i - 1).startsWith(QLatin1String("- [")));
        const bool pipe = lines.at(i).startsWith(QLatin1Char('|'));
        const bool prevPipe = (i > 0 && lines.at(i - 1).startsWith(QLatin1Char('|')));
        if (i > 0) {
            if ((check && prevCheck) || (pipe && prevPipe))
                md += QLatin1Char('\n');
            else
                md += QStringLiteral("\n\n");
        }
        md += lines.at(i);
    }
    if (!md.isEmpty() && !md.endsWith(QLatin1Char('\n')))
        md += QLatin1Char('\n');
    return md;
}

QString wText(const QString &text)
{
    QString t = stripControl(text);
    t.replace(QLatin1Char('\t'), QLatin1Char(' '));
    t.replace(QLatin1Char('\r'), QString());
    t.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (t.isEmpty())
        return QString();
    const bool preserve = t.startsWith(QLatin1Char(' ')) || t.endsWith(QLatin1Char(' '))
                          || t.contains(QStringLiteral("  "));
    QString o = QStringLiteral("<w:t");
    if (preserve)
        o += QStringLiteral(" xml:space=\"preserve\"");
    o += QLatin1Char('>');
    o += xmlEscape(t);
    o += QStringLiteral("</w:t>");
    return o;
}

QString runToWml(const InlineRun &r)
{
    if (r.text.isEmpty())
        return QString();
    QString o = QStringLiteral("<w:r>");
    const int hp = ptToHalfPoints(r.fontSizePt);
    if (r.bold || r.italic || !r.fontFamily.isEmpty() || hp > 0) {
        o += QStringLiteral("<w:rPr>");
        if (!r.fontFamily.isEmpty()) {
            const QString fam = xmlEscape(r.fontFamily);
            o += QStringLiteral("<w:rFonts w:ascii=\"") + fam
                 + QStringLiteral("\" w:hAnsi=\"") + fam + QStringLiteral("\"/>");
        }
        if (r.bold)
            o += QStringLiteral("<w:b/>");
        if (r.italic)
            o += QStringLiteral("<w:i/>");
        if (hp > 0) {
            const QString v = QString::number(hp);
            o += QStringLiteral("<w:sz w:val=\"") + v + QStringLiteral("\"/>");
            o += QStringLiteral("<w:szCs w:val=\"") + v + QStringLiteral("\"/>");
        }
        o += QStringLiteral("</w:rPr>");
    }
    o += wText(r.text);
    o += QStringLiteral("</w:r>");
    return o;
}

QString paragraphWml(const QString &styleId, const QVector<InlineRun> &runs)
{
    QString o = QStringLiteral("<w:p>");
    if (!styleId.isEmpty())
        o += QStringLiteral("<w:pPr><w:pStyle w:val=\"") + xmlEscape(styleId) + QStringLiteral("\"/></w:pPr>");
    for (const InlineRun &r : runs)
        o += runToWml(r);
    o += QStringLiteral("</w:p>");
    return o;
}

QString cellBordersWml()
{
    return QStringLiteral(
        "<w:tcBorders>"
        "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:start w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:end w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "</w:tcBorders>");
}

QString tableWml(const Block &b)
{
    int cols = 1;
    for (const TableRow &row : b.rows)
        cols = qMax(cols, row.cells.size());
    QString o = QStringLiteral("<w:tbl><w:tblPr>");
    o += QStringLiteral(
        "<w:tblW w:w=\"0\" w:type=\"auto\"/>"
        "<w:tblBorders>"
        "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "<w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"DDDDDD\"/>"
        "</w:tblBorders>"
        "</w:tblPr><w:tblGrid>");
    for (int i = 0; i < cols; ++i)
        o += QStringLiteral("<w:gridCol w:w=\"3120\"/>");
    o += QStringLiteral("</w:tblGrid>");
    for (const TableRow &row : b.rows) {
        o += QStringLiteral("<w:tr>");
        const QString styleId = row.header ? QStringLiteral("TableHeading") : QStringLiteral("TableContents");
        for (int c = 0; c < cols; ++c) {
            o += QStringLiteral("<w:tc><w:tcPr>");
            o += cellBordersWml();
            if (row.header)
                o += QStringLiteral("<w:shd w:fill=\"F0F0F0\" w:val=\"clear\"/>");
            o += QStringLiteral("</w:tcPr>");
            QVector<InlineRun> runs;
            if (c < row.cells.size())
                runs = row.cells.at(c).runs;
            if (row.header) {
                for (InlineRun &r : runs)
                    r.bold = true;
            }
            o += paragraphWml(styleId, runs);
            o += QStringLiteral("</w:tc>");
        }
        o += QStringLiteral("</w:tr>");
    }
    o += QStringLiteral("</w:tbl>");
    return o;
}

QString blocksToDocumentXml(const QVector<Block> &blocks)
{
    QString body;
    for (const Block &b : blocks) {
        if (b.type == Block::Table) {
            body += tableWml(b);
        } else if (b.type == Block::H1) {
            body += paragraphWml(QStringLiteral("Heading1"), b.runs);
        } else if (b.type == Block::H2) {
            body += paragraphWml(QStringLiteral("Heading2"), b.runs);
        } else if (b.type == Block::H3) {
            body += paragraphWml(QStringLiteral("Heading3"), b.runs);
        } else if (b.type == Block::CheckOpen || b.type == Block::CheckDone) {
            QVector<InlineRun> runs = b.runs;
            InlineRun pre;
            pre.text = (b.type == Block::CheckDone) ? QStringLiteral("- [x] ") : QStringLiteral("- [ ] ");
            runs.prepend(pre);
            body += paragraphWml(QString(), runs);
        } else {
            body += paragraphWml(QString(), b.runs);
        }
    }
    if (body.isEmpty())
        body = QStringLiteral("<w:p/>");
    body += QStringLiteral(
        "<w:sectPr>"
        "<w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\""
        " w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
        "<w:cols w:space=\"720\"/>"
        "<w:docGrid w:linePitch=\"360\"/>"
        "</w:sectPr>");

    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
               "<w:document"
               " xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\""
               " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
               " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
               " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
               " xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\""
               " mc:Ignorable=\"w14\">"
               "<w:body>")
           + body + QStringLiteral("</w:body></w:document>");
}

bool isKindleSceneBreakPlain(const QString &plain)
{
    const QString t = plain.trimmed();
    return t == QLatin1String(">>--->")
        || t == QLatin1String("#")
        || t == QLatin1String("***")
        || t == QLatin1String("* * *")
        || t == QLatin1String("•••")
        || t == QString::fromUtf8("• • •");
}

QVector<InlineRun> kindleStripRunFonts(QVector<InlineRun> runs)
{
    for (InlineRun &r : runs) {
        r.fontFamily.clear();
        r.fontSizePt.clear();
    }
    return runs;
}

QString kindleDocumentXml(const QVector<Block> &blocks)
{
    QString body;
    bool lastWasBreak = false;
    for (const Block &b : blocks) {
        if (b.type == Block::PageBreak) {
            if (lastWasBreak || body.isEmpty())
                continue;
            body += QStringLiteral("<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>");
            lastWasBreak = true;
            continue;
        }
        QVector<InlineRun> runs = kindleStripRunFonts(b.runs);
        if (b.type == Block::H1) {
            if (runsToPlain(runs).isEmpty())
                continue;
            body += paragraphWml(QStringLiteral("Heading1"), runs);
            lastWasBreak = false;
            continue;
        }
        if (b.type == Block::Title) {
            if (runsToPlain(runs).isEmpty())
                continue;
            body += paragraphWml(QStringLiteral("Title"), runs);
            lastWasBreak = false;
            continue;
        }
        if (b.type == Block::Author) {
            if (runsToPlain(runs).isEmpty())
                continue;
            body += paragraphWml(QStringLiteral("Author"), runs);
            lastWasBreak = false;
            continue;
        }
        {
            const QString plain = runsToPlain(runs);
            if (b.type == Block::SceneBreak || isKindleSceneBreakPlain(plain)) {
                InlineRun hash;
                hash.text = QStringLiteral("#");
                runs = {hash};
                body += paragraphWml(QStringLiteral("SceneBreak"), runs);
                lastWasBreak = false;
                continue;
            }
        }
        if (b.type == Block::Table) {
            for (const TableRow &row : b.rows) {
                for (const TableCell &cell : row.cells) {
                    QVector<InlineRun> cr = kindleStripRunFonts(cell.runs);
                    if (runsToPlain(cr).isEmpty())
                        continue;
                    body += paragraphWml(QStringLiteral("Normal"), cr);
                    lastWasBreak = false;
                }
            }
            continue;
        }
        if (runsToPlain(runs).isEmpty())
            continue;
        body += paragraphWml(QStringLiteral("Normal"), runs);
        lastWasBreak = false;
    }
    if (body.isEmpty())
        body = QStringLiteral("<w:p/>");
    body += QStringLiteral(
        "<w:sectPr>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\""
        " w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/>"
        "</w:sectPr>");

    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
               "<w:document"
               " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
               " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
               "<w:body>")
           + body + QStringLiteral("</w:body></w:document>");
}


const char *kContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\""
    " ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\""
    " ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "</Types>";

const char *kRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\""
    " Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\""
    " Target=\"word/document.xml\"/>"
    "</Relationships>";

const char *kDocRels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\""
    " Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\""
    " Target=\"styles.xml\"/>"
    "</Relationships>";

const char *kStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:styles xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults>"
    "<w:rPrDefault><w:rPr>"
    "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:cs=\"Calibri\"/>"
    "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
    "<w:lang w:val=\"en-US\"/>"
    "</w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\"/></w:pPr></w:pPrDefault>"
    "</w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/><w:qFormat/>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
    "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:uiPriority w:val=\"9\"/><w:qFormat/>"
    "<w:pPr><w:keepNext/><w:spacing w:before=\"360\" w:after=\"80\"/><w:outlineLvl w:val=\"0\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
    "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:uiPriority w:val=\"9\"/><w:qFormat/>"
    "<w:pPr><w:keepNext/><w:spacing w:before=\"280\" w:after=\"80\"/><w:outlineLvl w:val=\"1\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\">"
    "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:uiPriority w:val=\"9\"/><w:qFormat/>"
    "<w:pPr><w:keepNext/><w:spacing w:before=\"240\" w:after=\"40\"/><w:outlineLvl w:val=\"2\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"TableContents\">"
    "<w:name w:val=\"Table Contents\"/><w:basedOn w:val=\"Normal\"/>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"TableHeading\">"
    "<w:name w:val=\"Table Heading\"/><w:basedOn w:val=\"TableContents\"/>"
    "<w:pPr><w:jc w:val=\"left\"/></w:pPr>"
    "<w:rPr><w:b/></w:rPr>"
    "</w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\">"
    "<w:name w:val=\"Default Paragraph Font\"/><w:semiHidden/><w:unhideWhenUsed/>"
    "</w:style>"
    "</w:styles>";

const char *kKindleStyles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:styles xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults>"
    "<w:rPrDefault><w:rPr>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
    "<w:color w:val=\"000000\"/>"
    "<w:lang w:val=\"en-US\"/>"
    "</w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr>"
    "<w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "</w:pPr></w:pPrDefault>"
    "</w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/><w:qFormat/>"
    "<w:pPr>"
    "<w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "<w:ind w:firstLine=\"288\"/>"
    "</w:pPr>"
    "<w:rPr>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
    "<w:color w:val=\"000000\"/>"
    "</w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
    "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:uiPriority w:val=\"9\"/><w:qFormat/>"
    "<w:pPr>"
    "<w:keepNext/>"
    "<w:spacing w:before=\"0\" w:after=\"240\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "<w:ind w:firstLine=\"0\"/>"
    "<w:jc w:val=\"center\"/>"
    "<w:outlineLvl w:val=\"0\"/>"
    "</w:pPr>"
    "<w:rPr>"
    "<w:b/>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"28\"/><w:szCs w:val=\"28\"/>"
    "<w:color w:val=\"000000\"/>"
    "</w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Title\">"
    "<w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:qFormat/>"
    "<w:pPr>"
    "<w:spacing w:before=\"0\" w:after=\"200\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "<w:ind w:firstLine=\"0\"/>"
    "<w:jc w:val=\"center\"/>"
    "</w:pPr>"
    "<w:rPr>"
    "<w:b/>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"36\"/><w:szCs w:val=\"36\"/>"
    "<w:color w:val=\"000000\"/>"
    "</w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"Author\">"
    "<w:name w:val=\"Author\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:pPr>"
    "<w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "<w:ind w:firstLine=\"0\"/>"
    "<w:jc w:val=\"center\"/>"
    "</w:pPr>"
    "<w:rPr>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
    "<w:color w:val=\"000000\"/>"
    "</w:rPr>"
    "</w:style>"
    "<w:style w:type=\"paragraph\" w:styleId=\"SceneBreak\">"
    "<w:name w:val=\"Scene Break\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
    "<w:pPr>"
    "<w:spacing w:before=\"200\" w:after=\"200\" w:line=\"240\" w:lineRule=\"auto\"/>"
    "<w:ind w:firstLine=\"0\"/>"
    "<w:jc w:val=\"center\"/>"
    "</w:pPr>"
    "<w:rPr>"
    "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
    "<w:color w:val=\"000000\"/>"
    "</w:rPr>"
    "</w:style>"
    "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\">"
    "<w:name w:val=\"Default Paragraph Font\"/><w:semiHidden/><w:unhideWhenUsed/>"
    "</w:style>"
    "</w:styles>";

bool addZipEntry(zipFile zf, const char *name, const QByteArray &data)
{
    zip_fileinfo zi;
    std::memset(&zi, 0, sizeof(zi));
    if (zipOpenNewFileInZip(zf, name, &zi, nullptr, 0, nullptr, 0, nullptr,
                            Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK)
        return false;
    if (!data.isEmpty()) {
        if (zipWriteInFileInZip(zf, data.constData(), static_cast<unsigned>(data.size())) != ZIP_OK) {
            zipCloseFileInZip(zf);
            return false;
        }
    }
    return zipCloseFileInZip(zf) == ZIP_OK;
}

QByteArray readUtf8File(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Could not read ") + path + QStringLiteral(": ") + f.errorString();
        return {};
    }
    return f.readAll();
}

bool writeUtf8File(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("Could not finish writing ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    f.close();
    if (f.error() != QFile::NoError) {
        if (error)
            *error = QStringLiteral("Could not finish writing ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    return true;
}

QMap<QString, QByteArray> unzipAll(const QString &path, QString *error)
{
    QMap<QString, QByteArray> parts;
    unzFile uf = unzOpen(path.toLocal8Bit().constData());
    if (!uf) {
        if (error)
            *error = QStringLiteral("Could not open zip: ") + path;
        return parts;
    }
    int zerr = unzGoToFirstFile(uf);
    while (zerr == UNZ_OK) {
        unz_file_info info;
        char nameBuf[1024];
        std::memset(nameBuf, 0, sizeof(nameBuf));
        if (unzGetCurrentFileInfo(uf, &info, nameBuf, sizeof(nameBuf) - 1, nullptr, 0, nullptr, 0) != UNZ_OK) {
            if (error)
                *error = QStringLiteral("Could not read zip entry names");
            parts.clear();
            break;
        }
        const QString name = QString::fromUtf8(nameBuf);
        if (unzOpenCurrentFile(uf) != UNZ_OK) {
            if (error)
                *error = QStringLiteral("Could not open zip entry: ") + name;
            parts.clear();
            break;
        }
        QByteArray data;
        data.resize(static_cast<int>(info.uncompressed_size));
        int got = 0;
        while (got < data.size()) {
            const int n = unzReadCurrentFile(uf, data.data() + got, static_cast<unsigned>(data.size() - got));
            if (n < 0) {
                if (error)
                    *error = QStringLiteral("Could not inflate zip entry: ") + name;
                parts.clear();
                unzCloseCurrentFile(uf);
                unzClose(uf);
                return parts;
            }
            if (n == 0)
                break;
            got += n;
        }
        data.resize(got);
        unzCloseCurrentFile(uf);
        if (!name.endsWith(QLatin1Char('/')))
            parts.insert(name, data);
        zerr = unzGoToNextFile(uf);
    }
    unzClose(uf);
    return parts;
}


QString extractRFonts(const QString &frag)
{
    const QRegularExpression re(QStringLiteral("<w:rFonts\\b([^>]*)/?>"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(frag);
    if (!m.hasMatch())
        return QString();
    const QString attrs = m.captured(1);
    QString ascii = attrValue(attrs, QStringLiteral("w:ascii"));
    if (ascii.isEmpty())
        ascii = attrValue(attrs, QStringLiteral("w:hAnsi"));
    return ascii.trimmed();
}

QString extractSzPt(const QString &frag)
{
    const QRegularExpression szRe(QStringLiteral("<w:sz(?:\\s[^>]*)?w:val=\"([^\"]+)\""),
                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = szRe.match(frag);
    if (!m.hasMatch()) {
        const QRegularExpression csRe(QStringLiteral("<w:szCs(?:\\s[^>]*)?w:val=\"([^\"]+)\""),
                                      QRegularExpression::CaseInsensitiveOption);
        m = csRe.match(frag);
    }
    if (!m.hasMatch())
        return QString();
    return halfPointsToPt(m.captured(1));
}

QMap<QString, StyleInfo> parseStyleMap(const QString &xml)
{
    QMap<QString, StyleInfo> map;
    const int dd = xml.indexOf(QStringLiteral("<w:docDefaults"), 0, Qt::CaseInsensitive);
    if (dd >= 0) {
        const int dde = xml.indexOf(QStringLiteral("</w:docDefaults>"), dd, Qt::CaseInsensitive);
        if (dde > dd) {
            const QString frag = xml.mid(dd, dde - dd);
            StyleInfo def;
            def.fontFamily = extractRFonts(frag);
            def.fontSizePt = extractSzPt(frag);
            map.insert(QStringLiteral("#defaults"), def);
        }
    }
    const QRegularExpression styleRe(QStringLiteral("<w:style\\b([^>]*)>"),
                                     QRegularExpression::CaseInsensitiveOption);
    int pos = 0;
    while (true) {
        const QRegularExpressionMatch m = styleRe.match(xml, pos);
        if (!m.hasMatch())
            break;
        const int start = m.capturedStart();
        const int end = xml.indexOf(QStringLiteral("</w:style>"), start, Qt::CaseInsensitive);
        if (end < 0)
            break;
        pos = end + 10;
        const QString id = attrValue(m.captured(1), QStringLiteral("w:styleId"));
        if (id.isEmpty())
            continue;
        const QString body = xml.mid(start, end - start);
        StyleInfo s;
        const QRegularExpression based(QStringLiteral("<w:basedOn[^>]*w:val=\"([^\"]+)\""),
                                       QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch bm = based.match(body);
        if (bm.hasMatch())
            s.basedOn = bm.captured(1);
        s.fontFamily = extractRFonts(body);
        s.fontSizePt = extractSzPt(body);
        map.insert(id, s);
    }
    return map;
}

void resolveStyle(const QMap<QString, StyleInfo> &map, QString id, QString &family, QString &sizePt)
{
    QSet<QString> seen;
    QString cur = id;
    for (int hop = 0; hop < 10 && !cur.isEmpty() && !seen.contains(cur); ++hop) {
        seen.insert(cur);
        if (!map.contains(cur))
            break;
        const StyleInfo s = map.value(cur);
        if (family.isEmpty() && !s.fontFamily.isEmpty())
            family = s.fontFamily;
        if (sizePt.isEmpty() && !s.fontSizePt.isEmpty())
            sizePt = s.fontSizePt;
        if (!family.isEmpty() && !sizePt.isEmpty())
            return;
        cur = s.basedOn;
    }
    if (map.contains(QStringLiteral("#defaults"))) {
        const StyleInfo d = map.value(QStringLiteral("#defaults"));
        if (family.isEmpty())
            family = d.fontFamily;
        if (sizePt.isEmpty())
            sizePt = d.fontSizePt;
    }
}

bool wValOn(const QString &rPr, const QString &tag)
{
    const QRegularExpression re(QStringLiteral("<w:") + tag + QStringLiteral("(/|>|\\s[^>]*>)"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(rPr);
    if (!m.hasMatch())
        return false;
    const QString full = rPr.mid(m.capturedStart(), m.capturedLength()).toLower();
    if (full.contains(QLatin1String("w:val=\"0\"")) || full.contains(QLatin1String("w:val=\"false\""))
        || full.contains(QLatin1String("w:val=\"off\"")))
        return false;
    return true;
}

QVector<InlineRun> parseRunsFromXml(const QString &p, QString defFamily, QString defSize)
{
    const int pPrS = p.indexOf(QStringLiteral("<w:pPr"), 0, Qt::CaseInsensitive);
    const int pPrE = p.indexOf(QStringLiteral("</w:pPr>"), 0, Qt::CaseInsensitive);
    if (pPrS >= 0 && pPrE > pPrS) {
        const QString pPr = p.mid(pPrS, pPrE - pPrS);
        const QString f = extractRFonts(pPr);
        const QString s = extractSzPt(pPr);
        if (!f.isEmpty())
            defFamily = f;
        if (!s.isEmpty())
            defSize = s;
    }

    QVector<InlineRun> runs;
    const QRegularExpression rRe(QStringLiteral("<w:r[\\s>]"), QRegularExpression::CaseInsensitiveOption);
    int rpos = 0;
    while (true) {
        const QRegularExpressionMatch rm = rRe.match(p, rpos);
        if (!rm.hasMatch())
            break;
        const int rStart = rm.capturedStart();
        const int rEnd = p.indexOf(QStringLiteral("</w:r>"), rStart, Qt::CaseInsensitive);
        if (rEnd < 0)
            break;
        const QString r = p.mid(rStart, rEnd - rStart);
        rpos = rEnd + 6;
        QString rPr;
        const int rPrS = r.indexOf(QStringLiteral("<w:rPr"), 0, Qt::CaseInsensitive);
        const int rPrE = r.indexOf(QStringLiteral("</w:rPr>"), 0, Qt::CaseInsensitive);
        if (rPrS >= 0 && rPrE > rPrS)
            rPr = r.mid(rPrS, rPrE - rPrS);
        InlineRun run;
        run.bold = wValOn(rPr, QStringLiteral("b"));
        run.italic = wValOn(rPr, QStringLiteral("i"));
        run.fontFamily = extractRFonts(rPr);
        run.fontSizePt = extractSzPt(rPr);
        if (run.fontFamily.isEmpty())
            run.fontFamily = defFamily;
        if (run.fontSizePt.isEmpty())
            run.fontSizePt = defSize;
        QString text;
        const QRegularExpression tRe(QStringLiteral("<w:t(?:\\s[^>]*)?>([^<]*)</w:t>"),
                                     QRegularExpression::CaseInsensitiveOption);
        auto it = tRe.globalMatch(r);
        while (it.hasNext())
            text += decodeEntities(it.next().captured(1));
        if (r.contains(QStringLiteral("<w:tab"), Qt::CaseInsensitive))
            text += QLatin1Char(' ');
        run.text = text;
        if (!run.text.isEmpty()) {
            if (!runs.isEmpty() && sameRunStyle(runs.last(), run))
                runs.last().text += run.text;
            else
                runs.append(run);
        }
    }
    return runs;
}

Block parseParagraphXml(const QString &p, const QMap<QString, StyleInfo> &styles)
{
    Block b;
    b.type = Block::Paragraph;
    QString st;
    const QRegularExpression styleRe(QStringLiteral("<w:pStyle[^>]*w:val=\"([^\"]+)\""),
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch sm = styleRe.match(p);
    if (sm.hasMatch()) {
        st = sm.captured(1);
        if (st.compare(QLatin1String("Heading1"), Qt::CaseInsensitive) == 0 || st == QLatin1String("1"))
            b.type = Block::H1;
        else if (st.compare(QLatin1String("Heading2"), Qt::CaseInsensitive) == 0 || st == QLatin1String("2"))
            b.type = Block::H2;
        else if (st.compare(QLatin1String("Heading3"), Qt::CaseInsensitive) == 0 || st == QLatin1String("3"))
            b.type = Block::H3;
    }
    QString fam;
    QString sz;
    resolveStyle(styles, st.isEmpty() ? QStringLiteral("Normal") : st, fam, sz);
    b.runs = parseRunsFromXml(p, fam, sz);

    const QString plain = runsToPlain(b.runs);
    if (plain.startsWith(QLatin1String("- [ ] "))) {
        b.type = Block::CheckOpen;
        if (!b.runs.isEmpty()) {
            b.runs.first().text.remove(0, 6);
            if (b.runs.first().text.isEmpty())
                b.runs.removeFirst();
        }
    } else if (plain.startsWith(QLatin1String("- [x] "), Qt::CaseInsensitive)
               || plain.startsWith(QLatin1String("- [X] "))) {
        b.type = Block::CheckDone;
        if (!b.runs.isEmpty()) {
            b.runs.first().text.remove(0, 6);
            if (b.runs.first().text.isEmpty())
                b.runs.removeFirst();
        }
    }
    return b;
}

Block parseTableXml(const QString &tbl, const QMap<QString, StyleInfo> &styles)
{
    Block b;
    b.type = Block::Table;
    const QRegularExpression trRe(QStringLiteral("<w:tr[\\s>]"), QRegularExpression::CaseInsensitiveOption);
    int pos = 0;
    bool firstRow = true;
    while (true) {
        const QRegularExpressionMatch m = trRe.match(tbl, pos);
        if (!m.hasMatch())
            break;
        const int trStart = m.capturedStart();
        const int trEnd = tbl.indexOf(QStringLiteral("</w:tr>"), trStart, Qt::CaseInsensitive);
        if (trEnd < 0)
            break;
        const QString tr = tbl.mid(trStart, trEnd - trStart);
        pos = trEnd + 6;
        TableRow row;
        bool headingStyle = false;
        const QRegularExpression tcRe(QStringLiteral("<w:tc[\\s>]"), QRegularExpression::CaseInsensitiveOption);
        int cpos = 0;
        while (true) {
            const QRegularExpressionMatch cm = tcRe.match(tr, cpos);
            if (!cm.hasMatch())
                break;
            const int tcStart = cm.capturedStart();
            const int tcEnd = tr.indexOf(QStringLiteral("</w:tc>"), tcStart, Qt::CaseInsensitive);
            if (tcEnd < 0)
                break;
            const QString tc = tr.mid(tcStart, tcEnd - tcStart);
            cpos = tcEnd + 6;
            TableCell cell;
            const QRegularExpression pRe(QStringLiteral("<w:p[\\s>]"), QRegularExpression::CaseInsensitiveOption);
            int ppos = 0;
            while (true) {
                const QRegularExpressionMatch pm = pRe.match(tc, ppos);
                if (!pm.hasMatch())
                    break;
                const int pStart = pm.capturedStart();
                const int pEnd = tc.indexOf(QStringLiteral("</w:p>"), pStart, Qt::CaseInsensitive);
                if (pEnd < 0)
                    break;
                const QString p = tc.mid(pStart, pEnd - pStart);
                ppos = pEnd + 6;
                QString st;
                const QRegularExpression styleRe(QStringLiteral("<w:pStyle[^>]*w:val=\"([^\"]+)\""),
                                                 QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch sm = styleRe.match(p);
                if (sm.hasMatch()) {
                    st = sm.captured(1);
                    if (st.compare(QLatin1String("TableHeading"), Qt::CaseInsensitive) == 0)
                        headingStyle = true;
                }
                QString fam;
                QString sz;
                resolveStyle(styles, st.isEmpty() ? QStringLiteral("TableContents") : st, fam, sz);
                QVector<InlineRun> runs = parseRunsFromXml(p, fam, sz);
                if (!cell.runs.isEmpty() && !runs.isEmpty()) {
                    InlineRun sp;
                    sp.text = QStringLiteral(" ");
                    sp.fontFamily = cell.runs.last().fontFamily;
                    sp.fontSizePt = cell.runs.last().fontSizePt;
                    cell.runs.append(sp);
                }
                cell.runs += runs;
            }
            row.cells.append(cell);
        }
        if (firstRow || headingStyle)
            row.header = true;
        firstRow = false;
        if (!row.cells.isEmpty())
            b.rows.append(row);
    }
    return b;
}

struct XmlItem {
    int start = 0;
    int end = 0;
    bool table = false;
};

bool insideRange(int pos, const QVector<QPair<int, int>> &ranges)
{
    for (const auto &r : ranges) {
        if (pos >= r.first && pos < r.second)
            return true;
    }
    return false;
}

QVector<QPair<int, int>> collectTableRanges(const QString &xml)
{
    QVector<QPair<int, int>> out;
    const QRegularExpression tblRe(QStringLiteral("<w:tbl[\\s>]"), QRegularExpression::CaseInsensitiveOption);
    int pos = 0;
    while (true) {
        const QRegularExpressionMatch m = tblRe.match(xml, pos);
        if (!m.hasMatch())
            break;
        const int start = m.capturedStart();
        int depth = 1;
        int i = m.capturedEnd();
        while (i < xml.size() && depth > 0) {
            const QRegularExpressionMatch om = tblRe.match(xml, i);
            const int nextOpen = om.hasMatch() ? om.capturedStart() : -1;
            const int nextClose = xml.indexOf(QStringLiteral("</w:tbl>"), i, Qt::CaseInsensitive);
            if (nextClose < 0) {
                i = xml.size();
                break;
            }
            if (nextOpen >= 0 && nextOpen < nextClose) {
                ++depth;
                i = nextOpen + 5;
            } else {
                --depth;
                i = nextClose + 8;
            }
        }
        out.append(qMakePair(start, i));
        pos = i;
    }
    return out;
}

QVector<Block> parseDocumentXml(const QString &xml, const QMap<QString, StyleInfo> &styles)
{
    QVector<Block> blocks;
    const QVector<QPair<int, int>> tables = collectTableRanges(xml);
    QVector<XmlItem> items;
    for (const auto &r : tables) {
        XmlItem it;
        it.start = r.first;
        it.end = r.second;
        it.table = true;
        items.append(it);
    }
    const QRegularExpression pRe(QStringLiteral("<w:p[\\s>]"), QRegularExpression::CaseInsensitiveOption);
    int pos = 0;
    while (true) {
        const QRegularExpressionMatch m = pRe.match(xml, pos);
        if (!m.hasMatch())
            break;
        const int pStart = m.capturedStart();
        const int pEnd = xml.indexOf(QStringLiteral("</w:p>"), pStart, Qt::CaseInsensitive);
        if (pEnd < 0)
            break;
        pos = pEnd + 6;
        if (insideRange(pStart, tables))
            continue;
        XmlItem it;
        it.start = pStart;
        it.end = pEnd + 6;
        it.table = false;
        items.append(it);
    }
    std::sort(items.begin(), items.end(), [](const XmlItem &a, const XmlItem &b) {
        return a.start < b.start;
    });
    for (const XmlItem &it : items) {
        const QString slice = xml.mid(it.start, it.end - it.start);
        if (it.table) {
            Block tb = parseTableXml(slice, styles);
            if (!tb.rows.isEmpty())
                blocks.append(tb);
        } else {
            Block b = parseParagraphXml(slice, styles);
            if (b.runs.isEmpty() && b.type == Block::Paragraph)
                continue;
            blocks.append(b);
        }
    }
    return blocks;
}

bool inspectDocxParts(const QMap<QString, QByteArray> &parts, QString *error)
{
    const QStringList required = {
        QStringLiteral("[Content_Types].xml"),
        QStringLiteral("_rels/.rels"),
        QStringLiteral("word/document.xml"),
        QStringLiteral("word/_rels/document.xml.rels"),
    };
    for (const QString &r : required) {
        if (!parts.contains(r)) {
            if (error)
                *error = QStringLiteral("docx missing part: ") + r;
            return false;
        }
    }
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (it.key().endsWith(QLatin1String(".html"), Qt::CaseInsensitive)) {
            if (error)
                *error = QStringLiteral("docx contains leftover html part: ") + it.key();
            return false;
        }
    }
    const QString doc = QString::fromUtf8(parts.value(QStringLiteral("word/document.xml")));
    if (!doc.contains(QStringLiteral("<w:p"))) {
        if (error)
            *error = QStringLiteral("document.xml has no w:p");
        return false;
    }
    if (doc.contains(QStringLiteral("<html"), Qt::CaseInsensitive)) {
        if (error)
            *error = QStringLiteral("document.xml contains leftover <html");
        return false;
    }
    const QRegularExpression rawP(QStringLiteral("<p[\\s>/]"));
    if (rawP.match(doc).hasMatch()) {
        if (error)
            *error = QStringLiteral("document.xml contains raw HTML <p>");
        return false;
    }
    const QString ct = QString::fromUtf8(parts.value(QStringLiteral("[Content_Types].xml")));
    if (!ct.contains(QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"))
        || !ct.contains(QStringLiteral("application/vnd.openxmlformats-package.relationships+xml"))) {
        if (error)
            *error = QStringLiteral("[Content_Types].xml missing required OOXML content types");
        return false;
    }
    return true;
}

bool writeDocxArchive(const QString &path, const QByteArray &documentXml,
                      const QByteArray &stylesXml, QString *error)
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile::remove(path);
    zipFile zf = zipOpen(path.toLocal8Bit().constData(), APPEND_STATUS_CREATE);
    if (!zf) {
        if (error)
            *error = QStringLiteral("Could not create docx: ") + path;
        return false;
    }
    bool ok = addZipEntry(zf, "[Content_Types].xml", QByteArray(kContentTypes))
              && addZipEntry(zf, "_rels/.rels", QByteArray(kRels))
              && addZipEntry(zf, "word/document.xml", documentXml)
              && addZipEntry(zf, "word/_rels/document.xml.rels", QByteArray(kDocRels))
              && addZipEntry(zf, "word/styles.xml", stylesXml);
    if (zipClose(zf, nullptr) != ZIP_OK)
        ok = false;
    if (!ok) {
        if (error)
            *error = QStringLiteral("Could not write docx parts: ") + path;
        return false;
    }
    QString health;
    if (!DocumentIo::docxLooksHealthy(path, &health)) {
        if (error)
            *error = health;
        return false;
    }
    return true;
}


} // namespace

QString DocumentIo::detectFormatName(const QString &path)
{
    const QString e = extOf(path);
    if (e == QLatin1String("md") || e == QLatin1String("markdown") || e == QLatin1String("txt"))
        return QStringLiteral("markdown");
    if (e == QLatin1String("docx"))
        return QStringLiteral("docx");
    if (e == QLatin1String("html") || e == QLatin1String("htm"))
        return QStringLiteral("html");
    return QStringLiteral("unknown");
}

QString DocumentIo::markdownToHtml(const QString &markdown)
{
    const QStringList rawLines = markdown.split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r")));
    QStringList paras;
    QString cur;
    QStringList checks;
    auto flushPara = [&]() {
        if (!cur.trimmed().isEmpty()) {
            paras << (QStringLiteral("<p>") + inlineMarkdownToHtml(cur.trimmed()) + QStringLiteral("</p>"));
            cur.clear();
        }
    };
    auto flushChecks = [&]() {
        if (checks.isEmpty())
            return;
        QString ul = QStringLiteral("<ul class=\"checklist\">");
        for (const QString &c : checks)
            ul += c;
        ul += QStringLiteral("</ul>");
        paras << ul;
        checks.clear();
    };
    const QRegularExpression checkRe(QStringLiteral("^\\s*-\\s+\\[([ xX])\\]\\s+(.*)$"));
    const QRegularExpression hRe(QStringLiteral("^(#{1,3})\\s+(.*)$"));
    for (const QString &line : rawLines) {
        const QRegularExpressionMatch cm = checkRe.match(line);
        if (cm.hasMatch()) {
            flushPara();
            const bool done = cm.captured(1).toLower() == QLatin1String("x");
            QString inner = inlineMarkdownToHtml(cm.captured(2).trimmed());
            if (inner.isEmpty())
                inner = QStringLiteral("&#8203;");
            QString li = QStringLiteral("<li class=\"task");
            if (done)
                li += QStringLiteral(" done");
            li += QStringLiteral("\"><input type=\"checkbox\"");
            if (done)
                li += QStringLiteral(" checked");
            li += QStringLiteral(" contenteditable=\"false\"><span>");
            li += inner;
            li += QStringLiteral("</span></li>");
            checks << li;
            continue;
        }
        const QRegularExpressionMatch hm = hRe.match(line);
        if (hm.hasMatch()) {
            flushChecks();
            flushPara();
            const int level = hm.captured(1).size();
            const QString inner = inlineMarkdownToHtml(hm.captured(2).trimmed());
            paras << (QStringLiteral("<h%1>").arg(level) + inner + QStringLiteral("</h%1>").arg(level));
            continue;
        }
        if (line.trimmed().isEmpty()) {
            flushChecks();
            flushPara();
            continue;
        }
        flushChecks();
        if (!cur.isEmpty())
            cur += QLatin1Char(' ');
        cur += line.trimmed();
    }
    flushChecks();
    flushPara();
    if (paras.isEmpty())
        return QStringLiteral("<p></p>");
    return paras.join(QString());
}

QString DocumentIo::htmlToMarkdown(const QString &html)
{
    return blocksToMarkdown(parseHtmlBlocks(html));
}

bool DocumentIo::docxLooksHealthy(const QString &path, QString *error)
{
    QString err;
    const QMap<QString, QByteArray> parts = unzipAll(path, &err);
    if (parts.isEmpty()) {
        if (error)
            *error = err.isEmpty() ? QStringLiteral("empty docx zip") : err;
        return false;
    }
    return inspectDocxParts(parts, error);
}

bool DocumentIo::docxPeek(const QString &path,
                         bool *hasContentTypes,
                         bool *hasDocumentXml,
                         bool *leftoverHtml,
                         QString *error)
{
    if (hasContentTypes)
        *hasContentTypes = false;
    if (hasDocumentXml)
        *hasDocumentXml = false;
    if (leftoverHtml)
        *leftoverHtml = false;

    QString err;
    const QMap<QString, QByteArray> parts = unzipAll(path, &err);
    if (parts.isEmpty()) {
        if (error)
            *error = err.isEmpty() ? QStringLiteral("empty docx zip") : err;
        return false;
    }

    const bool ct = parts.contains(QStringLiteral("[Content_Types].xml"));
    const bool doc = parts.contains(QStringLiteral("word/document.xml"));
    bool leftover = false;
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (it.key().endsWith(QLatin1String(".html"), Qt::CaseInsensitive))
            leftover = true;
    }
    const QString xml = QString::fromUtf8(parts.value(QStringLiteral("word/document.xml")));
    if (xml.contains(QStringLiteral("<html"), Qt::CaseInsensitive))
        leftover = true;

    if (hasContentTypes)
        *hasContentTypes = ct;
    if (hasDocumentXml)
        *hasDocumentXml = doc;
    if (leftoverHtml)
        *leftoverHtml = leftover;
    return inspectDocxParts(parts, error);
}

QString DocumentIo::docxToHtml(const QString &path, QString *error)
{
    QString err;
    const QMap<QString, QByteArray> parts = unzipAll(path, &err);
    if (parts.isEmpty()) {
        if (error)
            *error = err.isEmpty() ? QStringLiteral("empty docx") : err;
        return QString();
    }
    if (!parts.contains(QStringLiteral("word/document.xml"))) {
        if (error)
            *error = QStringLiteral("docx missing word/document.xml");
        return QString();
    }
    const QString xml = QString::fromUtf8(parts.value(QStringLiteral("word/document.xml")));
    const QString stylesXml = QString::fromUtf8(parts.value(QStringLiteral("word/styles.xml")));
    return blocksToHtml(parseDocumentXml(xml, parseStyleMap(stylesXml)));
}

bool DocumentIo::htmlToDocx(const QString &path, const QString &html, QString *error)
{
    const QVector<Block> blocks = parseHtmlBlocks(html);
    return writeDocxArchive(path, blocksToDocumentXml(blocks).toUtf8(), QByteArray(kStyles), error);
}


bool DocumentIo::htmlToKindleDocx(const QString &path, const QString &html, QString *error)
{
    const QVector<Block> blocks = parseKindleHtmlBlocks(html);
    return writeDocxArchive(path, kindleDocumentXml(blocks).toUtf8(), QByteArray(kKindleStyles), error);
}


QString DocumentIo::htmlFromFile(const QString &path, QString *error)
{
    if (error)
        error->clear();
    const QString fmt = detectFormatName(path);
    if (fmt == QLatin1String("markdown")) {
        const QByteArray bytes = readUtf8File(path, error);
        if (error && !error->isEmpty())
            return QString();
        return markdownToHtml(QString::fromUtf8(bytes));
    }
    if (fmt == QLatin1String("docx"))
        return docxToHtml(path, error);
    const QByteArray bytes = readUtf8File(path, error);
    if (error && !error->isEmpty())
        return QString();
    return QString::fromUtf8(bytes);
}

bool DocumentIo::writeFromHtml(const QString &path, const QString &html, QString *error)
{
    if (error)
        error->clear();
    const QString fmt = detectFormatName(path);
    if (fmt == QLatin1String("markdown"))
        return writeUtf8File(path, htmlToMarkdown(html).toUtf8(), error);
    if (fmt == QLatin1String("docx"))
        return htmlToDocx(path, html, error);
    return writeUtf8File(path, html.toUtf8(), error);
}
