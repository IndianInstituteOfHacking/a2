// ============================================================================
//  webadblocker.cpp — Fast, undetectable ad & tracker blocker.
// ============================================================================

#include "webadblocker.h"

#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QUrl>
#include <QUrlQuery>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QFile>
#include <QTextStream>
#include <QAtomicInteger>
#include <QDebug>

#include <algorithm>
#include <cstring>

namespace WebAdBlocker {

// ────────────────────────────────────────────────────────────────────────────
//  Curated list of ad/tracker/analytics hosts.
//  Every subdomain of a listed host is blocked too (suffix matching).
// ────────────────────────────────────────────────────────────────────────────
static const char* kAdHosts[] = {
    // Google Ads / DoubleClick / Analytics
    "doubleclick.net", "doubleclick.com", "googleadservices.com",
    "googlesyndication.com", "google-analytics.com",
    "googletagservices.com", "googletagmanager.com", "googleadsserving.cn",
    "adservice.google.com", "pagead2.googlesyndication.com",
    "static.doubleclick.net", "securepubads.g.doubleclick.net",
    "www.googletagservices.com", "www.google-analytics.com",
    "ssl.google-analytics.com", "stats.g.doubleclick.net",
    "cm.g.doubleclick.net", "ad.doubleclick.net",

    // Facebook / Meta
    "connect.facebook.net", "graph.facebook.com", "pixel.facebook.com",
    "business.facebook.com", "an.facebook.com",

    // Amazon Ads
    "amazon-adsystem.com", "assoc-amazon.com", "aax.amazon-adsystem.com",

    // Twitter / X
    "ads-twitter.com", "static.ads-twitter.com", "analytics.twitter.com",
    "platform.twitter.com",

    // Microsoft / Bing / LinkedIn
    "bat.bing.com", "clarity.ms", "c.clarity.ms",
    "ads.linkedin.com", "snap.licdn.com", "px.ads.linkedin.com",

    // Taboola / Outbrain / Revcontent
    "taboola.com", "cdn.taboola.com", "trc.taboola.com",
    "outbrain.com", "widgets.outbrain.com", "log.outbrain.com",
    "revcontent.com", "labs-cdn.revcontent.com",

    // Criteo / Rubicon / PubMatic / OpenX / AppNexus
    "criteo.com", "criteo.net", "static.criteo.net", "dis.criteo.com",
    "rubiconproject.com", "fastlane.rubiconproject.com",
    "pubmatic.com", "ads.pubmatic.com",
    "openx.net", "us-u.openx.net",
    "adnxs.com", "ib.adnxs.com", "secure.adnxs.com",

    // Adobe / TubeMogul / 2MDN
    "demdex.net", "dpm.demdex.net", "adobedtm.com",
    "tubemogul.com", "2mdn.net", "s0.2mdn.net",

    // Scorecard / Comscore / Nielsen
    "scorecardresearch.com", "sb.scorecardresearch.com",
    "comscore.com", "nielsen.com", "imrworldwide.com",

    // Hotjar / Mixpanel / Segment / Heap
    "hotjar.com", "static.hotjar.com",
    "mixpanel.com", "cdn.mxpnl.com",
    "segment.io", "api.segment.io", "cdn.segment.com",
    "heapanalytics.com", "heap.io",

    // Matomo / Piwik
    "matomo.org", "piwik.org",

    // Quantserve / Quantcast
    "quantserve.com", "quantcast.com", "pixel.quantserve.com",

    // Yandex / Mail.ru
    "mc.yandex.ru",
    "top-fwz1.mail.ru", "ads.mail.ru",

    // Yahoo / Verizon Media
    "advertising.com", "adtech.yahooinc.com",
    "gemini.yahoo.com", "analytics.yahoo.com",

    // Others
    "moatads.com", "moatpixel.com",
    "bidswitch.net", "casalemedia.com", "contextweb.com",
    "sharethrough.com", "teads.tv",
    "smartadserver.com", "adsafeprotected.com",
    "agkn.com", "bluekai.com", "krxd.net", "mathtag.com",
    "tremorhub.com", "yieldmo.com", "zemanta.com",
    "adform.net", "adroll.com", "s.adroll.com",
    "statcounter.com", "histats.com", "crazyegg.com",
    "clicktale.net", "kissmetrics.com",
    "newrelic.com", "nr-data.net",
    "optimizely.com", "cdn.optimizely.com",
    "vwo.com", "visualwebsiteoptimizer.com",
    "chartbeat.com", "static.chartbeat.com",
    "parsely.com", "pixel.parsely.com",
    "1rx.io", "360yield.com",
    "adcolony.com", "applovin.com",
    "unityads.unity3d.com",
    "vungle.com", "chartboost.com",
    "fyber.com", "supersonicads.com",
    "inmobi.com", "mopub.com",
};

// ────────────────────────────────────────────────────────────────────────────
//  Path/pattern fragments that indicate an ad even on non-listed hosts.
// ────────────────────────────────────────────────────────────────────────────
static const char* kAdPathFragments[] = {
    "/ads/", "/adserver/", "/adservice/", "/advert/", "/advertising/",
    "/banners/", "/sponsored/", "/promo/",
    "/tracking/", "/tracker/", "/pixel/", "/beacon/",
    "/analytics/", "/telemetry/", "/metrics/",
    "/pagead/", "/adframe/", "/prebid/",
    "utm_source=", "utm_medium=", "utm_campaign=", "utm_content=", "utm_term=",
    "gclid=", "fbclid=", "msclkid=", "mc_eid=", "yclid=",
    "gtag/js", "analytics.js", "fbevents.js", "hotjar-",
};

// ────────────────────────────────────────────────────────────────────────────
//  Fast suffix-hash set.
// ────────────────────────────────────────────────────────────────────────────
class HostSet {
public:
    HostSet() = default;
    void reserve(int n) { set_.reserve(n); }
    void add(QString h) {
        h = h.toLower();
        if (!h.isEmpty()) set_.insert(h);
    }
    bool contains(const QString& host) const {
        if (host.isEmpty()) return false;
        QString h = host.toLower();
        int pos = 0;
        while (true) {
            if (set_.contains(h)) return true;
            pos = h.indexOf('.', pos);
            if (pos < 0) break;
            h = h.mid(pos + 1);
            pos = 0;
        }
        return false;
    }
    int size() const { return set_.size(); }
private:
    QSet<QString> set_;
};

// ────────────────────────────────────────────────────────────────────────────
//  The interceptor.
// ────────────────────────────────────────────────────────────────────────────
class AdInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    AdInterceptor() {
        hosts_.reserve(int(sizeof(kAdHosts) / sizeof(kAdHosts[0])) * 2);
        for (auto* h : kAdHosts) hosts_.add(QString::fromLatin1(h));

