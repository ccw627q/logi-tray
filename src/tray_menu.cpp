#include "tray_menu.h"
#include "osd_window.h"
#include <dwmapi.h>
#include <strsafe.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace Tray {

static const WCHAR* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* APP_NAME = L"logi-tray";
static const WCHAR* REG_SUBKEY = L"Software\\logi-tray";

static HWND g_hParentAppWnd = NULL;
static HWND g_hAcrylicMenu = NULL;
static HHOOK g_hMenuMouseHook = NULL;
static bool g_bModalLoop = false;

static int g_curDpi = 96;
static bool g_curDark = false;
static int g_mainHover = -1;

static inline int S(int val) {
    return MulDiv(val, g_curDpi, 96);
}

// Windows 11 / 10 Acrylic & Theme Hooks
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
typedef BOOL (WINAPI *pfnShouldAppsUseDarkMode)();
typedef BOOL (WINAPI *pfnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *pfnFlushMenuThemes)();

static pfnSetWindowCompositionAttribute fnSetWindowCompositionAttribute = NULL;
static pfnShouldAppsUseDarkMode fnShouldAppsUseDarkMode = NULL;
static pfnAllowDarkModeForWindow fnAllowDarkModeForWindow = NULL;
static pfnFlushMenuThemes fnFlushMenuThemes = NULL;

void InitTheme() {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        fnSetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    }
    HMODULE hUx = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUx) {
        fnShouldAppsUseDarkMode = (pfnShouldAppsUseDarkMode)GetProcAddress(hUx, MAKEINTRESOURCEA(132));
        fnAllowDarkModeForWindow = (pfnAllowDarkModeForWindow)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
        fnFlushMenuThemes = (pfnFlushMenuThemes)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    }
}

static bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD), type = 0;
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (val == 0);
        }
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (val == 0);
        }
        RegCloseKey(hKey);
    }
    if (fnShouldAppsUseDarkMode) return fnShouldAppsUseDarkMode();
    return false;
}

static void ApplyModernWindowStyle(HWND hWnd, bool isDark, int w = 0, int h = 0) {
    if (fnAllowDarkModeForWindow) {
        fnAllowDarkModeForWindow(hWnd, isDark ? TRUE : FALSE);
    }
    BOOL darkVal = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, 20, &darkVal, sizeof(darkVal)); // DWMWA_USE_IMMERSIVE_DARK_MODE

    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    RECT rc = {0};
    GetWindowRect(hWnd, &rc);
    int width = (w > 0) ? w : (rc.right - rc.left);
    int height = (h > 0) ? h : (rc.bottom - rc.top);
    if (width > 0 && height > 0) {
        HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, S(12), S(12));
        SetWindowRgn(hWnd, hRgn, TRUE);
    }

    if (fnSetWindowCompositionAttribute) {
        ACCENT_POLICY policy = {};
        if (isDark) {
            policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
            policy.AccentFlags = 0;
            policy.GradientColor = 0xCC1A1B20; // AABBGGRR
        } else {
            policy.AccentState = ACCENT_DISABLED;
            policy.AccentFlags = 0;
            policy.GradientColor = 0;
        }
        WINDOWCOMPOSITIONATTRIBDATA data = { 19, &policy, sizeof(policy) };
        fnSetWindowCompositionAttribute(hWnd, &data);
    }
}

static int g_batteryStyle = -1;

int GetBatteryStyle() {
    if (g_batteryStyle == -1) {
        HKEY hKey;
        DWORD val = 0, size = sizeof(DWORD), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExW(hKey, L"BatteryStyle", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
                g_batteryStyle = (int)val % 3;
            }
            RegCloseKey(hKey);
        }
        if (g_batteryStyle == -1) g_batteryStyle = 0;
    }
    return g_batteryStyle;
}

int CycleBatteryStyle() {
    g_batteryStyle = (GetBatteryStyle() + 1) % 3;
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)g_batteryStyle;
        RegSetValueExW(hKey, L"BatteryStyle", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
    return g_batteryStyle;
}

bool IsAutoRunEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path), type = 0;
        LSTATUS st = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS);
    }
    return false;
}

void ToggleAutoRun() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (IsAutoRunEnabled()) {
            RegDeleteValueW(hKey, APP_NAME);
        } else {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
        }
        RegCloseKey(hKey);
    }
}

void DismissAllMenus() {
    if (g_hMenuMouseHook) {
        UnhookWindowsHookEx(g_hMenuMouseHook);
        g_hMenuMouseHook = NULL;
    }
    if (g_hAcrylicMenu) {
        HWND h = g_hAcrylicMenu;
        g_hAcrylicMenu = NULL;
        DestroyWindow(h);
    }
    g_bModalLoop = false;
    if (g_hParentAppWnd) {
        PostMessageW(g_hParentAppWnd, WM_NULL, 0, 0);
    }
}

static LRESULT CALLBACK MenuMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
            wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN) {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            POINT pt = p->pt;
            RECT rM = {0};
            if (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) GetWindowRect(g_hAcrylicMenu, &rM);
            bool inM = (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) && PtInRect(&rM, pt);
            if (!inM) {
                DismissAllMenus();
            }
        }
    }
    return CallNextHookEx(g_hMenuMouseHook, nCode, wParam, lParam);
}

