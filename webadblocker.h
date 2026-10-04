// ============================================================================
//  webadblocker.h — Advanced ad & tracker blocker for QtWebEngine.
// ============================================================================
#pragma once

#include <QObject>
#include <QWebEngineProfile>

namespace WebAdBlocker {

void attach(QWebEngineProfile* profile);
int loadFilterList(const QString& filePath);
quint64 totalBlocked();
void setEnabled(bool on);
bool isEnabled();

} // namespace WebAdBlocker
