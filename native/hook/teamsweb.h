#pragma once

// Teams with "Glass inside the app": from DllMain in ms-teams.exe. Every web view Teams creates gets a transparent
// default background and a script that leaves its big dark panels clear; see teamsweb.cpp.
void StartTeamsWebGlass();