// --------------------------------------------------------------------------
// Main Acrylic Context Menu
// --------------------------------------------------------------------------

struct MenuItemData {
    int id;
    WCHAR label[64];
    WCHAR value[40];
    bool isHeader;
    bool isSeparator;
    bool isInteractive;
    bool isDisabled;
    int valueColor;   // 0 = default, 1 = accent/ok, 2 = warn
};

static std::vector<MenuItemData> g_mainItems;
static bool g_menuAnyOnline = false;

static int GetItemH(int idx) {
    if (idx < 0 || idx >= (int)g_mainItems.size()) return 0;
    if (g_mainItems[idx].isHeader) return S(52);
    if (g_mainItems[idx].isSeparator) return S(9);
    return S(32);
}

static int HitTestMain(int my) {
    int y = S(8);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        int h = GetItemH((int)i);
        if (my >= y && my < y + h) {
            if (g_mainItems[i].isHeader || g_mainItems[i].isSeparator || g_mainItems[i].isDisabled) return -1;
            return (int)i;
        }
        y += h;
    }
    return -1;
}

static LRESULT CALLBACK AcrylicMainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND hOther = (HWND)lParam;
                if (hOther != g_hAcrylicMenu) {
                    DismissAllMenus();
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            int my = HIWORD(lParam);
            int newH = HitTestMain(my);
            if (newH != g_mainHover) {
                g_mainHover = newH;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (g_mainHover != -1) {
                g_mainHover = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int idx = HitTestMain(HIWORD(lParam));
            if (idx >= 0 && g_mainItems[idx].isInteractive && !g_mainItems[idx].isDisabled) {
                int cmd = g_mainItems[idx].id;
                HWND hOwner = g_hParentAppWnd;
                DismissAllMenus();
                if (cmd == 1001) { // Toggle Auto Run
                    ToggleAutoRun();
                } else if (cmd == 1002) { // Exit
                    PostQuitMessage(0);
                } else if (cmd == 1003) { // Refresh now
                    Device::RequestRefresh();
                } else if (cmd == 1004) { // Manage custom devices
                    ShowDeviceDialog(hOwner);
                } else if (cmd >= DEVICE_CMD_BASE && cmd < DEVICE_CMD_BASE + MAX_MENU_DEVICES) {
                    Device::State st = Device::GetStateByIndex(cmd - DEVICE_CMD_BASE);
                    if (st.id[0]) {
                        Device::SetActiveDeviceId(st.id);
                        if (hOwner) PostMessageW(hOwner, WM_APP_STATE_UPDATE, 0, 0);
                    }
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBm = (HBITMAP)SelectObject(memDC, hbm);

            COLORREF bgCol = g_curDark ? RGB(24, 26, 32) : RGB(255, 255, 255);
            COLORREF borderCol = g_curDark ? RGB(50, 54, 65) : RGB(205, 212, 222);
            COLORREF hoverCol = g_curDark ? RGB(52, 58, 72) : RGB(232, 238, 248);
            COLORREF textCol = g_curDark ? RGB(235, 240, 248) : RGB(20, 24, 32);
            COLORREF mutedCol = g_curDark ? RGB(155, 165, 180) : RGB(105, 115, 130);
            COLORREF greenCol = g_curDark ? RGB(34, 197, 94) : RGB(16, 145, 60);
            COLORREF sepCol = g_curDark ? RGB(40, 44, 54) : RGB(225, 228, 236);
            COLORREF checkCol = g_curDark ? RGB(96, 205, 255) : RGB(0, 110, 215);
            COLORREF warnCol = g_curDark ? RGB(248, 113, 113) : RGB(198, 40, 40);

            HBRUSH bgBrush = CreateSolidBrush(bgCol);
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, 1, borderCol);
            HGDIOBJ oldPen = SelectObject(memDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            RoundRect(memDC, 0, 0, rc.right, rc.bottom, S(12), S(12));
            SelectObject(memDC, oldPen);
            DeleteObject(borderPen);

            HFONT hFontNormal = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontBold = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontSub = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            SetBkMode(memDC, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(memDC, hFontNormal);

            int y = S(8);
            for (size_t i = 0; i < g_mainItems.size(); i++) {
                int h = GetItemH((int)i);
                if (g_mainItems[i].isHeader) {
                    SelectObject(memDC, hFontBold);
                    SetTextColor(memDC, textCol);
                    RECT rTitle = { S(16), y + S(4), rc.right - S(16), y + S(26) };
                    DrawTextW(memDC, g_mainItems[i].label, -1, &rTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, hFontSub);
                    SetTextColor(memDC, g_menuAnyOnline ? greenCol : mutedCol);
                    RECT rSub = { S(16), y + S(26), rc.right - S(16), y + S(46) };
                    DrawTextW(memDC, g_mainItems[i].value, -1, &rSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    y += h;
                } else if (g_mainItems[i].isSeparator) {
                    HPEN pSep = CreatePen(PS_SOLID, 1, sepCol);
                    HGDIOBJ oP = SelectObject(memDC, pSep);
                    MoveToEx(memDC, S(12), y + S(4), NULL);
                    LineTo(memDC, rc.right - S(12), y + S(4));
                    SelectObject(memDC, oP);
                    DeleteObject(pSep);
                    y += h;
                } else {
                    RECT rItem = { S(6), y, rc.right - S(6), y + h };
                    if ((int)i == g_mainHover && g_mainItems[i].isInteractive && !g_mainItems[i].isDisabled) {
                        HBRUSH hH = CreateSolidBrush(hoverCol);
                        HPEN hP = CreatePen(PS_SOLID, 1, hoverCol);
                        HGDIOBJ oP = SelectObject(memDC, hP);
                        HGDIOBJ oB = SelectObject(memDC, hH);
                        RoundRect(memDC, rItem.left, rItem.top + 1, rItem.right, rItem.bottom - 1, S(6), S(6));
                        SelectObject(memDC, oP);
                        SelectObject(memDC, oB);
                        DeleteObject(hP);
                        DeleteObject(hH);
                    }

                    SelectObject(memDC, hFontNormal);
                    SetTextColor(memDC, g_mainItems[i].isDisabled ? mutedCol : textCol);
                    RECT rLabel = { S(14), y, rc.right - S(90), y + h };
                    DrawTextW(memDC, g_mainItems[i].label, -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (g_mainItems[i].value[0]) {
                        COLORREF vc = mutedCol;
                        if (g_mainItems[i].valueColor == 1) vc = greenCol;
                        else if (g_mainItems[i].valueColor == 2) vc = warnCol;
                        else if (wcscmp(g_mainItems[i].value, L"✓ 已开启") == 0) vc = checkCol;
                        SetTextColor(memDC, vc);
                        RECT rVal = { rc.right - S(115), y, rc.right - S(14), y + h };
                        DrawTextW(memDC, g_mainItems[i].value, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += h;
                }
            }

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBrush);
            DeleteObject(hFontNormal);
            DeleteObject(hFontBold);
            DeleteObject(hFontSub);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBm);
            DeleteObject(hbm);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Build the menu item model from the current device snapshot. Kept separate
// from ShowMenu so the exact rows the menu would render can also be inspected
// without creating a window (see GetMenuRowsForDiagnostics).
static void BuildMainItems() {
    Device::State devices[Device::MAX_DEVICES];
    int devCount = Device::GetDevices(devices, Device::MAX_DEVICES);
    WCHAR activeId[96];
    StringCchCopyW(activeId, ARRAYSIZE(activeId), Device::GetActiveDeviceId());

    int onlineCount = 0;
    for (int i = 0; i < devCount; ++i) {
        if (devices[i].isConnected) onlineCount++;
    }
    g_menuAnyOnline = (onlineCount > 0);

    g_mainItems.clear();

    MenuItemData itemHeader = { 0 };
    itemHeader.isHeader = true;
    StringCchCopyW(itemHeader.label, ARRAYSIZE(itemHeader.label), L"设备管理");
    if (devCount == 0) {
        StringCchCopyW(itemHeader.value, ARRAYSIZE(itemHeader.value), L"未检测到设备");
    } else {
        StringCchPrintfW(itemHeader.value, ARRAYSIZE(itemHeader.value),
                         L"%d 个设备 · %d 个在线", devCount, onlineCount);
    }
    g_mainItems.push_back(itemHeader);

    MenuItemData sep0 = { 0 };
    sep0.isSeparator = true;
    g_mainItems.push_back(sep0);

    int shown = (devCount < MAX_MENU_DEVICES) ? devCount : MAX_MENU_DEVICES;
    for (int i = 0; i < shown; ++i) {
        const Device::State& d = devices[i];
        MenuItemData row = { 0 };
        row.id = DEVICE_CMD_BASE + i;
        row.isInteractive = true;
        bool isActive = (activeId[0] && wcscmp(activeId, d.id) == 0);
        if (isActive) {
            StringCchPrintfW(row.label, ARRAYSIZE(row.label), L"● %s", d.modelName[0] ? d.modelName : L"未知设备");
        } else {
            StringCchPrintfW(row.label, ARRAYSIZE(row.label), L"    %s", d.modelName[0] ? d.modelName : L"未知设备");
        }

        if (!d.isConnected) {
            StringCchCopyW(row.value, ARRAYSIZE(row.value), L"未连接");
        } else if (!d.batteryReadable) {
            StringCchCopyW(row.value, ARRAYSIZE(row.value), L"不可读");
        } else if (d.battery < 0) {
            StringCchCopyW(row.value, ARRAYSIZE(row.value), L"未知");
        } else if (d.isCharging) {
            StringCchPrintfW(row.value, ARRAYSIZE(row.value), L"%d%% 充电中", d.battery);
            row.valueColor = 1;
        } else {
            StringCchPrintfW(row.value, ARRAYSIZE(row.value), L"%d%%", d.battery);
            row.valueColor = (d.battery <= 20) ? 2 : 0;
        }
        g_mainItems.push_back(row);
    }

    if (devCount > shown) {
        MenuItemData more = { 0 };
        StringCchPrintfW(more.label, ARRAYSIZE(more.label), L"    还有 %d 个设备…", devCount - shown);
        more.isDisabled = true;
        g_mainItems.push_back(more);
    }

    MenuItemData sep1 = { 0 };
    sep1.isSeparator = true;
    g_mainItems.push_back(sep1);

    MenuItemData itemRefresh = { 1003 };
    StringCchCopyW(itemRefresh.label, ARRAYSIZE(itemRefresh.label), L"立即刷新");
    itemRefresh.isInteractive = true;
    g_mainItems.push_back(itemRefresh);

    MenuItemData itemAdd = { 1004 };
    StringCchCopyW(itemAdd.label, ARRAYSIZE(itemAdd.label), L"添加 / 管理设备");
    itemAdd.isInteractive = true;
    g_mainItems.push_back(itemAdd);

    MenuItemData itemAuto = { 1001 };
    StringCchCopyW(itemAuto.label, ARRAYSIZE(itemAuto.label), L"开机自启动");
    itemAuto.isInteractive = true;
    StringCchCopyW(itemAuto.value, ARRAYSIZE(itemAuto.value), IsAutoRunEnabled() ? L"✓ 已开启" : L"未开启");
    g_mainItems.push_back(itemAuto);

    MenuItemData sep2 = { 0 };
    sep2.isSeparator = true;
    g_mainItems.push_back(sep2);

    MenuItemData itemExit = { 1002 };
    StringCchCopyW(itemExit.label, ARRAYSIZE(itemExit.label), L"退出程序");
    itemExit.isInteractive = true;
    g_mainItems.push_back(itemExit);
}

// Renders the rows the context menu would show, as plain text. Used by
// --selftest to verify multi-device handling without opening the menu.
int GetMenuRowsForDiagnostics(WCHAR* out, int outChars) {
    if (!out || outChars <= 0) return 0;
    out[0] = 0;

    BuildMainItems();

    int rows = 0;
    for (size_t i = 0; i < g_mainItems.size(); ++i) {
        const MenuItemData& it = g_mainItems[i];
        WCHAR line[192] = {0};
        if (it.isSeparator) {
            StringCchCopyW(line, ARRAYSIZE(line), L"  ---");
        } else if (it.isHeader) {
            StringCchPrintfW(line, ARRAYSIZE(line), L"  [%s]  %s", it.label, it.value);
        } else {
            StringCchPrintfW(line, ARRAYSIZE(line), L"  %s   %s", it.label, it.value);
        }
        if (rows > 0) StringCchCatW(out, outChars, L"\n");
        StringCchCatW(out, outChars, line);
        rows++;
    }
    return rows;
}

void ShowMenu(HWND hWndOwner) {
    if (g_bModalLoop) return;
    g_hParentAppWnd = hWndOwner;
    g_curDark = IsSystemDarkMode();

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
        pfnGetDpiForWindow fnGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (fnGetDpi && hWndOwner) g_curDpi = fnGetDpi(hWndOwner);
    }
    if (g_curDpi == 0) g_curDpi = 96;

    BuildMainItems();

    int totalH = S(16);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        totalH += GetItemH((int)i);
    }
    int menuW = S(236);

    int posX = pt.x - S(10);
    int posY = pt.y - totalH - S(10);
    if (posX + menuW > mi.rcWork.right) posX = mi.rcWork.right - menuW - S(8);
    if (posX < mi.rcWork.left) posX = mi.rcWork.left + S(8);
    if (posY < mi.rcWork.top) posY = pt.y + S(10);
    if (posY + totalH > mi.rcWork.bottom) posY = mi.rcWork.bottom - totalH - S(8);

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hWndOwner, GWLP_HINSTANCE);
    static bool s_mainRegistered = false;
    if (!s_mainRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AcrylicMainWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"LogiModernMainMenuWnd";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
        s_mainRegistered = true;
    }

    g_mainHover = -1;

    g_hAcrylicMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"LogiModernMainMenuWnd",
        L"",
        WS_POPUP,
        posX, posY, menuW, totalH,
        hWndOwner, NULL, hInst, NULL
    );

    ApplyModernWindowStyle(g_hAcrylicMenu, g_curDark, menuW, totalH);
    SetForegroundWindow(g_hAcrylicMenu);
    ShowWindow(g_hAcrylicMenu, SW_SHOW);
    UpdateWindow(g_hAcrylicMenu);

    g_hMenuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MenuMouseHookProc, hInst, 0);

    g_bModalLoop = true;
    MSG msg;
    while (g_bModalLoop && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DismissAllMenus();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) {
        PostQuitMessage((int)msg.wParam);
    }
}

// --------------------------------------------------------------------------
// Taskbar Battery Icon & Tooltip
// --------------------------------------------------------------------------

static const uint16_t FONT_5X9[10][9] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x11, 0x0E },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
    { 0x1E, 0x01, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x01, 0x1E },
    { 0x02, 0x06, 0x0A, 0x12, 0x12, 0x1F, 0x02, 0x02, 0x02 },
    { 0x1F, 0x10, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x0E },
    { 0x1F, 0x01, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x02, 0x0C },
};

static const uint16_t FONT_4X9_0[9] = {
    0x06, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x06
};

static const uint16_t FONT_3X5[10][5] = {
    { 0x07, 0x05, 0x05, 0x05, 0x07 },
    { 0x02, 0x06, 0x02, 0x02, 0x07 },
    { 0x07, 0x01, 0x07, 0x04, 0x07 },
    { 0x07, 0x01, 0x07, 0x01, 0x07 },
    { 0x05, 0x05, 0x07, 0x01, 0x01 },
    { 0x07, 0x04, 0x07, 0x01, 0x07 },
    { 0x07, 0x04, 0x07, 0x05, 0x07 },
    { 0x07, 0x01, 0x02, 0x02, 0x02 },
    { 0x07, 0x05, 0x07, 0x05, 0x07 },
    { 0x07, 0x05, 0x07, 0x01, 0x07 },
};

HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark) {
    if (size <= 0) size = 16;
    const int SS = 4;
    int W = size * SS;
    int H = size * SS;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvPixels = NULL;
    HBITMAP hbmColor = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvPixels, NULL, 0);
    if (!hbmColor || !pvPixels) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }

    uint32_t* hi = (uint32_t*)pvPixels;
    std::memset(hi, 0, W * H * 4);

    auto HiPixel = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < W && y >= 0 && y < H) {
            hi[y * W + x] = c;
        }
    };

    auto FillRoundRect = [&](int x0, int y0, int x1, int y1, uint32_t col, int rad) {
        if (rad > (x1 - x0) / 2) rad = (x1 - x0) / 2;
        if (rad > (y1 - y0) / 2) rad = (y1 - y0) / 2;
        if (rad < 0) rad = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                int dx = (x < x0 + rad) ? (x0 + rad - x) : (x > x1 - rad) ? (x - (x1 - rad)) : 0;
                int dy = (y < y0 + rad) ? (y0 + rad - y) : (y > y1 - rad) ? (y - (y1 - rad)) : 0;
                if (dx * dx + dy * dy <= rad * rad) HiPixel(x, y, col);
            }
        }
    };

    auto DrawDigitScaled = [&](const uint16_t* rows, int fw, int fh,
                               int x0, int y0, int scale, int bold_w, uint32_t c_digit) {
        for (int r = 0; r < fh; ++r) {
            for (int c = 0; c < fw; ++c) {
                if (!((rows[r] >> (fw - 1 - c)) & 1)) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale + bold_w; ++dx) {
                        int px = x0 + c * scale + dx;
                        int py = y0 + r * scale + dy;
                        HiPixel(px, py, c_digit);
                    }
                }
            }
        }
    };

    auto DrawBolt = [&](int sx, int sy, uint32_t col) {
        static const char* pattern[7] = {
            "..#",
            ".##",
            "###",
            ".#.",
            "##.",
            "#..",
            "..."
        };
        for (int r = 0; r < 7; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (pattern[r][c] == '#') {
                    for (int dy = 0; dy < SS; ++dy) {
                        for (int dx = 0; dx < SS; ++dx) {
                            HiPixel(sx + c * SS + dx, sy + r * SS + dy, col);
                        }
                    }
                }
            }
        }
    };

    auto L2S = [&](float v) {
        return (int)(v * (float)(size * SS) / 16.0f + 0.5f);
    };

    int style = GetBatteryStyle();

    // Style 0: classic battery capsule
    if (style == 0) {
        int bx0 = 0;
        int by0 = L2S(2.5f);
        int bw  = L2S(13.5f);
        int bh  = L2S(11.0f);
        int bx1 = bx0 + bw;
        int by1 = by0 + bh;
        int brad = L2S(2.5f);

        uint32_t c_shell = isDark ? 0xFFFFFFFF : 0xFF334155;
        uint32_t c_fill = isCharging ? 0xFF22C55E : ((battery <= 30) ? 0xFFEF4444 : 0xFF3B82F6);

        FillRoundRect(bx0, by0, bx1, by1, c_shell, brad);

        int cx0 = L2S(13.8f);
        int cy0 = L2S(5.2f);
        int cx1 = std::min(W - 1, cx0 + L2S(1.8f));
        int cy1 = cy0 + L2S(5.6f);
        FillRoundRect(cx0, cy0, cx1, cy1, c_shell, L2S(1.0f));

        int stroke_t = (int)(1.5f * SS + 0.5f);
        int cav_x0 = bx0 + stroke_t;
        int cav_y0 = by0 + stroke_t;
        int cav_x1 = bx1 - stroke_t;
        int cav_y1 = by1 - stroke_t;
        FillRoundRect(cav_x0, cav_y0, cav_x1, cav_y1, 0, 0);

        int pad = (int)(0.75f * SS + 0.5f);
        int in_x0 = cav_x0 + pad;
        int in_y0 = cav_y0 + pad;
        int in_x1 = cav_x1 - pad;
        int in_y1 = cav_y1 - pad;
        int in_w  = in_x1 - in_x0 + 1;
        int in_h  = in_y1 - in_y0 + 1;

        if (isConnected && battery > 0) {
            float ratio = std::clamp(battery / 100.0f, 0.08f, 1.0f);
            int fill_w = std::max(L2S(2.0f), (int)(in_w * ratio));
            FillRoundRect(in_x0, in_y0, in_x0 + fill_w, in_y1, c_fill, L2S(1.5f));
        } else if (!isConnected) {
            uint32_t c_dim = isDark ? 0x40606060 : 0x40B0B0B0;
            FillRoundRect(in_x0, in_y0, in_x1, in_y1, c_dim, L2S(1.5f));
            int dw = L2S(4.5f);
            int dh = L2S(2.0f);
            int sx = in_x0 + (in_w - dw) / 2;
            int sy = in_y0 + (in_h - dh) / 2;
            FillRoundRect(sx, sy, sx + dw, sy + dh, isDark ? 0xFFBBBBBB : 0xFF666666, 0);
        }

        if (isCharging) {
            int bolt_sx = cav_x0 + (cav_x1 - cav_x0 + 1 - 3 * SS) / 2;
            int bolt_sy = cav_y0 + (cav_y1 - cav_y0 + 1 - 7 * SS) / 2;
            DrawBolt(bolt_sx, bolt_sy, 0xFFFFFFFF);
        }
    }
    // Style 1: 360° ring progress
    else if (style == 1) {
        float cx = (float)W / 2.0f;
        float cy = (float)H / 2.0f;
        float R  = (float)(size * SS) * 5.8f / 16.0f;
        float T  = (float)(size * SS) * 3.0f / 16.0f;

        uint32_t c_track = isDark ? 0x40FFFFFF : 0x35000000;
        uint32_t c_fill  = isCharging ? 0xFF22C55E : ((battery <= 30) ? 0xFFEF4444 : 0xFF3B82F6);

        float sweep = isConnected ? std::clamp(360.0f * (battery / 100.0f), 0.0f, 360.0f) : 0.0f;
        float sweep_rad = sweep * 3.141592653589793f / 180.0f;
        float cap_r = T / 2.0f;
        float cap_r2 = cap_r * cap_r;

        float sx_cap = cx;
        float sy_cap = cy - R;
        float ex_cap = cx + R * std::sin(sweep_rad);
        float ey_cap = cy - R * std::cos(sweep_rad);

        int minX = std::max(0, (int)(cx - R - T));
        int maxX = std::min(W - 1, (int)(cx + R + T));
        int minY = std::max(0, (int)(cy - R - T));
        int maxY = std::min(H - 1, (int)(cy + R + T));

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                float px = (float)x + 0.5f;
                float py = (float)y + 0.5f;
                float dx = px - cx;
                float dy = py - cy;
                float dist = std::sqrt(dx * dx + dy * dy);

                if (isConnected && battery > 0 && ((px - sx_cap) * (px - sx_cap) + (py - sy_cap) * (py - sy_cap) <= cap_r2)) {
                    HiPixel(x, y, c_fill);
                    continue;
                }
                if (isConnected && battery > 0 && ((px - ex_cap) * (px - ex_cap) + (py - ey_cap) * (py - ey_cap) <= cap_r2)) {
                    HiPixel(x, y, c_fill);
                    continue;
                }

                if (std::abs(dist - R) <= cap_r) {
                    if (isConnected && battery > 0) {
                        float angle = std::atan2(dx, -dy) * (180.0f / 3.141592653589793f);
                        if (angle < 0.0f) angle += 360.0f;
                        if (angle <= sweep) {
                            HiPixel(x, y, c_fill);
                        } else {
                            HiPixel(x, y, c_track);
                        }
                    } else {
                        HiPixel(x, y, c_track);
                    }
                }
            }
        }

        if (isCharging) {
            DrawBolt((W - 3 * SS) / 2, (H - 7 * SS) / 2, c_fill);
        } else if (!isConnected) {
            int dw = L2S(4.0f);
            int dh = L2S(2.0f);
            FillRoundRect((W - dw) / 2, (H - dh) / 2, (W + dw) / 2, (H + dh) / 2, isDark ? 0xFF888888 : 0xFF666666, 0);
        }
    }
    // Style 2: big number + bottom mini bar
    else {
        COLORREF targetCol;
        if (!isConnected) {
            targetCol = isDark ? RGB(160, 160, 160) : RGB(100, 100, 100);
        } else if (isCharging) {
            targetCol = RGB(34, 197, 94); // Green
        } else if (battery <= 30) {
            targetCol = RGB(239, 68, 68); // Red
        } else {
            targetCol = isDark ? RGB(255, 255, 255) : RGB(20, 20, 20); // White / Dark
        }

        WCHAR sWide[16] = {0};
        if (!isConnected) {
            StringCchCopyW(sWide, ARRAYSIZE(sWide), L"-");
        } else {
            StringCchPrintfW(sWide, ARRAYSIZE(sWide), L"%d", battery);
        }

        int fontH = (battery == 100) ? (-H * 52 / 100) : (-H * 70 / 100);
        HFONT hFont = CreateFontW(
            fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI"
        );

        HGDIOBJ oldBm = SelectObject(hdcMem, hbmColor);
        HFONT oldFont = (HFONT)SelectObject(hdcMem, hFont);
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(255, 255, 255));

        int textH = (int)(H * 13.5f / 16.0f);
        RECT rcText = { 0, -SS, W, textH };
        DrawTextW(hdcMem, sWide, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        GdiFlush();
        SelectObject(hdcMem, oldFont);
        DeleteObject(hFont);
        SelectObject(hdcMem, oldBm);

        uint8_t tR = GetRValue(targetCol);
        uint8_t tG = GetGValue(targetCol);
        uint8_t tB = GetBValue(targetCol);

        for (int y = 0; y < textH; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t raw = hi[y * W + x];
                uint8_t gray = (raw & 0xFF);
                if (gray > 0) {
                    hi[y * W + x] = ((uint32_t)gray << 24) | ((uint32_t)tR << 16) | ((uint32_t)tG << 8) | tB;
                } else {
                    hi[y * W + x] = 0;
                }
            }
        }

        int bar_y0 = (int)(H * 14.0f / 16.0f);
        int bar_y1 = H - 1;
        uint32_t c_track = isDark ? 0x40FFFFFF : 0x30000000;
        FillRoundRect(0, bar_y0, W - 1, bar_y1, c_track, SS / 2);

        if (isConnected && battery > 0) {
            uint32_t c_fill = isCharging ? 0xFF22C55E : ((battery <= 30) ? 0xFFEF4444 : 0xFF3B82F6);
            int bar_w = std::clamp((int)(W * (battery / 100.0f)), 2 * SS, W);
            FillRoundRect(0, bar_y0, bar_w - 1, bar_y1, c_fill, SS / 2);
        }
    }

    // Downsample SSxSS to size x size with smooth box filtering
    BITMAPINFO bomi = {0};
    bomi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bomi.bmiHeader.biWidth = size;
    bomi.bmiHeader.biHeight = -size;
    bomi.bmiHeader.biPlanes = 1;
    bomi.bmiHeader.biBitCount = 32;
    bomi.bmiHeader.biCompression = BI_RGB;

    void* pvOut = NULL;
    HBITMAP hbmOut = CreateDIBSection(hdcMem, &bomi, DIB_RGB_COLORS, &pvOut, NULL, 0);
    if (!hbmOut || !pvOut) {
        DeleteObject(hbmColor);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return NULL;
    }

    uint32_t* out = (uint32_t*)pvOut;
    const int SS2 = SS * SS;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            for (int sy = 0; sy < SS; ++sy) {
                const uint32_t* row = hi + (y * SS + sy) * W + x * SS;
                for (int sx = 0; sx < SS; ++sx) {
                    uint32_t c = row[sx];
                    uint32_t a = (c >> 24) & 0xFF;
                    if (a > 0) {
                        sumA += a;
                        sumR += (c >> 16) & 0xFF;
                        sumG += (c >>  8) & 0xFF;
                        sumB += (c      ) & 0xFF;
                    }
                }
            }
            uint8_t avgA = (uint8_t)(sumA / SS2);
            if (avgA == 0) {
                out[y * size + x] = 0;
            } else {
                uint8_t avgR = (uint8_t)(sumR / SS2);
                uint8_t avgG = (uint8_t)(sumG / SS2);
                uint8_t avgB = (uint8_t)(sumB / SS2);
                uint8_t prR = (uint8_t)((avgR * avgA) / 255);
                uint8_t prG = (uint8_t)((avgG * avgA) / 255);
                uint8_t prB = (uint8_t)((avgB * avgA) / 255);
                out[y * size + x] = ((uint32_t)avgA << 24) | ((uint32_t)prR << 16) | ((uint32_t)prG << 8) | (uint32_t)prB;
            }
        }
    }

    int maskPitch = ((size + 15) / 16) * 2;
    BYTE maskBits[256] = {0};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t px = out[y * size + x];
            uint8_t a = (px >> 24) & 0xFF;
            if (a < 32) {
                maskBits[y * maskPitch + (x / 8)] |= (1 << (7 - (x % 8)));
            }
        }
    }
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, maskBits);

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmOut;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmOut);
    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    return hIcon;
}

