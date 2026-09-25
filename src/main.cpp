#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <hidsdi.h>
#include <strsafe.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <vector>
#include "device_manager.h"
#include "osd_window.h"
#include "alert_window.h"
#include "tray_menu.h"
#include "mock_hidpp.h"

static const UINT WM_APP_TRAYMSG = WM_APP + 1;
static const UINT WM_APP_STATE_UPDATE = Tray::WM_APP_STATE_UPDATE;

static HWND g_hMainWnd = NULL;
static NOTIFYICONDATAW g_nid = {0};
static UINT g_uTaskbarRestartMsg = 0;
static HICON g_hCurrentTrayIcon = NULL;

// ---------------------------------------------------------------------------
// --selftest
//
// Verifies multi-device handling on a machine that only has one real device.
// Two simulated receivers are injected at the HID++ layer, then the regular
// pipeline (interface scan, slot ping, feature resolution, battery poll,
// publish, tooltip, menu rows) runs against them. The report is written next
// to the exe so it can be inspected without a console.
// ---------------------------------------------------------------------------

static WCHAR g_selfTestReport[65536];
static volatile LONG g_selfTestUpdates = 0;
static int g_selfTestPass = 0;
static int g_selfTestFail = 0;

static void SelfTestOnState(const Device::State&, DWORD) {
    InterlockedIncrement(&g_selfTestUpdates);
}

static void Rep(const WCHAR* fmt, ...) {
    WCHAR line[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);
    StringCchCatW(g_selfTestReport, ARRAYSIZE(g_selfTestReport), line);
    StringCchCatW(g_selfTestReport, ARRAYSIZE(g_selfTestReport), L"\r\n");
}

static const Device::State* FindById(const Device::State* devs, int n, const WCHAR* id) {
    for (int i = 0; i < n; ++i) {
        if (wcscmp(devs[i].id, id) == 0) return &devs[i];
    }
    return NULL;
}

static bool AnyLevelPublished() {
    Device::State devs[Device::MAX_DEVICES];
    int n = Device::GetDevices(devs, Device::MAX_DEVICES);
    for (int i = 0; i < n; ++i) {
        if (devs[i].battery >= 0) return true;
    }
    return false;
}

static bool SleepyReportedOffline() {
    Device::State devs[Device::MAX_DEVICES];
    int n = Device::GetDevices(devs, Device::MAX_DEVICES);
    const Device::State* s = FindById(devs, n, Mock::SleepyDeviceId());
    return s && !s->isConnected;
}

static void SelfTestSample(const WCHAR* title) {
    Device::State devs[Device::MAX_DEVICES];
    int n = Device::GetDevices(devs, Device::MAX_DEVICES);

    Rep(L"--- %s ---", title);
    Rep(L"device count: %d", n);
    for (int i = 0; i < n; ++i) {
        Rep(L"  [%d] name=%s | mode=%s | connected=%d | battery=%d | charging=%d | readable=%d | id=%s",
            i, devs[i].modelName, devs[i].modeName,
            devs[i].isConnected ? 1 : 0, devs[i].battery,
            devs[i].isCharging ? 1 : 0, devs[i].batteryReadable ? 1 : 0, devs[i].id);
    }

    NOTIFYICONDATAW nid = {0};
    Tray::UpdateTooltip(nid, Device::GetActiveState());
    Rep(L"tray tooltip:");
    Rep(L"%s", nid.szTip);

    WCHAR rows[8192] = {0};
    int rowCount = Tray::GetMenuRowsForDiagnostics(rows, ARRAYSIZE(rows));
    Rep(L"menu rows (%d):", rowCount);
    Rep(L"%s", rows);
}

