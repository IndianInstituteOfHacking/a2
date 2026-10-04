// ============================================================================
//  downloadpanel.cpp
// ============================================================================

#include "downloadpanel.h"
#include "downloadmanager.h"

#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QTimer>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QFileInfo>
#include <QUrl>
#include <QDesktopServices>
#include <QWebEngineDownloadRequest>
#include <cmath>

// ────────────────────────────────────────────────────────────────────────────
static QString humanBytes(qint64 n) {
    if (n <= 0) return QStringLiteral("—");
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return QString::number(v, 'f', (u == 0 ? 0 : 1)) + " " + units[u];
}

// ============================================================================
//  DownloadButton
// ============================================================================
DownloadButton::DownloadButton(QWidget* parent)
    : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(36, 36);

    pulseAnim_ = new QVariantAnimation(this);
    pulseAnim_->setDuration(1200);
    pulseAnim_->setStartValue(0.0);
    pulseAnim_->setEndValue(1.0);
    pulseAnim_->setLoopCount(-1);
    QObject::connect(pulseAnim_, &QVariantAnimation::valueChanged,
                     this, [this](const QVariant& v){
        pulse_ = v.toReal();
        update();
    });
}

void DownloadButton::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (active_) {
        pulseAnim_->start();
    } else {
        pulseAnim_->stop();
        pulse_ = 0.0;
    }
    update();
}

bool DownloadButton::event(QEvent* e) {
    if (e->type() == QEvent::HoverEnter) { hover_ = true; update(); }
    else if (e->type() == QEvent::HoverLeave) { hover_ = false; update(); }
    return QAbstractButton::event(e);
}

void DownloadButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background pill on hover / when active
    if (hover_ || active_) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, hover_ ? 22 : 12));
        p.drawRoundedRect(rect(), 11, 11);
    }

    const QRectF r = rect();
    const QPointF c = r.center();
    const qreal s = qMin(r.width(), r.height());

    // Pulsing ring while active
    if (active_) {
        const qreal ringR = s * (0.30 + 0.20 * pulse_);
        const int alpha = int(140 * (1.0 - pulse_));
        p.setPen(QPen(QColor(93, 157, 241, alpha), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, ringR, ringR);
    }

    // Icon: down-arrow into a tray. Accent colour when active, dim otherwise.
    const QColor col = active_ ? QColor("#5d9df1")
                               : (hover_ ? QColor("#ffffff") : QColor("#b4b8c2"));
    p.setPen(QPen(col, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    const qreal cx = c.x(), cy = c.y() - 1;
    const qreal aw = s * 0.30;   // arrow width
    const qreal ah = s * 0.24;   // arrow height
    // Arrow shaft
    p.drawLine(QPointF(cx, cy - ah), QPointF(cx, cy + ah * 0.2));
    // Arrow head
    p.drawLine(QPointF(cx - aw * 0.5, cy + ah * 0.0),
               QPointF(cx, cy + ah * 0.3));
    p.drawLine(QPointF(cx + aw * 0.5, cy + ah * 0.0),
               QPointF(cx, cy + ah * 0.3));
    // Tray
    p.drawLine(QPointF(cx - aw * 0.55, cy + ah * 0.7),
               QPointF(cx + aw * 0.55, cy + ah * 0.7));
}

// ============================================================================
//  DownloadPanel
// ============================================================================
DownloadPanel::DownloadPanel(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Popup);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(380);

    liveTimer_ = new QTimer(this);
    liveTimer_->setInterval(500);
    QObject::connect(liveTimer_, &QTimer::timeout, this, [this]{ refresh(); });
}

void DownloadPanel::refresh() {
    // Count active downloads
    const auto& items = DownloadManager::instance().items();
    bool anyActive = false;
    for (const auto& it : items)
        if (it.state == 0 || it.state == 4) { anyActive = true; break; }

    if (!anyActive) liveTimer_->stop();
    else if (!liveTimer_->isActive()) liveTimer_->start();

    rebuildRowHeights();
    update();
}

void DownloadPanel::rebuildRowHeights() {
    rows_.clear();
    const auto& items = DownloadManager::instance().items();

    // Sort: active first, then most recent first
    QVector<DownloadItem> sorted = items;
    std::sort(sorted.begin(), sorted.end(),
              [](const DownloadItem& a, const DownloadItem& b){
        const int aActive = (a.state == 0 || a.state == 4) ? 0 : 1;
        const int bActive = (b.state == 0 || b.state == 4) ? 0 : 1;
        if (aActive != bActive) return aActive < bActive;
        return (b.startedAt) > (a.startedAt);
    });

    int y = 52;   // header height
    for (const auto& it : sorted) {
        Row r;
        r.id = it.id;
        r.y = y;
        r.h = 66;
        rows_.push_back(r);
        y += r.h;
    }

    const int totalH = y + 12;
    setFixedHeight(qMin(totalH, 520));
}

int DownloadPanel::rowAt(int y) const {
    for (int i = 0; i < rows_.size(); ++i)
        if (y >= rows_[i].y && y < rows_[i].y + rows_[i].h) return i;
    return -1;
}

void DownloadPanel::popupUnder(QWidget* anchor) {
    refresh();
    if (!anchor) return;
    QPoint g = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 8));
    g.setX(g.x() - width());
    move(g);
    show();
    raise();
}

void DownloadPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Outer rounded card
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(QColor("#2c2e34"), 1));
    p.setBrush(QColor("#1c1d21"));
    p.drawRoundedRect(card, 14, 14);

    // Header
    p.setPen(QColor("#f1f2f5"));
    QFont hf = font();
    hf.setPixelSize(13);
    hf.setBold(true);
    p.setFont(hf);
    p.drawText(QRectF(18, 12, width() - 36, 28), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("Downloads"));

    // Count
    const auto& items = DownloadManager::instance().items();
    p.setPen(QColor("#6b7280"));
    QFont sf = font();
    sf.setPixelSize(11);
    sf.setBold(false);
    p.setFont(sf);
    p.drawText(QRectF(18, 12, width() - 36, 28), Qt::AlignVCenter | Qt::AlignRight,
               QString::number(items.size()));

    // Separator under header
    p.setPen(QPen(QColor("#23262c"), 1));
    p.drawLine(QPointF(0, 48), QPointF(width(), 48));

    if (rows_.isEmpty()) {
        p.setPen(QColor("#6b7280"));
        QFont ef = font();
        ef.setPixelSize(12);
        p.setFont(ef);
        p.drawText(QRectF(0, 60, width(), 60),
                   Qt::AlignCenter, QStringLiteral("No downloads"));
        return;
    }

    // Rows
    for (int i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const DownloadItem* it = nullptr;
        for (const auto& x : items) if (x.id == r.id) { it = &x; break; }
        if (!it) continue;

        const bool hov = (i == hoverRow_);
        if (hov) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 10));
            p.drawRoundedRect(QRectF(6, r.y, width() - 12, r.h - 4), 10, 10);
        }

        // Icon box
        QRectF iconBox(14, r.y + 10, 36, 36);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#23262c"));
        p.drawRoundedRect(iconBox, 8, 8);

        // Arrow icon
        p.setPen(QPen(QColor("#9aa0ac"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF ic = iconBox.center();
        p.drawLine(QPointF(ic.x(), ic.y() - 8), QPointF(ic.x(), ic.y() + 3));
        p.drawLine(QPointF(ic.x() - 5, ic.y() - 2), QPointF(ic.x(), ic.y() + 3));
        p.drawLine(QPointF(ic.x() + 5, ic.y() - 2), QPointF(ic.x(), ic.y() + 3));
        p.drawLine(QPointF(ic.x() - 6, ic.y() + 8), QPointF(ic.x() + 6, ic.y() + 8));

        // Filename
        QFont nf = font();
        nf.setPixelSize(12);
        nf.setBold(false);
        p.setFont(nf);
        p.setPen(QColor("#f1f2f5"));
        const QRectF nameR(60, r.y + 8, width() - 130, 18);
        p.drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(nf).elidedText(it->fileName, Qt::ElideMiddle, int(nameR.width())));

        // Status / progress text
        const qint64 total = it->totalBytes;
        const qint64 recv  = it->receivedBytes;
        const double pct   = total > 0 ? qMin(100.0, (recv * 100.0) / total) : 0.0;

        QString status;
        QColor statusCol;
        switch (it->state) {
            case 0:  status = QString("%1 / %2 · %3%")
                                .arg(humanBytes(recv), humanBytes(total))
                                .arg(int(pct));
                     statusCol = QColor("#9aa0ac"); break;
            case 1:  status = QString("Completed · %1").arg(humanBytes(recv));
                     statusCol = QColor("#4aaf7a"); break;
            case 2:  status = QStringLiteral("Cancelled");
                     statusCol = QColor("#e0443b"); break;
            case 3:  status = QStringLiteral("Failed");
                     statusCol = QColor("#e0443b"); break;
            case 4:  status = QStringLiteral("Paused");
                     statusCol = QColor("#f1c75c"); break;
            default: status = QStringLiteral("Unknown");
                     statusCol = QColor("#9aa0ac"); break;
        }
        QFont sf2 = font();
        sf2.setPixelSize(11);
        p.setFont(sf2);
        p.setPen(statusCol);
        p.drawText(QRectF(60, r.y + 26, width() - 130, 16),
                   Qt::AlignVCenter | Qt::AlignLeft, status);

        // Progress bar
        const QRectF barBg(60, r.y + 46, width() - 74, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#23262c"));
        p.drawRoundedRect(barBg, 2, 2);

        if (pct > 0.0) {
            const QRectF barFg(barBg.x(), barBg.y(),
                               barBg.width() * (pct / 100.0), barBg.height());
            QColor c1, c2;
            if (it->state == 1) { c1 = QColor("#4aaf7a"); c2 = QColor("#6bd29a"); }
            else if (it->state == 2 || it->state == 3) { c1 = QColor("#e0443b"); c2 = QColor("#e0443b"); }
            else if (it->state == 4) { c1 = QColor("#6b7280"); c2 = QColor("#9aa0ac"); }
            else { c1 = QColor("#5d9df1"); c2 = QColor("#7ab0ff"); }
            QLinearGradient g(barFg.topLeft(), barFg.topRight());
            g.setColorAt(0, c1); g.setColorAt(1, c2);
            p.setBrush(g);
            p.drawRoundedRect(barFg, 2, 2);
        }
    }
}

void DownloadPanel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int i = rowAt(int(e->position().y()));
    if (i < 0) return;
    const Row& r = rows_[i];

    // Click anywhere on the row: open file if completed, else reveal folder.
    const auto& items = DownloadManager::instance().items();
    for (const auto& it : items) {
        if (it.id != r.id) continue;
        if (it.state == 1) {
            DownloadManager::instance().openFile(it.id);
        } else {
            DownloadManager::instance().openFolder(it.id);
        }
        break;
    }
    hide();
}

void DownloadPanel::leaveEvent(QEvent*) {
    hoverRow_ = -1;
    update();
}
