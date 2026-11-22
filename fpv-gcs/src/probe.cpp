#include "controller.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <iostream>
#include <QElapsedTimer>
#include <QThread>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); fpv::Controller controller;
    if (app.arguments().contains("--watch")) {
        fpv::RawHid hid;
        const auto native = hid.devices();
        for (const auto& device : native) if (device.axes > 8) { hid.select(device.id); break; }
        int count = 0; auto* ids = SDL_GetJoysticks(&count);
        QList<SDL_Joystick*> joysticks;
        for (int i = 0; i < count; ++i) if (auto* joystick = SDL_OpenJoystick(ids[i])) joysticks.append(joystick);
        SDL_free(ids);
        QElapsedTimer timer; timer.start(); qint64 next = 0;
        while (timer.elapsed() < 20000) {
            hid.pump(); SDL_PumpEvents(); SDL_UpdateJoysticks();
            if (timer.elapsed() >= next) {
                next += 100;
                QJsonArray devices;
                auto snapshot = hid.snapshot(); QJsonArray axes;
                for (int value : snapshot.axes) axes.append(value);
                devices.append(QJsonObject{{"backend", "HID"}, {"name", hid.selectedDevice().name}, {"axes", axes}});
                for (auto* joystick : joysticks) {
                    QJsonArray values;
                    for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i) values.append(SDL_GetJoystickAxis(joystick, i));
                    devices.append(QJsonObject{{"backend", "SDL"}, {"path", QString::fromUtf8(SDL_GetJoystickPath(joystick))}, {"axes", values}});
                }
                std::cout << QJsonDocument(QJsonObject{{"ms", timer.elapsed()}, {"devices", devices}}).toJson(QJsonDocument::Compact).constData() << std::endl;
            }
            QThread::msleep(2);
        }
        for (auto* joystick : joysticks) SDL_CloseJoystick(joystick);
        return 0;
    }
    if (app.arguments().contains("--native")) {
        fpv::RawHid hid;
        QJsonArray inventory;
        for (const auto& device : hid.devices()) {
            hid.select(device.id);
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 500) { hid.pump(); QThread::msleep(5); }
            const auto s = hid.snapshot(); QJsonArray axes;
            for (const int value : s.axes) axes.append(value);
            QJsonArray usageList, channelList;
            for (const auto& axis : device.axisUsages) usageList.append(QJsonObject{{"page", int(axis.page)},
                {"usage", int(axis.usage)}, {"arrayElement", axis.arrayElement}});
            if (const auto mappings = fpv::rcMappingsForHid(device.axisUsages)) {
                for (int channel = 0; channel < fpv::ChannelCount; ++channel) {
                    const auto& mapping = (*mappings)[channel];
                    channelList.append(QJsonObject{{"channel", channel + 1}, {"axisOneBased", mapping.index + 1},
                        {"value", fpv::evaluate(mapping, s).value}});
                }
            }
            inventory.append(QJsonObject{{"name", device.name}, {"path", device.path},
                {"axisCount", device.axes}, {"receivedAllAxes", s.connected}, {"axisValues", axes},
                {"axisUsages", usageList}, {"automaticRcChannels", channelList}});
        }
        std::cout << QJsonDocument(QJsonObject{{"nativeHid", inventory}, {"error", hid.error()}}).toJson().constData();
        return hid.error().isEmpty() ? 0 : 1;
    }
    QJsonArray list;
    for (const auto& device : controller.devices()) {
        if (!controller.select(device.id)) continue;
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 500) { controller.update(); QThread::msleep(5); }
        const auto& s = controller.snapshot(); QJsonArray axes, buttons, hats;
        for (int value : s.axes) axes.append(value);
        for (int i = 0; i < int(s.buttons.size()); ++i) if (s.buttons[i]) buttons.append(i + 1);
        for (int value : s.hats) hats.append(value);
        list.append(QJsonObject{{"name", device.name}, {"sdlPath", QString::fromUtf8(SDL_GetJoystickPathForID(device.id))},
            {"description", controller.description()}, {"profileIdentity", controller.identity()},
            {"axes", int(s.axes.size())}, {"buttons", int(s.buttons.size())}, {"hats", int(s.hats.size())},
            {"axisValues", axes}, {"pressedInputsOneBased", buttons}, {"hatValues", hats}});
    }
    std::cout << QJsonDocument(QJsonObject{{"sdlVersion", SDL_GetVersion()}, {"controllers", list}, {"error", controller.error()}}).toJson().constData();
    return controller.error().isEmpty() ? 0 : 1;
}
