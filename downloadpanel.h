// ============================================================================
//  downloadpanel.h — Small popup panel that shows live download progress,
//  anchored under the toolbar. Includes a floating "downloading" button
//  that pulses while any download is active.
// ============================================================================
#pragma once

#include <QWidget>
#include <QAbstractButton>
#include <functional>

class QTimer;
class QVariantAnimation;

// ----------------------------------------------------------------------------
//  DownloadButton — small icon on the toolbar showing download activity.
//  Pulses while a download is active; shows a static icon when idle.
// ----------------------------------------------------------------------------
class DownloadButton : public QAbstractButton {
public:
    explicit DownloadButton(QWidget* parent = nullptr);

    void setActive(bool active);      // start/stop the pulse animation
    bool isActive() const { return active_; }

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent*) override;

private:
    bool hover_ = false;
    bool active_ = false;
    qreal pulse_ = 0.0;               // 0..1
    QVariantAnimation* pulseAnim_ = nullptr;
};

// ----------------------------------------------------------------------------
//  DownloadPanel — popup list of active/recent downloads.
// ----------------------------------------------------------------------------
class DownloadPanel : public QWidget {
public:
    explicit DownloadPanel(QWidget* parent = nullptr);

    // Rebuild the list from DownloadManager.
    void refresh();

    // Show the panel anchored under the given widget.
    void popupUnder(QWidget* anchor);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    void rebuildRowHeights();
    int rowAt(int y) const;

    struct Row { QString id; int y; int h; };
    QVector<Row> rows_;
    int hoverRow_ = -1;
    QTimer* liveTimer_ = nullptr;
};
