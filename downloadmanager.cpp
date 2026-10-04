// ============================================================================
//  downloadmanager.cpp — Full download manager (Qt 6.2+ compatible, moc-free).
//  Uses tick-based state detection to avoid version-specific signal enums.
// ============================================================================

#include "downloadmanager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QTimer>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>
#include <QRandomGenerator>
#include <QWebEngineProfile>
#include <QWebEngineDownloadRequest>

// ────────────────────────────────────────────────────────────────────────────
DownloadManager& DownloadManager::instance() {
    static DownloadManager s;
    return s;
}

DownloadManager::DownloadManager() {
    defaultFolder_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (defaultFolder_.isEmpty())
        defaultFolder_ = QDir::homePath() + "/Downloads";

    load();

    progressTimer_ = new QTimer();
    progressTimer_->setInterval(500);
    QObject::connect(progressTimer_, &QTimer::timeout, [this] { tickProgress(); });
    progressTimer_->start();
}

// ────────────────────────────────────────────────────────────────────────────
QString DownloadManager::settingsPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/downloads.json";
}

void DownloadManager::load() {
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.readAll();
    f.close();

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();

    defaultFolder_ = root.value("defaultFolder").toString(defaultFolder_);
    if (defaultFolder_.isEmpty())
        defaultFolder_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    items_.clear();
    for (const QJsonValue& v : root.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        DownloadItem it;
        it.id            = o.value("id").toString();
        it.fileName      = o.value("fileName").toString();
        it.fullPath      = o.value("fullPath").toString();
        it.sourceUrl     = o.value("sourceUrl").toString();
        it.totalBytes    = qint64(o.value("totalBytes").toDouble(0));
        it.receivedBytes = qint64(o.value("receivedBytes").toDouble(0));
        it.startedAt     = qint64(o.value("startedAt").toDouble(0));
        it.finishedAt    = qint64(o.value("finishedAt").toDouble(0));
        it.state         = o.value("state").toInt(0);
        it.mimeType      = o.value("mimeType").toString();
        if (!it.id.isEmpty()) items_.push_back(it);
    }
}