static void ShortLabel(const WCHAR* name, WCHAR* out, int maxLen) {
    if (!name || !name[0]) {
        StringCchCopyW(out, maxLen, L"设备");
        return;
    }
    if ((int)wcslen(name) <= maxLen - 2) {
        StringCchCopyW(out, maxLen, name);
        return;
    }
    wcsncpy(out, name, maxLen - 2);
    out[maxLen - 2] = 0;
    StringCchCatW(out, maxLen, L"…");
}

static void BatteryLabel(const Device::State& d, WCHAR* out, int maxLen) {
    if (!d.isConnected) StringCchCopyW(out, maxLen, L"未连接");
    else if (!d.batteryReadable) StringCchCopyW(out, maxLen, L"不可读");
    else if (d.battery < 0) StringCchCopyW(out, maxLen, L"未知");
    else if (d.isCharging) StringCchPrintfW(out, maxLen, L"%d%% 充电中", d.battery);
    else StringCchPrintfW(out, maxLen, L"%d%%", d.battery);
}

void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& active) {
    Device::State devices[Device::MAX_DEVICES];
    int n = Device::GetDevices(devices, Device::MAX_DEVICES);

    WCHAR tip[128] = {0};
    if (n == 0) {
        StringCchCopyW(tip, ARRAYSIZE(tip), L"未检测到设备");
    } else {
        // The tray icon reflects the active device, so its row leads the tip;
        // the remaining rows keep list order behind it.
        int order[Device::MAX_DEVICES];
        int count = 0;
        bool haveActive = (active.id[0] != 0);
        if (haveActive) {
            for (int i = 0; i < n; ++i) {
                if (wcscmp(active.id, devices[i].id) == 0) { order[count++] = i; break; }
            }
        }
        haveActive = (count > 0);
        for (int i = 0; i < n && count < Device::MAX_DEVICES; ++i) {
            if (haveActive && wcscmp(active.id, devices[i].id) == 0) continue;
            order[count++] = i;
        }

        int shown = (count < 3) ? count : 3;
        for (int k = 0; k < shown; ++k) {
            const Device::State& d = devices[order[k]];
            WCHAR name[18] = {0}, bat[20] = {0}, line[48] = {0};
            ShortLabel(d.modelName, name, ARRAYSIZE(name));
            BatteryLabel(d, bat, ARRAYSIZE(bat));
            if (haveActive && k == 0) {
                StringCchPrintfW(line, ARRAYSIZE(line), L"● %s  %s", name, bat);
            } else {
                StringCchPrintfW(line, ARRAYSIZE(line), L"%s  %s", name, bat);
            }
            if (k > 0) StringCchCatW(tip, ARRAYSIZE(tip), L"\n");
            StringCchCatW(tip, ARRAYSIZE(tip), line);
        }
        if (count > shown) StringCchCatW(tip, ARRAYSIZE(tip), L"\n…");
    }

    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), tip);
}

