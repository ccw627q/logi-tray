#include "mock_hidpp.h"
#include <cstring>
#include <cstdlib>

namespace Mock {

static int g_forced = 0;
static DWORD g_startTick = 0;

// ---------------------------------------------------------------------------
// Simulated device table
// ---------------------------------------------------------------------------

struct SimDevice {
    BYTE slot;
    const WCHAR* name;
    WORD batteryFeature;   // 0x1004 unified, 0x1000 legacy, 0 = none at all
    BYTE batteryIndex;
    BYTE level;            // raw level byte reported by the device
    BYTE approxFlags;      // unified-battery coarse flags, used when level == 0
    BYTE status;           // Logi::BATTERY_STATUS_*
    bool sleeps;           // stops answering after SleepyAfterMs()
    BYTE nameIndex;
    // How the device refuses a feature it does not have. Real hardware does
    // both: some answer ROOT.GetFeature with index 0 (wired mice such as the
    // G502), others send an error frame. Covering both keeps the self-test
    // honest about the "device is here but has no battery" case.
    bool absentAsZero;
};

static const BYTE NAME_INDEX = 0x06;

// Receiver 0 mimics a Lightspeed receiver: a wired-style direct device plus
// three paired slots, one of which has no battery feature and one of which is
// empty.
static const SimDevice RX0[] = {
    { 0xFF, L"G304 Lightspeed", 0x1004, 0x08, 87, 0x08, 0, false, NAME_INDEX, false },
    { 0x01, L"MX Master 3S",    0x1004, 0x05, 55, 0x04, 1, false, NAME_INDEX, false },
    { 0x02, L"MX Keys S",       0x1000, 0x03, 72, 0x00, 0, false, NAME_INDEX, true  },
    { 0x03, L"Logi Demo Keys",  0x0000, 0x00,  0, 0x00, 0, false, NAME_INDEX, true  },
};

// Receiver 1 mimics a Unifying receiver: a low-battery mouse, a keyboard that
// only reports a coarse level, and a device that falls asleep.
static const SimDevice RX1[] = {
    { 0x01, L"M720 Triathlon", 0x1004, 0x02, 12, 0x02, 0, false, NAME_INDEX, false },
    { 0x02, L"K380 Keyboard",  0x1004, 0x04,  0, 0x04, 0, false, NAME_INDEX, false },
    { 0x03, L"MX Anywhere 3",  0x1004, 0x07, 64, 0x04, 0, true,  NAME_INDEX, false },
};

struct SimReceiver {
    const WCHAR* key;
    const SimDevice* devs;
    int devCount;
};

static const SimReceiver RECEIVERS[] = {
    { L"sim#c53f#0", RX0, ARRAYSIZE(RX0) },
    { L"sim#c52b#1", RX1, ARRAYSIZE(RX1) },
};

static const int RECEIVER_MAX = ARRAYSIZE(RECEIVERS);
static const DWORD SLEEPY_AFTER_MS = 20000;

static const WCHAR* SLEEPY_ID = L"sim#c52b#1#03";

// Expected published state once every device has settled.
static const Expectation EXPECTATIONS[] = {
    { L"sim#c53f#0#FF", L"G304 Lightspeed",  87, 0, 1, 1 },
    { L"sim#c53f#0#01", L"MX Master 3S",     55, 1, 1, 1 },
    { L"sim#c53f#0#02", L"MX Keys S",        72, 0, 1, 1 },
    { L"sim#c53f#0#03", L"Logi Demo Keys",   -1, 0, 0, 1 },
    { L"sim#c52b#1#01", L"M720 Triathlon",   12, 0, 1, 1 },
    { L"sim#c52b#1#02", L"K380 Keyboard",    65, 0, 1, 1 },
    // Falls asleep: battery goes unknown, and the level is no longer readable
    // either, so only the connection state is asserted here.
    { L"sim#c52b#1#03", L"MX Anywhere 3",    -1, -1, -1, 0 },
};

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------

static int ParseEnv() {
    WCHAR buf[16] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOGI_TRAY_SIM", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return 0;
    int v = _wtoi(buf);
    if (v <= 0) return 0;
    return (v > RECEIVER_MAX) ? RECEIVER_MAX : v;
}

int ReceiverCount() {
    if (g_forced > 0) return g_forced;
    static int cached = -1;
    if (cached < 0) cached = ParseEnv();
    return cached;
}

bool Enabled() {
    return ReceiverCount() > 0;
}

void Enable(int receivers) {
    if (receivers < 0) receivers = 0;
    if (receivers > RECEIVER_MAX) receivers = RECEIVER_MAX;
    g_forced = receivers;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

HANDLE MakeHandle(int receiver, int collection) {
    return (HANDLE)(uintptr_t)(HANDLE_BASE + (uintptr_t)(receiver * 0x10 + collection));
}

static bool InRange(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    return v >= HANDLE_BASE && v < HANDLE_BASE + 0x100;
}

bool IsMockHandle(HANDLE h) {
    return Enabled() && InRange(h);
}

HANDLE MockHandleOf(const Logi::DeviceGroup& g) {
    if (!Enabled()) return NULL;
    for (int i = 0; i < g.count; ++i) {
        if (InRange(g.handles[i].h)) return g.handles[i].h;
    }
    return NULL;
}

const WCHAR* ReceiverKey(int receiver) {
    if (receiver < 0 || receiver >= RECEIVER_MAX) return L"";
    return RECEIVERS[receiver].key;
}

int ExpectationCount() { return ARRAYSIZE(EXPECTATIONS); }

const Expectation* ExpectationAt(int i) {
    if (i < 0 || i >= (int)ARRAYSIZE(EXPECTATIONS)) return NULL;
    return &EXPECTATIONS[i];
}

const WCHAR* SleepyDeviceId() { return SLEEPY_ID; }

DWORD SleepyAfterMs() { return SLEEPY_AFTER_MS; }

// ---------------------------------------------------------------------------
// HID++ request dispatch
// ---------------------------------------------------------------------------

static BYTE FuncLo(BYTE function) {
    return (BYTE)((function & 0xF0) | Logi::SW_ID);
}

static void SetReply(Logi::HidppReply& r, BYTE devNumber, const BYTE* payload, int len) {
    r.reportId = Logi::HIDPP_REPORT_LONG;
    r.devNumber = devNumber;
    r.payloadLen = len;
    memcpy(r.payload, payload, len);
}

// HID++ 2.0 error frame: the device echoes the request it refused.
static Logi::Outcome Unsupported(Logi::HidppReply& r, BYTE devNumber, BYTE reqHi, BYTE reqLo) {
    BYTE p[4] = { 0xFF, reqHi, reqLo, 0x00 };
    SetReply(r, devNumber, p, 4);
    return Logi::OUTCOME_NEGATIVE_ACK;
}

static BYTE FeatureIndexOf(const SimDevice& d, WORD feature) {
    if (feature == Logi::FEATURE_ROOT) return 0x01;
    if (feature == Logi::FEATURE_DEVICE_NAME) return d.nameIndex;
    if (d.batteryFeature && feature == d.batteryFeature) return d.batteryIndex;
    return 0;
}

Logi::Outcome Request(HANDLE h, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                      const BYTE* params, int paramLen, Logi::HidppReply& reply) {
    if (!InRange(h)) return Logi::OUTCOME_NONE;

    int rIdx = (int)(((uintptr_t)h - HANDLE_BASE) / 0x10);
    if (rIdx < 0 || rIdx >= ReceiverCount()) return Logi::OUTCOME_NONE;
    const SimReceiver& rx = RECEIVERS[rIdx];

    if (g_startTick == 0) g_startTick = GetTickCount();

    const SimDevice* d = NULL;
    for (int i = 0; i < rx.devCount; ++i) {
        if (rx.devs[i].slot == devNumber) d = &rx.devs[i];
    }
    // Empty slot: the receiver answers with an error frame, same as hardware.
    if (!d) return Unsupported(reply, devNumber, reqHi, reqLo);

    if (d->sleeps && (GetTickCount() - g_startTick) >= SLEEPY_AFTER_MS) {
        return Logi::OUTCOME_TIMEOUT;   // a sleeping device simply stays silent
    }

    // ROOT ping (function 1)
    if (reqHi == 0x00 && reqLo == FuncLo(0x10)) {
        BYTE p[6] = { 0x00, reqLo, 4, 2, 0x00, 0x00 };
        SetReply(reply, devNumber, p, 6);
        return Logi::OUTCOME_ANSWER;
    }

    // ROOT.GetFeatureInfo (function 0)
    if (reqHi == 0x00 && reqLo == FuncLo(0x00)) {
        if (paramLen < 2) return Unsupported(reply, devNumber, reqHi, reqLo);
        WORD feature = (WORD)((params[0] << 8) | params[1]);
        BYTE idx = FeatureIndexOf(*d, feature);
        if (idx == 0 && !d->absentAsZero) return Unsupported(reply, devNumber, reqHi, reqLo);
        BYTE p[6] = { 0x00, reqLo, idx, 0x00, 0x00, 0x00 };
        SetReply(reply, devNumber, p, 6);
        return Logi::OUTCOME_ANSWER;
    }

    // Battery: 0x1004 answers function 1, 0x1000 answers function 0.
    // Checked before DEVICE_NAME because the two share the (feature index,
    // function) request shape.
    if (d->batteryFeature && reqHi == d->batteryIndex) {
        bool unified = (d->batteryFeature == Logi::FEATURE_UNIFIED_BATTERY);
        if (reqLo != FuncLo(unified ? 0x10 : 0x00)) {
            return Unsupported(reply, devNumber, reqHi, reqLo);
        }
        BYTE p[6] = { d->batteryIndex, reqLo, d->level, d->approxFlags, d->status, 0x00 };
        SetReply(reply, devNumber, p, 6);
        return Logi::OUTCOME_ANSWER;
    }

    // DEVICE_NAME: function 0 = length, function 1 = 16-char fragment
    if (d->nameIndex && reqHi == d->nameIndex) {
        int nameLen = (int)wcslen(d->name);
        if (reqLo == FuncLo(0x00)) {
            BYTE p[6] = { d->nameIndex, reqLo, (BYTE)nameLen, 0x00, 0x00, 0x00 };
            SetReply(reply, devNumber, p, 6);
            return Logi::OUTCOME_ANSWER;
        }
        if (reqLo == FuncLo(0x10)) {
            int offset = (paramLen >= 1) ? params[0] : 0;
            BYTE p[20] = {0};
            p[0] = d->nameIndex;
            p[1] = reqLo;
            int n = 0;
            while (n < 16 && offset + n < nameLen) {
                p[2 + n] = (BYTE)d->name[offset + n];
                ++n;
            }
            if (n == 0) return Unsupported(reply, devNumber, reqHi, reqLo);
            SetReply(reply, devNumber, p, 2 + n);
            return Logi::OUTCOME_ANSWER;
        }
    }

    return Unsupported(reply, devNumber, reqHi, reqLo);
}

} // namespace Mock