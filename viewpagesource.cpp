// ============================================================================
//  viewpagesource.cpp — "View page source" with HTML syntax highlighting.
//
//  - Fetches raw HTML from a QWebEnginePage via toHtml()
//  - Escapes it and wraps it in a self-contained dark-themed viewer
//  - Applies lightweight regex-based syntax highlighting:
//        · tags         → soft blue
//        · attributes   → teal
//        · strings      → amber
//        · comments     → dim gray italic
//        · doctype      → pink
//  - No external deps; uses only Qt + std.
//
//  main.cpp integration (ONE line at each spot):
//      #include "viewpagesource.h"
//
//      // inside Browser::viewPageSource(QWebEnginePage* p)
//      ViewPageSource::show(p, [this](const QString& html, const QString& title){
//          openHtmlInNewTab(html, title);
//      });
// ============================================================================

#include "viewpagesource.h"

#include <QWebEnginePage>
#include <QString>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <functional>

namespace ViewPageSource {

// ---------------------------------------------------------------------------
//  Escape the raw HTML for embedding inside <pre>.
// ---------------------------------------------------------------------------
static QString escape(const QString& s) {
    QString out;
    out.reserve(s.size() + s.size() / 4);
    for (QChar ch : s) {
        switch (ch.unicode()) {
        case '&': out += QStringLiteral("&amp;");  break;
        case '<': out += QStringLiteral("&lt;");   break;
        case '>': out += QStringLiteral("&gt;");   break;
        case '"': out += QStringLiteral("&quot;"); break;
        case '\'':out += QStringLiteral("&#39;");  break;
        default:  out += ch;                        break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Highlight: takes *already-escaped* HTML text and wraps tokens in spans.
//  The classes are defined in the stylesheet below.
// ---------------------------------------------------------------------------
static QString highlight(const QString& escaped) {
    QString s = escaped;

    // ---- 1) Comments: <!-- ... --> ------------------------------------
    // Use a single pass with a placeholder to avoid re-matching inside
    // already-wrapped tags.
    {
        static const QRegularExpression re(
            QStringLiteral("&lt;!--[\\s\\S]*?--&gt;"));
        s.replace(re, QStringLiteral("<span class=\"cm\">\\0</span>"));
    }

    // ---- 2) Doctype: <!DOCTYPE ...> ----------------------------------
    {
        static const QRegularExpression re(
            QStringLiteral("&lt;!DOCTYPE[^&]*&gt;"),
            QRegularExpression::CaseInsensitiveOption);
        s.replace(re, QStringLiteral("<span class=\"dt\">\\0</span>"));
    }

    // ---- 3) Opening / closing tag names: <div  </div  -----------------
    //   &lt;  optional-/  name
    {
        static const QRegularExpression re(
            QStringLiteral("(&lt;/?)([a-zA-Z][a-zA-Z0-9:_-]*)"));
        s.replace(re, QStringLiteral("\\1<span class=\"tg\">\\2</span>"));
    }

    // ---- 4) Attribute names ------------------------------------------
    //   a space, name, then "="
    {
        static const QRegularExpression re(
            QStringLiteral("(\\s)([a-zA-Z_:][a-zA-Z0-9_:.-]*)(=)"));
        s.replace(re, QStringLiteral("\\1<span class=\"at\">\\2</span>\\3"));
    }

    // ---- 5) Attribute values: "..." or '...' -------------------------
    {
        static const QRegularExpression re(
            QStringLiteral("(&quot;.*?&quot;|&#39;.*?&#39;)"));
        s.replace(re, QStringLiteral("<span class=\"st\">\\0</span>"));
    }

    // ---- 6) Tag end: &gt; --------------------------------------------
    //   We colour it to match the tag color for a clean look.
    {
        static const QRegularExpression re(QStringLiteral("&gt;"));
        s.replace(re, QStringLiteral("<span class=\"tg\">&gt;</span>"));
    }

    return s;
}

// ---------------------------------------------------------------------------
//  Build the final HTML document for the viewer.
// ---------------------------------------------------------------------------
static QString buildViewer(const QString& rawHtml, const QString& pageTitle) {
    const QString escaped  = escape(rawHtml);
    const QString colored  = highlight(escaped);
    const QString title    = pageTitle.isEmpty() ? QStringLiteral("Page source")
                                                  : pageTitle + " — Source";
    const QString safeTitle = escape(title);

    // Line count for the info bar.
    int lineCount = rawHtml.count('\n') + 1;
    const QString sizeText = QStringLiteral("%1 lines · %2 KB")
        .arg(lineCount)
        .arg(rawHtml.size() / 1024);

    return QStringLiteral(R"HTML(<!doctype html>
<html><head><meta charset='utf-8'>
<title>%1</title>
<style>
    html, body { margin:0; padding:0; background:#0e0e11; color:#d7d9de; }
    body {
        font: 13px/1.55 ui-monospace, 'JetBrains Mono', 'Fira Code',
              Menlo, Consolas, 'DejaVu Sans Mono', monospace;
    }
    .bar {
        position: sticky; top:0; z-index:10;
        display:flex; align-items:center; gap:14px;
        padding: 10px 16px;
        background:#141519;
        border-bottom: 1px solid #23262c;
        font-size: 12px; color:#9aa0ac;
    }
    .bar .title { color:#e6e8ec; font-weight:600; }
    .bar .dot   { width:8px; height:8px; border-radius:4px;
                  background:#5d9df1; box-shadow:0 0 8px #5d9df1aa; }
    .bar .meta  { margin-left:auto; color:#6b7280; }

    pre {
        margin:0; padding: 16px 20px 40px;
        white-space: pre-wrap; word-break: break-word;
        tab-size: 4; counter-reset: line;
    }

    /* token colors */
    .tg { color:#7aa2f7; }          /* tags */
    .at { color:#7dd3c0; }          /* attribute names */
    .st { color:#e0af68; }          /* attribute values / strings */
    .cm { color:#5f6673; font-style:italic; }  /* comments */
    .dt { color:#bb9af7; }          /* doctype */
</style>
</head>
<body>
    <div class="bar">
        <span class="dot"></span>
        <span class="title">%2</span>
        <span class="meta">%3</span>
    </div>
<pre>%4</pre>
</body></html>)HTML")
        .arg(safeTitle)
        .arg(safeTitle)
        .arg(sizeText)
        .arg(colored);
}

// ---------------------------------------------------------------------------
//  Public entry point.
// ---------------------------------------------------------------------------
void show(QWebEnginePage* page, OpenInNewTabFn openInNewTab) {
    if (!page || !openInNewTab) return;

    // Capture title before async fetch.
    const QString title = page->title();

    page->toHtml([openInNewTab, title](const QString& rawHtml) {
        const QString viewer = buildViewer(rawHtml, title);
        openInNewTab(viewer, QStringLiteral("Page source"));
    });
}

} // namespace ViewPageSource
