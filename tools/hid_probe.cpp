#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <iostream>
#include <vector>

int main() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) {
            std::cout << "Elevated=" << elevation.TokenIsElevated << '\n';
        }
        CloseHandle(token);
    }
    GUID guid{}; HidD_GetHidGuid(&guid);
    auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        std::cerr << "Device enumeration failed: " << GetLastError() << '\n';
        return 1;
    }
    bool found = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA entry{sizeof(entry)};
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
        auto h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        if (HidD_GetAttributes(h, &attributes) && attributes.VendorID == 0x0430) {
            found = true;
            std::cout << "Sun device VID=" << std::hex << attributes.VendorID << " PID=" << attributes.ProductID << std::dec << std::endl;
            auto readHandle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            const auto readError = readHandle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            std::cout << "Shared input open success=" << (readHandle != INVALID_HANDLE_VALUE)
                      << " error=" << readError << std::endl;
            if (readHandle != INVALID_HANDLE_VALUE) CloseHandle(readHandle);
            PHIDP_PREPARSED_DATA data = nullptr;
            if (HidD_GetPreparsedData(h, &data)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS) {
                    std::cout << "usage=" << caps.UsagePage << ":" << caps.Usage << " inputBytes=" << caps.InputReportByteLength << std::endl;
                    if (caps.UsagePage == 1 && caps.Usage == 6 && caps.InputReportByteLength > 0) {
                        std::vector<BYTE> report(caps.InputReportByteLength, 0);
                        auto ok = HidD_GetInputReport(h, report.data(), static_cast<ULONG>(report.size()));
                        const auto reportError = ok ? ERROR_SUCCESS : GetLastError();
                        std::cout << "GetInputReport success=" << static_cast<int>(ok) << " error=" << reportError << std::endl;
                        std::cout << "Report contents are intentionally not printed.\n";
                    }
                } else {
                    std::cerr << "Could not inspect HID capabilities.\n";
                }
                HidD_FreePreparsedData(data);
            } else {
                std::cerr << "Could not read HID metadata: " << GetLastError() << '\n';
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (!found) std::cout << "No accessible Sun USB HID device (VID 0430) found.\n";
    std::cout << "Diagnostic only: this does not remap keys or install anything.\n";
    return found ? 0 : 2;
}
