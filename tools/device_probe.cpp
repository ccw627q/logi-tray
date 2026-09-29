// device_probe.cpp — diagnostic tool: enumerate Logitech HID interfaces and probe HID++ 2.0
// Build: g++ -O2 -std=c++17 -municode device_probe.cpp logi_protocol.cpp -o device_probe.exe -lhid -lsetupapi
#include "logi_protocol.h"
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdio.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static void PrintHex(const BYTE* p, int n) {
    for (int i = 0; i < n; ++i) printf("%02X ", p[i]);
}

static bool GetReportLengths(HANDLE hDev, DWORD& inLen, DWORD& outLen) {
    PHIDP_PREPARSED_DATA pData = NULL;
    if (!HidD_GetPreparsedData(hDev, &pData)) return false;
    HIDP_CAPS caps;
    bool ok = (HidP_GetCaps(pData, &caps) == HIDP_STATUS_SUCCESS);
    HidD_FreePreparsedData(pData);
    if (!ok) return false;
    inLen = caps.InputReportByteLength;
    outLen = caps.OutputReportByteLength;
    return (inLen > 0);
}

// Try a HID++ ping with the same write logic as the app (long report on 20-byte
// interfaces, short report otherwise), dumping raw replies
static bool TryPing(HANDLE hDev, BYTE devNumber, int& protoMajor, int& protoMinor, DWORD timeoutMs) {
    DWORD inLen = 0, outLen = 0;
    if (!GetReportLengths(hDev, inLen, outLen) || outLen == 0) return false;

    BYTE report[64] = {0};
    DWORD writeLen = 0;
    if (outLen >= 20) {
        report[0] = 0x11;
        writeLen = 20;
    } else {
        report[0] = 0x10;
        writeLen = outLen;
    }
    report[1] = devNumber;
    report[2] = 0x00;
    report[3] = 0x1B;       // ping | swid
    report[6] = 0x5A;       // mark
    printf("    sent(rid=%02X len=%lu): ", report[0], writeLen);
    PrintHex(report, (int)writeLen);
    printf("\n");

    OVERLAPPED ovW = {0};
    ovW.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD written = 0;
    BOOL wok = WriteFile(hDev, report, writeLen, &written, &ovW);
    if (!wok) {
        DWORD werr = GetLastError();
        if (werr != ERROR_IO_PENDING || !GetOverlappedResult(hDev, &ovW, &written, TRUE)) {
            printf("    write FAILED err=%lu\n", werr);
            CloseHandle(ovW.hEvent);
            return false;
        }
    }
    CloseHandle(ovW.hEvent);

    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BYTE buf[64] = {0};
    DWORD deadline = GetTickCount() + timeoutMs;
    bool matched = false;
    while (GetTickCount() < deadline) {
        DWORD remain = (deadline > GetTickCount()) ? (deadline - GetTickCount()) : 0;
        OVERLAPPED ov = {0};
        ov.hEvent = hEvent;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hDev, buf, inLen, &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD wr = WaitForSingleObject(hEvent, remain);
                if (wr == WAIT_OBJECT_0) {
                    GetOverlappedResult(hDev, &ov, &bytesRead, FALSE);
                } else {
                    CancelIo(hDev);
                    break;
                }
            } else if (err == ERROR_SUCCESS) {
                ResetEvent(hEvent);
                continue;
            } else {
                break;
            }
        }
        ResetEvent(hEvent);
        if (bytesRead > 0) {
            printf("    recv: "); PrintHex(buf, (int)bytesRead); printf("\n");
        }
        if (bytesRead < 2) continue;

        // match ping reply: payload [0x00, 0x1B, ..., mark, major, minor]
        if (buf[2] == 0x00 && buf[3] == 0x1B && bytesRead >= 7 && buf[6] == 0x5A) {
            protoMajor = (bytesRead >= 9) ? buf[7] : 0;
            protoMinor = (bytesRead >= 9) ? buf[8] : 0;
            matched = true;
            break;
        }
    }
    CloseHandle(hEvent);
    return matched;
}

