// ============================================================================
//  downloadmanager.h — Central download manager (moc-free, Qt 6.2+).
// ============================================================================
#pragma once

#include <QString>
#include <QVector>
#include <QDateTime>
#include <QHash>
#include <QPointer>

class QTimer;
class QWebEngineProfile;
class QWebEngineDownloadRequest;

struct DownloadItem {
    QString id;
    QString fileName;
    QString fullPath;
    QString sourceUrl;
    qint64 totalBytes = 0;
    qint64 receivedBytes = 0;
    qint64 startedAt = 0;
    qint64 finishedAt = 0;
    int    state = 0;   // 0=InProgress, 1=Completed, 2=Cancelled, 3=Interrupted
    QString mimeType;
};

class DownloadManager {
public:
    static DownloadManager& instance();

    QString defaultFolder() const { return defaultFolder_; }
    void setDefaultFolder(const QString& path);

    void attach(QWebEngineProfile* profile);

    QVector<DownloadItem> items() const { return items_; }
    int count() const { return items_.size(); }
    int activeCount() const;

    void pause(const QString& id);
    void resume(const QString& id);
    void cancel(const QString& id);
    void remove(const QString& id);
    void clearFinished();
    void openFile(const QString& id);
    void openFolder(const QString& id);
    void retry(const QString& id);

private:
    DownloadManager();
    void load();
    void save() const;
    static QString settingsPath();

    void handleNewRequest(QWebEngineDownloadRequest* req);
    void tickProgress();
    int indexOf(const QString& id) const;

    QString defaultFolder_;
    QVector<DownloadItem> items_;

    QHash<QString, QPointer<QWebEngineDownloadRequest>> live_;
    QTimer* progressTimer_ = nullptr;
};
