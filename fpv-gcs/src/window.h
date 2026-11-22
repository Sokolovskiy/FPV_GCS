#pragma once
#include "controller.h"
#include "profile.h"
#include "udp.h"
#include <QMainWindow>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QProgressBar>
#include <QLineEdit>

namespace fpv {
class VideoPanel;
class Window : public QMainWindow {
public:
    Window();
private:
    void closeEvent(QCloseEvent* event) override;
    Qt::WindowStates m_windowedState = Qt::WindowNoState;
    VideoPanel* m_video;
    Controller m_controller;
    UdpSender m_udp;
    Profiles m_profiles;
    Mappings m_mappings = defaultMappings();
    Channels m_channels = evaluateAll(m_mappings, Snapshot{});
    QComboBox* m_devices;
    QLabel* m_status;
    QTableWidget* m_table;
    std::array<QProgressBar*, ChannelCount> m_bars{};
    std::array<QLineEdit*, ChannelCount> m_functions{};
    QList<Device> m_known;
    QString m_loadedIdentity;
    void refreshDevices();
    void selectDevice(SDL_JoystickID id);
    void refreshValues();
    void configure(int index);
    void save();
    void rawMonitor();
};
}
