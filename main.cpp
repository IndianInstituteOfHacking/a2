// ============================================================================
//  RootBrowser — main.cpp (bookmarks + downloads + download panel)
//
//  Build:
//    g++ -std=c++17 -O2 -fPIC \
//        main.cpp connector.cpp viewpagesource.cpp webadblocker.cpp \
//        bookmarkstore.cpp bookmarkpage.cpp \
//        downloadmanager.cpp downloadpage.cpp downloadpanel.cpp \
//        -o graphite \
//        $(pkg-config --cflags --libs Qt6WebEngineWidgets Qt6Widgets)
//    ./graphite
// ============================================================================

#include "connector.h"
#include "webadblocker.h"
#include "bookmarkstore.h"
#include "bookmarkpage.h"
#include "downloadmanager.h"
#include "downloadpage.h"
#include "downloadpanel.h"

#include <QApplication>
#include <QWidget>
#include <QAbstractButton>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QStackedWidget>
#include <QMenu>
#include <QAction>
#include <QShortcut>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QContextMenuEvent>
#include <QChildEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QElapsedTimer>
#include <QDateTime>
#include <QLocale>
#include <QUrl>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QPalette>
#include <QWindow>
#include <QImage>
#include <QStandardPaths>
#include <QIcon>
#include <QPixmap>
#include <QClipboard>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QFileDialog>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineHistory>
#include <QWebEngineDownloadRequest>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <functional>
#include <algorithm>
#include <cmath>

static const char* kAppName = "RootBrowser";
static const double kPi = 3.14159265358979323846;

// ============================================================================
//  YouTube ad neutralization script
// ============================================================================
static const char* kYouTubeAdNeutralizer = R"JS(
(function() {
    'use strict';
    const AD_KEYS = [
        'adPlacements', 'playerAds', 'adSlots', 'adBreakHeartbeatParams',
        'adThrottled', 'adSafetyReason', 'importantForAds', 'adParams'
    ];
    function stripAds(obj) {
        if (!obj || typeof obj !== 'object') return obj;
        for (const k of AD_KEYS) { try { delete obj[k]; } catch (e) {} }
        if (obj.playerResponse) stripAds(obj.playerResponse);
        return obj;
    }
    let _ytInitialPlayerResponse = undefined;
    try {
        Object.defineProperty(window, 'ytInitialPlayerResponse', {
            configurable: true,
            get() { return _ytInitialPlayerResponse; },
            set(v) { _ytInitialPlayerResponse = stripAds(v); }
        });
    } catch (e) {}
    const origFetch = window.fetch;
    if (origFetch) {
        window.fetch = function(input, init) {
            const url = (typeof input === 'string') ? input
                       : (input && input.url) ? input.url : '';
            const p = origFetch.apply(this, arguments);
            if (url.indexOf('/youtubei/v1/player') === -1) return p;
            return p.then(resp => resp.clone().json().then(data => {
                stripAds(data);
                return new Response(JSON.stringify(data), {
                    status: resp.status, statusText: resp.statusText,
                    headers: resp.headers
                });
            }).catch(() => resp));
        };
    }
    const skipObserver = new MutationObserver(() => {
        const skip = document.querySelector('.ytp-ad-skip-button, .ytp-skip-ad-button');
        if (skip) { try { skip.click(); } catch (e) {} }
        const overlay = document.querySelector('.ytp-ad-overlay-close-button');
        if (overlay) { try { overlay.click(); } catch (e) {} }
    });
    try {
        skipObserver.observe(document.documentElement, { childList: true, subtree: true });
    } catch (e) {}
})();
)JS";

// ============================================================================
//  FaviconLoader
// ============================================================================
class FaviconLoader : public QObject {
public:
    static FaviconLoader& instance() { static FaviconLoader f; return f; }

    QPixmap get(const QString& pageUrl, std::function<void(const QPixmap&)> ready = {}) {
        const QString key = cacheKey(pageUrl);
        if (key.isEmpty()) return QPixmap();
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (ready && !it->isNull()) ready(*it);
            return *it;
        }
        if (pending_.contains(key)) {
            if (ready) callbacks_[key].append(ready);
            return QPixmap();
        }
        pending_.insert(key);
        callbacks_[key].clear();
        if (ready) callbacks_[key].append(ready);
        if (loadFromDisk(key)) return QPixmap();
        fetch(key, 1);
        return QPixmap();
    }

private:
    FaviconLoader() { nam_ = new QNetworkAccessManager(this); }

    static QString cacheKey(const QString& pageUrl) {
        QUrl u = QUrl::fromUserInput(pageUrl);
        QString host = u.host().toLower();
        if (host.startsWith("www.")) host = host.mid(4);
        return host;
    }
    static QString diskPath(const QString& key) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/favicons";
        QDir().mkpath(dir);
        return dir + "/" + key + ".png";
    }
    bool loadFromDisk(const QString& key) {
        const QString path = diskPath(key);
        if (!QFile::exists(path)) return false;
        QPixmap pm;
        if (!pm.load(path) || pm.isNull()) return false;
        cache_.insert(key, pm);
        pending_.remove(key);
        notify(key, pm);
        return true;
    }
    void saveToDisk(const QString& key, const QPixmap& pm) { pm.save(diskPath(key), "PNG"); }

    QString sourceUrl(int n, const QString& key) const {
        switch (n) {
        case 1: return QString("https://%1/favicon.ico").arg(key);
        case 2: return QString("https://icons.duckduckgo.com/ip3/%1.ico").arg(key);
        case 3: return QString("https://www.google.com/s2/favicons?domain=%1&sz=64").arg(key);
        default: return QString();
        }
    }

    void fetch(const QString& key, int source) {
        const QString url = sourceUrl(source, key);
        if (url.isEmpty()) { pending_.remove(key); callbacks_[key].clear(); return; }
        QNetworkRequest req{QUrl(url)};
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      "Mozilla/5.0 (X11; Linux x86_64) RootBrowser/1.0");
        req.setTransferTimeout(6000);
        QNetworkReply* r = nam_->get(req);
        QObject::connect(r, &QNetworkReply::finished, this, [this, r, key, source]() {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) { fetch(key, source + 1); return; }
            const QByteArray data = r->readAll();
            QPixmap pm;
            if (!pm.loadFromData(data) || pm.isNull() || pm.width() < 8) {
                fetch(key, source + 1); return;
            }
            QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            bool hasContent = false;
            for (int y = 0; y < img.height() && !hasContent; y += 2)
                for (int x = 0; x < img.width(); x += 2)
                    if (qAlpha(img.pixel(x, y)) > 30) { hasContent = true; break; }
            if (!hasContent) { fetch(key, source + 1); return; }
            if (pm.width() < 32)
                pm = pm.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            cache_.insert(key, pm);
            saveToDisk(key, pm);
            pending_.remove(key);
            notify(key, pm);
        });
    }
    void notify(const QString& key, const QPixmap& pm) {
        if (!callbacks_.contains(key)) return;
        for (auto& cb : callbacks_[key]) if (cb) cb(pm);
        callbacks_[key].clear();
    }
    QNetworkAccessManager* nam_ = nullptr;
    QHash<QString, QPixmap> cache_;
    QHash<QString, QList<std::function<void(const QPixmap&)>>> callbacks_;
    QSet<QString> pending_;
};

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------
static QColor lerpColor(const QColor& a, const QColor& b, qreal t) {
    t = std::clamp<qreal>(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

struct Engine { const char* name; const char* url; };
static const Engine kEngines[] = {
    {"Google",     "https://www.google.com/search?q=%1"},
    {"DuckDuckGo", "https://duckduckgo.com/?q=%1"},
    {"Bing",       "https://www.bing.com/search?q=%1"},
    {"Brave",      "https://search.brave.com/search?q=%1"},
};
static const int kEngineCount = 4;
static int g_engine = 0;

static QString normalizeInput(QString s) {
    s = s.trimmed();
    if (s.isEmpty()) return QString();

    if (s.startsWith("rootbrowser://", Qt::CaseInsensitive)
        || s.startsWith("rootbrowser:", Qt::CaseInsensitive)
        || s.startsWith("rootbrowser-goto:", Qt::CaseInsensitive)
        || s.startsWith("rootbrowser-bookmark-del:", Qt::CaseInsensitive)
        || s.startsWith("rootbrowser-dl-", Qt::CaseInsensitive))
        return s;

    if (s.contains("://") || s.startsWith("about:") || s.startsWith("file:")) return s;
    const bool hasSpace = s.contains(' ');
    if (!hasSpace && (s.startsWith("localhost") || s.startsWith("127.0.0.1")))
        return "http://" + s;
    if (!hasSpace && s.contains('.'))
        return "https://" + s;
    return QString(kEngines[g_engine].url)
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(s)));
}

