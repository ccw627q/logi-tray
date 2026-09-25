#pragma once
#include <windows.h>
#include <shellapi.h>
#include "device_manager.h"

namespace Tray {

// Posted to the main window when the tray icon/tooltip must be rebuilt.
constexpr UINT WM_APP_STATE_UPDATE = WM_APP + 2;

// Device rows in the tray menu use DEVICE_CMD_BASE + device index.
constexpr int DEVICE_CMD_BASE = 2000;
constexpr int MAX_MENU_DEVICES = 8;

// Dialog resource id for the custom device manager.
constexpr int IDD_DEVICE_MANAGER = 200;

void InitTheme();
void ShowMenu(HWND hWndOwner);
void ShowDeviceDialog(HWND hWndOwner);
void DismissAllMenus();

// Renders the rows the context menu would show as plain text (newline
// separated). Used by --selftest to check multi-device handling headlessly.
int GetMenuRowsForDiagnostics(WCHAR* out, int outChars);

HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark);
void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& active);

int CycleBatteryStyle();
int GetBatteryStyle();

bool IsAutoRunEnabled();
void ToggleAutoRun();

} // namespace Tray