static void ProbeInterface(const WCHAR* path) {
    // Show tail of path including VID/PID/instance
    const WCHAR* tail = wcsrchr(path, L'#');
    wprintf(L"\n--- Interface: ...%s\n", tail ? tail : path);

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    BOOL rw = (h != INVALID_HANDLE_VALUE);
    if (!rw) {
        h = CreateFileW(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        printf("  open FAILED err=%lu\n", GetLastError());
        return;
    }
    printf("  open %s\n", rw ? "RW" : "READ-ONLY");

    // Only vendor interfaces (FF00/FF01/FFBC) can carry HID++ traffic
    HIDP_CAPS caps = {0};
    PHIDP_PREPARSED_DATA pData = NULL;
    if (HidD_GetPreparsedData(h, &pData)) {
        if (HidP_GetCaps(pData, &caps) == HIDP_STATUS_SUCCESS) {
            printf("  usagePage=%04X usage=%04X inLen=%lu outLen=%lu\n",
                   caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                   caps.OutputReportByteLength);
        }
        HidD_FreePreparsedData(pData);
    }
    if (!rw) {
        CloseHandle(h);
        return;
    }
    bool isVendor = (caps.UsagePage & 0xFF00) == 0xFF00;
    if (!isVendor) {
        printf("  not a vendor interface, skipping\n");
        CloseHandle(h);
        return;
    }
    printf("  HID++ probe:\n");
    BYTE foundDev = 0;
    for (BYTE dev = 0xFF; ; dev++) {
        if (dev == 0xFF) printf("  ping direct(FF):\n");
        else printf("  ping slot %u:\n", (unsigned)dev);
        int maj = -1, minr = -1;
        if (TryPing(h, dev, maj, minr, 400)) {
            printf("    -> OK proto=%d.%d (dev %02X)\n", maj, minr, dev);
            foundDev = dev;
        } else {
            printf("    -> no reply\n");
        }
        Sleep(100);
        if (dev == 4) break;
        if (dev == 0xFF) dev = 0;  // continue with slot 0 -> 1..4
    }

    if (foundDev != 0) {
        printf("  full chain on dev %02X:\n", foundDev);
        BYTE idx = 0;
        WORD batteryFeature = 0;
        if (Logi::FindFeatureIndex(h, foundDev, Logi::FEATURE_UNIFIED_BATTERY, idx, 500)) {
            batteryFeature = Logi::FEATURE_UNIFIED_BATTERY;
            printf("    unified battery (0x1004) at idx=%u\n", idx);
        } else if (Logi::FindFeatureIndex(h, foundDev, Logi::FEATURE_BATTERY_LEVEL_STATUS, idx, 500)) {
            batteryFeature = Logi::FEATURE_BATTERY_LEVEL_STATUS;
            printf("    legacy battery (0x1000) at idx=%u\n", idx);
        } else {
            printf("    NO battery feature found!\n");
        }
        // Dump the raw ROOT.getFeature replies for the features we care about.
        // This runs even when no battery feature was found: "answered with
        // index 0" and "did not answer at all" look identical from the outside
        // and must be told apart, because only the latter means offline.
        {
            Logi::DeviceGroup g;
            Logi::GroupAddHandle(g, h);
            const WORD lookups[] = { 0x1004, 0x1000, 0x0005, 0x1B04 };
            for (WORD feat : lookups) {
                BYTE params[2] = { (BYTE)(feat >> 8), (BYTE)(feat & 0xFF) };
                Logi::HidppReply rr;
                Logi::Outcome out = Logi::GroupRequest(g, foundDev, 0x00, 0x00, params, 2, rr, 500);
                const char* tag = (out == Logi::OUTCOME_ANSWER) ? "ANSWER" :
                                  (out == Logi::OUTCOME_NEGATIVE_ACK) ? "NEGATIVE_ACK" :
                                  (out == Logi::OUTCOME_TIMEOUT) ? "TIMEOUT" :
                                  (out == Logi::OUTCOME_WRITE_FAIL) ? "WRITE_FAIL" : "NONE";
                printf("    lookup %04X: %-12s", feat, tag);
                if (out == Logi::OUTCOME_ANSWER || out == Logi::OUTCOME_NEGATIVE_ACK) {
                    printf(" rid=%02X dev=%02X payload=", rr.reportId, rr.devNumber);
                    PrintHex(rr.payload, rr.payloadLen);
                    if (out == Logi::OUTCOME_ANSWER) printf(" -> idx=%u", rr.payload[2]);
                }
                printf("\n");
                Sleep(50);
            }
        }

        if (idx != 0) {
            int level = -1;
            bool charging = false;
            if (Logi::ReadBattery(h, foundDev, batteryFeature, idx, level, charging)) {
                printf("    battery: %d%% %s\n", level, charging ? "charging" : "discharging");
            } else {
                printf("    battery read FAILED\n");
            }
            // step-by-step decode of ReadBattery's two requests
            Logi::HidppReply r10, r00;
            if (Logi::FeatureRequest(h, foundDev, idx, 0x10, NULL, 0, r10, 500)) {
                printf("    battery func10 reply: rid=%02X dev=%02X payload=", r10.reportId, r10.devNumber);
                PrintHex(r10.payload, r10.payloadLen);
                printf("\n    -> level(byte2)=%d status(byte4)=%d\n", r10.payload[2], r10.payload[4]);
            } else {
                printf("    battery func10 FAILED\n");
            }
            if (Logi::FeatureRequest(h, foundDev, idx, 0x00, NULL, 0, r00, 500)) {
                printf("    battery func00 reply: rid=%02X dev=%02X payload=", r00.reportId, r00.devNumber);
                PrintHex(r00.payload, r00.payloadLen);
                printf("\n    -> level(byte2)=%d status(byte4)=%d\n", r00.payload[2], r00.payload[4]);
            } else {
                printf("    battery func00 FAILED\n");
            }
            // dump raw battery responses (repeated) to verify reply layout and determinism
            for (int i = 0; i < 3; ++i) {
                Logi::HidppReply raw;
                if (Logi::FeatureRequest(h, foundDev, idx, 0x00, NULL, 0, raw, 500)) {
                    printf("    raw battery reply #%d: rid=%02X dev=%02X payload=", i, raw.reportId, raw.devNumber);
                    PrintHex(raw.payload, raw.payloadLen);
                    printf("\n");
                } else {
                    printf("    raw battery request #%d FAILED\n", i);
                }
                Sleep(100);
            }
        }
        WCHAR name[64] = {0};
        if (Logi::ReadDeviceName(h, foundDev, name, 64)) {
            printf("    device name: %ls\n", name);
        }
    }

    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Stress mode: repeat the exact requests the app issues, on the same merged
// group the app builds, so intermittent presence failures can be reproduced.
// ---------------------------------------------------------------------------

static const char* Tag(Logi::Outcome o) {
    switch (o) {
        case Logi::OUTCOME_ANSWER:       return "ANSWER";
        case Logi::OUTCOME_NEGATIVE_ACK: return "NEGATIVE_ACK";
        case Logi::OUTCOME_TIMEOUT:      return "TIMEOUT";
        case Logi::OUTCOME_WRITE_FAIL:   return "WRITE_FAIL";
        default:                         return "NONE";
    }
}

// Mirrors Device::GroupKeyFromPath: interface identity with the collection
// index stripped, so sibling collections land in one group.
static void StressGroupKey(const WCHAR* path, WCHAR* out, int maxLen) {
    WCHAR lower[MAX_PATH];
    wcscpy_s(lower, MAX_PATH, path);
    _wcslwr_s(lower, MAX_PATH);
    const WCHAR* first = wcschr(lower + 1, L'#');
    if (!first) { wcsncpy_s(out, maxLen, lower, _TRUNCATE); return; }
    const WCHAR* start = first + 1;
    const WCHAR* end = wcschr(start, L'#');
    if (!end) end = lower + wcslen(lower);
    const WCHAR* col = wcsstr(start, L"&col");
    if (col && col < end) end = col;
    size_t len = (size_t)(end - start);
    if (len >= (size_t)maxLen) len = maxLen - 1;
    wcsncpy_s(out, maxLen, start, len);
    out[len] = 0;
}

struct StressGroup {
    WCHAR key[64];
    Logi::DeviceGroup g;
    int open;
};

static int RunStress(int iterations) {
    printf("=== Logitech HID++ stress (%d iterations, 1 s apart) ===\n", iterations);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed err=%lu\n", GetLastError());
        return 1;
    }

    StressGroup groups[8];
    int nGroups = 0;
    ZeroMemory(groups, sizeof(groups));

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, pDetail, reqSize, NULL, NULL)) {
            WCHAR lower[MAX_PATH];
            wcscpy_s(lower, MAX_PATH, pDetail->DevicePath);
            _wcslwr_s(lower, MAX_PATH);
            if (wcsstr(lower, L"vid_046d")) {
                WCHAR key[64] = {0};
                StressGroupKey(pDetail->DevicePath, key, ARRAYSIZE(key));
                StressGroup* sg = NULL;
                for (int k = 0; k < nGroups; ++k) {
                    if (_wcsicmp(groups[k].key, key) == 0) { sg = &groups[k]; break; }
                }
                if (!sg && nGroups < 8) {
                    sg = &groups[nGroups++];
                    ZeroMemory(sg, sizeof(StressGroup));
                    wcsncpy_s(sg->key, ARRAYSIZE(sg->key), key, _TRUNCATE);
                }
                if (sg) {
                    HANDLE h = CreateFileW(pDetail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                    if (h == INVALID_HANDLE_VALUE) {
                        h = CreateFileW(pDetail->DevicePath, GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                    }
                    if (h != INVALID_HANDLE_VALUE) {
                        if (Logi::GroupAddHandle(sg->g, h)) sg->open = 1;
                        else CloseHandle(h);
                    }
                }
            }
        }
        free(pDetail);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);

    for (int k = 0; k < nGroups; ++k) {
        if (!groups[k].open) continue;
        if (!Logi::GroupHasWritable(groups[k].g)) {
            wprintf(L"\n--- group %s: no writable handle, skipped\n", groups[k].key);
            continue;
        }
        wprintf(L"\n--- group %s (handles=%d) ---\n", groups[k].key, groups[k].g.count);

        int maj = 0, minr = 0;
        if (Logi::GroupPing(groups[k].g, 0xFF, maj, minr, 400) != Logi::OUTCOME_ANSWER) {
            wprintf(L"  no device at 0xFF, skipped\n");
            continue;
        }

        int pingOk = 0, featAnswered = 0;
        for (int i = 0; i < iterations; ++i) {
            int pmaj = 0, pmin = 0;
            Logi::Outcome po = Logi::GroupPing(groups[k].g, 0xFF, pmaj, pmin, 400);
            BYTE idx = 0;
            Logi::Outcome fo = Logi::GroupGetFeatureInfo(groups[k].g, 0xFF, 0x1004, idx, 500);
            if (po == Logi::OUTCOME_ANSWER) pingOk++;
            if (fo == Logi::OUTCOME_ANSWER || fo == Logi::OUTCOME_NEGATIVE_ACK) featAnswered++;
            printf("  #%02d ping=%-13s feat=%-13s idx=%u\n", i, Tag(po), Tag(fo), idx);
            fflush(stdout);
            Sleep(1000);
        }
        printf("  => ping answered %d/%d, feature lookup answered %d/%d\n",
               pingOk, iterations, featAnswered, iterations);
    }

    for (int k = 0; k < nGroups; ++k) {
        if (groups[k].open) Logi::CloseGroup(groups[k].g);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--stress") == 0) {
        int n = (argc >= 3) ? _wtoi(argv[2]) : 20;
        if (n <= 0) n = 20;
        return RunStress(n);
    }

    printf("=== Logitech HID++ probe ===\n");
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed err=%lu\n", GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    int count = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (reqSize == 0) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, pDetail, reqSize, NULL, NULL)) {
            WCHAR lower[MAX_PATH];
            wcscpy_s(lower, MAX_PATH, pDetail->DevicePath);
            _wcslwr_s(lower, MAX_PATH);
            if (wcsstr(lower, L"vid_046d")) {
                count++;
                ProbeInterface(pDetail->DevicePath);
            }
        }
        free(pDetail);
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    printf("\n=== %d Logitech interface(s) found ===\n", count);
    fflush(stdout);
    if (argc < 2) {
        printf("Press Enter to exit...\n");
        getchar();
    }
    return 0;
}