static QString titleFor(const QString& u) {
    if (u.isEmpty()) return "New Tab";
    QUrl q(u);
    QUrlQuery qq(q);
    const QString term = qq.queryItemValue("q", QUrl::FullyDecoded);
    if (!term.isEmpty()) return term;
    QString h = q.host();
    if (h.startsWith("www.")) h = h.mid(4);
    return h.isEmpty() ? u : h;
}

// ----------------------------------------------------------------------------
//  Toolbar icons
// ----------------------------------------------------------------------------
enum class Ic { Back, Forward, Reload, Home, Menu, Plus, Close, Search, Full, Exit };

static void drawIcon(QPainter& p, Ic k, const QRectF& r, const QColor& col) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = r.width();
    auto P = [&](qreal x, qreal y) { return QPointF(r.x() + x * s, r.y() + y * s); };
    p.setPen(QPen(col, std::max<qreal>(1.5, s * 0.085),
                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (k) {
    case Ic::Back: {
        p.drawLine(P(.82, .5), P(.2, .5));
        QPointF pts[3] = {P(.48, .24), P(.2, .5), P(.48, .76)};
        p.drawPolyline(pts, 3);
        break;
    }
    case Ic::Forward: {
        p.drawLine(P(.18, .5), P(.8, .5));
        QPointF pts[3] = {P(.52, .24), P(.8, .5), P(.52, .76)};
        p.drawPolyline(pts, 3);
        break;
    }
    case Ic::Reload: {
        QRectF a(r.x() + .2 * s, r.y() + .2 * s, .6 * s, .6 * s);
        p.drawArc(a, 110 * 16, 290 * 16);
        const qreal th = 40.0 * kPi / 180.0, rad = .3 * s;
        const QPointF c = P(.5, .5);
        const QPointF e(c.x() + rad * std::cos(th), c.y() - rad * std::sin(th));
        const QPointF t(-std::sin(th), -std::cos(th));
        const QPointF tip(e.x() + t.x() * .08 * s, e.y() + t.y() * .08 * s);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const qreal ph = sgn * 40.0 * kPi / 180.0;
            const QPointF b(-t.x(), -t.y());
            const QPointF w(b.x() * std::cos(ph) - b.y() * std::sin(ph),
                            b.x() * std::sin(ph) + b.y() * std::cos(ph));
            p.drawLine(tip, QPointF(tip.x() + w.x() * .22 * s, tip.y() + w.y() * .22 * s));
        }
        break;
    }
    case Ic::Home: {
        QPointF roof[3] = {P(.16, .52), P(.5, .2), P(.84, .52)};
        p.drawPolyline(roof, 3);
        QPointF body[4] = {P(.27, .45), P(.27, .8), P(.73, .8), P(.73, .45)};
        p.drawPolyline(body, 4);
        break;
    }
    case Ic::Menu: {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        const qreal d = s * .075;
        p.drawEllipse(P(.5, .2), d, d);
        p.drawEllipse(P(.5, .5), d, d);
        p.drawEllipse(P(.5, .8), d, d);
        break;
    }
    case Ic::Plus:
        p.drawLine(P(.5, .22), P(.5, .78));
        p.drawLine(P(.22, .5), P(.78, .5));
        break;
    case Ic::Close:
        p.drawLine(P(.28, .28), P(.72, .72));
        p.drawLine(P(.72, .28), P(.28, .72));
        break;
    case Ic::Search:
        p.drawEllipse(P(.45, .45), .24 * s, .24 * s);
        p.drawLine(P(.63, .63), P(.82, .82));
        break;
    case Ic::Full: {
        QPointF a[3] = {P(.2, .38), P(.2, .2), P(.38, .2)};
        QPointF b[3] = {P(.62, .2), P(.8, .2), P(.8, .38)};
        QPointF c[3] = {P(.2, .62), P(.2, .8), P(.38, .8)};
        QPointF d[3] = {P(.62, .8), P(.8, .8), P(.8, .62)};
        p.drawPolyline(a, 3); p.drawPolyline(b, 3); p.drawPolyline(c, 3); p.drawPolyline(d, 3);
        break;
    }
    case Ic::Exit: {
        QPointF a[3] = {P(.38, .2), P(.38, .38), P(.2, .38)};
        QPointF b[3] = {P(.62, .2), P(.62, .38), P(.8, .38)};
        QPointF c[3] = {P(.38, .8), P(.38, .62), P(.2, .62)};
        QPointF d[3] = {P(.62, .8), P(.62, .62), P(.8, .62)};
        p.drawPolyline(a, 3); p.drawPolyline(b, 3); p.drawPolyline(c, 3); p.drawPolyline(d, 3);
        break;
    }
    }
    p.restore();
}

// ----------------------------------------------------------------------------
//  Small building blocks
// ----------------------------------------------------------------------------
class IconButton : public QAbstractButton {
public:
    explicit IconButton(Ic k, QWidget* parent = nullptr, int box = 36)
        : QAbstractButton(parent), kind_(k), box_(box) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(box, box);
    }
    void setKind(Ic k) { kind_ = k; update(); }
protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::HoverEnter) { hover_ = true; update(); }
        else if (e->type() == QEvent::HoverLeave) { hover_ = false; update(); }
        return QAbstractButton::event(e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool en = isEnabled();
        if (en && (hover_ || isDown())) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, isDown() ? 38 : 20));
            p.drawRoundedRect(rect(), 11, 11);
        }
        QColor c = !en ? QColor(255, 255, 255, 48)
                       : (hover_ ? QColor("#ffffff") : QColor("#b4b8c2"));
        const qreal ic = box_ * 0.55;
        drawIcon(p, kind_, QRectF((width() - ic) / 2.0, (height() - ic) / 2.0, ic, ic), c);
    }
private:
    Ic kind_;
    int box_;
    bool hover_ = false;
};

class Panel : public QWidget {
public:
    Panel(const QColor& c, bool bottomLine, QWidget* parent = nullptr)
        : QWidget(parent), c_(c), line_(bottomLine) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), c_);
        if (line_) p.fillRect(QRect(0, height() - 1, width(), 1), QColor("#2a2b31"));
    }
private:
    QColor c_;
    bool line_;
};

class AddressEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
protected:
    void focusInEvent(QFocusEvent* e) override {
        QLineEdit::focusInEvent(e);
        if (e->reason() == Qt::MouseFocusReason)
            QTimer::singleShot(0, this, [this] { selectAll(); });
        else
            selectAll();
    }
};

// ----------------------------------------------------------------------------
//  Tab strip
// ----------------------------------------------------------------------------
class TabStrip : public QWidget {
public:
    std::function<void(int)> onCurrent, onClose;
    std::function<void()> onNew, onMin, onMaxToggle, onCloseWin, onDrag;

    explicit TabStrip(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(46);
        setMouseTracking(true);
    }
    int addTab(const QString& t) {
        titles_.push_back(t); icons_.push_back(QPixmap());
        update(); return int(titles_.size()) - 1;
    }
    void removeTab(int i) {
        if (i < 0 || i >= titles_.size()) return;
        titles_.removeAt(i);
        icons_.removeAt(i);
        if (cur_ >= titles_.size()) cur_ = int(titles_.size()) - 1;
        hover_ = -1; hoverClose_ = false;
        update();
    }
    void setTitle(int i, const QString& t) { if (i >= 0 && i < titles_.size()) { titles_[i] = t; update(); } }
    void setIcon(int i, const QPixmap& pm) { if (i >= 0 && i < icons_.size()) { icons_[i] = pm; update(); } }
    void setNote(const QString& n) { note_ = n; update(); }
    void setMaximized(bool m) { maximized_ = m; update(); }
    void setCurrent(int i) { cur_ = i; update(); }
    int count() const { return int(titles_.size()); }

protected:
    static constexpr int kCtl = 124;

