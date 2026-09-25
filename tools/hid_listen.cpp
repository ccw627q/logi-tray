// hid_listen.cpp — passively read input reports from vendor HID interfaces (no writes).
// Usage: hid_listen.exe <vid_hex> [seconds]
// Build: g++ -O2 -std=c++17 -municode hid_listen.cpp -o hid_listen.exe -lhid -lsetupapi
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdio.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static volatile LONG g_stop = 0;
static DWORD g_t0 = 0;

static void PrintHex(const BYTE* p, int n) {
    for (int i = 0; i < n; ++i) printf("%02X ", p[i]);
}

struct Ctx {
    HANDLE h;
    DWORD inLen;
    WCHAR name[64];
};

static DWORD WINAPI ReaderThread(LPVOID param) {
    Ctx* c = (Ctx*)param;
    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BYTE buf[256];
    int reportCount = 0;
    while (!g_stop) {
        OVERLAPPED ov = {0};
        ov.hEvent = hEvent;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(c->h, buf, c->inLen, &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Wait in small slices so we can notice the stop flag.
                DWORD waited = 0;
                while (waited < 500) {
                    DWORD r = WaitForSingleObject(hEvent, 100);
                    if (r == WAIT_OBJECT_0) break;
                    if (g_stop) break;
                    waited += 100;
                }
                if (g_stop) { CancelIo(c->h); break; }
                if (waited >= 500) {
                    // still idle: cancel this pending read and re-arm
                    CancelIo(c->h);
                    WaitForSingleObject(hEvent, 200);
                    ResetEvent(hEvent);
                    continue;
                }
                if (!GetOverlappedResult(c->h, &ov, &bytesRead, FALSE)) {
                    ResetEvent(hEvent);
                    continue;
                }
            } else {
                break;
            }
        }
        ResetEvent(hEvent);
        if (bytesRead > 0) {
            DWORD ms = GetTickCount() - g_t0;
            printf("[%5lu.%03lu ms] [%ls] #%d (%lu bytes): ",
                   ms / 1000, ms % 1000, c->name, ++reportCount, bytesRead);
            PrintHex(buf, (int)bytesRead);
            printf("\n");
            fflush(stdout);
        }
    }
    CloseHandle(hEvent);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    WCHAR filter[32] = L"vid_";
    if (argc >= 2) wcscat_s(filter, 32, argv[1]);
    else wcscat_s(filter, 32, L"046d");
    _wcslwr_s(filter, 32);

    int seconds = (argc >= 3) ? _wtoi(argv[2]) : 6;
    printf("=== passive HID listen, filter=%ls, %d s ===\n", filter, seconds);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return 1;

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(devData);
    HANDLE threads[16] = {0};
    Ctx* ctxs[16] = {0};
    int nt = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &hidGuid, i, &devData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, NULL, 0, &reqSize, NULL);
        if (!reqSize) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &devData, pDetail, reqSize, NULL, NULL)) {
            WCHAR lower[MAX_PATH];
            wcscpy_s(lower, MAX_PATH, pDetail->DevicePath);
            _wcslwr_s(lower, MAX_PATH);
            if (wcsstr(lower, filter)) {
                HANDLE h = CreateFileW(pDetail->DevicePath, GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    PHIDP_PREPARSED_DATA pData = NULL;
                    HIDP_CAPS caps = {0};
                    if (HidD_GetPreparsedData(h, &pData)) {
                        HidP_GetCaps(pData, &caps);
                        HidD_FreePreparsedData(pData);
                    }
                    // listen on every interface that can produce input reports
                    if (caps.InputReportByteLength > 0 && nt < 16) {
                        const WCHAR* tail = wcsrchr(pDetail->DevicePath, L'#');
                        ctxs[nt] = (Ctx*)calloc(1, sizeof(Ctx));
                        ctxs[nt]->h = h;
                        ctxs[nt]->inLen = caps.InputReportByteLength;
                        wcsncpy_s(ctxs[nt]->name, 64, tail ? tail + 1 : L"?", 63);
                        printf("listening on usagePage=%04X inLen=%lu : %ls\n",
                               caps.UsagePage, caps.InputReportByteLength, ctxs[nt]->name);
                        fflush(stdout);
                        nt++;
                    } else {
                        CloseHandle(h);
                    }
                }
            }
        }
        free(pDetail);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);

    if (nt == 0) { printf("no HID input interface found\n"); return 0; }

    g_t0 = GetTickCount();
    for (int i = 0; i < nt; ++i) {
        threads[i] = CreateThread(NULL, 0, ReaderThread, ctxs[i], 0, NULL);
    }

    Sleep(seconds * 1000);
    InterlockedExchange(&g_stop, 1);
    for (int i = 0; i < nt; ++i) {
        CancelIo(ctxs[i]->h);
    }
    for (int i = 0; i < nt; ++i) {
        WaitForSingleObject(threads[i], 2000);
        CloseHandle(ctxs[i]->h);
        free(ctxs[i]);
    }
    printf("=== done ===\n");
    fflush(stdout);
    return 0;
}