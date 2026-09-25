// hid_desc.cpp — dump HID report descriptors + caps for a given VID filter.
// Usage: hid_desc.exe <vid_hex>   e.g. hid_desc.exe 046d
// Build: g++ -O2 -std=c++17 -municode hid_desc.cpp -o hid_desc.exe -lhid -lsetupapi
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdio.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// hidclass.h is not shipped with MinGW's ddk headers; IOCTL_HID_GET_REPORT_DESCRIPTOR
// is HID_CTL_CODE(0) = CTL_CODE(FILE_DEVICE_KEYBOARD=0x0B, 0, METHOD_NEITHER=3, FILE_ANY_ACCESS=0)
#ifndef IOCTL_HID_GET_REPORT_DESCRIPTOR
#define IOCTL_HID_GET_REPORT_DESCRIPTOR 0x000B0191
#endif

static void DumpDescriptor(HANDLE h) {
    HIDD_ATTRIBUTES attrs = {0};
    attrs.Size = sizeof(attrs);
    if (HidD_GetAttributes(h, &attrs)) {
        printf("  VID=%04X PID=%04X ver=%04X\n", attrs.VendorID, attrs.ProductID, attrs.VersionNumber);
    }

    // Report descriptor via IOCTL_HID_GET_REPORT_DESCRIPTOR
    BYTE desc[1024] = {0};
    DWORD bytes = 0;
    if (DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR, NULL, 0,
                        desc, sizeof(desc), &bytes, NULL) && bytes > 0) {
        printf("  report descriptor (%lu bytes):\n   ", bytes);
        for (DWORD i = 0; i < bytes; ++i) {
            printf("%02X ", desc[i]);
            if ((i + 1) % 16 == 0) printf("\n   ");
        }
        printf("\n");
    } else {
        printf("  report descriptor: FAILED err=%lu\n", GetLastError());
    }

    // Decode usage pages present in the descriptor (first byte of each item pair heuristic):
    // scan for 0x05 (Usage Page, 1-byte) / 0x06 (Usage Page, 2-byte) items
    printf("  usage pages seen:");
    for (DWORD i = 0; i < bytes; ++i) {
        if (desc[i] == 0x05 && i + 1 < bytes) {
            printf(" %02X", desc[i + 1]);
            i += 1;
        } else if (desc[i] == 0x06 && i + 2 < bytes) {
            printf(" %02X%02X", desc[i + 2], desc[i + 1]);
            i += 2;
        }
    }
    printf("\n");
}

// Same rule as device_manager: sibling collections of one interface share a key.
static void GroupKeyFromPath(const WCHAR* path, WCHAR* out, int maxLen) {
    WCHAR lower[MAX_PATH];
    wcscpy_s(lower, MAX_PATH, path);
    _wcslwr_s(lower, MAX_PATH);
    const WCHAR* first = wcschr(lower + 1, L'#');
    if (!first) { wcsncpy_s(out, maxLen, lower, maxLen - 1); return; }
    const WCHAR* start = first + 1;
    const WCHAR* end = wcschr(start, L'#');
    if (!end) end = lower + wcslen(lower);
    const WCHAR* col = wcsstr(start, L"&col");
    if (col && col < end) end = col;
    int len = (int)(end - start);
    if (len >= maxLen) len = maxLen - 1;
    wcsncpy_s(out, maxLen, start, len);
}

int wmain(int argc, wchar_t** argv) {
    WCHAR filter[32] = L"vid_";
    if (argc >= 2) {
        wcscat_s(filter, 32, argv[1]);
    } else {
        wcscat_s(filter, 32, L"046d");
    }
    _wcslwr_s(filter, 32);
    printf("=== HID descriptor dump, filter=%ls ===\n", filter);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return 1;

    SP_DEVICE_INTERFACE_DATA devData = {0};
    devData.cbSize = sizeof(devData);
    int n = 0;
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
                n++;
                WCHAR key[160] = {0};
                GroupKeyFromPath(pDetail->DevicePath, key, 160);
                const WCHAR* p1 = wcschr(pDetail->DevicePath, L'#');
                const WCHAR* inst = NULL;
                if (p1) {
                    const WCHAR* p2 = wcschr(p1 + 1, L'#');
                    inst = p2 ? p2 + 1 : NULL;
                }
                wprintf(L"\n--- group=%s\n    instance=%s\n", key, inst ? inst : L"?");
                HANDLE h = CreateFileW(pDetail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                       OPEN_EXISTING, 0, NULL);
                if (h == INVALID_HANDLE_VALUE) {
                    printf("  open RW FAILED err=%lu\n", GetLastError());
                    h = CreateFileW(pDetail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
                }
                if (h == INVALID_HANDLE_VALUE) {
                    printf("  open FAILED err=%lu\n", GetLastError());
                } else {
                    PHIDP_PREPARSED_DATA pData = NULL;
                    if (HidD_GetPreparsedData(h, &pData)) {
                        HIDP_CAPS caps = {0};
                        if (HidP_GetCaps(pData, &caps) == HIDP_STATUS_SUCCESS) {
                            printf("  caps: usagePage=%04X usage=%04X inLen=%lu outLen=%lu featureLen=%lu\n",
                                   caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                                   caps.OutputReportByteLength, caps.FeatureReportByteLength);
                        }
                        HidD_FreePreparsedData(pData);
                    }
                    DumpDescriptor(h);
                    CloseHandle(h);
                }
            }
        }
        free(pDetail);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    printf("\n=== %d interface(s) ===\n", n);
    fflush(stdout);
    return 0;
}