    qreal tabW() const {
        const int n = std::max<int>(1, int(titles_.size()));
        return std::clamp<qreal>((width() - 12 - 50 - kCtl - 30) / n, 84.0, 240.0);
    }
    QRectF tabRect(int i) const { return QRectF(10 + i * tabW(), 9, tabW() - 2, height() - 9); }
    QRectF closeRect(int i) const {
        QRectF r = tabRect(i);
        return QRectF(r.right() - 28, r.center().y() - 10, 20, 20);
    }
    bool closeVisible(int i) const { return i == cur_ || tabW() >= 112; }
    QRectF plusRect() const {
        qreal x = 10 + titles_.size() * tabW() + 4;
        x = std::min<qreal>(x, width() - kCtl - 40);
        return QRectF(x, 12, 30, 30);
    }
    QRectF ctlRect(int k) const {
        const qreal right = width() - 8 - (2 - k) * 38;
        return QRectF(right - 36, 8, 36, 30);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#0c0c0e"));
        QFont f = font();
        f.setPixelSize(13);
        p.setFont(f);
        const QFontMetrics fm(f);

        for (int i = 0; i < titles_.size(); ++i) {
            const QRectF r = tabRect(i);
            const bool act = (i == cur_);
            p.setPen(Qt::NoPen);
            if (act) {
                QPainterPath path;
                path.setFillRule(Qt::WindingFill);
                path.addRoundedRect(r, 11, 11);
                path.addRect(QRectF(r.x(), r.center().y(), r.width(), r.height() / 2));
                p.setBrush(QColor("#1c1d21"));
                p.drawPath(path);
            } else if (i == hover_) {
                p.setBrush(QColor(255, 255, 255, 14));
                p.drawRoundedRect(r.adjusted(0, 3, 0, -6), 10, 10);
            }

            const QRectF fav(r.x() + 13, r.center().y() - 8, 17, 17);
            if (!icons_[i].isNull()) {
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawPixmap(fav.toRect(), icons_[i]);
            } else {
                QLinearGradient g(fav.topLeft(), fav.bottomRight());
                g.setColorAt(0, QColor("#99a1b3"));
                g.setColorAt(1, QColor("#4f5565"));
                p.setBrush(g);
                p.drawEllipse(fav);
                QFont ff = f; ff.setPixelSize(10); ff.setBold(true);
                p.setFont(ff);
                p.setPen(QColor("#0c0c0e"));
                p.drawText(fav, Qt::AlignCenter, titles_[i].left(1).toUpper());
                p.setFont(f);
            }

            const bool showClose = closeVisible(i);
            const qreal tx = fav.right() + 9;
            const qreal tw = r.right() - tx - (showClose ? 32 : 12);
            p.setPen(act ? QColor("#f1f2f5") : QColor("#9aa0ac"));
            p.drawText(QRectF(tx, r.y(), tw, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(titles_[i], Qt::ElideRight, int(tw)));

            if (showClose) {
                const QRectF cr = closeRect(i);
                if (i == hover_ && hoverClose_) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(255, 255, 255, 30));
                    p.drawEllipse(cr);
                }
                drawIcon(p, Ic::Close, cr.adjusted(4, 4, -4, -4),
                         (i == hover_ && hoverClose_) ? QColor("#ffffff") : QColor("#8d929e"));
            }
        }

        const QRectF pr = plusRect();
        if (hoverPlus_) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 22));
            p.drawRoundedRect(pr, 10, 10);
        }
        drawIcon(p, Ic::Plus, pr.adjusted(7, 7, -7, -7),
                 hoverPlus_ ? QColor("#ffffff") : QColor("#9aa0ac"));

        for (int k = 0; k < 3; ++k) {
            const QRectF cr = ctlRect(k);
            const bool h = (hoverCtl_ == k);
            if (h) {
                p.setPen(Qt::NoPen);
                p.setBrush(k == 2 ? QColor(226, 70, 70) : QColor(255, 255, 255, 24));
                p.drawRoundedRect(cr, 9, 9);
            }
            p.setPen(QPen(h ? QColor("#ffffff") : QColor("#9aa0ac"), 1.4,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            const QPointF c = cr.center();
            if (k == 0) {
                p.drawLine(c + QPointF(-5, 0), c + QPointF(5, 0));
            } else if (k == 1) {
                if (maximized_) {
                    p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 2, 7, 7), 1.5, 1.5);
                    p.drawLine(QPointF(c.x() - 2, c.y() - 5), QPointF(c.x() + 5, c.y() - 5));
                    p.drawLine(QPointF(c.x() + 5, c.y() - 5), QPointF(c.x() + 5, c.y() + 2));
                } else {
                    p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 5, 10, 10), 2, 2);
                }
            } else {
                p.drawLine(c + QPointF(-4.5, -4.5), c + QPointF(4.5, 4.5));
                p.drawLine(c + QPointF(4.5, -4.5), c + QPointF(-4.5, 4.5));
            }
        }

        if (!note_.isEmpty()) {
            const qreal w = fm.horizontalAdvance(note_) + 30;
            const QRectF nr(width() - kCtl - w - 6, 10, w, 30);
            p.setPen(QPen(QColor(255, 255, 255, 24), 1));
            p.setBrush(QColor("#1f2025"));
            p.drawRoundedRect(nr, 15, 15);
            p.setPen(QColor("#d4d8e1"));
            p.drawText(nr, Qt::AlignCenter, note_);
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        int h = -1; bool hc = false;
        for (int i = 0; i < titles_.size(); ++i) {
            if (tabRect(i).contains(pos)) {
                h = i;
                hc = closeVisible(i) && closeRect(i).contains(pos);
                break;
            }
        }
        const bool hp = plusRect().contains(pos);
        int ctl = -1;
        for (int k = 0; k < 3; ++k) if (ctlRect(k).contains(pos)) ctl = k;
        if (h != hover_ || hc != hoverClose_ || hp != hoverPlus_ || ctl != hoverCtl_) {
            hover_ = h; hoverClose_ = hc; hoverPlus_ = hp; hoverCtl_ = ctl;
            setCursor((h >= 0 || hp || ctl >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent*) override {
        hover_ = -1; hoverClose_ = false; hoverPlus_ = false; hoverCtl_ = -1;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        if (e->button() == Qt::MiddleButton) {
            for (int i = 0; i < titles_.size(); ++i)
                if (tabRect(i).contains(pos)) { if (onClose) onClose(i); return; }
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        for (int k = 0; k < 3; ++k) {
            if (ctlRect(k).contains(pos)) {
                if (k == 0 && onMin) onMin();
                else if (k == 1 && onMaxToggle) onMaxToggle();
                else if (k == 2 && onCloseWin) onCloseWin();
                return;
            }
        }
        for (int i = 0; i < titles_.size(); ++i) {
            if (tabRect(i).contains(pos)) {
                if (closeVisible(i) && closeRect(i).contains(pos)) {
                    if (onClose) onClose(i);
                } else if (i != cur_) {
                    cur_ = i; update();
                    if (onCurrent) onCurrent(i);
                }
                return;
            }
        }
        if (plusRect().contains(pos)) { if (onNew) onNew(); return; }
        if (onDrag) onDrag();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        for (int i = 0; i < titles_.size(); ++i) if (tabRect(i).contains(pos)) return;
        for (int k = 0; k < 3; ++k) if (ctlRect(k).contains(pos)) return;
        if (!plusRect().contains(pos) && onMaxToggle) onMaxToggle();
    }

private:
    QStringList titles_;
    QVector<QPixmap> icons_;
    QString note_;
    int cur_ = 0, hover_ = -1, hoverCtl_ = -1;
    bool hoverClose_ = false, hoverPlus_ = false, maximized_ = false;
};

// ----------------------------------------------------------------------------
//  Home page widgets
// ----------------------------------------------------------------------------
class Tile : public QWidget {
public:
    std::function<void(const QString&)> onClick;

    Tile(const QString& label, const QString& url, const QColor& accent, QWidget* parent = nullptr)
        : QWidget(parent), label_(label), url_(url), accent_(accent) {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setMinimumSize(118, 108);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        anim_ = new QVariantAnimation(this);
        anim_->setDuration(150);
        anim_->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            hov_ = v.toReal(); update();
        });
        FaviconLoader::instance().get(url_, [this](const QPixmap& pm) {
            icon_ = pm;
            update();
        });
    }

protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::HoverEnter) animateTo(1.0);
        else if (e->type() == QEvent::HoverLeave) animateTo(0.0);
        return QWidget::event(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && onClick)
            onClick(url_);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(2, 5, -2, -2);
        r.translate(0, -3.0 * hov_);

        p.setPen(QPen(QColor(255, 255, 255, int(14 + 24 * hov_)), 1));
        p.setBrush(lerpColor(QColor("#17181b"), QColor("#212329"), hov_));
        p.drawRoundedRect(r, 18, 18);

        const QRectF b(r.center().x() - 22, r.y() + 15, 44, 44);

        if (!icon_.isNull()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, int(8 + 10 * hov_)));
            p.drawRoundedRect(b, 12, 12);
            QPixmap scaled = icon_.scaled(int(b.width()) - 12, int(b.height()) - 12,
                                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF target(b.center().x() - scaled.width() / 2.0,
                                b.center().y() - scaled.height() / 2.0,
                                scaled.width(), scaled.height());
            p.drawPixmap(target, scaled, QRectF(scaled.rect()));
        } else {
            QLinearGradient g(b.topLeft(), b.bottomRight());
            g.setColorAt(0, accent_.lighter(112));
            g.setColorAt(1, accent_.darker(160));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(b, 13, 13);
            QFont f = font();
            f.setPixelSize(19); f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(255, 255, 255, 235));
            p.drawText(b, Qt::AlignCenter, label_.left(1).toUpper());
        }

        QFont f = font();
        f.setPixelSize(13); f.setBold(false);
        p.setFont(f);
        p.setPen(lerpColor(QColor("#aeb3be"), QColor("#ffffff"), hov_));
        p.drawText(QRectF(r.x(), b.bottom() + 9, r.width(), 22), Qt::AlignCenter, label_);
    }

