// Read-only Windows HID descriptor inspection. Does not open a device for output.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <vector>
#include <iostream>

int main() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return 1;
    std::vector<RAWINPUTDEVICELIST> devices(count);
    if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return 1;
    int found = 0;
    for (const auto& device : devices) {
        if (device.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info); UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1)) continue;
        if (info.hid.dwVendorId != 0x1209 || info.hid.dwProductId != 0x4f54) continue;
        ++found;
        UINT bytes = 0;
        GetRawInputDeviceInfoW(device.hDevice, RIDI_PREPARSEDDATA, nullptr, &bytes);
        std::vector<BYTE> data(bytes);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_PREPARSEDDATA, data.data(), &bytes) == UINT(-1)) return 2;
        auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(data.data());
        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) return 3;
        std::cout << "USB 1209:4f54 top-level usagePage=" << caps.UsagePage << " usage=" << caps.Usage
                  << " inputReportBytes=" << caps.InputReportByteLength << " valueCaps=" << caps.NumberInputValueCaps
                  << " buttonCaps=" << caps.NumberInputButtonCaps << '\n';
        USHORT valuesCount = caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> values(valuesCount);
        if (valuesCount && HidP_GetValueCaps(HidP_Input, values.data(), &valuesCount, preparsed) != HIDP_STATUS_SUCCESS) return 4;
        for (USHORT i = 0; i < valuesCount; ++i) {
            const auto& value = values[i];
            std::cout << "VALUE page=" << value.UsagePage << " report=" << int(value.ReportID)
                      << " bits=" << value.BitSize << " count=" << value.ReportCount
                      << " logicalMin=" << value.LogicalMin << " logicalMax=" << value.LogicalMax;
            if (value.IsRange) std::cout << " usageRange=" << value.Range.UsageMin << ".." << value.Range.UsageMax;
            else std::cout << " usage=" << value.NotRange.Usage;
            std::cout << '\n';
        }
        USHORT buttonsCount = caps.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> buttons(buttonsCount);
        if (buttonsCount && HidP_GetButtonCaps(HidP_Input, buttons.data(), &buttonsCount, preparsed) != HIDP_STATUS_SUCCESS) return 5;
        for (USHORT i = 0; i < buttonsCount; ++i) {
            const auto& button = buttons[i];
            std::cout << "BUTTON page=" << button.UsagePage << " report=" << int(button.ReportID);
            if (button.IsRange) std::cout << " usageRange=" << button.Range.UsageMin << ".." << button.Range.UsageMax;
            else std::cout << " usage=" << button.NotRange.Usage;
            std::cout << '\n';
        }
    }
    return found ? 0 : 6;
}
