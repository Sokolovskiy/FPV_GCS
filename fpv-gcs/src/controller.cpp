#include "controller.h"
#include <QCryptographicHash>
#include <algorithm>

namespace fpv {
Controller::Controller(QObject* parent) : QObject(parent) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    m_initialized = SDL_Init(SDL_INIT_JOYSTICK);
    if (!m_initialized) m_error = QString::fromUtf8(SDL_GetError());
    m_raw = std::make_unique<RawHid>();
}
Controller::~Controller() {
    if (m_joystick) SDL_CloseJoystick(m_joystick);
    m_raw.reset();
    if (m_initialized) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}
QList<Device> controllerDeviceList(const QList<HidDevice>& native, const QList<SdlDevice>& legacy) {
    QList<Device> result;
    for (const auto& device : native) if (device.axes > 8)
        result.append({device.id, QString("%1 · %2 axes").arg(device.name).arg(device.axes)});
    for (const auto& device : legacy) {
        const HidDevice* match = nullptr;
        for (const auto& hid : native) {
            // USB IDs are shared by different radios. Only the physical HID path
            // identifies which SDL entry is a duplicate of a native input.
            if (device.path.isEmpty() || hid.path.compare(device.path, Qt::CaseInsensitive) != 0 ||
                hid.vendor != device.vendor || hid.product != device.product) continue;
            if (match) { match = nullptr; break; }
            match = &hid;
        }
        if (match && match->axes > 8) continue;
        // SDL can assign the same cached product name to radios sharing USB IDs.
        // Keep the SDL ID/backend for legacy buttons and existing profile identities.
        result.append({device.id, match ? match->name : device.name});
    }
    return result;
}
QList<Device> Controller::devices() const {
    const auto native = m_raw->devices();
    QList<SdlDevice> legacy;
    if (!m_initialized) return controllerDeviceList(native, legacy);
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; i < count; ++i) {
        legacy.append({ids[i], QString::fromUtf8(SDL_GetJoystickNameForID(ids[i])),
            QString::fromUtf8(SDL_GetJoystickPathForID(ids[i])),
            SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i])});
    }
    SDL_free(ids);
    return controllerDeviceList(native, legacy);
}
bool Controller::select(SDL_JoystickID id) {
    if (m_joystick) SDL_CloseJoystick(m_joystick);
    m_joystick = nullptr; m_snapshot = {}; m_selected = id;
    m_identity.clear(); m_description.clear();
    m_native = false;
    if (id == 0) { emit deviceChanged(); return true; }
    const auto identity = deviceIdentity(id);
    if (identity.isEmpty()) { emit deviceChanged(); return false; }
    if (m_raw->select(id)) {
        m_native = true; m_raw->pump(); m_snapshot = m_raw->snapshot();
        const auto device = m_raw->selectedDevice();
        m_identity = identity;
        m_description = QString("%1 | %2 осей | Windows HID").arg(device.name).arg(device.axes);
        m_error.clear(); emit deviceChanged(); return true;
    }
    m_joystick = SDL_OpenJoystick(id);
    if (!m_joystick) { m_error = QString::fromUtf8(SDL_GetError()); emit deviceChanged(); return false; }
    update();
    m_identity = identity;
    auto name = QString::fromUtf8(SDL_GetJoystickName(m_joystick));
    for (const auto& device : devices()) if (device.id == id) { name = device.name; break; }
    m_description = QString("%1  |  USB %2:%3")
        .arg(name)
        .arg(SDL_GetJoystickVendor(m_joystick), 4, 16, QChar('0')).arg(SDL_GetJoystickProduct(m_joystick), 4, 16, QChar('0'));
    m_error.clear(); emit deviceChanged(); return true;
}
QString Controller::deviceIdentity(SDL_JoystickID id) const {
    for (const auto& device : m_raw->devices()) if (device.id == id) {
        auto fingerprint = QString("WindowsHID|%1|%2|%3|%4").arg(device.path).arg(device.vendor).arg(device.product).arg(device.axes);
        for (const auto& axis : device.axisUsages) {
            fingerprint += QString("|%1:%2").arg(axis.page).arg(axis.usage);
            if (axis.arrayElement >= 0) fingerprint += QString(":%1").arg(axis.arrayElement);
        }
        return QString::fromLatin1(QCryptographicHash::hash(fingerprint.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (!joystick) return {};
    char guid[33]{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
    const auto serial = QString::fromUtf8(SDL_GetJoystickSerial(joystick));
    const auto path = QString::fromUtf8(SDL_GetJoystickPath(joystick));
    const auto fingerprint = QString("%1|%2|%3|%4|%5|%6|%7").arg(guid, serial, serial.isEmpty() ? path : QString())
        .arg(SDL_GetJoystickVendor(joystick)).arg(SDL_GetJoystickProduct(joystick))
        .arg(SDL_GetNumJoystickAxes(joystick)).arg(SDL_GetNumJoystickButtons(joystick))
        + QString("|%1").arg(SDL_GetNumJoystickHats(joystick));
    SDL_CloseJoystick(joystick);
    return QString::fromLatin1(QCryptographicHash::hash(fingerprint.toUtf8(), QCryptographicHash::Sha256).toHex());
}
bool Controller::reconnect(const QString& identity) {
    if (identity.isEmpty()) return false;
    SDL_JoystickID match = 0;
    for (const auto& device : devices()) if (deviceIdentity(device.id) == identity) {
        if (match) return false; // Ambiguous identity: never choose an arbitrary radio.
        match = device.id;
    }
    if (!match) return false;
    if (match == m_selected) return true; // Keep collecting fresh reports on this handle.
    return select(match);
}
std::optional<Mappings> Controller::radioMappings() const {
    return m_native ? rcMappingsForHid(m_raw->selectedDevice().axisUsages) : std::nullopt;
}
void Controller::update() {
    m_raw->pump();
    if (m_native) { m_snapshot = m_raw->snapshot(); return; }
    if (!m_initialized) return;
    SDL_PumpEvents();
    SDL_UpdateJoysticks();
    // Drain only SDL events; the Qt event loop remains responsible for Windows UI.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {}
    if (!m_joystick || !SDL_JoystickConnected(m_joystick)) {
        m_snapshot = {};
        return;
    }
    m_snapshot.connected = true;
    m_snapshot.axes.resize(std::max(0, SDL_GetNumJoystickAxes(m_joystick)));
    m_snapshot.buttons.resize(std::max(0, SDL_GetNumJoystickButtons(m_joystick)));
    m_snapshot.hats.resize(std::max(0, SDL_GetNumJoystickHats(m_joystick)));
    SDL_ClearError();
    for (int i = 0; i < int(m_snapshot.axes.size()); ++i) m_snapshot.axes[i] = SDL_GetJoystickAxis(m_joystick, i);
    for (int i = 0; i < int(m_snapshot.buttons.size()); ++i) m_snapshot.buttons[i] = SDL_GetJoystickButton(m_joystick, i);
    for (int i = 0; i < int(m_snapshot.hats.size()); ++i) m_snapshot.hats[i] = SDL_GetJoystickHat(m_joystick, i);
    if (const char* error = SDL_GetError(); error && *error) {
        m_error = QString::fromUtf8(error); m_snapshot = {};
    }
}
}