private:
    void animateTo(qreal v) {
        anim_->stop();
        anim_->setStartValue(hov_);
        anim_->setEndValue(v);
        anim_->start();
    }
    QString label_, url_;
    QColor accent_;
    QPixmap icon_;
    qreal hov_ = 0.0;
    QVariantAnimation* anim_;
};

class SearchBox : public QWidget {
public:
    std::function<void(const QString&)> onSubmit;
    std::function<void()> onEngine;

    explicit SearchBox(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(68);
        auto* l = new QHBoxLayout(this);
        l->setContentsMargins(24, 0, 20, 0);
        l->setSpacing(10);

        eng_ = new QPushButton(this);
        eng_->setCursor(Qt::PointingHandCursor);
        eng_->setFocusPolicy(Qt::NoFocus);
        eng_->setFixedHeight(30);
        eng_->setToolTip("Change search engine");
        eng_->setStyleSheet(
            "QPushButton{background:rgba(255,255,255,0.06);border:none;border-radius:15px;"
            "padding:0 14px;color:#c5c9d2;font-size:12px;}"
            "QPushButton:hover{background:rgba(255,255,255,0.12);color:#ffffff;}");
        refreshEngine();

        edit_ = new QLineEdit(this);
        edit_->setPlaceholderText("Search the web or type a URL");
        edit_->setStyleSheet("QLineEdit{background:transparent;border:none;}");
        QFont f = edit_->font();
        f.setPixelSize(17);
        edit_->setFont(f);
        QPalette pal = edit_->palette();
        pal.setColor(QPalette::Text, QColor("#f2f3f6"));
        pal.setColor(QPalette::PlaceholderText, QColor(160, 166, 182, 140));
        pal.setColor(QPalette::Highlight, QColor("#59627a"));
        pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        edit_->setPalette(pal);
        edit_->installEventFilter(this);

        go_ = new IconButton(Ic::Search, this, 40);

        l->addWidget(eng_);
        l->addWidget(edit_, 1);
        l->addWidget(go_);

        QObject::connect(edit_, &QLineEdit::returnPressed, this, [this] { submit(); });
        QObject::connect(go_, &QAbstractButton::clicked, this, [this] { submit(); });
        QObject::connect(eng_, &QAbstractButton::clicked, this, [this] {
            g_engine = (g_engine + 1) % kEngineCount;
            refreshEngine();
            if (onEngine) onEngine();
        });

        anim_ = new QVariantAnimation(this);
        anim_->setDuration(180);
        anim_->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            glow_ = v.toReal(); update();
        });
    }

    void refreshEngine() { eng_->setText(kEngines[g_engine].name); }
    void clear() { edit_->clear(); }
    void focusEdit() { edit_->setFocus(); }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == edit_) {
            if (e->type() == QEvent::FocusIn) animateTo(1.0);
            else if (e->type() == QEvent::FocusOut) animateTo(0.0);
        }
        return QWidget::eventFilter(o, e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(8, 8, -8, -8);
        const qreal rad = r.height() / 2.0;
        p.setBrush(Qt::NoBrush);
        if (glow_ > 0.01) {
            for (int i = 3; i >= 1; --i) {
                p.setPen(QPen(QColor(140, 152, 180, int(20.0 * glow_ / i)), i * 3.0));
                p.drawRoundedRect(r, rad, rad);
            }
        }
        p.setPen(QPen(lerpColor(QColor("#2c2e35"), QColor("#7a849c"), glow_), 1.2));
        p.setBrush(lerpColor(QColor("#151619"), QColor("#101114"), glow_));
        p.drawRoundedRect(r, rad, rad);
    }

private:
    void animateTo(qreal v) {
        anim_->stop();
        anim_->setStartValue(glow_);
        anim_->setEndValue(v);
        anim_->start();
    }
    void submit() {
        const QString t = edit_->text().trimmed();
        if (!t.isEmpty() && onSubmit) onSubmit(t);
    }
    QPushButton* eng_;
    QLineEdit* edit_;
    IconButton* go_;
    QVariantAnimation* anim_;
    qreal glow_ = 0.0;
};

class HomePage : public QWidget {
public:
    std::function<void(const QString&)> onNavigate;
    std::function<void(const QPoint&)> onContextMenu;

    explicit HomePage(QWidget* parent = nullptr) : QWidget(parent) {
        clk_.start();

        clock_ = new QLabel(this);
        clock_->setAlignment(Qt::AlignCenter);
        setLabelColor(clock_, QColor("#f4f5f7"));

        greet_ = new QLabel(this);
        greet_->setAlignment(Qt::AlignCenter);
        setLabelColor(greet_, QColor("#8b909d"));

        hint_ = new QLabel("Ctrl+T  New tab      Ctrl+L  Address bar      F11  Full screen", this);
        hint_->setAlignment(Qt::AlignCenter);
        setLabelColor(hint_, QColor("#5b5f6a"));
        QFont hf = hint_->font(); hf.setPixelSize(12); hint_->setFont(hf);

        search_ = new SearchBox(this);
        search_->setMaximumWidth(700);
        search_->onSubmit = [this](const QString& t) { if (onNavigate) onNavigate(t); };

        auto* grid = new QWidget(this);
        grid->setMaximumWidth(800);
        auto* g = new QGridLayout(grid);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(14);
        g->setVerticalSpacing(14);
        struct T { const char* n; const char* u; const char* c; };
        static const T tiles[] = {
            {"GitHub",         "https://github.com",               "#6e7681"},
            {"YouTube",        "https://www.youtube.com",          "#e0443b"},
            {"Wikipedia",      "https://www.wikipedia.org",        "#8e949f"},
            {"Reddit",         "https://www.reddit.com",           "#e8602c"},
            {"Stack Overflow", "https://stackoverflow.com",        "#e07a1f"},
            {"Hacker News",    "https://news.ycombinator.com",     "#ee7418"},
            {"Gmail",          "https://mail.google.com",          "#d9483b"},
            {"Maps",           "https://maps.google.com",          "#2f9e5b"},
        };
        for (int i = 0; i < 8; ++i) {
            auto* t = new Tile(tiles[i].n, tiles[i].u, QColor(tiles[i].c), grid);
            t->onClick = [this](const QString& u) { if (onNavigate) onNavigate(u); };
            g->addWidget(t, i / 4, i % 4);
        }

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(24, 16, 24, 14);
        v->setSpacing(0);
        v->addStretch(3);
        v->addWidget(clock_);
        v->addWidget(greet_);
        v->addSpacing(34);
        auto* sr = new QHBoxLayout;
        sr->addStretch(1); sr->addWidget(search_, 100); sr->addStretch(1);
        v->addLayout(sr);
        v->addSpacing(34);
        auto* gr = new QHBoxLayout;
        gr->addStretch(1); gr->addWidget(grid, 100); gr->addStretch(1);
        v->addLayout(gr);
        v->addStretch(4);
        v->addWidget(hint_);

        tickClock();
        updateFonts();

        tick_ = new QTimer(this);
        QObject::connect(tick_, &QTimer::timeout, this, [this] { tickClock(); });
        anim_ = new QTimer(this);
        anim_->setInterval(40);
        QObject::connect(anim_, &QTimer::timeout, this, [this] { update(); });
    }

    void clearSearch() { search_->clear(); }
    void focusSearch() { search_->focusEdit(); }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        tick_->start(1000); anim_->start(); tickClock();
    }
    void hideEvent(QHideEvent* e) override {
        QWidget::hideEvent(e);
        tick_->stop(); anim_->stop();
    }
    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); updateFonts(); }

    void contextMenuEvent(QContextMenuEvent* ev) override {
        if (onContextMenu) {
            onContextMenu(ev->pos());
            ev->accept();
        } else {
            QWidget::contextMenuEvent(ev);
        }
    }

    void paintEvent(QPaintEvent*) override {
        const QSize s(std::max(2, width() / 4), std::max(2, height() / 4));
        if (bg_.size() != s) bg_ = QImage(s, QImage::Format_ARGB32_Premultiplied);
        QPainter b(&bg_);
        QLinearGradient lg(0, 0, 0, s.height());
        lg.setColorAt(0, QColor("#08080a"));
        lg.setColorAt(1, QColor("#151619"));
        b.fillRect(bg_.rect(), lg);

        const qreal t = clk_.elapsed() / 1000.0;
        const qreal big = std::max(s.width(), s.height());
        struct Orb { int r, g, b; qreal sp, ph, rad, a; };
        static const Orb orbs[] = {
            {92, 100, 124, 0.11, 0.0, 0.62, 72},
            {74, 70, 90,   0.08, 2.1, 0.55, 62},
            {62, 76, 86,   0.09, 4.2, 0.50, 56},
        };
        for (const Orb& o : orbs) {
            const QPointF c(s.width() * (0.5 + 0.34 * std::sin(t * o.sp + o.ph)),
                            s.height() * (0.42 + 0.28 * std::cos(t * o.sp * 0.8 + o.ph * 1.3)));
            QRadialGradient g(c, big * o.rad);
            g.setColorAt(0, QColor(o.r, o.g, o.b, int(o.a)));
            g.setColorAt(1, QColor(o.r, o.g, o.b, 0));
            b.fillRect(bg_.rect(), g);
        }
        QRadialGradient vg(QPointF(s.width() / 2.0, s.height() * 0.45), big * 0.8);
        vg.setColorAt(0.6, QColor(0, 0, 0, 0));
        vg.setColorAt(1.0, QColor(0, 0, 0, 150));
        b.fillRect(bg_.rect(), vg);
        b.end();

        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(rect(), bg_);
    }

