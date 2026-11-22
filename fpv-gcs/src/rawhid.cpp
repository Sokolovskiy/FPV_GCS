#include "rawhid.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

namespace fpv {
bool hidReportsFresh(const std::vector<bool>& seen, const std::vector<std::int64_t>& received, std::int64_t now) {
    if (seen.empty() || seen.size() != received.size()) return false;
    for (size_t i = 0; i < seen.size(); ++i)
        if (!seen[i] || now < received[i] || now - received[i] > 100) return false;
    return true;
}
static std::int64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::optional<Mappings> rcMappingsForHid(const std::vector<HidAxisUsage>& axes) {
    if (axes.size() != ChannelCount) return std::nullopt;
    // EdgeTX 2.10: use only Axis mode, avoiding its mixed Axis/Sim layout.
    // A repeated Slider usage is a HID value array, in radio channel order.
    const auto sliders = std::count_if(axes.begin(), axes.end(), [](const HidAxisUsage& axis) {
        return axis.page == 1 && axis.usage == 0x36;
    });
    if (sliders == 10) {
        // Observed Windows/EdgeTX 2.10 descriptor: repeated Slider usages are
        // separate scalar caps, with every data index in reverse report order.
        // Accept only this exact layout; do not guess duplicate-usage ordering.
        bool reversedScalars = true;
        for (int input = 0; input < ChannelCount; ++input) {
            const auto& axis = axes[input];
            const unsigned expected = input < 10 ? 0x36 : 0x35 - unsigned(input - 10);
            if (axis.page != 1 || axis.usage != expected || axis.arrayElement >= 0) reversedScalars = false;
        }
        if (reversedScalars) {
            auto mappings = defaultMappings();
            for (int channel = 0; channel < ChannelCount; ++channel) {
                auto& mapping = mappings[channel]; mapping.source = Source::Axis;
                mapping.index = ChannelCount - 1 - channel;
                mapping.label = QString("CH%1 пульта · USB").arg(channel + 1);
            }
            return mappings;
        }
        auto mappings = defaultMappings();
        std::array<bool, ChannelCount> assigned{};
        for (int input = 0; input < int(axes.size()); ++input) {
            const auto& axis = axes[input];
            int channel = -1;
            if (axis.page == 1 && axis.usage >= 0x30 && axis.usage <= 0x35 && axis.arrayElement < 0)
                channel = int(axis.usage - 0x30);
            if (axis.page == 1 && axis.usage == 0x36 && axis.arrayElement >= 0 && axis.arrayElement < 10)
                channel = 6 + axis.arrayElement;
            if (channel < 0 || assigned[channel]) return std::nullopt;
            assigned[channel] = true;
            auto& mapping = mappings[channel]; mapping.source = Source::Axis; mapping.index = input;
            mapping.label = QString("CH%1 пульта · USB").arg(channel + 1);
        }
        return mappings;
    }
    constexpr unsigned simulation[] = {0xb0, 0xb8, 0xba, 0xbb, 0xc4, 0xc5, 0xc8};
    auto mappings = defaultMappings();
    for (int channel = 0; channel < ChannelCount; ++channel) {
        const unsigned page = channel < 9 ? 1 : 2;
        const unsigned usage = channel < 9 ? 0x30 + channel : simulation[channel - 9];
        int found = -1;
        for (int input = 0; input < int(axes.size()); ++input) {
            // EdgeTX's combined Input item is reported by Windows with the final
            // Simulation page also on the preceding Desktop usages 0x30..0x38.
            // Accept that complete observed layout, still requiring all 16 unique usages.
            const bool matchingPage = axes[input].page == page || (page == 1 && axes[input].page == 2);
            if (!matchingPage || axes[input].usage != usage) continue;
            if (found >= 0) return std::nullopt;
            found = input;
        }
        if (found < 0) return std::nullopt;
        auto& mapping = mappings[channel];
        mapping.source = Source::Axis; mapping.index = found;
        mapping.label = QString("CH%1 пульта · USB").arg(channel + 1);
    }
    return mappings;
}
std::optional<std::uint32_t> packedHidValue(const std::vector<std::uint8_t>& bytes, int element, int bits) {
    if (element < 0 || bits < 1 || bits > 32) return std::nullopt;
    const auto offset = std::uint64_t(element) * unsigned(bits);
    if (offset + unsigned(bits) > std::uint64_t(bytes.size()) * 8) return std::nullopt;
    std::uint32_t raw = 0;
    for (int bit = 0; bit < bits; ++bit) {
        const auto position = offset + unsigned(bit);
        raw |= std::uint32_t((bytes[size_t(position / 8)] >> (position % 8)) & 1) << bit;
    }
    return raw;
}
int normalizeHidValue(std::uint32_t raw, int bits, std::int64_t minimum, std::int64_t maximum) {
    if (bits < 1 || bits > 32 || maximum <= minimum) return 0;
    std::int64_t value = raw;
    if (minimum < 0) {
        if (bits == 32) value = std::int32_t(raw);
        else if (raw & (std::uint32_t(1) << (bits - 1))) value -= (std::int64_t(1) << bits);
    }
    const double position = double(std::clamp(value, minimum, maximum) - minimum) / double(maximum - minimum);
    return std::clamp(int(std::lround(position * 65535.0 - 32768.0)), -32768, 32767);
}
struct RawHid::Impl {
    struct Axis {
        USHORT dataIndex; UCHAR report; int bits; LONG minimum, maximum; USHORT page, usage;
        USHORT link = 0; int arrayCount = 1, arrayElement = -1;
    };
    struct State {
        HidDevice device;
        HANDLE handle = nullptr;
        std::vector<BYTE> preparsed;
        HIDP_CAPS caps{};
        std::vector<Axis> fields;
        std::vector<bool> seen;
        std::vector<std::int64_t> received;
        Snapshot values;
        bool removed = false;
    };
    HWND window = nullptr;
    std::map<HANDLE, State> states;
    unsigned nextId = 0x80000000u;
    unsigned selected = 0;
    QString lastError;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_INPUT) self->input(reinterpret_cast<HRAWINPUT>(lparam), DWORD(GetMessageTime()));
        if (self && message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL) {
            auto state = self->states.find(reinterpret_cast<HANDLE>(lparam));
            if (state != self->states.end()) { state->second.removed = true; state->second.values.connected = false; }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    Impl() {
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"FPVGCS_HID_ReadOnly";
        RegisterClassW(&cls);
        window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, this);
        if (!window) { lastError = QString("Cannot create HID input window (%1)").arg(GetLastError()); return; }
        RAWINPUTDEVICE registrations[] = {{1, 4, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
            {1, 5, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}, {1, 8, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}};
        if (!RegisterRawInputDevices(registrations, 3, sizeof(RAWINPUTDEVICE)))
            lastError = QString("Cannot register HID inputs (%1)").arg(GetLastError());
    }
    ~Impl() { if (window) DestroyWindow(window); }
    static PHIDP_PREPARSED_DATA data(State& state) { return reinterpret_cast<PHIDP_PREPARSED_DATA>(state.preparsed.data()); }
    bool describe(HANDLE handle, const RID_DEVICE_INFO& info, State& state) {
        UINT length = 0;
        if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &length) == UINT(-1) || !length) return false;
        state.preparsed.resize(length);
        if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, state.preparsed.data(), &length) == UINT(-1)) return false;
        if (HidP_GetCaps(data(state), &state.caps) != HIDP_STATUS_SUCCESS) return false;
        USHORT count = state.caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> capabilities(count);
        if (count && HidP_GetValueCaps(HidP_Input, capabilities.data(), &count, data(state)) != HIDP_STATUS_SUCCESS) return false;
        capabilities.resize(count);
        for (const auto& value : capabilities) {
            // Read scalar absolute controls across standard and vendor pages. Hats,
            // battery and relative controls are not full RC axes.
            if (!value.IsAbsolute || value.BitSize < 2 || value.BitSize > 32 || value.LogicalMax <= value.LogicalMin ||
                value.UsagePage == 6 || value.UsagePage == 0x85) continue;
            if (!value.ReportCount || (!value.IsRange && value.ReportCount > 4096)) continue;
            if (!value.IsRange && value.ReportCount > 1) {
                if (value.UsagePage == 1 && value.NotRange.Usage == 0x39) continue;
                for (int element = 0; element < value.ReportCount; ++element)
                    state.fields.push_back({value.NotRange.DataIndex, value.ReportID, value.BitSize,
                        value.LogicalMin, value.LogicalMax, value.UsagePage, value.NotRange.Usage,
                        value.LinkCollection, value.ReportCount, element});
                continue;
            }
            const int first = value.IsRange ? value.Range.DataIndexMin : value.NotRange.DataIndex;
            const int last = value.IsRange ? value.Range.DataIndexMax : first;
            for (int index = first; index <= last; ++index) {
                const USHORT usage = value.IsRange ? value.Range.UsageMin + index - first : value.NotRange.Usage;
                if (value.UsagePage == 1 && usage == 0x39) continue;
                state.fields.push_back({USHORT(index), value.ReportID, value.BitSize, value.LogicalMin, value.LogicalMax, value.UsagePage, usage});
            }
        }
        std::stable_sort(state.fields.begin(), state.fields.end(), [](const Axis& a, const Axis& b) { return a.dataIndex < b.dataIndex; });
        if (state.fields.empty()) return false;
        state.handle = handle; state.device.id = nextId++; state.device.vendor = info.hid.dwVendorId; state.device.product = info.hid.dwProductId;
        state.device.axes = int(state.fields.size());
        for (const auto& axis : state.fields) state.device.axisUsages.push_back({axis.page, axis.usage, axis.arrayElement});
        UINT pathLength = 0; GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &pathLength);
        std::vector<wchar_t> path(pathLength + 1);
        if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, path.data(), &pathLength) != UINT(-1)) state.device.path = QString::fromWCharArray(path.data());
        HANDLE metadata = CreateFileW(reinterpret_cast<LPCWSTR>(state.device.path.utf16()), 0,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (metadata != INVALID_HANDLE_VALUE) {
            wchar_t name[256]{};
            if (HidD_GetProductString(metadata, name, sizeof(name))) state.device.name = QString::fromWCharArray(name);
            CloseHandle(metadata);
        }
        if (state.device.name.isEmpty()) state.device.name = QString("USB RC %1:%2").arg(state.device.vendor, 4, 16, QChar('0')).arg(state.device.product, 4, 16, QChar('0'));
        state.values.axes.resize(state.fields.size()); state.seen.resize(state.fields.size());
        state.received.resize(state.fields.size());
        return true;
    }
    QList<HidDevice> scan() {
        QList<HidDevice> result;
        if (!window || !lastError.isEmpty()) return result;
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return result;
        std::vector<RAWINPUTDEVICELIST> devices(count);
        if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return result;
        std::set<HANDLE> present;
        for (const auto& device : devices) {
            if (device.dwType != RIM_TYPEHID) continue;
            RID_DEVICE_INFO info{}; info.cbSize = sizeof(info); UINT size = sizeof(info);
            if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1)) continue;
            if (info.hid.usUsagePage != 1 || (info.hid.usUsage != 4 && info.hid.usUsage != 5 && info.hid.usUsage != 8)) continue;
            present.insert(device.hDevice);
            auto state = states.find(device.hDevice);
            if (state == states.end() || state->second.removed) {
                State fresh;
                if (!describe(device.hDevice, info, fresh)) continue;
                states[device.hDevice] = std::move(fresh);
            }
            result.append(states[device.hDevice].device);
        }
        for (auto& [handle, state] : states) if (!present.contains(handle)) { state.removed = true; state.values.connected = false; }
        return result;
    }
    void input(HRAWINPUT input, DWORD queuedAt) {
        // Windows may queue reports while the UI thread is stalled. Dispatch
        // time must not turn a historical USB report into fresh RC input.
        if (DWORD(GetTickCount() - queuedAt) > 100) return;
        UINT size = 0;
        if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || size < offsetof(RAWINPUT, data.hid.bRawData) || size > 1024 * 1024) return;
        std::vector<BYTE> bytes(size);
        if (GetRawInputData(input, RID_INPUT, bytes.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
        const auto* raw = reinterpret_cast<const RAWINPUT*>(bytes.data());
        if (raw->header.dwType != RIM_TYPEHID) return;
        auto entry = states.find(raw->header.hDevice);
        if (entry == states.end() || entry->second.removed) return;
        auto& state = entry->second;
        const auto& hid = raw->data.hid;
        const size_t offset = offsetof(RAWINPUT, data.hid.bRawData);
        if (!hid.dwSizeHid || offset > bytes.size() || size_t(hid.dwSizeHid) * hid.dwCount > bytes.size() - offset) return;
        for (DWORD packet = 0; packet < hid.dwCount; ++packet) {
            const BYTE* source = hid.bRawData + packet * hid.dwSizeHid;
            std::vector<char> report(state.caps.InputReportByteLength, 0);
            const bool noIds = std::all_of(state.fields.begin(), state.fields.end(), [](const Axis& axis) { return axis.report == 0; });
            if (hid.dwSizeHid == report.size()) std::copy(source, source + hid.dwSizeHid, report.begin());
            else if (noIds && hid.dwSizeHid + 1 == report.size()) std::copy(source, source + hid.dwSizeHid, report.begin() + 1);
            else continue;
            ULONG count = HidP_MaxDataListLength(HidP_Input, data(state));
            std::vector<HIDP_DATA> fields(count);
            if (HidP_GetData(HidP_Input, fields.data(), &count, data(state), report.data(), ULONG(report.size())) != HIDP_STATUS_SUCCESS) continue;
            for (ULONG item = 0; item < count; ++item) {
                for (size_t axisIndex = 0; axisIndex < state.fields.size(); ++axisIndex) {
                    const auto& axis = state.fields[axisIndex];
                    if (axis.arrayCount == 1 && axis.dataIndex == fields[item].DataIndex && axis.report == BYTE(report[0])) {
                        state.values.axes[axisIndex] = normalizeHidValue(fields[item].RawValue, axis.bits, axis.minimum, axis.maximum);
                        state.seen[axisIndex] = true;
                        state.received[axisIndex] = monotonicMs();
                    }
                }
            }
            // HidP_GetData does not return usage value arrays. Read each array
            // once, then unpack every element without collapsing repeated sliders.
            for (size_t axisIndex = 0; axisIndex < state.fields.size(); ++axisIndex) {
                const auto& axis = state.fields[axisIndex];
                if (axis.arrayElement != 0 || axis.report != BYTE(report[0])) continue;
                std::vector<std::uint8_t> packed((size_t(axis.bits) * axis.arrayCount + 7) / 8);
                if (HidP_GetUsageValueArray(HidP_Input, axis.page, axis.link, axis.usage,
                    reinterpret_cast<PCHAR>(packed.data()), USHORT(packed.size()), data(state),
                    report.data(), ULONG(report.size())) != HIDP_STATUS_SUCCESS) continue;
                const auto receivedAt = monotonicMs();
                for (int element = 0; element < axis.arrayCount; ++element) {
                    const size_t target = axisIndex + element;
                    if (target >= state.fields.size() || state.fields[target].dataIndex != axis.dataIndex ||
                        state.fields[target].arrayElement != element) break;
                    const auto rawValue = packedHidValue(packed, element, axis.bits);
                    if (!rawValue) break;
                    state.values.axes[target] = normalizeHidValue(*rawValue, axis.bits, axis.minimum, axis.maximum);
                    state.seen[target] = true; state.received[target] = receivedAt;
                }
            }
            state.values.connected = std::all_of(state.seen.begin(), state.seen.end(), [](bool seen) { return seen; });
        }
    }
};
RawHid::RawHid() : m_impl(std::make_unique<Impl>()) {}
RawHid::~RawHid() = default;
QList<HidDevice> RawHid::devices() { return m_impl->scan(); }
bool RawHid::select(unsigned id) {
    m_impl->selected = id;
    for (auto& [handle, state] : m_impl->states) if (state.device.id == id && !state.removed) {
        std::fill(state.seen.begin(), state.seen.end(), false);
        state.values.connected = false;
        return true;
    }
    return false;
}
void RawHid::pump() {
    if (!m_impl->window) return;
    MSG message;
    while (PeekMessageW(&message, m_impl->window, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
Snapshot RawHid::snapshot() const {
    for (const auto& [handle, state] : m_impl->states) if (state.device.id == m_impl->selected && !state.removed) {
        if (!state.values.connected || !hidReportsFresh(state.seen, state.received, monotonicMs())) return {};
        return state.values;
    }
    return {};
}
HidDevice RawHid::selectedDevice() const {
    for (const auto& [handle, state] : m_impl->states) if (state.device.id == m_impl->selected && !state.removed) return state.device;
    return {};
}
QString RawHid::error() const { return m_impl->lastError; }
}
