#pragma once
#include <windows.h>
#include <cstdint>

namespace Logi {

// Logitech HID++ protocol constants
constexpr BYTE HIDPP_REPORT_SHORT = 0x10;   // Short message report ID
constexpr BYTE HIDPP_REPORT_LONG  = 0x11;   // Long message report ID
constexpr BYTE DEV_INDEX_DIRECT   = 0xFF;   // Directly-connected device index
constexpr BYTE SW_ID              = 0x0B;   // Software ID (1-15, matched in replies)

// HID++ 2.0 features
constexpr WORD FEATURE_ROOT                 = 0x0000;
constexpr WORD FEATURE_DEVICE_NAME          = 0x0005;
constexpr WORD FEATURE_BATTERY_LEVEL_STATUS = 0x1000; // legacy Battery Level Status
constexpr WORD FEATURE_UNIFIED_BATTERY      = 0x1004; // modern Unified Battery

// HID++ batteryStatus enumeration
constexpr BYTE BATTERY_STATUS_DISCHARGING = 0;
constexpr BYTE BATTERY_STATUS_RECHARGING  = 1;

constexpr int MAX_GROUP_HANDLES = 8;

// Outcome of one HID++ exchange. Distinguishing these lets the caller tell a
// dead link (fast failure) apart from a device that is merely asleep.
enum Outcome {
    OUTCOME_NONE = 0,      // nothing to talk to
    OUTCOME_ANSWER,        // well-formed reply to this exact request
    OUTCOME_NEGATIVE_ACK,  // device answered "unsupported" (0x8F / 2.0 error frame)
    OUTCOME_TIMEOUT,       // no reply within the deadline
    OUTCOME_WRITE_FAIL,    // every write attempt was rejected
};

struct HidppReply {
    BYTE reportId = 0;
    BYTE devNumber = 0;
    BYTE payload[20] = {0};  // data after [reportId, devNumber]
    int payloadLen = 0;
};

struct GroupHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD inLen = 0;
    DWORD outLen = 0;
};

// One physical HID++ interface plus its sibling collections.
//
// Logitech receivers split the write endpoint and the reply endpoint across
// collections of the same interface (and long replies from a protocol 4.2
// device only surface on one of them), so a request must be written on one
// handle and read back from every handle in the group.
struct DeviceGroup {
    GroupHandle handles[MAX_GROUP_HANDLES];
    int count = 0;
};

bool GroupAddHandle(DeviceGroup& g, HANDLE h);
bool GroupHasWritable(const DeviceGroup& g);
void CloseGroup(DeviceGroup& g);

Outcome GroupRequest(DeviceGroup& g, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                     const BYTE* params, int paramLen,
                     HidppReply& reply, DWORD timeoutMs);

// HID++ 1.0 ping (ROOT function 1). Returns OUTCOME_ANSWER when the addressed
// device slot exists and replied.
Outcome GroupPing(DeviceGroup& g, BYTE devNumber, int& protoMajor, int& protoMinor, DWORD timeoutMs);

// ROOT.GetFeatureInfo: resolves a feature ID to its per-device index.
// outIndex is 0 when the device is online but lacks the feature.
Outcome GroupGetFeatureInfo(DeviceGroup& g, BYTE devNumber, WORD feature,
                            BYTE& outIndex, DWORD timeoutMs);

// Read battery level (%) and charging state.
// featureId picks the report format: 0x1004 uses function 0x10 getBatteryInfo,
// 0x1000 uses function 0x00 getBatteryLevelStatus (its function 0x10 is a
// capability query, not a level report).
Outcome GroupReadBattery(DeviceGroup& g, BYTE devNumber, WORD featureId, BYTE featureIndex,
                         int& levelPercent, bool& isCharging, DWORD timeoutMs);

bool GroupReadDeviceName(DeviceGroup& g, BYTE devNumber, WCHAR* outName, int maxLen);

// ---- single-handle helpers (diagnostics / tools) ----
bool FeatureRequest(HANDLE hDev, BYTE devNumber, BYTE featureIndex, BYTE function,
                    const BYTE* params, int paramLen, HidppReply& reply, DWORD timeoutMs = 400);
bool Ping(HANDLE hDev, BYTE devNumber, int& protoMajor, int& protoMinor, DWORD timeoutMs = 400);
bool FindFeatureIndex(HANDLE hDev, BYTE devNumber, WORD feature, BYTE& outIndex, DWORD timeoutMs = 400);
bool ReadBattery(HANDLE hDev, BYTE devNumber, WORD featureId, BYTE featureIndex,
                 int& levelPercent, bool& isCharging);
bool ReadDeviceName(HANDLE hDev, BYTE devNumber, WCHAR* outName, int maxLen);

} // namespace Logi