private:
    static void setLabelColor(QLabel* l, const QColor& c) {
        QPalette p = l->palette();
        p.setColor(QPalette::WindowText, c);
        l->setPalette(p);
    }
    void updateFonts() {
        QFont f = clock_->font();
        f.setPixelSize(std::clamp(int(height() * 0.12), 52, 120));
        f.setWeight(QFont::Light);
        clock_->setFont(f);
        QFont g = greet_->font();
        g.setPixelSize(std::clamp(int(height() * 0.026), 14, 19));
        greet_->setFont(g);
    }
    void tickClock() {
        const QTime t = QTime::currentTime();
        clock_->setText(t.toString("HH:mm"));
        const int h = t.hour();
        const char* g = h < 5 ? "Good night" : h < 12 ? "Good morning"
                                  : h < 17 ? "Good afternoon" : "Good evening";
        greet_->setText(QString("%1   \u00B7   %2")
                            .arg(QLocale().toString(QDate::currentDate(), "dddd, d MMMM"))
                            .arg(g));
    }

    QLabel *clock_, *greet_, *hint_;
    SearchBox* search_;
    QTimer *tick_, *anim_;
    QElapsedTimer clk_;
    QImage bg_;
};

// ----------------------------------------------------------------------------
//  WebView
// ----------------------------------------------------------------------------
class WebView : public QWebEngineView {
public:
    std::function<void(const QPoint&)> onContextMenu;

    explicit WebView(QWidget* parent = nullptr) : QWebEngineView(parent) {
        installEventFilter(this);
        QTimer::singleShot(0, this, [this]{ installOnChildren(); });
        QTimer::singleShot(500, this, [this]{ installOnChildren(); });
    }

protected:
    void installOnChildren() {
        for (auto* w : findChildren<QWidget*>()) {
            if (!w->property("ctxFiltered").toBool()) {
                w->installEventFilter(this);
                w->setProperty("ctxFiltered", true);
            }
        }
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::ContextMenu) {
            installOnChildren();
            auto* ce = static_cast<QContextMenuEvent*>(ev);
            const QPoint local = mapFromGlobal(ce->globalPos());
            if (onContextMenu) {
                onContextMenu(local);
                return true;
            }
        } else if (ev->type() == QEvent::ChildAdded) {
            if (auto* child = static_cast<QChildEvent*>(ev)->child()) {
                if (auto* w = qobject_cast<QWidget*>(child)) {
                    w->installEventFilter(this);
                    w->setProperty("ctxFiltered", true);
                    for (auto* sub : w->findChildren<QWidget*>()) {
                        sub->installEventFilter(this);
                        sub->setProperty("ctxFiltered", true);
                    }
                }
            }
        }
        return QWebEngineView::eventFilter(obj, ev);
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        if (onContextMenu) {
            onContextMenu(event->pos());
            event->accept();
        } else {
            QWebEngineView::contextMenuEvent(event);
        }
    }
};

// ----------------------------------------------------------------------------
//  WebPage
// ----------------------------------------------------------------------------
class WebPage : public QWebEnginePage {
public:
    std::function<QWebEnginePage*()> onNewWindow;
    std::function<bool(const QUrl&)> onSpecialScheme;

    WebPage(QWebEngineProfile* prof, QObject* parent)
        : QWebEnginePage(prof, parent) {}

protected:
    QWebEnginePage* createWindow(WebWindowType) override {
        return onNewWindow ? onNewWindow() : nullptr;
    }

    bool acceptNavigationRequest(const QUrl& url,
                                 NavigationType type,
                                 bool isMainFrame) override
    {
        if (isMainFrame && onSpecialScheme && onSpecialScheme(url))
            return false;
        return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    }
};

class LoadBar : public QWidget {
public:
    explicit LoadBar(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(2); }
    void setValue(int v) { v_ = v; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (v_ <= 0 || v_ >= 100) return;
        QPainter p(this);
        const int w = width() * v_ / 100;
        QLinearGradient g(0, 0, std::max(1, w), 0);
        g.setColorAt(0, QColor("#6b7690"));
        g.setColorAt(1, QColor("#cfd6ea"));
        p.fillRect(QRect(0, 0, w, height()), g);
    }
private:
    int v_ = 0;
};

