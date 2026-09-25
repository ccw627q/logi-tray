#pragma once
#include <windows.h>
#include "logi_protocol.h"

namespace Device {

constexpr int MAX_DEVICES = 12;

enum Kind {
    KIND_LOGI   = 0,   // auto-discovered Logitech HID++ device
    KIND_CUSTOM = 1,   // user-added device, presence tracked by name match
};

struct State {
    Kind kind = KIND_LOGI;
    bool isConnected = false;
    bool isCharging = false;
    int battery = -1;                // -1 = unknown / offline
    bool batteryReadable = true;     // false: protocol exposes no readable level
    WCHAR id[96] = {0};              // stable identity across re-plugs
    WCHAR modelName[64] = {0};       // device friendly name (truncated)
    WCHAR modeName[32] = {0};        // connection mode label (Lightspeed / 优联 / 直连 ...)
};

typedef void (*StateCallback)(const State& state, DWORD changeMask);

constexpr DWORD CHANGE_CONNECTED = 0x01;
constexpr DWORD CHANGE_BATTERY   = 0x02;
constexpr DWORD CHANGE_LIST      = 0x04;

bool Start(HWND hNotifyWnd, StateCallback callback);
void Stop();

// Device arrival/removal notification from WM_DEVICECHANGE.
void NotifyDeviceChange();

// Ask the worker to poll right away instead of waiting for the next interval.
void RequestRefresh();

int GetDeviceCount();
int GetDevices(State* out, int maxCount);
State GetStateByIndex(int index);

// The active device drives the tray icon. Falls back to the first connected
// device, then to the first known device.
void SetActiveDeviceId(const WCHAR* id);
const WCHAR* GetActiveDeviceId();
State GetActiveState();

// ---- user-added devices (persisted in HKCU\Software\logi-tray) ----
int GetCustomDeviceCount();
bool GetCustomDevice(int index, WCHAR* name, int nameMax, WCHAR* match, int matchMax);
bool AddCustomDevice(const WCHAR* name, const WCHAR* match);
bool RemoveCustomDevice(int index);

} // namespace Device