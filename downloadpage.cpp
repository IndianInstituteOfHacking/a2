// ============================================================================
//  downloadpage.cpp — Full-featured downloads manager page.
// ============================================================================

#include "downloadpage.h"
#include "downloadmanager.h"

#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

namespace DownloadPage {

static QString esc(const QString& s) {
    QString out;
    out.reserve(s.size() + 16);
    for (QChar c : s) {
        switch (c.unicode()) {
        case '&': out += QStringLiteral("&amp;"); break;
        case '<': out += QStringLiteral("&lt;");  break;
        case '>': out += QStringLiteral("&gt;");  break;
        case '"': out += QStringLiteral("&quot;");break;
        case '\'':out += QStringLiteral("&#39;"); break;
        default:  out += c; break;
        }
    }
    return out;
}

QString build() {
    QJsonArray arr;
    const auto& items = DownloadManager::instance().items();
    for (const DownloadItem& it : items) {
        QJsonObject o;
        o["id"]            = it.id;
        o["fileName"]      = it.fileName;
        o["fullPath"]      = it.fullPath;
        o["sourceUrl"]     = it.sourceUrl;
        o["totalBytes"]    = double(it.totalBytes);
        o["receivedBytes"] = double(it.receivedBytes);
        o["startedAt"]     = double(it.startedAt);
        o["finishedAt"]    = double(it.finishedAt);
        o["state"]         = it.state;
        o["mimeType"]      = it.mimeType;
        arr.append(o);
    }
    QJsonObject root;
    root["items"] = arr;
    root["defaultFolder"] = DownloadManager::instance().defaultFolder();

    const QString dataJson =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));

    const QString css = QStringLiteral(R"CSS(
        * { box-sizing: border-box; }
        html, body {
            margin: 0; padding: 0; min-height: 100%;
            background: #0c0c0e; color: #e6e8ec;
            font-family: 'Segoe UI', 'Inter', Ubuntu, 'Noto Sans',
                         Cantarell, 'DejaVu Sans', sans-serif;
            font-size: 13px;
        }
        .page {
            max-width: 1200px; margin: 0 auto;
            padding: 40px 28px 60px;
        }
        .header {
            display: flex; align-items: center; gap: 14px;
            margin-bottom: 22px;
        }
        .icon {
            width: 44px; height: 44px; border-radius: 12px;
            background: linear-gradient(135deg, #6b7690 0%, #3d6dd1 100%);
            display: flex; align-items: center; justify-content: center;
            box-shadow: 0 4px 16px rgba(93,157,241,0.35);
            flex-shrink: 0;
        }
        .icon svg { width: 24px; height: 24px; }
        .h1 {
            margin: 0; font-size: 26px; font-weight: 500;
            letter-spacing: -0.3px; color: #f4f5f7;
        }
        .count { color: #6b7280; font-size: 13px; margin-left: 6px; }
        .actions { margin-left: auto; display: flex; gap: 10px; }
        .btn {
            background: #2b2d34; color: #f1f2f5;
            border: 1px solid #3a3d45; border-radius: 10px;
            padding: 8px 14px; font-size: 12px; cursor: pointer;
            transition: background 0.12s, border-color 0.12s;
        }
        .btn:hover { background: #35383f; }
        .btn.danger:hover { background: #4a2020; border-color: #7a2d2d; color: #ffb3b3; }
        .btn.primary {
            background: #5d9df1; color: #0c0c0e; border-color: #5d9df1;
            font-weight: 600;
        }
        .btn.primary:hover { background: #7ab0ff; }

        .folderRow {
            display: flex; align-items: center; gap: 10px;
            background: #141519; border: 1px solid #23262c;
            border-radius: 12px; padding: 10px 14px; margin-bottom: 20px;
        }
        .folderRow .label { color: #9aa0ac; font-size: 12px; flex-shrink: 0; }
        .folderRow .path {
            flex: 1; color: #d7d9de; font-size: 12px;
            font-family: ui-monospace, Menlo, Consolas, monospace;
            background: #1c1d21; padding: 6px 10px; border-radius: 6px;
            white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
        }

        .list { display: flex; flex-direction: column; gap: 10px; }

        .item {
            background: #141519; border: 1px solid #23262c;
            border-radius: 12px; padding: 14px 16px;
            display: flex; flex-direction: column; gap: 10px;
            transition: border-color 0.12s;
        }
        .item:hover { border-color: #2e3138; }

        .itemTop { display: flex; align-items: center; gap: 12px; }
        .itemIcon {
            width: 36px; height: 36px; border-radius: 8px;
            background: #23262c; flex-shrink: 0;
            display: flex; align-items: center; justify-content: center;
            color: #9aa0ac;
        }
        .itemMeta { flex: 1; min-width: 0; }
        .itemName {
            color: #f1f2f5; font-size: 13px; font-weight: 500;
            white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
        }
        .itemUrl {
            color: #6b7280; font-size: 11px; margin-top: 3px;
            white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
        }
        .itemActions { display: flex; gap: 6px; flex-shrink: 0; }
        .itemActions button {
            background: transparent; border: 1px solid #3a3d45;
            color: #c8ccd6; border-radius: 8px;
            padding: 6px 10px; font-size: 11px; cursor: pointer;
            transition: all 0.12s;
        }
        .itemActions button:hover { background: #23262c; color: #f1f2f5; }
        .itemActions button.danger:hover {
            background: #4a2020; border-color: #7a2d2d; color: #ffb3b3;
        }

        .progressWrap {
            height: 6px; background: #23262c; border-radius: 3px; overflow: hidden;
        }
        .progressBar {
            height: 100%; background: linear-gradient(90deg, #5d9df1, #7ab0ff);
            border-radius: 3px;
            transition: width 0.3s ease;
        }
        .progressBar.paused { background: #6b7280; }
        .progressBar.done   { background: linear-gradient(90deg, #4aaf7a, #6bd29a); }
        .progressBar.error  { background: #e0443b; }

        .statsRow {
            display: flex; gap: 18px; color: #9aa0ac; font-size: 11px;
            align-items: center;
        }
        .statsRow .stat b { color: #d7d9de; font-weight: 500; }
        .statsRow .spacer { flex: 1; }
        .speed { color: #5d9df1; font-weight: 600; }

        .empty { text-align: center; padding: 80px 20px; color: #9aa0ac; }
        .empty svg { width: 64px; height: 64px; color: #2c2e34; margin-bottom: 20px; }
        .empty h2 { color: #f1f2f5; font-weight: 400; font-size: 20px; margin: 0 0 8px 0; }
        .empty p { margin: 0; font-size: 13px; }
    )CSS");

    const QString html = QStringLiteral(R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Downloads — RootBrowser</title>
<style>%1</style>
</head><body>
<div class="page">

    <div class="header">
        <div class="icon">
            <svg viewBox="0 0 24 24" fill="#ffffff">
                <path d="M5 20h14v-2H5v2zM19 9h-4V3H9v6H5l7 7 7-7z"/>
            </svg>
        </div>
        <h1 class="h1">Downloads<span class="count" id="count"></span></h1>
        <div class="actions">
            <button class="btn" id="changeFolder">Change folder…</button>
            <button class="btn danger" id="clearAll">Clear finished</button>
        </div>
    </div>

    <div class="folderRow">
        <div class="label">Save to</div>
        <div class="path" id="folderPath"></div>
    </div>

    <div class="list" id="list"></div>
</div>

<script>
const DATA = %2;
const items = DATA.items || [];
const defaultFolder = DATA.defaultFolder || '';

const listEl   = document.getElementById('list');
const countEl  = document.getElementById('count');
const folderEl = document.getElementById('folderPath');

folderEl.textContent = defaultFolder;

function esc(s) {
    return String(s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function humanBytes(n) {
    if (!n || n < 0) return '—';
    const u = ['B','KB','MB','GB','TB'];
    let v = n, i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    return v.toFixed(i === 0 ? 0 : 1) + ' ' + u[i];
}

function humanTime(sec) {
    if (!sec || sec < 0) return '—';
    sec = Math.floor(sec);
    if (sec < 60)  return sec + 's';
    if (sec < 3600) return Math.floor(sec/60) + 'm ' + (sec%60) + 's';
    return Math.floor(sec/3600) + 'h ' + Math.floor((sec%3600)/60) + 'm';
}

function stateLabel(s) {
    switch (s) {
        case 0: return 'In progress';
        case 1: return 'Completed';
        case 2: return 'Cancelled';
        case 3: return 'Interrupted';
        case 4: return 'Paused';
        default: return 'Unknown';
    }
}

function stateClass(s) {
    switch (s) {
        case 0: return 'active';
        case 1: return 'done';
        case 2: return 'error';
        case 3: return 'error';
        case 4: return 'paused';
        default: return '';
    }
}

function timeAgo(ts) {
    if (!ts) return '';
    const now = Math.floor(Date.now()/1000);
    const d = now - ts;
    if (d < 60) return 'just now';
    if (d < 3600) return Math.floor(d/60) + ' min ago';
    if (d < 86400) return Math.floor(d/3600) + ' hr ago';
    return new Date(ts*1000).toLocaleDateString();
}

function render() {
    countEl.textContent = ' · ' + items.length;

    if (items.length === 0) {
        listEl.innerHTML = `
            <div class="empty">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
                    <path d="M5 20h14v-2H5v2zM19 9h-4V3H9v6H5l7 7 7-7z"/>
                </svg>
                <h2>No downloads yet</h2>
                <p>Files you download will appear here.</p>
            </div>`;
        return;
    }

    const sorted = items.slice().sort((a, b) => {
        const aActive = a.state === 0 ? 0 : 1;
        const bActive = b.state === 0 ? 0 : 1;
        if (aActive !== bActive) return aActive - bActive;
        return (b.startedAt || 0) - (a.startedAt || 0);
    });

    let html = '';
    for (const it of sorted) {
        const total = it.totalBytes || 0;
        const recv  = it.receivedBytes || 0;
        const pct   = total > 0 ? Math.min(100, (recv / total) * 100) : 0;
        const cls   = stateClass(it.state);

        let speedTxt = '';
        if (it.state === 0 && it.startedAt) {
            const elapsed = Math.max(1, Math.floor(Date.now()/1000) - it.startedAt);
            const speed = recv / elapsed;
            speedTxt = humanBytes(speed) + '/s';
        }

        let etaTxt = '';
        if (it.state === 0 && total > 0 && recv > 0 && it.startedAt) {
            const elapsed = Math.max(1, Math.floor(Date.now()/1000) - it.startedAt);
            const speed = recv / elapsed;
            const remain = (total - recv) / speed;
            etaTxt = humanTime(remain);
        }

        let actions = '';
        if (it.state === 0) {
            actions += `<button data-act="pause" data-id="${esc(it.id)}">Pause</button>`;
            actions += `<button data-act="cancel" data-id="${esc(it.id)}" class="danger">Cancel</button>`;
        } else if (it.state === 4) {
            actions += `<button data-act="resume" data-id="${esc(it.id)}">Resume</button>`;
            actions += `<button data-act="cancel" data-id="${esc(it.id)}" class="danger">Cancel</button>`;
        } else if (it.state === 1) {
            actions += `<button data-act="open" data-id="${esc(it.id)}">Open</button>`;
            actions += `<button data-act="folder" data-id="${esc(it.id)}">Folder</button>`;
            actions += `<button data-act="remove" data-id="${esc(it.id)}" class="danger">Remove</button>`;
        } else {
            actions += `<button data-act="retry" data-id="${esc(it.id)}">Retry</button>`;
            actions += `<button data-act="remove" data-id="${esc(it.id)}" class="danger">Remove</button>`;
        }

        html += `
        <div class="item">
            <div class="itemTop">
                <div class="itemIcon">
                    <svg width="18" height="18" viewBox="0 0 24 24" fill="none"
                         stroke="currentColor" stroke-width="1.8">
                        <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>
                        <polyline points="14 2 14 8 20 8"/>
                    </svg>
                </div>
                <div class="itemMeta">
                    <div class="itemName">${esc(it.fileName || 'unknown')}</div>
                    <div class="itemUrl">${esc(it.sourceUrl || '')}</div>
                </div>
                <div class="itemActions">${actions}</div>
            </div>

            <div class="progressWrap">
                <div class="progressBar ${cls}" style="width: ${pct.toFixed(1)}%"></div>
            </div>

            <div class="statsRow">
                <div class="stat"><b>${humanBytes(recv)}</b> / <b>${humanBytes(total)}</b> (${pct.toFixed(1)}%)</div>
                ${speedTxt ? `<div class="stat speed">↓ ${speedTxt}</div>` : ''}
                ${etaTxt ? `<div class="stat">ETA <b>${etaTxt}</b></div>` : ''}
                <div class="spacer"></div>
                <div class="stat">${stateLabel(it.state)}</div>
                <div class="stat">${timeAgo(it.finishedAt || it.startedAt)}</div>
            </div>
        </div>`;
    }
    listEl.innerHTML = html;

    document.querySelectorAll('.itemActions button').forEach(btn => {
        btn.addEventListener('click', () => {
            const act = btn.getAttribute('data-act');
            const id  = btn.getAttribute('data-id');
            window.location.href = 'rootbrowser-dl-' + act + ':' + id;
        });
    });
}

document.getElementById('changeFolder').addEventListener('click', () => {
    window.location.href = 'rootbrowser-dl-folder:pick';
});

document.getElementById('clearAll').addEventListener('click', () => {
    window.location.href = 'rootbrowser-dl-clear:all';
});

render();
</script>
</body></html>)HTML")
        .arg(css, dataJson);

    return html;
}

} // namespace DownloadPage
