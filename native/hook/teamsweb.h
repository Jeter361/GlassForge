#pragma once

// WebView2 apps with "Glass inside the app" (Teams, the new Outlook): from DllMain. Every web view the app creates
// gets a transparent default background and a script that leaves its big dark panels clear; see teamsweb.cpp.
void StartWebViewGlass();