// ----------------------------------------------------------------------------
//  Browser
// ----------------------------------------------------------------------------
class Browser : public QWidget, public Host {
public:
    Browser() {
        setWindowTitle(kAppName);
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setMouseTracking(true);
        setMinimumSize(780, 660);
        resize(1280, 800);

        profile_ = new QWebEngineProfile("graphite", this);
        QWebEngineSettings* st = profile_->settings();
        st->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
        st->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, true);
        st->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, true);

        WebAdBlocker::attach(profile_);
        DownloadManager::instance().attach(profile_);
        installYouTubeAdNeutralizer();

        ContextMenu::setHost(this);
        ContextMenu::attach(profile_, this);

        noteTimer_ = new QTimer(this);
        noteTimer_->setSingleShot(true);
        QObject::connect(noteTimer_, &QTimer::timeout, this,
                         [this] { tabs_->setNote(QString()); });

        auto* root = new QVBoxLayout(this);
        root_ = root;
        root->setContentsMargins(kEdge, kEdge, kEdge, kEdge);
        root->setSpacing(0);

        tabs_ = new TabStrip(this);

        toolbar_ = new Panel(QColor("#1c1d21"), true, this);
        toolbar_->setFixedHeight(56);
        auto* tb = new QHBoxLayout(toolbar_);
        tb->setContentsMargins(12, 0, 12, 0);
        tb->setSpacing(4);

        back_    = new IconButton(Ic::Back, toolbar_);    back_->setToolTip("Back (Alt+Left)");
        fwd_     = new IconButton(Ic::Forward, toolbar_); fwd_->setToolTip("Forward (Alt+Right)");
        reload_  = new IconButton(Ic::Reload, toolbar_);  reload_->setToolTip("Reload (F5)");
        homeBtn_ = new IconButton(Ic::Home, toolbar_);    homeBtn_->setToolTip("Home (Alt+Home)");
        full_    = new IconButton(Ic::Full, toolbar_);    full_->setToolTip("Full screen (F11)");
        menuBtn_ = new IconButton(Ic::Menu, toolbar_);    menuBtn_->setToolTip("Menu");

        // Download button (hidden until first download starts)
        dlBtn_ = new DownloadButton(toolbar_);
        dlBtn_->setToolTip("Downloads");
        dlBtn_->hide();

        // Download popup panel
        dlPanel_ = new DownloadPanel(this);

        QObject::connect(dlBtn_, &QAbstractButton::clicked, this, [this]{
            dlPanel_->popupUnder(dlBtn_);
        });

        addr_ = new AddressEdit(toolbar_);
        addr_->setPlaceholderText("Search or enter address");
        addr_->setFixedHeight(38);
        addr_->setClearButtonEnabled(true);
        QFont af = addr_->font(); af.setPixelSize(14); addr_->setFont(af);
        QPalette ap = addr_->palette();
        ap.setColor(QPalette::PlaceholderText, QColor(160, 166, 182, 130));
        addr_->setPalette(ap);
        addr_->setStyleSheet(
            "QLineEdit{background:#121316;border:1px solid #2a2c32;border-radius:19px;"
            "padding:0 12px 0 4px;color:#e8e9ec;selection-background-color:#59627a;}"
            "QLineEdit:focus{background:#0f1013;border:1px solid #6f7990;}");
        {
            QPixmap pm(36, 36);
            pm.setDevicePixelRatio(2.0);
            pm.fill(Qt::transparent);
            QPainter pp(&pm);
            drawIcon(pp, Ic::Search, QRectF(0, 0, 18, 18), QColor("#8a8f9c"));
            pp.end();
            addr_->addAction(QIcon(pm), QLineEdit::LeadingPosition);
        }

        tb->addWidget(back_);
        tb->addWidget(fwd_);
        tb->addWidget(reload_);
        tb->addWidget(homeBtn_);
        tb->addSpacing(8);
        tb->addWidget(addr_, 1);
        tb->addSpacing(8);
        tb->addWidget(dlBtn_);   // ← download button before full/menu
        tb->addWidget(full_);
        tb->addWidget(menuBtn_);

        bar_ = new LoadBar(this);

        stack_ = new QStackedWidget(this);
        home_ = new HomePage(stack_);
        stack_->addWidget(home_);

        root->addWidget(tabs_);
        root->addWidget(toolbar_);
        root->addWidget(bar_);
        root->addWidget(stack_, 1);

        tabs_->onCurrent = [this](int i) { cur_ = i; refresh(); };
        tabs_->onClose   = [this](int i) { closeTab(i); };
        tabs_->onNew     = [this] { newTab(); };
        tabs_->onMin       = [this] { showMinimized(); };
        tabs_->onCloseWin  = [this] { close(); };
        tabs_->onMaxToggle = [this] {
            if (isFullScreen()) return;
            if (isMaximized()) showNormal(); else showMaximized();
        };
        tabs_->onDrag = [this] {
            if (!isFullScreen() && windowHandle()) windowHandle()->startSystemMove();
        };

        home_->onNavigate = [this](const QString& s) { navigate(s); };
        home_->onContextMenu = [this](const QPoint& pos) {
            QMenu menu(this);
            ContextMenu::populateMenuForHost(&menu, this);
            if (!menu.isEmpty())
                menu.exec(home_->mapToGlobal(pos));
        };

        QObject::connect(back_,    &QAbstractButton::clicked, this, [this] { goBack(); });
        QObject::connect(fwd_,     &QAbstractButton::clicked, this, [this] { goForward(); });
        QObject::connect(reload_,  &QAbstractButton::clicked, this, [this] { reloadPage(); });
        QObject::connect(homeBtn_, &QAbstractButton::clicked, this, [this] { goHome(); });
        QObject::connect(full_,    &QAbstractButton::clicked, this, [this] { toggleFullscreen(); });
        QObject::connect(menuBtn_, &QAbstractButton::clicked, this, [this] { showMenu(); });
        QObject::connect(addr_, &QLineEdit::returnPressed, this,
                         [this] { navigate(addr_->text()); });

        // ── Live download-activity timer (500ms)
        auto* dlTimer = new QTimer(this);
        QObject::connect(dlTimer, &QTimer::timeout, this, [this]{
            const int active = DownloadManager::instance().activeCount();
            const int total  = DownloadManager::instance().count();

            if (total > 0) {
                dlBtn_->show();
                dlBtn_->setActive(active > 0);
            } else {
                dlBtn_->hide();
                dlBtn_->setActive(false);
            }

            if (active > 0)
                refreshDownloadsPage();

            if (dlPanel_ && dlPanel_->isVisible())
                dlPanel_->refresh();
        });
        dlTimer->start(500);

        auto sc = [this](const QKeySequence& k, std::function<void()> fn) {
            auto* s = new QShortcut(k, this);
            QObject::connect(s, &QShortcut::activated, this, [fn] { fn(); });
            return s;
        };
        sc(QKeySequence("Ctrl+T"),         [this] { newTab(); });
        sc(QKeySequence("Ctrl+W"),         [this] { closeTab(cur_); });
        sc(QKeySequence("Ctrl+L"),         [this] { focusAddress(); });
        sc(QKeySequence("F6"),             [this] { focusAddress(); });
        sc(QKeySequence("F11"),            [this] { toggleFullscreen(); });
        sc(QKeySequence("Alt+Left"),       [this] { goBack(); });
        sc(QKeySequence("Alt+Right"),      [this] { goForward(); });
        sc(QKeySequence("Alt+Home"),       [this] { goHome(); });
        sc(QKeySequence("F5"),             [this] { reloadPage(); });
        sc(QKeySequence("Ctrl+R"),         [this] { reloadPage(); });
        sc(QKeySequence("Ctrl+Tab"),       [this] { switchBy(1); });
        sc(QKeySequence("Ctrl+Shift+Tab"), [this] { switchBy(-1); });
        sc(QKeySequence("Ctrl+Q"),         [this] { close(); });
        sc(QKeySequence("Ctrl++"),         [this] { zoomBy(0.1); });
        sc(QKeySequence("Ctrl+="),         [this] { zoomBy(0.1); });
        sc(QKeySequence("Ctrl+-"),         [this] { zoomBy(-0.1); });
        sc(QKeySequence("Ctrl+0"),         [this] { zoomBy(0.0, true); });

        sc(QKeySequence("Ctrl+B"),         [this] { showBookmarksPage(); });
        sc(QKeySequence("Ctrl+D"),         [this] { bookmarkCurrentPage(); });
        sc(QKeySequence("Ctrl+J"),         [this] { showDownloadsPage(); });

        escSc_ = sc(QKeySequence("Esc"),   [this] { onEscape(); });
        escSc_->setEnabled(false);

        data_.push_back(TabData());
        tabs_->addTab("New Tab");
        cur_ = 0;
        refresh();
    }

    ~Browser() override {
        for (auto& t : data_) { delete t.view; t.view = nullptr; }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void openUrl(const QString& u) { navigate(u); }

    // ========================================================================
    //  Host
    // ========================================================================
    void openUrlInNewTab(const QUrl& u) override {
        TabData t;
        t.onHome = false;
        t.view = makeView();
        data_.push_back(t);
        tabs_->addTab(titleFor(u.toString()));
        cur_ = int(data_.size()) - 1;
        t.view->load(u);
        refresh();
        t.view->setFocus();
    }
    void openSearchInNewTab(const QString& q) override {
        const QString u = normalizeInput(q);
        if (u.isEmpty()) return;
        openUrlInNewTab(QUrl(u));
    }
    void openHtmlInNewTab(const QString& html, const QString& title) override {
        TabData t;
        t.onHome = false;
        t.view = makeView();
        data_.push_back(t);
        tabs_->addTab(title.isEmpty() ? "Page" : title);
        cur_ = int(data_.size()) - 1;
        t.view->setHtml(html);
        refresh();
        t.view->setFocus();
    }
    void saveCurrentPage(QWebEnginePage* p) override {
        ContextMenu::savePageAs(this, p);
        showNote("Saving page…");
    }
    void printCurrentPage(QWebEnginePage* p) override {
        ContextMenu::printPageToPdf(p);
        showNote("Exported to PDF in Downloads");
    }
    void viewPageSource(QWebEnginePage* p) override {
        ContextMenu::openPageSource(p);
    }
    void inspectPage(QWebEnginePage* p) override {
        ContextMenu::openDevTools(p);
    }
    void toggleFullscreen() override {
        if (webFull_) { onEscape(); return; }
        if (isFullScreen()) {
            if (wasMax_) showMaximized(); else showNormal();
        } else {
            wasMax_ = isMaximized();
            showFullScreen();
        }
    }

protected:
    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::WindowStateChange && full_ && escSc_ && root_) {
            full_->setKind(isFullScreen() ? Ic::Exit : Ic::Full);
            escSc_->setEnabled(isFullScreen());
            const int m = (isMaximized() || isFullScreen()) ? 0 : kEdge;
            root_->setContentsMargins(m, m, m, m);
            tabs_->setMaximized(isMaximized());
            update();
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0c0c0e"));
        if (!isMaximized() && !isFullScreen()) {
            p.setPen(QColor(255, 255, 255, 30));
            p.drawRect(rect().adjusted(kEdge - 1, kEdge - 1, -(kEdge - 1), -(kEdge - 1)));
        }
    }

    Qt::Edges edgesAt(const QPoint& p) const {
        Qt::Edges e;
        if (isMaximized() || isFullScreen()) return e;
        const bool L = p.x() < kEdge, R = p.x() >= width() - kEdge;
        const bool T = p.y() < kEdge, B = p.y() >= height() - kEdge;
        if (L) e |= Qt::LeftEdge;
        if (R) e |= Qt::RightEdge;
        if (T) e |= Qt::TopEdge;
        if (B) e |= Qt::BottomEdge;
        const int c = 14;
        if (L || R) { if (p.y() < c) e |= Qt::TopEdge; if (p.y() >= height() - c) e |= Qt::BottomEdge; }
        if (T || B) { if (p.x() < c) e |= Qt::LeftEdge; if (p.x() >= width() - c) e |= Qt::RightEdge; }
        return e;
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        const Qt::Edges e = edgesAt(ev->position().toPoint());
        const bool h = e.testFlag(Qt::LeftEdge) || e.testFlag(Qt::RightEdge);
        const bool v = e.testFlag(Qt::TopEdge) || e.testFlag(Qt::BottomEdge);
        if (h && v) {
            const bool diagF = (e.testFlag(Qt::LeftEdge) && e.testFlag(Qt::TopEdge)) ||
                               (e.testFlag(Qt::RightEdge) && e.testFlag(Qt::BottomEdge));
            setCursor(diagF ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        } else if (h) setCursor(Qt::SizeHorCursor);
        else if (v) setCursor(Qt::SizeVerCursor);
        else unsetCursor();
    }
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton && windowHandle()) {
            const Qt::Edges e = edgesAt(ev->position().toPoint());
            if (e.testFlag(Qt::LeftEdge) || e.testFlag(Qt::RightEdge) ||
                e.testFlag(Qt::TopEdge) || e.testFlag(Qt::BottomEdge)) {
                windowHandle()->startSystemResize(e);
                return;
            }
        }
        QWidget::mousePressEvent(ev);
    }
    void leaveEvent(QEvent* ev) override { unsetCursor(); QWidget::leaveEvent(ev); }