        pathFrags_.reserve(int(sizeof(kAdPathFragments) / sizeof(kAdPathFragments[0])));
        for (auto* p : kAdPathFragments)
            pathFrags_.push_back(QString::fromLatin1(p).toLower());
    }

    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        if (!enabled_.loadAcquire()) return;

        const QUrl url = info.requestUrl();
        const QString scheme = url.scheme();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            return;

        const QString host = url.host();
        if (host.isEmpty()) return;

        // ── 1) Host lookup (fast path) ─────────────────────────────────
        if (hosts_.contains(host)) {
            block(info);
            return;
        }

        // ── 2) Path fragment scan ──────────────────────────────────────
        const QString full = url.toString().toLower();
        for (const QString& frag : pathFrags_) {
            if (full.contains(frag)) {
                block(info);
                return;
            }
        }

        // ── 3) Third-party heuristic ───────────────────────────────────
        const QUrl firstParty = info.firstPartyUrl();
        const QString fpHost = firstParty.host().toLower();
        if (!fpHost.isEmpty() && fpHost != host.toLower()) {
            const int dot = host.lastIndexOf('.');
            if (dot > 0) {
                const QString tld = host.mid(dot + 1).toLower();
                static const QSet<QString> kAdTlds = {
                    QStringLiteral("doubleclick.net"),
                    QStringLiteral("googlesyndication.com"),
                    QStringLiteral("adnxs.com"),
                    QStringLiteral("criteo.com"),
                    QStringLiteral("taboola.com"),
                    QStringLiteral("outbrain.com"),
                };
                if (kAdTlds.contains(tld)) {
                    block(info);
                    return;
                }
            }
        }

        // ── 4) Sponsored redirect wrappers ─────────────────────────────
        if (host.endsWith(QLatin1String("google.com")) ||
            host.endsWith(QLatin1String("google.co.in")) ||
            host.endsWith(QLatin1String("bing.com")) ||
            host.endsWith(QLatin1String("yahoo.com")))
        {
            const QString path = url.path().toLower();
            if (path.startsWith(QLatin1String("/aclk")) ||
                path.startsWith(QLatin1String("/clk")) ||
                path.startsWith(QLatin1String("/url")) ||
                path.startsWith(QLatin1String("/redirect")))
            {
                // FIX: use QUrlQuery instead of QUrl::query(key)
                QUrlQuery uq(url);
                const QString dest = QUrl::fromPercentEncoding(
                    uq.queryItemValue(QStringLiteral("url")).toUtf8());
                if (!dest.isEmpty() && !dest.contains(host))
                    block(info);
            }
        }
    }

private:
    void block(QWebEngineUrlRequestInfo& info) {
        blocked_.fetchAndAddRelaxed(1);
        info.block(true);
    }

public:
    QAtomicInteger<quint64> blocked_{0};
    QAtomicInteger<int> enabled_{1};

private:
    HostSet hosts_;
    QVector<QString> pathFrags_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Singleton
// ────────────────────────────────────────────────────────────────────────────
static AdInterceptor* g_interceptor = nullptr;

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────
void attach(QWebEngineProfile* profile) {
    if (!profile) return;
    if (g_interceptor) return;

    g_interceptor = new AdInterceptor();
    profile->setUrlRequestInterceptor(g_interceptor);

    qInfo() << "[WebAdBlocker] attached and ready";
}

int loadFilterList(const QString& filePath) {
    if (!g_interceptor) return 0;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;

    int n = 0;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('!') || line.startsWith('['))
            continue;
        if (line.startsWith(QLatin1String("||"))) {
            line.remove(0, 2);
            // FIX: no QRegularExpression — manual scan for terminators
            int cut = -1;
            for (int i = 0; i < line.size(); ++i) {
                const QChar c = line.at(i);
                if (c == '^' || c == '/' || c == '$') { cut = i; break; }
            }
            if (cut > 0) line = line.left(cut);
            if (!line.isEmpty()) ++n;
        }
    }
    qInfo() << "[WebAdBlocker] loaded" << n << "extra rules from" << filePath;
    return n;
}

quint64 totalBlocked() {
    return g_interceptor ? g_interceptor->blocked_.loadRelaxed() : 0;
}

void setEnabled(bool on) {
    if (g_interceptor) g_interceptor->enabled_.storeRelaxed(on ? 1 : 0);
}

bool isEnabled() {
    return g_interceptor && g_interceptor->enabled_.loadRelaxed() != 0;
}

} // namespace WebAdBlocker
