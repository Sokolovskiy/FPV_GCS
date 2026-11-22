#pragma once
#include "channel.h"
#include <SDL3/SDL.h>
#include <QObject>
#include <QTimer>
#include <QList>
#include "rawhid.h"

namespace fpv {
struct Device { SDL_JoystickID id; QString name; };
struct SdlDevice {
    SDL_JoystickID id;
    QString name, path;
    unsigned vendor = 0, product = 0;
};
QList<Device> controllerDeviceList(const QList<HidDevice>& native, const QList<SdlDevice>& legacy);
class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;
    QList<Device> devices() const;
    bool select(SDL_JoystickID id);
    QString deviceIdentity(SDL_JoystickID id) const;
    bool reconnect(const QString& identity);
    void update();
    const Snapshot& snapshot() const { return m_snapshot; }
    QString identity() const { return m_identity; }
    QString description() const { return m_description; }
    QString error() const { return m_error; }
    std::optional<Mappings> radioMappings() const;
    SDL_JoystickID selected() const { return m_selected; }
signals:
    void deviceChanged();
private:
    SDL_Joystick* m_joystick = nullptr;
    SDL_JoystickID m_selected = 0;
    Snapshot m_snapshot;
    QString m_identity, m_description, m_error;
    bool m_initialized = false;
    std::unique_ptr<RawHid> m_raw;
    bool m_native = false;
};
}
