#include "logi_protocol.h"
#include "mock_hidpp.h"
#include <hidsdi.h>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "hid.lib")

namespace Logi {

bool GroupAddHandle(DeviceGroup& g, HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || g.count >= MAX_GROUP_HANDLES) return false;

    // Simulated receivers are not real kernel handles, so they carry their
    // report sizes with them instead of being queried through HidD_*.
    if (Mock::IsMockHandle(h)) {
        g.handles[g.count].h = h;
        g.handles[g.count].inLen = 20;
        g.handles[g.count].outLen = 20;
        g.count++;
        return true;
    }

    PHIDP_PREPARSED_DATA pData = NULL;
    if (!HidD_GetPreparsedData(h, &pData)) return false;
    HIDP_CAPS caps;
    bool ok = (HidP_GetCaps(pData, &caps) == HIDP_STATUS_SUCCESS);
    HidD_FreePreparsedData(pData);
    if (!ok || caps.InputReportByteLength == 0) return false;

    g.handles[g.count].h = h;
    g.handles[g.count].inLen = caps.InputReportByteLength;
    g.handles[g.count].outLen = caps.OutputReportByteLength;
    g.count++;
    return true;
}

bool GroupHasWritable(const DeviceGroup& g) {
    for (int i = 0; i < g.count; ++i) {
        if (g.handles[i].outLen >= 7) return true;
    }
    return false;
}

void CloseGroup(DeviceGroup& g) {
    for (int i = 0; i < g.count; ++i) {
        if (g.handles[i].h != INVALID_HANDLE_VALUE && !Mock::IsMockHandle(g.handles[i].h)) {
            CloseHandle(g.handles[i].h);
        }
        g.handles[i].h = INVALID_HANDLE_VALUE;
    }
    g.count = 0;
}

// ---------------------------------------------------------------------------
// Reply parsing
// ---------------------------------------------------------------------------

// Match a raw HID input report against the request we sent.
// Returns true when the report belongs to this exchange.
static bool ParseReply(const BYTE* buf, DWORD got, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                       HidppReply& reply, Outcome& out) {
    if (got < 3) return false;

    BYTE rid = buf[0];
    if (rid != HIDPP_REPORT_SHORT && rid != HIDPP_REPORT_LONG) return false;

    BYTE rdev = buf[1];
    // Direct devices may echo 0x00 instead of 0xFF
    if (rdev != devNumber && rdev != (BYTE)(devNumber ^ 0xFF)) return false;

    int len = (int)got - 2;
    if (len > 20) len = 20;
    if (len < 2) return false;

    HidppReply r;
    r.reportId = rid;
    r.devNumber = rdev;
    r.payloadLen = len;
    memcpy(r.payload, buf + 2, len);

    // HID++ 1.0 error frame: the receiver answers for empty slots this way
    // instead of staying silent, which makes offline detection instant.
    if (r.payload[0] == 0x8F) {
        reply = r;
        out = OUTCOME_NEGATIVE_ACK;
        return true;
    }
    // HID++ 2.0 error frame echoes the request id after 0xFF
    if (r.payload[0] == 0xFF && len >= 3 && r.payload[1] == reqHi && r.payload[2] == reqLo) {
        reply = r;
        out = OUTCOME_NEGATIVE_ACK;
        return true;
    }
    if (r.payload[0] == reqHi && r.payload[1] == reqLo) {
        reply = r;
        out = OUTCOME_ANSWER;
        return true;
    }
    return false;
}