private:
    struct TabData {
        WebView* view = nullptr;
        bool onHome = true;
    };

    int indexOf(WebView* v) const {
        for (int i = 0; i < data_.size(); ++i) if (data_[i].view == v) return i;
        return -1;
    }

    static QString shown(const QUrl& u) {
        const QString s = u.toString();
        return s == "about:blank" ? QString() : s;
    }

    void installYouTubeAdNeutralizer() {
        QWebEngineScript s;
        s.setName(QStringLiteral("graphite-yt-ad-neutralizer"));
        s.setSourceCode(QString::fromUtf8(kYouTubeAdNeutralizer));
        s.setInjectionPoint(QWebEngineScript::DocumentCreation);
        s.setWorldId(QWebEngineScript::MainWorld);
        s.setRunsOnSubFrames(true);
        profile_->scripts()->insert(s);
    }

    WebView* makeView() {
        auto* v = new WebView(stack_);
        auto* pg = new WebPage(profile_, v);
        pg->setBackgroundColor(QColor("#0c0c0e"));
        pg->onNewWindow = [this]() -> QWebEnginePage* { return openBlankTab(); };

        pg->onSpecialScheme = [this](const QUrl& url) -> bool {
            const QString s = url.toString();

            if (s.startsWith("rootbrowser-goto:", Qt::CaseInsensitive)) {
                const QString target = s.mid(QStringLiteral("rootbrowser-goto:").length());
                TabData& t = data_[cur_];
                if (t.view) t.view->load(QUrl(target));
                return true;
            }
            if (s.startsWith("rootbrowser-bookmark-del:", Qt::CaseInsensitive)) {
                const QString target = s.mid(QStringLiteral("rootbrowser-bookmark-del:").length());
                BookmarkStore::instance().remove(target);
                const TabData& t = data_[cur_];
                if (t.view)
                    t.view->setHtml(BookmarkPage::build(),
                                    QUrl(QStringLiteral("rootbrowser://bookmarks")));
                return true;
            }

            if (s.startsWith("rootbrowser-dl-pause:", Qt::CaseInsensitive)) {
                DownloadManager::instance().pause(s.mid(21));
                refreshDownloadsPage();
                return true;
            }
            if (s.startsWith("rootbrowser-dl-resume:", Qt::CaseInsensitive)) {
                DownloadManager::instance().resume(s.mid(22));
                refreshDownloadsPage();
                return true;
            }
            if (s.startsWith("rootbrowser-dl-cancel:", Qt::CaseInsensitive)) {
                DownloadManager::instance().cancel(s.mid(22));
                refreshDownloadsPage();
                return true;
            }
            if (s.startsWith("rootbrowser-dl-remove:", Qt::CaseInsensitive)) {
                DownloadManager::instance().remove(s.mid(22));
                refreshDownloadsPage();
                return true;
            }
            if (s.startsWith("rootbrowser-dl-open:", Qt::CaseInsensitive)) {
                DownloadManager::instance().openFile(s.mid(20));
                return true;
            }
            if (s.startsWith("rootbrowser-dl-folder:", Qt::CaseInsensitive)) {
                const QString arg = s.mid(22);
                if (arg == "pick") {
                    const QString dir = QFileDialog::getExistingDirectory(
                        this, "Choose download folder",
                        DownloadManager::instance().defaultFolder());
                    if (!dir.isEmpty())
                        DownloadManager::instance().setDefaultFolder(dir);
                    refreshDownloadsPage();
                } else {
                    DownloadManager::instance().openFolder(arg);
                }
                return true;
            }
            if (s.startsWith("rootbrowser-dl-retry:", Qt::CaseInsensitive)) {
                DownloadManager::instance().retry(s.mid(21));
                refreshDownloadsPage();
                return true;
            }
            if (s.startsWith("rootbrowser-dl-clear:", Qt::CaseInsensitive)) {
                DownloadManager::instance().clearFinished();
                refreshDownloadsPage();
                return true;
            }

            return false;
        };

        v->setPage(pg);
        stack_->addWidget(v);

        v->onContextMenu = [this, v](const QPoint& pos) {
            auto* page = v->page();
            if (!page) return;
            QWebEngineContextMenuRequest* req = v->lastContextMenuRequest();
            QMenu menu(this);
            ContextMenu::populateMenu(&menu, page, req);
            if (!menu.isEmpty())
                menu.exec(v->mapToGlobal(pos));
        };

        QObject::connect(v, &QWebEngineView::titleChanged, this,
                         [this, v](const QString&) {
            const int i = indexOf(v); if (i >= 0) syncTitle(i);
        });
        QObject::connect(v, &QWebEngineView::urlChanged, this,
                         [this, v](const QUrl& u) {
            const int i = indexOf(v);
            if (i < 0) return;
            syncTitle(i);
            if (i == cur_) {
                if (!addr_->hasFocus() && !data_[i].onHome)
                    addr_->setText(shown(u));
                updateNav();
            }
        });
        QObject::connect(v, &QWebEngineView::iconChanged, this,
                         [this, v](const QIcon& ic) {
            const int i = indexOf(v);
            if (i >= 0) tabs_->setIcon(i, ic.isNull() ? QPixmap() : ic.pixmap(32, 32));
        });
        QObject::connect(v, &QWebEngineView::loadStarted, this, [this, v] {
            if (indexOf(v) == cur_) bar_->setValue(6);
        });
        QObject::connect(v, &QWebEngineView::loadProgress, this, [this, v](int p) {
            if (indexOf(v) == cur_) bar_->setValue(std::max(6, p));
        });
        QObject::connect(v, &QWebEngineView::loadFinished, this, [this, v](bool) {
            if (indexOf(v) == cur_) { bar_->setValue(100); updateNav(); }
        });
        QObject::connect(pg, &QWebEnginePage::fullScreenRequested, this,
                         [this](QWebEngineFullScreenRequest r) {
            r.accept();
            setWebFullscreen(r.toggleOn());
        });
        QObject::connect(pg, &QWebEnginePage::windowCloseRequested, this, [this, v] {
            const int i = indexOf(v); if (i >= 0) closeTab(i);
        });
        return v;
    }

    QWebEnginePage* openBlankTab() {
        TabData t;
        t.onHome = false;
        t.view = makeView();
        data_.push_back(t);
        tabs_->addTab("New Tab");
        cur_ = int(data_.size()) - 1;
        refresh();
        return t.view->page();
    }

    void syncTitle(int i) {
        const TabData& t = data_[i];
        QString title = "New Tab";
        if (!t.onHome && t.view) {
            title = t.view->title();
            if (title.isEmpty()) title = titleFor(t.view->url().toString());
        }
        tabs_->setTitle(i, title);
        if (i == cur_)
            setWindowTitle((t.onHome || !t.view) ? QString(kAppName)
                                                 : title + " \u2014 " + kAppName);
    }

    void updateNav() {
        const TabData& t = data_[cur_];
        bool b = false, f = false;
        if (t.view && !t.onHome) { b = true; f = t.view->history()->canGoForward(); }
        else if (t.view && t.onHome) { f = true; }
        back_->setEnabled(b);
        fwd_->setEnabled(f);
    }

    void refresh() {
        const TabData& t = data_[cur_];
        const bool page = (t.view && !t.onHome);
        stack_->setCurrentWidget(page ? static_cast<QWidget*>(t.view)
                                      : static_cast<QWidget*>(home_));
        addr_->setText(page ? shown(t.view->url()) : QString());
        tabs_->setCurrent(cur_);
        syncTitle(cur_);
        updateNav();
        bar_->setValue(0);
    }

    void showBookmarksPage() {
        TabData& t = data_[cur_];
        if (!t.view) t.view = makeView();
        t.onHome = false;
        t.view->setHtml(BookmarkPage::build(),
                        QUrl(QStringLiteral("rootbrowser://bookmarks")));
        home_->clearSearch();
        refresh();
        t.view->setFocus();
    }

    void bookmarkCurrentPage() {
        const TabData& t = data_[cur_];
        if (!t.view || t.onHome) return;
        const QString url = t.view->url().toString();
        const QString title = t.view->title();
        if (url.isEmpty()) return;
        BookmarkStore::instance().addOrUpdate(url, title);
        showNote("Bookmarked");
    }

    void showDownloadsPage() {
        TabData& t = data_[cur_];
        if (!t.view) t.view = makeView();
        t.onHome = false;
        t.view->setHtml(DownloadPage::build(),
                        QUrl(QStringLiteral("rootbrowser://downloads")));
        home_->clearSearch();
        refresh();
        t.view->setFocus();
    }

    void refreshDownloadsPage() {
        const TabData& t = data_[cur_];
        if (!t.view) return;
        const QString url = t.view->url().toString();
        if (url != "rootbrowser://downloads") return;
        t.view->setHtml(DownloadPage::build(),
                        QUrl(QStringLiteral("rootbrowser://downloads")));
    }

    void navigate(const QString& input) {
        const QString u = normalizeInput(input);
        if (u.isEmpty()) return;

        if (u == "rootbrowser://bookmarks" || u == "rootbrowser:bookmarks") {
            showBookmarksPage();
            return;
        }
        if (u == "rootbrowser://downloads" || u == "rootbrowser:downloads") {
            showDownloadsPage();
            return;
        }

        if (u.startsWith("rootbrowser-goto:", Qt::CaseInsensitive)) {
            const QString url = u.mid(QStringLiteral("rootbrowser-goto:").length());
            TabData& t = data_[cur_];
            if (!t.view) t.view = makeView();
            t.onHome = false;
            t.view->load(QUrl(url));
            refresh();
            return;
        }
        if (u.startsWith("rootbrowser-bookmark-del:", Qt::CaseInsensitive)) {
            const QString url = u.mid(QStringLiteral("rootbrowser-bookmark-del:").length());
            BookmarkStore::instance().remove(url);
            showBookmarksPage();
            return;
        }

        TabData& t = data_[cur_];
        if (!t.view) t.view = makeView();
        t.onHome = false;
        t.view->load(QUrl::fromUserInput(u));
        home_->clearSearch();
        refresh();
        t.view->setFocus();
    }
    void goBack() {
        TabData& t = data_[cur_];
        if (!t.view || t.onHome) return;
        if (t.view->history()->canGoBack()) t.view->back();
        else { t.onHome = true; refresh(); }
    }
    void goForward() {
        TabData& t = data_[cur_];
        if (!t.view) return;
        if (t.onHome) { t.onHome = false; refresh(); t.view->setFocus(); }
        else t.view->forward();
    }
    void goHome() {
        TabData& t = data_[cur_];
        if (t.onHome) { home_->focusSearch(); return; }
        t.onHome = true;
        refresh();
    }
    void reloadPage() {
        const TabData& t = data_[cur_];
        if (t.view && !t.onHome) t.view->reload();
        else refresh();
    }
    void zoomBy(double d, bool reset = false) {
        const TabData& t = data_[cur_];
        if (!t.view || t.onHome) return;
        const double z = reset ? 1.0 : std::clamp(t.view->zoomFactor() + d, 0.25, 5.0);
        t.view->setZoomFactor(z);
        showNote(QString("Zoom %1%").arg(int(std::lround(z * 100))));
    }

    void newTab() {
        data_.push_back(TabData());
        tabs_->addTab("New Tab");
        cur_ = int(data_.size()) - 1;
        refresh();
        focusAddress();
    }
    void closeTab(int i) {
        if (i < 0 || i >= data_.size()) return;
        if (data_.size() == 1) { close(); return; }
        WebView* v = data_[i].view;
        data_.removeAt(i);
        tabs_->removeTab(i);
        if (cur_ > i) cur_--;
        else if (cur_ == i) cur_ = std::min<int>(i, int(data_.size()) - 1);
        if (v) { stack_->removeWidget(v); v->hide(); v->deleteLater(); }
        refresh();
    }
    void switchBy(int d) {
        const int n = int(data_.size());
        cur_ = ((cur_ + d) % n + n) % n;
        refresh();
    }
    void focusAddress() { addr_->setFocus(); addr_->selectAll(); }

    void showNote(const QString& t) {
        tabs_->setNote(t);
        noteTimer_->start(3500);
    }

    void setWebFullscreen(bool on) {
        if (on == webFull_) return;
        webFull_ = on;
        tabs_->setVisible(!on);
        toolbar_->setVisible(!on);
        bar_->setVisible(!on);
        if (on) {
            prevFull_ = isFullScreen();
            if (!prevFull_) { wasMax_ = isMaximized(); showFullScreen(); }
        } else if (!prevFull_) {
            if (wasMax_) showMaximized(); else showNormal();
        }
    }
    void onEscape() {
        if (webFull_) {
            const TabData& t = data_[cur_];
            if (t.view) t.view->triggerPageAction(QWebEnginePage::ExitFullScreen);
            setWebFullscreen(false);
        } else if (isFullScreen()) {
            toggleFullscreen();
        }
    }

    void showMenu() {
        QMenu m(this);
        auto add = [&](const QString& text, std::function<void()> fn) {
            QAction* a = m.addAction(text);
            QObject::connect(a, &QAction::triggered, this, [fn] { fn(); });
        };
        add("New tab\tCtrl+T",       [this] { newTab(); });
        add("Close tab\tCtrl+W",     [this] { closeTab(cur_); });
        m.addSeparator();

        add("Bookmarks\tCtrl+B", [this] { showBookmarksPage(); });
        add("Bookmark this page\tCtrl+D", [this] { bookmarkCurrentPage(); });
        add("Downloads\tCtrl+J", [this] { showDownloadsPage(); });

        m.addSeparator();
        add("Home\tAlt+Home",        [this] { goHome(); });
        add("Reload\tF5",            [this] { reloadPage(); });
        add("Zoom in\tCtrl++",       [this] { zoomBy(0.1); });
        add("Zoom out\tCtrl+-",      [this] { zoomBy(-0.1); });
        add("Reset zoom\tCtrl+0",    [this] { zoomBy(0.0, true); });
        m.addSeparator();
        add(isFullScreen() ? "Exit full screen\tF11" : "Full screen\tF11",
            [this] { toggleFullscreen(); });
        m.addSeparator();
        add("Exit\tCtrl+Q",          [this] { close(); });
        QPoint pt = menuBtn_->mapToGlobal(QPoint(menuBtn_->width(), menuBtn_->height() + 6));
        pt.setX(pt.x() - m.sizeHint().width());
        m.exec(pt);
    }

    QWebEngineProfile* profile_ = nullptr;
    TabStrip* tabs_ = nullptr;
    Panel* toolbar_ = nullptr;
    LoadBar* bar_ = nullptr;
    IconButton *back_ = nullptr, *fwd_ = nullptr, *reload_ = nullptr,
               *homeBtn_ = nullptr, *full_ = nullptr, *menuBtn_ = nullptr;
    DownloadButton* dlBtn_ = nullptr;
    DownloadPanel*  dlPanel_ = nullptr;
    AddressEdit* addr_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    HomePage* home_ = nullptr;
    QShortcut* escSc_ = nullptr;
    QTimer* noteTimer_ = nullptr;
    QList<TabData> data_;
    int cur_ = 0;
    QVBoxLayout* root_ = nullptr;
    static constexpr int kEdge = 5;
    bool wasMax_ = false, webFull_ = false, prevFull_ = false;
};

// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName(kAppName);
    app.setStyle("Fusion");

    QFont f = app.font();
    f.setFamilies({"Segoe UI Variable Text", "Segoe UI", "Inter", "Ubuntu",
                   "Noto Sans", "Cantarell", "DejaVu Sans"});
    f.setStyleStrategy(QFont::PreferAntialias);
    app.setFont(f);

    QPalette pal;
    pal.setColor(QPalette::Window,          QColor("#0c0c0e"));
    pal.setColor(QPalette::WindowText,      QColor("#e6e8ec"));
    pal.setColor(QPalette::Base,            QColor("#121316"));
    pal.setColor(QPalette::AlternateBase,   QColor("#1c1d21"));
    pal.setColor(QPalette::Text,            QColor("#e6e8ec"));
    pal.setColor(QPalette::Button,          QColor("#1c1d21"));
    pal.setColor(QPalette::ButtonText,      QColor("#e6e8ec"));
    pal.setColor(QPalette::Highlight,       QColor("#59627a"));
    pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    pal.setColor(QPalette::ToolTipBase,     QColor("#1c1d21"));
    pal.setColor(QPalette::ToolTipText,     QColor("#dcdee4"));
    pal.setColor(QPalette::PlaceholderText, QColor(160, 166, 182, 130));
    app.setPalette(pal);

    app.setStyleSheet(
        "QToolTip{background:#1c1d21;color:#dcdee4;border:1px solid #2c2e34;padding:5px 8px;}"
        "QMenu{background:#1c1d21;color:#d7d9de;border:1px solid #2c2e34;padding:6px;border-radius:10px;}"
        "QMenu::item{padding:8px 30px 8px 34px;color:#d7d9de;border-radius:6px;}"
        "QMenu::item:selected{background:#2b2d34;color:#ffffff;}"
        "QMenu::item:disabled{color:#5c606b;}"
        "QMenu::separator{height:1px;background:#2c2e34;margin:6px 8px;}"
        "QMenu::right-arrow{width:8px;height:8px;margin-right:10px;}"
        "QMenu::indicator{width:0;}");

    Browser w;
    w.show();
    if (argc > 1) w.openUrl(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
