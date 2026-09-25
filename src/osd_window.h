#pragma once
#include <windows.h>

namespace Osd {

bool Init(HINSTANCE hInstance);
void Cleanup();

// Shows custom 2-line or 3-line notification with auto-sizing geometry
void Show(const WCHAR* line1, const WCHAR* line2, const WCHAR* line3 = nullptr);

// Formatted battery status notification
void ShowBattery(const WCHAR* modelName, int battery, bool isCharging, const WCHAR* modeName);

// Formatted notification when Battery Icon Style changes
void ShowStyleOsd(int style);

HWND GetWindowHandle();

} // namespace Osd
