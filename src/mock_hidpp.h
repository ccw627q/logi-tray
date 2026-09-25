#pragma once
#include <windows.h>
#include <cstdint>
#include "logi_protocol.h"

// Simulated Logitech receivers. They answer HID++ requests exactly like real
// hardware, so the whole multi-device pipeline (interface scan -> slot ping ->
// feature resolution -> battery poll -> publish -> tray menu) can be exercised
// without owning several physical devices.
//
// Enabled by the LOGI_TRAY_SIM environment variable (value = number of
// simulated receivers, 1-2) or explicitly by Mock::Enable() from --selftest.
// When disabled nothing here is consulted and real HID traffic is untouched.
namespace Mock {

// Sentinel handle base. Deliberately far outside the range Windows hands out
// for real kernel handles, so a real device can never be mistaken for a
// simulated one.
constexpr uintptr_t HANDLE_BASE = 0x7FF50000;

bool Enabled();
int ReceiverCount();
void Enable(int receivers);

HANDLE MakeHandle(int receiver, int collection);
bool IsMockHandle(HANDLE h);
HANDLE MockHandleOf(const Logi::DeviceGroup& g);

// Group key of a simulated receiver. It embeds a real Logitech PID so the
// regular mode-name resolution reports Lightspeed / Unifying.
const WCHAR* ReceiverKey(int receiver);

// Answer one HID++ request from a simulated device.
Logi::Outcome Request(HANDLE h, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                      const BYTE* params, int paramLen, Logi::HidppReply& reply);

// What the simulation is expected to expose, for self-test assertions.
struct Expectation {
    const WCHAR* id;    // device id as published by Device: "<groupKey>#<slot>"
    const WCHAR* name;
    int battery;        // -1 = unknown
    int charging;       // -1 = don't care
    int readable;       // -1 = don't care
    int connected;
};

int ExpectationCount();
const Expectation* ExpectationAt(int i);

// Slot that stops answering after SleepyAfterMs(), to exercise the path where
// one device falls asleep while the others keep reporting.
const WCHAR* SleepyDeviceId();
DWORD SleepyAfterMs();

} // namespace Mock