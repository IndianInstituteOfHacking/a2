// ============================================================================
//  viewpagesource.h — standalone "View page source" viewer.
//  Self-contained. main.cpp ko sirf ek line add karni hai.
// ============================================================================
#pragma once

#include <QString>
#include <QUrl>

class QWebEnginePage;
class Browser;  // forward decl (optional)

namespace ViewPageSource {

// Callback type: gives main.cpp the fully-prepared HTML string to open in a
// new tab. main.cpp just calls tab->setHtml(html).
using OpenInNewTabFn = std::function<void(const QString& html, const QString& title)>;

// Fetch page->toHtml() and prepare a syntax-highlighted dark-theme viewer.
// `openInNewTab` will be called with the final HTML when ready.
void show(QWebEnginePage* page, OpenInNewTabFn openInNewTab);

} // namespace ViewPageSource
