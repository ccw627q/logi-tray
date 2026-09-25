#include "device_manager.h"
#include "mock_hidpp.h"
#include <hidsdi.h>
#include <setupapi.h>
#include <strsafe.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace Device {

// ---------------------------------------------------------------------------
// Diagnostics log next to the exe
// ---------------------------------------------------------------------------

static void WideToUtf8(const WCHAR* w, char* out, int maxLen) {
    if (!out || maxLen <= 0) return;
    out[0] = 0;
    if (!w) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0 || n > maxLen) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
}

static void DebugLog(const char* fmt, ...) {
    WCHAR exeDir[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    WCHAR* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = 0;
    WCHAR logPath[MAX_PATH];
    StringCchPrintfW(logPath, MAX_PATH, L"%s\\logi-tray.log", exeDir);
    FILE* f = _wfopen(logPath, L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool EqualsNoCase(const WCHAR* a, const WCHAR* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        WCHAR ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb - L'A' + L'a');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static bool ContainsNoCase(const WCHAR* hay, const WCHAR* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = wcslen(needle);
    for (const WCHAR* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            WCHAR ca = p[i], cb = needle[i];
            if (ca >= L'A' && ca <= L'Z') ca = (WCHAR)(ca - L'A' + L'a');
            if (cb >= L'A' && cb <= L'Z') cb = (WCHAR)(cb - L'A' + L'a');
            if (ca != cb) break;
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const WCHAR* LOGITECH_VID = L"vid_046d";

// Receiver PID -> connection mode label
static const struct { const WCHAR* pid; const WCHAR* name; } RECEIVER_MODES[] = {
    { L"c52b", L"优联 (Unifying)" },
    { L"c52e", L"优联 (Unifying)" },
    { L"c532", L"优联 (Unifying)" },
    { L"c537", L"优联 (Unifying)" },
    { L"c539", L"Lightspeed" },
    { L"c53f", L"Lightspeed" },
    { L"c541", L"Lightspeed" },
    { L"c543", L"Lightspeed" },
    { L"c545", L"Lightspeed" },
    { L"c547", L"Lightspeed" },
    { L"c548", L"Lightspeed" },
    { L"c54d", L"Lightspeed" },
    { L"c54f", L"Lightspeed" },
    { L"c550", L"Lightspeed" },
    { L"c551", L"Lightspeed" },
    { L"c552", L"Lightspeed" },
    { L"c553", L"Lightspeed" },
    { L"c557", L"Lightspeed" },
    { L"c55f", L"Lightspeed" },
};

// Each Logitech receiver exposes one HID++ interface per top-level collection
// (mi_00, mi_01, ...), so a machine with two receivers plus a wired device can
// easily need more than four groups.
static const int MAX_GROUPS = 8;
static const int MAX_SLOTS  = 6;
static const int MAX_CUSTOM = 4;

static const WCHAR* REG_SUBKEY = L"Software\\logi-tray";
static const WCHAR* REG_CUSTOM = L"CustomDevices";

// ---------------------------------------------------------------------------
// Worker-owned state
// ---------------------------------------------------------------------------

struct TrackedGroup {
    WCHAR key[64] = {0};
    Logi::DeviceGroup group;
    bool open = false;
    DWORD lastScan = 0;
};

struct KnownDev {
    State state;
    WCHAR groupKey[64] = {0};
    BYTE devNumber = 0;
    WORD batteryFeatureId = 0;
    BYTE batteryFeatureIndex = 0;
    bool featuresResolved = false;
    bool noBatteryFeature = false;
    int failCount = 0;
    DWORD offlineSince = 0;
};

static TrackedGroup g_groups[MAX_GROUPS];
static int g_groupCount = 0;
static KnownDev g_devs[MAX_DEVICES];
static int g_devCount = 0;

// ---------------------------------------------------------------------------
// Shared state (guarded)
// ---------------------------------------------------------------------------

static std::vector<State> g_snapshot;
static WCHAR g_activeId[96] = {0};
static CRITICAL_SECTION g_cs;
static CRITICAL_SECTION g_csCustom;

struct CustomEntry {
    WCHAR name[64] = {0};
    WCHAR match[48] = {0};
};

static CustomEntry g_custom[MAX_CUSTOM];
static int g_customCount = 0;
static bool g_customPresent[MAX_CUSTOM] = {false};

static HANDLE g_hWorker = NULL;
static HANDLE g_hStop = NULL;
static HANDLE g_hWake = NULL;
static StateCallback g_callback = nullptr;
static volatile bool g_needRediscover = false;
// Set by the UI thread when the custom-device list changes, so the worker
// republishes at once instead of waiting for the 5 s presence tick.
static volatile bool g_customDirty = false;

// ---------------------------------------------------------------------------
// Custom device persistence
// ---------------------------------------------------------------------------

static void SaveCustomDevicesLocked() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) return;
    if (g_customCount == 0) {
        RegDeleteValueW(hKey, REG_CUSTOM);
        RegCloseKey(hKey);
        return;
    }
    std::vector<WCHAR> buf;
    for (int i = 0; i < g_customCount; ++i) {
        buf.insert(buf.end(), g_custom[i].name, g_custom[i].name + wcslen(g_custom[i].name));
        buf.push_back(L'|');
        buf.insert(buf.end(), g_custom[i].match, g_custom[i].match + wcslen(g_custom[i].match));
        buf.push_back(0);
    }
    buf.push_back(0);
    RegSetValueExW(hKey, REG_CUSTOM, 0, REG_MULTI_SZ, (const BYTE*)buf.data(), (DWORD)(buf.size() * sizeof(WCHAR)));
    RegCloseKey(hKey);
}

static void LoadCustomDevicesLocked() {
    g_customCount = 0;
    for (int i = 0; i < MAX_CUSTOM; ++i) g_customPresent[i] = false;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(hKey, REG_CUSTOM, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && size > 2 * sizeof(WCHAR)) {
        std::vector<WCHAR> buf(size / sizeof(WCHAR) + 2, 0);
        if (RegQueryValueExW(hKey, REG_CUSTOM, NULL, &type, (LPBYTE)buf.data(), &size) == ERROR_SUCCESS) {
            const WCHAR* p = buf.data();
            while (*p && g_customCount < MAX_CUSTOM) {
                const WCHAR* sep = wcschr(p, L'|');
                if (sep) {
                    size_t nlen = (size_t)(sep - p);
                    if (nlen >= ARRAYSIZE(g_custom[0].name)) nlen = ARRAYSIZE(g_custom[0].name) - 1;
                    wcsncpy(g_custom[g_customCount].name, p, nlen);
                    g_custom[g_customCount].name[nlen] = 0;
                    StringCchCopyW(g_custom[g_customCount].match, ARRAYSIZE(g_custom[0].match), sep + 1);
                    if (g_custom[g_customCount].name[0]) g_customCount++;
                }
                p += wcslen(p) + 1;
            }
        }
    }
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// HID interface helpers
// ---------------------------------------------------------------------------

// Group key: the interface identity with the collection index stripped, so all
// sibling collections of one HID++ interface end up in the same group.
static void GroupKeyFromPath(const WCHAR* path, WCHAR* out, int maxLen) {
    WCHAR lower[MAX_PATH];
    StringCchCopyW(lower, MAX_PATH, path);
    _wcslwr_s(lower, MAX_PATH);

    const WCHAR* first = wcschr(lower + 1, L'#');
    if (!first) { StringCchCopyW(out, maxLen, lower); return; }
    const WCHAR* start = first + 1;
    const WCHAR* end = wcschr(start, L'#');
    if (!end) end = lower + wcslen(lower);
    const WCHAR* col = wcsstr(start, L"&col");
    if (col && col < end) end = col;

    size_t len = (size_t)(end - start);
    if (len >= (size_t)maxLen) len = maxLen - 1;
    wcsncpy(out, start, len);
    out[len] = 0;
}

static void ResolveModeName(const WCHAR* key, BYTE devNumber, WCHAR* out, int maxLen) {
    for (const auto& m : RECEIVER_MODES) {
        if (wcsstr(key, m.pid)) {
            StringCchCopyW(out, maxLen, m.name);
            return;
        }
    }
    StringCchCopyW(out, maxLen, (devNumber == Logi::DEV_INDEX_DIRECT) ? L"USB 直连" : L"无线接收器");
}

// FILE_FLAG_OVERLAPPED is required: without it the HID driver treats ReadFile as
// a blocking read and never times out when the receiver is idle.
static HANDLE OpenDevice(const WCHAR* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    return h;
}

// ---------------------------------------------------------------------------
// Device registry
// ---------------------------------------------------------------------------

static KnownDev* FindDev(const WCHAR* id) {
    for (int i = 0; i < g_devCount; ++i) {
        if (EqualsNoCase(g_devs[i].state.id, id)) return &g_devs[i];
    }
    return NULL;
}

static KnownDev* AddDev(const WCHAR* id) {
    if (g_devCount >= MAX_DEVICES) return NULL;
    KnownDev* d = &g_devs[g_devCount++];
    // Value-initialize so State's defaults survive. ZeroMemory would leave
    // battery at 0, which reads as a real "0 %" level and trips the low-battery
    // alert for devices that expose no battery feature at all.
    *d = KnownDev();
    StringCchCopyW(d->state.id, ARRAYSIZE(d->state.id), id);
    return d;
}

static void RemoveDevAt(int index) {
    if (index < 0 || index >= g_devCount) return;
    for (int i = index; i < g_devCount - 1; ++i) g_devs[i] = g_devs[i + 1];
    g_devCount--;
}

static TrackedGroup* FindGroup(const WCHAR* key) {
    for (int i = 0; i < g_groupCount; ++i) {
        if (g_groups[i].open && EqualsNoCase(g_groups[i].key, key)) return &g_groups[i];
    }
    return NULL;
}

static void CloseAllGroups() {
    for (int i = 0; i < g_groupCount; ++i) {
        if (g_groups[i].open) Logi::CloseGroup(g_groups[i].group);
        g_groups[i].open = false;
    }
    g_groupCount = 0;
}

// Register any device slot on this group that answers a HID++ ping.
static void ScanGroup(TrackedGroup& g) {
    if (!g.open || !Logi::GroupHasWritable(g.group)) return;

    BYTE candidates[1 + MAX_SLOTS];
    int nCand = 0;
    candidates[nCand++] = Logi::DEV_INDEX_DIRECT;
    for (BYTE s = 1; s <= MAX_SLOTS; ++s) candidates[nCand++] = s;

    for (int i = 0; i < nCand; ++i) {
        BYTE dev = candidates[i];
        WCHAR id[96];
        StringCchPrintfW(id, ARRAYSIZE(id), L"%s#%02X", g.key, dev);
        if (FindDev(id)) continue;

        int major = 0, minor = 0;
        if (Logi::GroupPing(g.group, dev, major, minor, 350) != Logi::OUTCOME_ANSWER) continue;

        KnownDev* d = AddDev(id);
        if (!d) continue;
        StringCchCopyW(d->groupKey, ARRAYSIZE(d->groupKey), g.key);
        d->devNumber = dev;
        d->state.kind = KIND_LOGI;
        d->state.isConnected = true;
        ResolveModeName(g.key, dev, d->state.modeName, ARRAYSIZE(d->state.modeName));

        WCHAR nm[64] = {0};
        if (Logi::GroupReadDeviceName(g.group, dev, nm, ARRAYSIZE(nm))) {
            StringCchCopyW(d->state.modelName, ARRAYSIZE(d->state.modelName), nm);
        }
        if (d->state.modelName[0] == 0) {
            StringCchCopyW(d->state.modelName, ARRAYSIZE(d->state.modelName), L"罗技设备");
        }

        char nameUtf[128] = {0}, modeUtf[64] = {0};
        WideToUtf8(d->state.modelName, nameUtf, sizeof(nameUtf));
        WideToUtf8(d->state.modeName, modeUtf, sizeof(modeUtf));
        DebugLog("device found: %s | mode=%s | dev=0x%02X | proto=%d.%d",
                 nameUtf, modeUtf, dev, major, minor);
    }
}

// Re-enumerate Logitech HID interfaces and rebuild the group handle set.
// Known devices survive; only their transport handles are refreshed.
static void RediscoverAll() {
    CloseAllGroups();

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA did = {0};
    did.cbSize = sizeof(did);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &did); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &did, det, reqSize, NULL, NULL)) {
            WCHAR lower[MAX_PATH];
            StringCchCopyW(lower, MAX_PATH, det->DevicePath);
            _wcslwr_s(lower, MAX_PATH);

            if (wcsstr(lower, LOGITECH_VID)) {
                WCHAR key[64] = {0};
                GroupKeyFromPath(det->DevicePath, key, ARRAYSIZE(key));
                TrackedGroup* g = FindGroup(key);
                if (!g) {
                    for (int k = 0; k < g_groupCount; ++k) {
                        if (EqualsNoCase(g_groups[k].key, key)) { g = &g_groups[k]; break; }
                    }
                }
                if (!g && g_groupCount < MAX_GROUPS) {
                    g = &g_groups[g_groupCount++];
                    ZeroMemory(g, sizeof(TrackedGroup));
                    StringCchCopyW(g->key, ARRAYSIZE(g->key), key);
                }
                if (g) {
                    HANDLE h = OpenDevice(det->DevicePath);
                    if (h != INVALID_HANDLE_VALUE) {
                        if (Logi::GroupAddHandle(g->group, h)) g->open = true;
                        else CloseHandle(h);
                    }
                }
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);

    // Simulation mode: add receivers that answer HID++ in-process so the
    // multi-device paths can be exercised on a machine with only one device.
    if (Mock::Enabled()) {
        int added = 0;
        for (int r = 0; r < Mock::ReceiverCount() && g_groupCount < MAX_GROUPS; ++r) {
            TrackedGroup* g = &g_groups[g_groupCount++];
            ZeroMemory(g, sizeof(TrackedGroup));
            StringCchCopyW(g->key, ARRAYSIZE(g->key), Mock::ReceiverKey(r));
            // Two collections per receiver, mirroring how real hardware splits
            // the write endpoint from the reply endpoint.
            for (int c = 0; c < 2; ++c) {
                HANDLE h = Mock::MakeHandle(r, c);
                if (Logi::GroupAddHandle(g->group, h)) g->open = true;
            }
            if (g->open) added++;
        }
        DebugLog("sim: appended %d synthetic receiver(s)", added);
    }

    for (int i = 0; i < g_groupCount; ++i) {
        if (g_groups[i].open) {
            ScanGroup(g_groups[i]);
            g_groups[i].lastScan = GetTickCount();
        }
    }
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

// Resolve the battery feature once, then read the level.
// Returns false when the device did not answer at all.
static bool PollDev(KnownDev& d) {
    TrackedGroup* g = FindGroup(d.groupKey);
    if (!g) return false;

    if (!d.featuresResolved) {
        BYTE idx = 0;
        Logi::Outcome o1 = Logi::GroupGetFeatureInfo(g->group, d.devNumber, Logi::FEATURE_UNIFIED_BATTERY, idx, 500);
        if (o1 == Logi::OUTCOME_ANSWER && idx != 0) {
            d.batteryFeatureId = Logi::FEATURE_UNIFIED_BATTERY;
            d.batteryFeatureIndex = idx;
            d.featuresResolved = true;
        } else {
            BYTE idx2 = 0;
            Logi::Outcome o2 = Logi::GroupGetFeatureInfo(g->group, d.devNumber, Logi::FEATURE_BATTERY_LEVEL_STATUS, idx2, 500);
            if (o2 == Logi::OUTCOME_ANSWER && idx2 != 0) {
                d.batteryFeatureId = Logi::FEATURE_BATTERY_LEVEL_STATUS;
                d.batteryFeatureIndex = idx2;
                d.featuresResolved = true;
            } else if (o1 == Logi::OUTCOME_NEGATIVE_ACK || o2 == Logi::OUTCOME_NEGATIVE_ACK) {
                // Device answers but exposes no battery feature at all.
                d.featuresResolved = true;
                d.noBatteryFeature = true;
                d.state.batteryReadable = false;
            } else {
                return false;
            }
        }
        char nameUtf[128] = {0};
        WideToUtf8(d.state.modelName, nameUtf, sizeof(nameUtf));
        DebugLog("battery feature: %s = 0x%04X@%u (readable=%d)", nameUtf,
                 d.batteryFeatureId, d.batteryFeatureIndex, d.noBatteryFeature ? 0 : 1);
    }

    if (d.noBatteryFeature) return true;

    int level = -1;
    bool charging = false;
    Logi::Outcome out = Logi::GroupReadBattery(g->group, d.devNumber, d.batteryFeatureId,
                                               d.batteryFeatureIndex, level, charging, 600);
    if (out != Logi::OUTCOME_ANSWER) return false;

    d.state.battery = level;
    d.state.isCharging = charging;
    d.state.batteryReadable = (level >= 0);
    return true;
}

static void PollAll() {
    for (int i = 0; i < g_devCount; ++i) {
        KnownDev& d = g_devs[i];
        if (d.state.kind != KIND_LOGI) continue;

        int prevBattery = d.state.battery;
        bool prevCharging = d.state.isCharging;

        if (PollDev(d)) {
            d.failCount = 0;
            d.offlineSince = 0;
            if (!d.state.isConnected) {
                char nameUtf[128] = {0};
                WideToUtf8(d.state.modelName, nameUtf, sizeof(nameUtf));
                DebugLog("online: %s", nameUtf);
                d.state.isConnected = true;
            }
            if (d.state.battery != prevBattery || d.state.isCharging != prevCharging) {
                char nameUtf[128] = {0};
                WideToUtf8(d.state.modelName, nameUtf, sizeof(nameUtf));
                DebugLog("battery: %s = %d%% %s", nameUtf, d.state.battery,
                         d.state.isCharging ? "charging" : "discharging");
            }
            continue;
        }

        if (d.failCount < 100) d.failCount++;
        if (d.failCount >= 2 && d.state.isConnected) {
            char nameUtf[128] = {0};
            WideToUtf8(d.state.modelName, nameUtf, sizeof(nameUtf));
            DebugLog("offline: %s", nameUtf);
            d.state.isConnected = false;
            d.state.battery = -1;
            d.state.isCharging = false;
            d.offlineSince = GetTickCount();
        } else if (!d.state.isConnected && d.offlineSince == 0) {
            d.offlineSince = GetTickCount();
        }
    }
}

// ---------------------------------------------------------------------------
// Custom (user-added) devices
// ---------------------------------------------------------------------------

static bool ReadDevString(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pData, DWORD prop, WCHAR* out, int maxLen) {
    out[0] = 0;
    DWORD type = 0, size = 0;
    BYTE buf[1024] = {0};
    if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, pData, prop, &type, buf, sizeof(buf), &size)) return false;
    if (type != REG_SZ && type != REG_MULTI_SZ) return false;
    StringCchCopyW(out, maxLen, (const WCHAR*)buf);
    return out[0] != 0;
}

static void UpdateCustomPresence() {
    WCHAR matches[MAX_CUSTOM][48];
    int n = 0;
    EnterCriticalSection(&g_csCustom);
    for (int i = 0; i < g_customCount; ++i) {
        StringCchCopyW(matches[n], 48, g_custom[i].match);
        n++;
    }
    LeaveCriticalSection(&g_csCustom);
    if (n == 0) return;

    bool present[MAX_CUSTOM] = {false};

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd = {0};
        dd.cbSize = sizeof(dd);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &dd); ++i) {
            WCHAR name[256] = {0}, desc[256] = {0}, inst[512] = {0};
            ReadDevString(hDevInfo, &dd, SPDRP_FRIENDLYNAME, name, ARRAYSIZE(name));
            ReadDevString(hDevInfo, &dd, SPDRP_DEVICEDESC, desc, ARRAYSIZE(desc));
            SetupDiGetDeviceInstanceIdW(hDevInfo, &dd, inst, ARRAYSIZE(inst), NULL);

            for (int k = 0; k < n; ++k) {
                if (ContainsNoCase(name, matches[k]) ||
                    ContainsNoCase(desc, matches[k]) ||
                    ContainsNoCase(inst, matches[k])) {
                    present[k] = true;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    EnterCriticalSection(&g_csCustom);
    for (int k = 0; k < n; ++k) g_customPresent[k] = present[k];
    LeaveCriticalSection(&g_csCustom);
}

// Sync the configured custom devices into the known-device list.
static void ApplyCustomStates() {
    CustomEntry cfg[MAX_CUSTOM];
    bool present[MAX_CUSTOM] = {false};
    int n = 0;
    EnterCriticalSection(&g_csCustom);
    for (int i = 0; i < g_customCount; ++i) {
        cfg[n] = g_custom[i];
        present[n] = g_customPresent[i];
        n++;
    }
    LeaveCriticalSection(&g_csCustom);

    // Drop custom devices that were removed from the configuration.
    for (int i = 0; i < g_devCount; ) {
        if (g_devs[i].state.kind == KIND_CUSTOM) {
            bool stillConfigured = false;
            for (int k = 0; k < n; ++k) {
                if (EqualsNoCase(g_devs[i].state.modelName, cfg[k].name)) { stillConfigured = true; break; }
            }
            if (!stillConfigured) { RemoveDevAt(i); continue; }
        }
        ++i;
    }

    for (int k = 0; k < n; ++k) {
        WCHAR id[96];
        StringCchPrintfW(id, ARRAYSIZE(id), L"custom:%s", cfg[k].name);
        KnownDev* d = FindDev(id);
        if (!d) {
            d = AddDev(id);
            if (!d) continue;
            d->state.kind = KIND_CUSTOM;
            d->state.batteryReadable = false;
            StringCchCopyW(d->state.modelName, ARRAYSIZE(d->state.modelName), cfg[k].name);
            StringCchCopyW(d->state.modeName, ARRAYSIZE(d->state.modeName), L"手动添加");
        }
        d->state.isConnected = present[k];
        if (!present[k]) {
            d->state.battery = -1;
            d->state.isCharging = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

static void Publish() {
    State snap[MAX_DEVICES];
    int n = 0;
    for (int i = 0; i < g_devCount && n < MAX_DEVICES; ++i) snap[n++] = g_devs[i].state;

    DWORD masks[MAX_DEVICES] = {0};
    bool listChanged = false;

    EnterCriticalSection(&g_cs);
    if (n != (int)g_snapshot.size()) listChanged = true;
    for (int i = 0; i < n; ++i) {
        const State* old = NULL;
        for (size_t j = 0; j < g_snapshot.size(); ++j) {
            if (EqualsNoCase(g_snapshot[j].id, snap[i].id)) { old = &g_snapshot[j]; break; }
        }
        if (!old) {
            masks[i] = CHANGE_CONNECTED | CHANGE_BATTERY | CHANGE_LIST;
            listChanged = true;
        } else {
            if (old->isConnected != snap[i].isConnected) masks[i] |= CHANGE_CONNECTED;
            if (old->battery != snap[i].battery ||
                old->isCharging != snap[i].isCharging ||
                old->batteryReadable != snap[i].batteryReadable) {
                masks[i] |= CHANGE_BATTERY;
            }
        }
    }
    g_snapshot.assign(snap, snap + n);
    LeaveCriticalSection(&g_cs);

    if (listChanged && n > 0 && masks[0] == 0) masks[0] = CHANGE_LIST;
    if (g_callback) {
        for (int i = 0; i < n; ++i) {
            if (masks[i]) g_callback(snap[i], masks[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

static DWORD ComputeWaitMs() {
    bool anyOffline = false, lowBattery = false;
    for (int i = 0; i < g_devCount; ++i) {
        if (g_devs[i].state.kind != KIND_LOGI) continue;
        if (!g_devs[i].state.isConnected) anyOffline = true;
        else if (g_devs[i].state.battery >= 0 && g_devs[i].state.battery <= 20) lowBattery = true;
    }
    if (anyOffline) return 5000;
    if (lowBattery) return 15000;
    return 30000;
}

static bool AnyOfflineLong(DWORD now) {
    for (int i = 0; i < g_devCount; ++i) {
        if (g_devs[i].state.kind != KIND_LOGI) continue;
        if (!g_devs[i].state.isConnected && g_devs[i].offlineSince != 0 &&
            now - g_devs[i].offlineSince >= 8000) {
            return true;
        }
    }
    return false;
}

static DWORD WINAPI WorkerThread(LPVOID) {
    EnterCriticalSection(&g_csCustom);
    LoadCustomDevicesLocked();
    LeaveCriticalSection(&g_csCustom);

    RediscoverAll();
    ApplyCustomStates();
    Publish();

    DWORD lastCustom = GetTickCount();
    DWORD lastDiscover = GetTickCount();

    for (;;) {
        if (WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0) break;

        PollAll();

        DWORD now = GetTickCount();
        if (now - lastCustom >= 5000 || g_customDirty) {
            g_customDirty = false;
            UpdateCustomPresence();
            ApplyCustomStates();
            Publish();
            lastCustom = now;
        }

        HANDLE hs[2] = { g_hStop, g_hWake };
        DWORD wr = WaitForMultipleObjects(2, hs, FALSE, ComputeWaitMs());
        if (wr == WAIT_OBJECT_0) break;

        now = GetTickCount();
        bool rediscover = false;
        if (wr == WAIT_OBJECT_0 + 1 && g_needRediscover) {
            g_needRediscover = false;
            rediscover = true;
        }
        if (now - lastDiscover >= 60000) rediscover = true;
        if (AnyOfflineLong(now)) rediscover = true;

        if (rediscover) {
            RediscoverAll();
            lastDiscover = GetTickCount();
        }

        // Pick up a device that was asleep when the group was first scanned.
        for (int i = 0; i < g_groupCount; ++i) {
            if (!g_groups[i].open) continue;
            if (now - g_groups[i].lastScan >= 8000) {
                ScanGroup(g_groups[i]);
                g_groups[i].lastScan = now;
            }
        }
    }

    CloseAllGroups();
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool Start(HWND hNotifyWnd, StateCallback callback) {
    (void)hNotifyWnd;
    g_callback = callback;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_csCustom);

    g_hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hWake = CreateEventW(NULL, FALSE, FALSE, NULL);

    g_hWorker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    return (g_hWorker != NULL);
}

void Stop() {
    if (g_hStop) SetEvent(g_hStop);
    if (g_hWorker) {
        WaitForSingleObject(g_hWorker, 5000);
        CloseHandle(g_hWorker);
        g_hWorker = NULL;
    }
    if (g_hStop) { CloseHandle(g_hStop); g_hStop = NULL; }
    if (g_hWake) { CloseHandle(g_hWake); g_hWake = NULL; }
    EnterCriticalSection(&g_cs);
    g_snapshot.clear();
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    DeleteCriticalSection(&g_csCustom);
}

void NotifyDeviceChange() {
    g_needRediscover = true;
    if (g_hWake) SetEvent(g_hWake);
}

void RequestRefresh() {
    if (g_hWake) SetEvent(g_hWake);
}

int GetDeviceCount() {
    EnterCriticalSection(&g_cs);
    int n = (int)g_snapshot.size();
    LeaveCriticalSection(&g_cs);
    return n;
}

int GetDevices(State* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    EnterCriticalSection(&g_cs);
    int n = 0;
    for (size_t i = 0; i < g_snapshot.size() && n < maxCount; ++i) out[n++] = g_snapshot[i];
    LeaveCriticalSection(&g_cs);
    return n;
}

State GetStateByIndex(int index) {
    State s;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < (int)g_snapshot.size()) s = g_snapshot[index];
    else StringCchCopyW(s.modelName, ARRAYSIZE(s.modelName), L"未检测到设备");
    LeaveCriticalSection(&g_cs);
    return s;
}

void SetActiveDeviceId(const WCHAR* id) {
    EnterCriticalSection(&g_cs);
    if (id && id[0]) StringCchCopyW(g_activeId, ARRAYSIZE(g_activeId), id);
    else g_activeId[0] = 0;
    LeaveCriticalSection(&g_cs);
}

const WCHAR* GetActiveDeviceId() {
    static WCHAR buf[96];
    EnterCriticalSection(&g_cs);
    StringCchCopyW(buf, ARRAYSIZE(buf), g_activeId);
    LeaveCriticalSection(&g_cs);
    return buf;
}

State GetActiveState() {
    State result;
    EnterCriticalSection(&g_cs);
    bool found = false;
    if (g_activeId[0]) {
        for (size_t i = 0; i < g_snapshot.size(); ++i) {
            if (EqualsNoCase(g_snapshot[i].id, g_activeId)) { result = g_snapshot[i]; found = true; break; }
        }
    }
    if (!found) {
        for (size_t i = 0; i < g_snapshot.size(); ++i) {
            if (g_snapshot[i].isConnected) { result = g_snapshot[i]; found = true; break; }
        }
    }
    if (!found && !g_snapshot.empty()) result = g_snapshot[0];
    if (!found && g_snapshot.empty()) {
        result.isConnected = false;
        result.battery = -1;
        StringCchCopyW(result.modelName, ARRAYSIZE(result.modelName), L"未检测到设备");
    }
    LeaveCriticalSection(&g_cs);
    return result;
}

int GetCustomDeviceCount() {
    EnterCriticalSection(&g_csCustom);
    int n = g_customCount;
    LeaveCriticalSection(&g_csCustom);
    return n;
}

bool GetCustomDevice(int index, WCHAR* name, int nameMax, WCHAR* match, int matchMax) {
    bool ok = false;
    EnterCriticalSection(&g_csCustom);
    if (index >= 0 && index < g_customCount) {
        if (name && nameMax > 0) StringCchCopyW(name, nameMax, g_custom[index].name);
        if (match && matchMax > 0) StringCchCopyW(match, matchMax, g_custom[index].match);
        ok = true;
    }
    LeaveCriticalSection(&g_csCustom);
    return ok;
}

bool AddCustomDevice(const WCHAR* name, const WCHAR* match) {
    if (!name || !name[0] || !match || !match[0]) return false;

    bool ok = false;
    EnterCriticalSection(&g_csCustom);
    if (g_customCount < MAX_CUSTOM) {
        bool dup = false;
        for (int i = 0; i < g_customCount; ++i) {
            if (EqualsNoCase(g_custom[i].name, name)) { dup = true; break; }
        }
        if (!dup) {
            StringCchCopyW(g_custom[g_customCount].name, ARRAYSIZE(g_custom[0].name), name);
            StringCchCopyW(g_custom[g_customCount].match, ARRAYSIZE(g_custom[0].match), match);
            g_customCount++;
            SaveCustomDevicesLocked();
            ok = true;
        }
    }
    LeaveCriticalSection(&g_csCustom);

    if (ok) {
        g_customDirty = true;
        RequestRefresh();
    }
    return ok;
}

bool RemoveCustomDevice(int index) {
    bool ok = false;
    EnterCriticalSection(&g_csCustom);
    if (index >= 0 && index < g_customCount) {
        for (int i = index; i < g_customCount - 1; ++i) g_custom[i] = g_custom[i + 1];
        g_customCount--;
        g_customPresent[g_customCount] = false;
        SaveCustomDevicesLocked();
        ok = true;
    }
    LeaveCriticalSection(&g_csCustom);
    if (ok) {
        g_customDirty = true;
        RequestRefresh();
    }
    return ok;
}

} // namespace Device