// Read from every handle in the group concurrently and return the first reply
// that belongs to this request.
static Outcome ReadReplyFromGroup(DeviceGroup& g, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                                  HidppReply& reply, DWORD timeoutMs) {
    struct Pending {
        OVERLAPPED ov;
        HANDLE ev;
        BYTE buf[64];
        DWORD got;
        bool done;
    };
    Pending pend[MAX_GROUP_HANDLES];
    int n = g.count;
    Outcome result = OUTCOME_TIMEOUT;

    for (int i = 0; i < n; ++i) {
        ZeroMemory(&pend[i], sizeof(Pending));
        pend[i].ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        pend[i].ov.hEvent = pend[i].ev;
        pend[i].done = true;
    }

    // Arm one read on handle i. A group may contain busy collections (keyboard
    // or mouse endpoints) that keep streaming unrelated reports, so a
    // non-matching report must be followed by another read instead of leaving
    // the handle drained while the real reply is still on its way.
    auto Arm = [&](int i) -> bool {
        ResetEvent(pend[i].ev);
        pend[i].got = 0;
        DWORD want = (g.handles[i].inLen <= sizeof(pend[i].buf)) ? g.handles[i].inLen : sizeof(pend[i].buf);
        DWORD got = 0;
        if (ReadFile(g.handles[i].h, pend[i].buf, want, &got, &pend[i].ov)) {
            pend[i].got = got;
            pend[i].done = true;
            return true;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            pend[i].done = false;
            return true;
        }
        pend[i].done = true;
        return false;
    };

    for (int i = 0; i < n; ++i) {
        if (!Arm(i)) continue;
        if (pend[i].done && ParseReply(pend[i].buf, pend[i].got, devNumber, reqHi, reqLo, reply, result)) {
            goto cleanup;
        }
    }

    {
        DWORD deadline = GetTickCount() + timeoutMs;
        for (;;) {
            DWORD now = GetTickCount();
            if ((int)(deadline - now) <= 0) break;
            DWORD remain = deadline - now;

            HANDLE evs[MAX_GROUP_HANDLES];
            int map[MAX_GROUP_HANDLES];
            int m = 0;
            for (int i = 0; i < n; ++i) {
                if (!pend[i].done) {
                    evs[m] = pend[i].ev;
                    map[m] = i;
                    ++m;
                }
            }
            if (m == 0) break;

            DWORD wr = WaitForMultipleObjects(m, evs, FALSE, remain);
            if (wr == WAIT_TIMEOUT) break;

            int idx = map[wr - WAIT_OBJECT_0];
            DWORD got = 0;
            if (!GetOverlappedResult(g.handles[idx].h, &pend[idx].ov, &got, FALSE)) got = 0;

            if (got >= 3 && ParseReply(pend[idx].buf, got, devNumber, reqHi, reqLo, reply, result)) break;

            // Unrelated report: re-arm this handle and keep listening.
            if (!Arm(idx)) continue;
            if (pend[idx].done && pend[idx].got >= 3 &&
                ParseReply(pend[idx].buf, pend[idx].got, devNumber, reqHi, reqLo, reply, result)) break;
        }
    }

cleanup:
    // Cancel and drain anything still outstanding so the handles stay reusable.
    for (int i = 0; i < n; ++i) {
        if (!pend[i].done) {
            CancelIoEx(g.handles[i].h, &pend[i].ov);
            DWORD got = 0;
            GetOverlappedResult(g.handles[i].h, &pend[i].ov, &got, TRUE);
        }
        CloseHandle(pend[i].ev);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Group request
// ---------------------------------------------------------------------------

struct WritePlan {
    int handleIdx;
    BYTE reportId;
    DWORD len;
};

static int BuildWritePlans(const DeviceGroup& g, WritePlan* plans) {
    int n = 0;
    // Long reports on a 20-byte interface first: Lightspeed receivers reject
    // short writes there but accept 0x11.
    for (int i = 0; i < g.count; ++i) {
        if (g.handles[i].outLen >= 20) {
            plans[n].handleIdx = i;
            plans[n].reportId = HIDPP_REPORT_LONG;
            plans[n].len = 20;
            ++n;
        }
    }
    // Short reports on a 7-byte interface: the usual write endpoint on
    // receivers, whose reply then arrives on a sibling collection.
    for (int i = 0; i < g.count; ++i) {
        if (g.handles[i].outLen >= 7 && g.handles[i].outLen < 20) {
            plans[n].handleIdx = i;
            plans[n].reportId = HIDPP_REPORT_SHORT;
            plans[n].len = g.handles[i].outLen;
            ++n;
        }
    }
    // Last resort: a short report on a long-report interface.
    for (int i = 0; i < g.count; ++i) {
        if (g.handles[i].outLen >= 20) {
            plans[n].handleIdx = i;
            plans[n].reportId = HIDPP_REPORT_SHORT;
            plans[n].len = 20;
            ++n;
        }
    }
    return n;
}

Outcome GroupRequest(DeviceGroup& g, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                     const BYTE* params, int paramLen,
                     HidppReply& reply, DWORD timeoutMs) {
    if (g.count == 0) return OUTCOME_NONE;

    // Simulated receivers answer in-process; there is no transport to write to.
    HANDLE mockHandle = Mock::MockHandleOf(g);
    if (mockHandle) {
        (void)timeoutMs;
        return Mock::Request(mockHandle, devNumber, reqHi, reqLo, params, paramLen, reply);
    }

    BYTE body[19] = {0};
    body[0] = devNumber;
    body[1] = reqHi;
    body[2] = reqLo;
    for (int i = 0; i < paramLen && i < 3; ++i) {
        body[3 + i] = params[i];
    }

    WritePlan plans[3 * MAX_GROUP_HANDLES];
    int nPlans = BuildWritePlans(g, plans);
    if (nPlans == 0) return OUTCOME_NONE;

    bool anyWriteOk = false;
    for (int p = 0; p < nPlans; ++p) {
        GroupHandle& gh = g.handles[plans[p].handleIdx];
        BYTE report[64] = {0};
        report[0] = plans[p].reportId;
        memcpy(report + 1, body, plans[p].len - 1);

        OVERLAPPED ov = {0};
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        DWORD written = 0;
        BOOL ok = WriteFile(gh.h, report, plans[p].len, &written, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ok = GetOverlappedResult(gh.h, &ov, &written, TRUE);
            }
        }
        CloseHandle(ov.hEvent);
        if (!ok || written != plans[p].len) continue;
        anyWriteOk = true;

        Outcome out = ReadReplyFromGroup(g, devNumber, reqHi, reqLo, reply, timeoutMs);
        if (out == OUTCOME_ANSWER || out == OUTCOME_NEGATIVE_ACK) return out;
    }

    return anyWriteOk ? OUTCOME_TIMEOUT : OUTCOME_WRITE_FAIL;
}

// ---------------------------------------------------------------------------
// Feature helpers
// ---------------------------------------------------------------------------

Outcome GroupPing(DeviceGroup& g, BYTE devNumber, int& protoMajor, int& protoMinor, DWORD timeoutMs) {
    HidppReply reply;
    BYTE reqLo = (BYTE)((0x10 & 0xF0) | SW_ID);  // ROOT function 1 = ping
    Outcome out = GroupRequest(g, devNumber, 0x00, reqLo, NULL, 0, reply, timeoutMs);
    protoMajor = 0;
    protoMinor = 0;
    if (out != OUTCOME_ANSWER) return out;
    protoMajor = (reply.payloadLen >= 4) ? reply.payload[2] : 0;
    protoMinor = (reply.payloadLen >= 4) ? reply.payload[3] : 0;
    return OUTCOME_ANSWER;
}

Outcome GroupGetFeatureInfo(DeviceGroup& g, BYTE devNumber, WORD feature,
                            BYTE& outIndex, DWORD timeoutMs) {
    BYTE params[2] = { (BYTE)(feature >> 8), (BYTE)(feature & 0xFF) };
    BYTE reqLo = (BYTE)((0x00 & 0xF0) | SW_ID);  // ROOT, function 0 = getFeatureInfo

    HidppReply reply;
    Outcome out = GroupRequest(g, devNumber, 0x00, reqLo, params, 2, reply, timeoutMs);
    outIndex = 0;
    if (out == OUTCOME_ANSWER && reply.payloadLen >= 3) {
        outIndex = reply.payload[2];
    }
    return out;
}

Outcome GroupReadBattery(DeviceGroup& g, BYTE devNumber, WORD featureId, BYTE featureIndex,
                         int& levelPercent, bool& isCharging, DWORD timeoutMs) {
    // 0x1004 -> function 1 getBatteryInfo; 0x1000 -> function 0 getBatteryLevelStatus
    BYTE function = (featureId == FEATURE_UNIFIED_BATTERY) ? 0x10 : 0x00;
    BYTE reqLo = (BYTE)((function & 0xF0) | SW_ID);

    HidppReply reply;
    Outcome out = GroupRequest(g, devNumber, featureIndex, reqLo, NULL, 0, reply, timeoutMs);
    if (out != OUTCOME_ANSWER || reply.payloadLen < 3) return out;

    // Payload echoes the header: [featureIndex, func|swid, params...]
    int level = reply.payload[2];
    BYTE status = (reply.payloadLen >= 5) ? reply.payload[4] : BATTERY_STATUS_DISCHARGING;
    if (level <= 0) {
        // Unified Battery devices that report only coarse levels:
        // bit0=critical, bit1=low, bit2=good, bit3=full
        BYTE approx = (featureId == FEATURE_UNIFIED_BATTERY && reply.payloadLen >= 4) ? reply.payload[3] : 0;
        level = (approx & 0x01) ? 10 : (approx & 0x02) ? 30 : (approx & 0x04) ? 65 : (approx & 0x08) ? 100 : -1;
    }
    if (level > 100) level = 100;

    levelPercent = level;   // -1 when the device reports no usable level
    isCharging = (status == BATTERY_STATUS_RECHARGING);
    return OUTCOME_ANSWER;
}

bool GroupReadDeviceName(DeviceGroup& g, BYTE devNumber, WCHAR* outName, int maxLen) {
    if (!outName || maxLen <= 0) return false;
    outName[0] = 0;

    BYTE idx = 0;
    if (GroupGetFeatureInfo(g, devNumber, FEATURE_DEVICE_NAME, idx, 400) != OUTCOME_ANSWER || idx == 0) {
        return false;
    }

    HidppReply reply;
    BYTE reqLen = (BYTE)((0x00 & 0xF0) | SW_ID);  // getDeviceNameLength
    if (GroupRequest(g, devNumber, idx, reqLen, NULL, 0, reply, 400) != OUTCOME_ANSWER || reply.payloadLen < 3) {
        return false;
    }
    int nameLen = reply.payload[2];
    if (nameLen <= 0 || nameLen > 63) nameLen = 63;

    BYTE reqFrag = (BYTE)((0x10 & 0xF0) | SW_ID);  // getDeviceName, 16-char fragments
    char frag[17] = {0};
    int copied = 0;
    int offset = 0;
    while (offset < nameLen) {
        BYTE p[1] = { (BYTE)offset };
        if (GroupRequest(g, devNumber, idx, reqFrag, p, 1, reply, 400) != OUTCOME_ANSWER) break;
        int fragLen = reply.payloadLen - 2;
        if (fragLen > 16) fragLen = 16;
        if (fragLen <= 0) break;
        memcpy(frag, reply.payload + 2, fragLen);
        frag[fragLen] = 0;
        for (int i = 0; i < fragLen && copied < nameLen && copied < maxLen - 1; ++i) {
            outName[copied++] = (unsigned char)frag[i];
        }
        offset += fragLen;
        if (fragLen < 16) break;
    }
    outName[copied] = 0;
    return (copied > 0);
}

// ---------------------------------------------------------------------------
// Single-handle helpers: wrap one handle in a throwaway group
// ---------------------------------------------------------------------------

static Outcome SingleRequest(HANDLE hDev, BYTE devNumber, BYTE reqHi, BYTE reqLo,
                             const BYTE* params, int paramLen,
                             HidppReply& reply, DWORD timeoutMs) {
    DeviceGroup g;
    if (!GroupAddHandle(g, hDev)) return OUTCOME_NONE;
    return GroupRequest(g, devNumber, reqHi, reqLo, params, paramLen, reply, timeoutMs);
}

bool FeatureRequest(HANDLE hDev, BYTE devNumber, BYTE featureIndex, BYTE function,
                    const BYTE* params, int paramLen, HidppReply& reply, DWORD timeoutMs) {
    BYTE reqLo = (BYTE)((function & 0xF0) | SW_ID);
    return SingleRequest(hDev, devNumber, featureIndex, reqLo, params, paramLen, reply, timeoutMs)
           == OUTCOME_ANSWER;
}

bool Ping(HANDLE hDev, BYTE devNumber, int& protoMajor, int& protoMinor, DWORD timeoutMs) {
    HidppReply reply;
    BYTE reqLo = (BYTE)((0x10 & 0xF0) | SW_ID);  // HID++ 1.0 ping, function 1
    if (SingleRequest(hDev, devNumber, 0x00, reqLo, NULL, 0, reply, timeoutMs) != OUTCOME_ANSWER) {
        return false;
    }
    // Reply payload: [0x00, reqLo, major, minor, ...]
    protoMajor = (reply.payloadLen >= 4) ? reply.payload[2] : 0;
    protoMinor = (reply.payloadLen >= 4) ? reply.payload[3] : 0;
    return true;
}

bool FindFeatureIndex(HANDLE hDev, BYTE devNumber, WORD feature, BYTE& outIndex, DWORD timeoutMs) {
    BYTE params[2] = { (BYTE)(feature >> 8), (BYTE)(feature & 0xFF) };
    HidppReply reply;
    if (!FeatureRequest(hDev, devNumber, 0x00, 0x00, params, 2, reply, timeoutMs)) return false;
    outIndex = reply.payload[2];
    return (outIndex != 0);
}

bool ReadBattery(HANDLE hDev, BYTE devNumber, WORD featureId, BYTE featureIndex,
                 int& levelPercent, bool& isCharging) {
    DeviceGroup g;
    if (!GroupAddHandle(g, hDev)) return false;
    return GroupReadBattery(g, devNumber, featureId, featureIndex, levelPercent, isCharging, 400)
           == OUTCOME_ANSWER;
}

bool ReadDeviceName(HANDLE hDev, BYTE devNumber, WCHAR* outName, int maxLen) {
    DeviceGroup g;
    if (!GroupAddHandle(g, hDev)) return false;
    return GroupReadDeviceName(g, devNumber, outName, maxLen);
}

} // namespace Logi