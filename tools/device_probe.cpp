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
        if (idx != 0) {
            int level = -1;
            bool charging = false;
            if (Logi::ReadBattery(h, foundDev, batteryFeature, idx, level, charging)) {
                printf("    battery: %d%% %s\n", level, charging ? "charging" : "discharging");
            } else {
                printf("    battery read FAILED\n");
            }
            // dump raw root feature table lookups to identify the battery feature
            {
                const WORD lookups[] = { 0x1004, 0x1000, 0x0005, 0x1B04 };
                for (WORD feat : lookups) {
                    BYTE params[2] = { (BYTE)(feat >> 8), (BYTE)(feat & 0xFF) };
                    Logi::HidppReply rr;
                    if (Logi::FeatureRequest(h, foundDev, 0x00, 0x00, params, 2, rr, 500)) {
                        printf("    root lookup %04X: rid=%02X dev=%02X payload=", feat, rr.reportId, rr.devNumber);
                        PrintHex(rr.payload, rr.payloadLen);
                        printf("  -> idx=%u\n", rr.payload[2]);
                    } else {
                        printf("    root lookup %04X FAILED\n", feat);
                    }
                    Sleep(50);
                }
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

int wmain(int argc, wchar_t** argv) {
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