// --------------------------------------------------------------------------
// Custom device manager dialog
// --------------------------------------------------------------------------

static const int IDC_DEVLIST  = 201;
static const int IDC_NAMEEDIT = 202;
static const int IDC_MATCHEDIT = 203;
static const int IDC_ADDBTN   = 204;
static const int IDC_DELBTN   = 205;
static const int IDC_HINT     = 206;
static const int IDC_LBL_NAME = 207;
static const int IDC_LBL_MATCH = 208;

static void TrimInPlace(WCHAR* s) {
    if (!s) return;
    WCHAR* p = s;
    while (*p == L' ' || *p == L'\t') ++p;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
    size_t len = wcslen(s);
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = 0;
}

static void FillDeviceList(HWND hDlg) {
    HWND hList = GetDlgItem(hDlg, IDC_DEVLIST);
    if (!hList) return;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    int n = Device::GetCustomDeviceCount();
    for (int i = 0; i < n; ++i) {
        WCHAR name[64] = {0}, match[48] = {0};
        if (Device::GetCustomDevice(i, name, ARRAYSIZE(name), match, ARRAYSIZE(match))) {
            WCHAR line[128] = {0};
            StringCchPrintfW(line, ARRAYSIZE(line), L"%s   [%s]", name, match);
            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line);
        }
    }
}