static void SelfTestChecks() {
    Device::State devs[Device::MAX_DEVICES];
    int n = Device::GetDevices(devs, Device::MAX_DEVICES);

    Rep(L"--- expectations ---");
    for (int e = 0; e < Mock::ExpectationCount(); ++e) {
        const Mock::Expectation* ex = Mock::ExpectationAt(e);
        const Device::State* got = FindById(devs, n, ex->id);
        if (!got) {
            Rep(L"[FAIL] %s : never reported as a device", ex->id);
            g_selfTestFail++;
            continue;
        }
        bool ok = (wcscmp(got->modelName, ex->name) == 0)
                  && (got->battery == ex->battery)
                  && (ex->charging < 0 || (got->isCharging ? 1 : 0) == ex->charging)
                  && (ex->readable < 0 || (got->batteryReadable ? 1 : 0) == ex->readable)
                  && ((got->isConnected ? 1 : 0) == ex->connected);
        Rep(L"[%s] %s : name=%s battery=%d charging=%d readable=%d connected=%d (expected %s / %d / %d / %d)",
            ok ? L"PASS" : L"FAIL", ex->id, got->modelName, got->battery,
            got->isCharging ? 1 : 0, got->batteryReadable ? 1 : 0, got->isConnected ? 1 : 0,
            ex->name, ex->battery, ex->charging, ex->connected);
        if (ok) g_selfTestPass++; else g_selfTestFail++;
    }

    bool ghost = (FindById(devs, n, L"sim#c53f#0#04") != NULL);
    Rep(L"[%s] empty receiver slot sim#c53f#0#04 is not published as a device",
        ghost ? L"FAIL" : L"PASS");
    if (ghost) g_selfTestFail++; else g_selfTestPass++;

    // A device whose level cannot be read must not look like a real "0 %",
    // otherwise the low-battery alert fires for it.
    int bogus = 0;
    for (int i = 0; i < n; ++i) {
        if (!devs[i].batteryReadable && devs[i].battery >= 0) bogus++;
    }
    Rep(L"[%s] no unreadable device reports a numeric level (found %d)",
        bogus == 0 ? L"PASS" : L"FAIL", bogus);
    if (bogus == 0) g_selfTestPass++; else g_selfTestFail++;

    HICON icon = Tray::CreateBatteryIcon(12, false, true, GetSystemMetrics(SM_CXSMICON), true);
    Rep(L"[%s] tray icon renders for a 12%% discharging device", icon ? L"PASS" : L"FAIL");
    if (icon) { DestroyIcon(icon); g_selfTestPass++; } else g_selfTestFail++;
}

static void SelfTestActiveSwitch() {
    const WCHAR* target = L"sim#c52b#1#01";
    Device::SetActiveDeviceId(target);
    Device::State a = Device::GetActiveState();
    bool ok = (wcscmp(a.id, target) == 0) && (a.battery == 12);
    Rep(L"[%s] switching the active device updates the tray source (id=%s battery=%d)",
        ok ? L"PASS" : L"FAIL", a.id, a.battery);
    if (ok) g_selfTestPass++; else g_selfTestFail++;

    NOTIFYICONDATAW nid = {0};
    Tray::UpdateTooltip(nid, a);
    Rep(L"tooltip after switch:");
    Rep(L"%s", nid.szTip);

    // The tip must lead with the device the icon now reflects, marked the same
    // way the menu marks it.
    WCHAR first[64] = {0};
    StringCchCopyW(first, ARRAYSIZE(first), nid.szTip);
    WCHAR* nl = wcschr(first, L'\n');
    if (nl) *nl = 0;
    bool leads = (first[0] == L'●') && (wcsstr(first, a.modelName) != NULL);
    Rep(L"[%s] tooltip leads with the active device: %s", leads ? L"PASS" : L"FAIL", first);
    if (leads) g_selfTestPass++; else g_selfTestFail++;
}

static bool SelfTestSave() {
    WCHAR path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    StringCchCatW(path, MAX_PATH, L"logi-tray-selftest.txt");

    int need = WideCharToMultiByte(CP_UTF8, 0, g_selfTestReport, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return false;
    std::vector<char> utf8((size_t)need);
    WideCharToMultiByte(CP_UTF8, 0, g_selfTestReport, -1, utf8.data(), need, NULL, NULL);

    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    fwrite(utf8.data(), 1, (size_t)(need - 1), f);
    fclose(f);
    return true;
}

static int RunSelfTest() {
    Mock::Enable(2);

    StringCchCopyW(g_selfTestReport, ARRAYSIZE(g_selfTestReport),
                   L"logi-tray multi-device self-test\r\n");
    Rep(L"simulated receivers: %d", Mock::ReceiverCount());
    Rep(L"sleepy device: %s (stops answering %u ms after the first request)",
        Mock::SleepyDeviceId(), (unsigned)Mock::SleepyAfterMs());
    Rep(L"real Logitech interfaces are still scanned, so both sources appear below.");

    if (!Device::Start(NULL, SelfTestOnState)) {
        Rep(L"FAIL: Device::Start returned false");
        SelfTestSave();
        return 1;
    }

    // The worker republishes on a 5 s tick, so nudge it until the simulated
    // levels have actually landed in the snapshot.
    for (int i = 0; i < 15 && !AnyLevelPublished(); ++i) {
        Sleep(1000);
        Device::RequestRefresh();
    }
    SelfTestSample(L"sample 1: every simulated device awake");

    // Keep polling until the sleepy device has crossed into "offline" and that
    // state has been republished.
    for (int i = 0; i < 25 && !SleepyReportedOffline(); ++i) {
        Sleep(1000);
        Device::RequestRefresh();
    }
    SelfTestSample(L"sample 2: after the sleepy device stopped answering");

    SelfTestChecks();
    SelfTestActiveSwitch();
    Rep(L"checks: %d passed, %d failed", g_selfTestPass, g_selfTestFail);
    Rep(L"state callbacks delivered: %ld", (long)g_selfTestUpdates);

    Device::Stop();

    return SelfTestSave() ? 0 : 1;
}

static void RefreshTrayUI(const Device::State& state) {
    if (!g_hMainWnd) return;

    bool isDark = true;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD), type = 0;
        if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
            isDark = (val == 0);
        }
        RegCloseKey(hKey);
    }

    int iconSize = GetSystemMetrics(SM_CXSMICON);
    if (iconSize <= 0) iconSize = 16;

    int battery = (state.battery >= 0) ? state.battery : 0;
    HICON hNewIcon = Tray::CreateBatteryIcon(battery, state.isCharging, state.isConnected, iconSize, isDark);
    if (hNewIcon) {
        g_nid.hIcon = hNewIcon;
        Tray::UpdateTooltip(g_nid, state);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);

        if (g_hCurrentTrayIcon) {
            DestroyIcon(g_hCurrentTrayIcon);
        }
        g_hCurrentTrayIcon = hNewIcon;
    }

    Alert::CheckBattery(state);
}