void DownloadManager::save() const {
    QJsonArray arr;
    for (const DownloadItem& it : items_) {
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
    root["defaultFolder"] = defaultFolder_;
    root["items"] = arr;

    QFile f(settingsPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
}

// ────────────────────────────────────────────────────────────────────────────
void DownloadManager::setDefaultFolder(const QString& path) {
    if (path.isEmpty()) return;
    defaultFolder_ = path;
    QDir().mkpath(defaultFolder_);
    save();
}

// ────────────────────────────────────────────────────────────────────────────
void DownloadManager::attach(QWebEngineProfile* profile) {
    if (!profile) return;
    QObject::connect(profile, &QWebEngineProfile::downloadRequested,
            profile, [this](QWebEngineDownloadRequest* req) {
        handleNewRequest(req);
    });
}

// ────────────────────────────────────────────────────────────────────────────
static QString makeId() {
    return QStringLiteral("%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
}

void DownloadManager::handleNewRequest(QWebEngineDownloadRequest* req) {
    if (!req) return;

    QDir().mkpath(defaultFolder_);
    const QString path = defaultFolder_ + "/" + req->downloadFileName();
    req->setDownloadDirectory(defaultFolder_);
    req->setDownloadFileName(req->downloadFileName());
    req->accept();

    DownloadItem it;
    it.id            = makeId();
    it.fileName      = req->downloadFileName();
    it.fullPath      = path;
    it.sourceUrl     = req->url().toString();
    it.totalBytes    = req->totalBytes();
    it.receivedBytes = req->receivedBytes();
    it.startedAt     = QDateTime::currentSecsSinceEpoch();
    it.state         = int(req->state());   // 0 usually (InProgress)
    it.mimeType      = req->mimeType();

    items_.push_back(it);
    live_.insert(it.id, QPointer<QWebEngineDownloadRequest>(req));

    // Live byte updates via the receivedBytesChanged signal (safe in all
    // Qt 6.x versions — no enum parameter involved).
    QObject::connect(req, &QWebEngineDownloadRequest::receivedBytesChanged,
            req, [this, id = it.id]() {
        const int i = indexOf(id);
        if (i < 0) return;
        if (auto lp = live_.value(id)) {
            items_[i].receivedBytes = lp->receivedBytes();
            items_[i].totalBytes    = lp->totalBytes();
        }
    });

    save();
}

// ────────────────────────────────────────────────────────────────────────────
//  Called every 500ms. Handles:
//    · Byte count refresh for active downloads
//    · Completion detection via isFinished()
//    · Saving the final state to disk
// ────────────────────────────────────────────────────────────────────────────
void DownloadManager::tickProgress() {
    bool anyFinished = false;

    for (auto it = live_.begin(); it != live_.end(); ++it) {
        QPointer<QWebEngineDownloadRequest>& lp = it.value();
        if (!lp) continue;

        const int i = indexOf(it.key());
        if (i < 0) continue;

        // Refresh live byte counters
        items_[i].receivedBytes = lp->receivedBytes();
        items_[i].totalBytes    = lp->totalBytes();

        // Detect completion / cancellation (only once)
        if (lp->isFinished() && items_[i].finishedAt == 0) {
            items_[i].finishedAt = QDateTime::currentSecsSinceEpoch();
            items_[i].fullPath   = lp->downloadDirectory() + "/" + lp->downloadFileName();
            items_[i].fileName   = lp->downloadFileName();
            items_[i].receivedBytes = lp->receivedBytes();
            items_[i].totalBytes    = lp->totalBytes();

            // Map QWebEngineDownloadRequest::State to our int.
            // Enum values (Qt 6.x):
            //   0 = DownloadRequested
            //   1 = DownloadInProgress
            //   2 = DownloadCompleted
            //   3 = DownloadCancelled
            //   4 = DownloadInterrupted
            const int st = int(lp->state());
            if (st == 2)      items_[i].state = 1;   // completed
            else if (st == 3) items_[i].state = 2;   // cancelled
            else if (st == 4) items_[i].state = 3;   // interrupted
            else              items_[i].state = 1;   // fallback: treat as completed

            anyFinished = true;
        }
    }

    if (anyFinished) save();
}

// ────────────────────────────────────────────────────────────────────────────
int DownloadManager::indexOf(const QString& id) const {
    for (int i = 0; i < items_.size(); ++i)
        if (items_[i].id == id) return i;
    return -1;
}

int DownloadManager::activeCount() const {
    int n = 0;
    for (const auto& it : items_)
        if (it.state == 0) ++n;
    return n;
}

// ────────────────────────────────────────────────────────────────────────────
void DownloadManager::pause(const QString& id) {
    if (auto lp = live_.value(id)) {
        lp->pause();
        const int i = indexOf(id);
        if (i >= 0) items_[i].state = 4;
        save();
    }
}

void DownloadManager::resume(const QString& id) {
    if (auto lp = live_.value(id)) {
        lp->resume();
        const int i = indexOf(id);
        if (i >= 0) items_[i].state = 0;
        save();
    }
}

void DownloadManager::cancel(const QString& id) {
    if (auto lp = live_.value(id)) {
        lp->cancel();
        const int i = indexOf(id);
        if (i >= 0) {
            items_[i].state = 2;
            items_[i].finishedAt = QDateTime::currentSecsSinceEpoch();
        }
        save();
    }
}

void DownloadManager::remove(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return;
    if (items_[i].state == 0) return;   // don't remove active
    items_.removeAt(i);
    live_.remove(id);
    save();
}

void DownloadManager::clearFinished() {
    for (int i = items_.size() - 1; i >= 0; --i) {
        if (items_[i].state != 0 && items_[i].state != 4) {
            live_.remove(items_[i].id);
            items_.removeAt(i);
        }
    }
    save();
}

void DownloadManager::openFile(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return;
    if (items_[i].state != 1) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(items_[i].fullPath));
}

void DownloadManager::openFolder(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return;
    const QString folder = QFileInfo(items_[i].fullPath).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void DownloadManager::retry(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return;
    if (!items_[i].sourceUrl.isEmpty())
        QDesktopServices::openUrl(QUrl(items_[i].sourceUrl));
}