static INT_PTR CALLBACK DeviceDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowTextW(hDlg, L"设备管理");
            SetDlgItemTextW(hDlg, IDC_HINT, L"添加后按关键字自动检测在线状态（最多 4 个）");
            SetDlgItemTextW(hDlg, IDC_LBL_NAME, L"设备名称");
            SetDlgItemTextW(hDlg, IDC_LBL_MATCH, L"匹配关键字");
            SetDlgItemTextW(hDlg, IDC_ADDBTN, L"添加");
            SetDlgItemTextW(hDlg, IDC_DELBTN, L"删除选中");
            SetDlgItemTextW(hDlg, IDCANCEL, L"关闭");
            if (IsSystemDarkMode()) {
                BOOL dark = TRUE;
                DwmSetWindowAttribute(hDlg, 20, &dark, sizeof(dark));
            }
            FillDeviceList(hDlg);
            return TRUE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_ADDBTN) {
                WCHAR name[64] = {0}, match[48] = {0};
                GetDlgItemTextW(hDlg, IDC_NAMEEDIT, name, ARRAYSIZE(name));
                GetDlgItemTextW(hDlg, IDC_MATCHEDIT, match, ARRAYSIZE(match));
                TrimInPlace(name);
                TrimInPlace(match);
                if (!name[0] || !match[0]) {
                    MessageBoxW(hDlg,
                                L"请填写设备名称和匹配关键字。\n例如名称「我的键盘」、关键字「MAG75」。",
                                L"设备管理", MB_ICONINFORMATION);
                    return TRUE;
                }
                if (!Device::AddCustomDevice(name, match)) {
                    MessageBoxW(hDlg,
                                L"添加失败：同名设备已存在，或已达 4 个上限。",
                                L"设备管理", MB_ICONWARNING);
                    return TRUE;
                }
                SetDlgItemTextW(hDlg, IDC_NAMEEDIT, L"");
                SetDlgItemTextW(hDlg, IDC_MATCHEDIT, L"");
                FillDeviceList(hDlg);
                return TRUE;
            }
            if (id == IDC_DELBTN) {
                int sel = (int)SendMessageW(GetDlgItem(hDlg, IDC_DEVLIST), LB_GETCURSEL, 0, 0);
                if (sel == LB_ERR) {
                    MessageBoxW(hDlg, L"请先在列表中选择要删除的设备。", L"设备管理", MB_ICONINFORMATION);
                    return TRUE;
                }
                Device::RemoveCustomDevice(sel);
                FillDeviceList(hDlg);
                return TRUE;
            }
            if (id == IDOK || id == IDCANCEL) {
                EndDialog(hDlg, 0);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(hDlg, 0);
            return TRUE;
    }
    return FALSE;
}

void ShowDeviceDialog(HWND hWndOwner) {
    HINSTANCE hInst = hWndOwner
        ? (HINSTANCE)GetWindowLongPtrW(hWndOwner, GWLP_HINSTANCE)
        : GetModuleHandleW(NULL);
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_DEVICE_MANAGER), hWndOwner, DeviceDialogProc, 0);
}

} // namespace Tray