static void ShowBatteryOsd(const Device::State& st) {
    if (st.isConnected) {
        Osd::ShowBattery(st.modelName, st.battery, st.isCharging, st.modeName);
    } else {
        const WCHAR* mName = st.modelName[0] ? st.modelName : L"罗技设备";
        Osd::Show(mName, L"设备休眠 / 未连接", L"请移动鼠标或重新插拔接收器唤醒");
    }
}

static void OnDeviceStateChanged(const Device::State& state, DWORD changeMask) {
    if (!g_hMainWnd) return;
    PostMessageW(g_hMainWnd, WM_APP_STATE_UPDATE, 0, 0);
}

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_uTaskbarRestartMsg) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        RefreshTrayUI(Device::GetActiveState());
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAYMSG: {
            if (lParam == WM_LBUTTONUP) {
                ShowBatteryOsd(Device::GetActiveState());
            } else if (lParam == WM_RBUTTONUP) {
                SetForegroundWindow(hWnd);
                Tray::ShowMenu(hWnd);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                int s = Tray::CycleBatteryStyle();
                Osd::ShowStyleOsd(s);
                RefreshTrayUI(Device::GetActiveState());
            }
            return 0;
        }

        case WM_APP_STATE_UPDATE: {
            RefreshTrayUI(Device::GetActiveState());
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            RefreshTrayUI(Device::GetActiveState());
            return 0;
        }

        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
                Device::NotifyDeviceChange();
            }
            return 0;
        }

        case WM_DESTROY: {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hCurrentTrayIcon) {
                DestroyIcon(g_hCurrentTrayIcon);
                g_hCurrentTrayIcon = NULL;
            }
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Runs before the single-instance check so it can be used while the normal
    // tray app is already running.
    if (pCmdLine && wcsstr(pCmdLine, L"--selftest")) {
        return RunSelfTest();
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\logi-traySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    Tray::InitTheme();

    // Enable modern Per-Monitor V2 DPI awareness
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *pfnSetDpiAwareV2)(DPI_AWARENESS_CONTEXT);
        pfnSetDpiAwareV2 fnSetDpiAwareV2 = (pfnSetDpiAwareV2)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (fnSetDpiAwareV2) {
            fnSetDpiAwareV2(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"LogiTrayMessageWnd";
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(
        0, wc.lpszClassName, L"LogiTray",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    g_uTaskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    Osd::Init(hInstance);
    Alert::Init(hInstance);

    // Register PnP device notifications
    DEV_BROADCAST_DEVICEINTERFACE_W dbFilter = {0};
    dbFilter.dbcc_size = sizeof(dbFilter);
    dbFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&dbFilter.dbcc_classguid);
    HDEVNOTIFY hDevNotify = RegisterDeviceNotificationW(g_hMainWnd, &dbFilter, DEVICE_NOTIFY_WINDOW_HANDLE | 0x00000004);

    // Register Notify Icon
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAYMSG;
    g_nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"正在检测设备…");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Start background HID++ device manager
    Device::Start(g_hMainWnd, OnDeviceStateChanged);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hDevNotify) {
        UnregisterDeviceNotification(hDevNotify);
        hDevNotify = NULL;
    }
    Device::Stop();
    Osd::Cleanup();
    Alert::Cleanup();

    CloseHandle(hMutex);
    return 0;
}
