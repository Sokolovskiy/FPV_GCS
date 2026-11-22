#include "channel.h"
#include "udp.h"
#include <QUdpSocket>
#include "controller.h"
#include "profile.h"
#include "learning.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

using namespace fpv;
class Tests : public QObject {
    Q_OBJECT
private slots:
    void twoRadiosSharingUsbIdsHaveDistinctListings() {
        HidDevice mk2; mk2.id = 0x80000000u; mk2.vendor = 0x1209; mk2.product = 0x4f54;
        mk2.name = "Radiomaster TX16S Joystick"; mk2.path = "hid#radio-mk2"; mk2.axes = 13;
        auto mk3 = mk2; mk3.id++; mk3.name = "TX16SMK3 Joystick"; mk3.path = "hid#radio-mk3"; mk3.axes = 5;
        QList<SdlDevice> legacy = {
            {1, "OpenTX TX16SMK3 Joystick", "HID#RADIO-MK2", 0x1209, 0x4f54},
            {2, "OpenTX TX16SMK3 Joystick", "HID#RADIO-MK3", 0x1209, 0x4f54}};
        const auto devices = controllerDeviceList({mk2, mk3}, legacy);
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices[0].id, mk2.id); QVERIFY(devices[0].name.startsWith(mk2.name));
        QCOMPARE(devices[1].id, 2u); QCOMPARE(devices[1].name, mk3.name);
        // An unrelated or unavailable path must never be hidden just for sharing VID/PID.
        legacy[0].path = "hid#another-radio";
        QCOMPARE(controllerDeviceList({mk2, mk3}, legacy).size(), 3);
        legacy[0].path.clear();
        QCOMPARE(controllerDeviceList({mk2, mk3}, legacy).size(), 3);
        QCOMPARE(controllerDeviceList({}, legacy).size(), 2);
    }
    void udpRetriesAfterSourceAddressReturns() {
        QUdpSocket receiver; QVERIFY(receiver.bind(QHostAddress(QHostAddress::LocalHost), 0));
        UdpSender sender; sender.address = QHostAddress::LocalHost; sender.port = receiver.localPort();
        sender.sourceAddress = QHostAddress("203.0.113.1");
        Channels channels; channels.fill(1500);
        QVERIFY(!sender.send(channels, true)); QCOMPARE(sender.sent, quint64(0));
        sender.sourceAddress = QHostAddress::LocalHost;
        QVERIFY(sender.send(channels, true)); QVERIFY(receiver.waitForReadyRead(1000));
        QCOMPARE(sender.sent, quint64(1));
    }
    void hidRequiresEveryAxisFreshAfterReconnect() {
        std::vector<bool> seen(16, false);
        std::vector<std::int64_t> received(16, 0);
        QVERIFY(!hidReportsFresh(seen, received, 1000));
        std::fill(seen.begin(), seen.end(), true); std::fill(received.begin(), received.end(), 1000);
        QVERIFY(hidReportsFresh(seen, received, 1100));
        received[4] = 999; QVERIFY(!hidReportsFresh(seen, received, 1100));
        received[4] = 1101; QVERIFY(!hidReportsFresh(seen, received, 1100));
        std::fill(received.begin(), received.end(), 1100); seen[4] = false;
        QVERIFY(!hidReportsFresh(seen, received, 1100));
    }
    void unavailableMappingSuppressesCommands() {
        auto mappings = defaultMappings();
        Snapshot snapshot; snapshot.connected = true; snapshot.axes.assign(16, 0);
        QVERIFY(!commandsReady(mappings, snapshot));
        for (int i = 0; i < 16; ++i) { mappings[i].source = Source::Axis; mappings[i].index = i; }
        QVERIFY(commandsReady(mappings, snapshot));
        snapshot.axes.resize(8); QVERIFY(!commandsReady(mappings, snapshot));
        snapshot.connected = false; QVERIFY(!commandsReady(mappings, snapshot));
    }
    void reconnectOnlyRecognizedController() {
        Controller controller;
        SDL_VirtualJoystickDesc description; SDL_INIT_INTERFACE(&description);
        description.type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
        description.naxes = 16; description.nbuttons = 24;
        description.name = "Task 7 selected radio";
        description.vendor_id = 0x1209; description.product_id = 0x1234;
        const auto original = SDL_AttachVirtualJoystick(&description); QVERIFY(original);
        QVERIFY(controller.select(original));
        const auto identity = controller.identity(); QVERIFY(!identity.isEmpty());
        SDL_Joystick* oldHandle = SDL_OpenJoystick(original); QVERIFY(oldHandle);
        QVERIFY(SDL_SetJoystickVirtualAxis(oldHandle, 4, -32768));
        controller.update(); QCOMPARE(controller.snapshot().axes[4], -32768);
        QVERIFY(SDL_DetachVirtualJoystick(original)); controller.update();
        QVERIFY(!controller.snapshot().connected);
        description.name = "Task 7 different radio"; description.product_id = 0x5678;
        const auto different = SDL_AttachVirtualJoystick(&description); QVERIFY(different);
        controller.update(); QVERIFY(!controller.reconnect(identity));
        QCOMPARE(controller.selected(), original); QVERIFY(!controller.snapshot().connected);
        description.name = "Task 7 selected radio"; description.product_id = 0x1234;
        const auto returned = SDL_AttachVirtualJoystick(&description); QVERIFY(returned);
        controller.update(); QVERIFY(controller.reconnect(identity));
        QCOMPARE(controller.selected(), returned); QCOMPARE(controller.identity(), identity);
        SDL_Joystick* newHandle = SDL_OpenJoystick(returned); QVERIFY(newHandle);
        QVERIFY(SDL_SetJoystickVirtualAxis(newHandle, 4, 32767));
        controller.update(); QVERIFY(controller.snapshot().connected);
        QCOMPARE(controller.snapshot().axes[4], 32767);
        const auto ambiguous = SDL_AttachVirtualJoystick(&description); QVERIFY(ambiguous);
        controller.update(); QVERIFY(!controller.reconnect(identity));
        SDL_DetachVirtualJoystick(ambiguous); SDL_DetachVirtualJoystick(returned); SDL_DetachVirtualJoystick(different);
        SDL_CloseJoystick(newHandle); SDL_CloseJoystick(oldHandle);
    }
    void udpWireFormatAndDisconnect() {
        Channels channels;
        for (int i = 0; i < 16; ++i) channels[i] = 1000 + i * 60;
        const auto bytes = encodeRcPacket(channels, 0x12345678, 0xffffffff, 0x0102030405060708ULL);
        QCOMPARE(bytes.size(), 56);
        QCOMPARE(bytes.left(24).toHex(), QByteArray("465243310110003812345678ffffffff0102030405060708"));
        for (int i = 0; i < 16; ++i) {
            const int value = (quint8(bytes[24 + i * 2]) << 8) | quint8(bytes[25 + i * 2]);
            QCOMPARE(value, int(channels[i]));
        }
        QUdpSocket receiver; QVERIFY(receiver.bind(QHostAddress(QHostAddress::LocalHost), 0));
        UdpSender sender; sender.address = QHostAddress::LocalHost; sender.sourceAddress = QHostAddress::LocalHost; sender.port = receiver.localPort();
        QVERIFY(!sender.send(channels, false)); QCOMPARE(sender.sent, quint64(0));
        QVERIFY(!receiver.waitForReadyRead(40));
        QVERIFY(sender.send(channels, true)); QVERIFY(receiver.waitForReadyRead(1000));
        QByteArray received(56, char(0)); QCOMPARE(receiver.readDatagram(received.data(), received.size()), qint64(56));
        QCOMPARE(received.mid(24), bytes.mid(24)); QCOMPARE(received.mid(12, 4).toHex(), QByteArray("00000000"));
        QVERIFY(!sender.send(channels, false)); QVERIFY(!receiver.waitForReadyRead(40));
        QVERIFY(sender.send(channels, true)); QVERIFY(receiver.waitForReadyRead(1000));
        receiver.readDatagram(received.data(), received.size());
        QCOMPARE(received.mid(12, 4).toHex(), QByteArray("00000001")); QCOMPARE(sender.sent, quint64(2));
        channels[15] = 2001; QVERIFY(!sender.send(channels, true)); QVERIFY(!receiver.waitForReadyRead(40));
        QCOMPARE(sender.sent, quint64(2));
    }

    void identifyByMovementWithoutCalibration() {
        Snapshot baseline; baseline.connected = true; baseline.axes.assign(16, 0);
        AxisMotionCapture capture(baseline);
        auto sample = baseline; sample.axes[14] = 40; capture.observe(sample);
        QCOMPARE(capture.detectedAxis(), -1);
        sample.axes[9] = 3000; capture.observe(sample);
        QCOMPARE(capture.detectedAxis(), 9);
        capture.observe(baseline); QCOMPARE(capture.detectedAxis(), 9);
        sample.axes[8] = -3000; capture.observe(sample);
        QCOMPARE(capture.detectedAxis(), -2);
        sample.connected = false; capture.observe(sample); QCOMPARE(capture.detectedAxis(), -3);
        AxisMotionCapture changed(baseline); sample = baseline; sample.axes.pop_back(); changed.observe(sample);
        QCOMPARE(changed.detectedAxis(), -3);
        baseline.axes[2] = -32768; AxisMotionCapture throttle(baseline);
        sample = baseline; sample.axes[2] += 2500; throttle.observe(sample);
        QCOMPARE(throttle.detectedAxis(), 2);
        // Detection keeps the full HID range, even for a short movement.
        Mapping mapping; mapping.source = Source::Axis; mapping.index = 2;
        sample.axes[2] = 0; QCOMPARE(evaluate(mapping, sample).value, 1500);
        sample.axes[2] = 32767; QCOMPARE(evaluate(mapping, sample).value, 2000);
    }
    void automaticRadioChannelLayout() {
        std::vector<HidAxisUsage> axes;
        for (unsigned usage = 0x30; usage <= 0x38; ++usage) axes.push_back({1, usage});
        for (unsigned usage : {0xb0u, 0xb8u, 0xbau, 0xbbu, 0xc4u, 0xc5u, 0xc8u}) axes.push_back({2, usage});
        // Windows data indices need not follow USB channel/usage order.
        std::reverse(axes.begin(), axes.end());
        const auto mappings = rcMappingsForHid(axes); QVERIFY(mappings.has_value());
        for (int channel = 0; channel < 16; ++channel) {
            QCOMPARE((*mappings)[channel].index, 15 - channel);
            QCOMPARE((*mappings)[channel].source, Source::Axis);
            QVERIFY(!(*mappings)[channel].inverted);
            for (int position : {-32768, 0, 32767}) {
                Snapshot snapshot; snapshot.connected = true; snapshot.axes.resize(16);
                snapshot.axes[15 - channel] = position;
                const auto values = evaluateAll(*mappings, snapshot);
                for (int other = 0; other < 16; ++other)
                    QCOMPARE(int(values[other]), other != channel ? 1500 : position < 0 ? 1000 : position > 0 ? 2000 : 1500);
            }
        }
        QCOMPARE(int(evaluateAll(*mappings, Snapshot{})[2]), 1000);
        // Exact Windows descriptor observed after USB Advanced reconfiguration.
        for (auto& axis : axes) axis.page = 2;
        const auto windowsMappings = rcMappingsForHid(axes); QVERIFY(windowsMappings);
        for (int channel = 0; channel < 16; ++channel) QCOMPARE((*windowsMappings)[channel].index, 15 - channel);
        axes[0] = axes[1]; QVERIFY(!rcMappingsForHid(axes));
        axes.pop_back(); QVERIFY(!rcMappingsForHid(axes));
    }
    void repeatedSlidersPreserveAllSixteenChannels() {
        std::vector<HidAxisUsage> axes;
        // Windows may enumerate the scalar caps in reverse usage order.
        for (unsigned usage = 0x35; usage >= 0x30; --usage) axes.push_back({1, usage});
        for (int element = 0; element < 10; ++element) axes.push_back({1, 0x36, element});
        const auto mappings = rcMappingsForHid(axes); QVERIFY(mappings);
        for (int channel = 0; channel < ChannelCount; ++channel) {
            const auto& mapping = (*mappings)[channel];
            QCOMPARE(mapping.index, channel < 6 ? 5 - channel : channel);
            for (int position : {-32768, 0, 32767}) {
                Snapshot snapshot; snapshot.connected = true; snapshot.axes.assign(16, 0);
                snapshot.axes[mapping.index] = position;
                const auto values = evaluateAll(*mappings, snapshot);
                for (int other = 0; other < ChannelCount; ++other)
                    QCOMPARE(int(values[other]), other != channel ? 1500 : position < 0 ? 1000 : position > 0 ? 2000 : 1500);
            }
        }
        axes.back().arrayElement = 0; QVERIFY(!rcMappingsForHid(axes));
        axes.back().arrayElement = 10; QVERIFY(!rcMappingsForHid(axes));
        axes.back().arrayElement = -1; QVERIFY(!rcMappingsForHid(axes));
        axes.back().arrayElement = 9; axes[0].page = 2; QVERIFY(!rcMappingsForHid(axes));
    }
    void packedUsageArraysKeepIndependentValues() {
        for (int bits : {11, 16, 32}) {
            const std::vector<std::uint32_t> values = bits == 32
                ? std::vector<std::uint32_t>{0, 0x80000000u, 0xffffffffu, 0x12345678u}
                : std::vector<std::uint32_t>{0, 1024, 2047, 319, 815, 17, 2018, 42, 1200, 400};
            std::vector<std::uint8_t> bytes((values.size() * bits + 7) / 8);
            for (size_t element = 0; element < values.size(); ++element)
                for (int bit = 0; bit < bits; ++bit) if ((values[element] >> bit) & 1) {
                    const auto position = element * bits + bit;
                    bytes[position / 8] |= std::uint8_t(1u << (position % 8));
                }
            for (int element = 0; element < int(values.size()); ++element) {
                const auto value = packedHidValue(bytes, element, bits); QVERIFY(value);
                QCOMPARE(*value, values[element]);
            }
            QVERIFY(!packedHidValue(bytes, int(values.size()), bits));
        }
        QVERIFY(!packedHidValue({}, 0, 16));
        QVERIFY(!packedHidValue({0}, -1, 8));
        QVERIFY(!packedHidValue({0}, 0, 33));
        QVERIFY(!packedHidValue({0}, 0, 0));
    }
    void edgeTx210ScalarSlidersFollowPhysicalReportOrder() {
        // Exact physical MK2 descriptor, independently checked by feeding a
        // synthetic input report to Windows HidP_GetData (no device output).
        std::vector<HidAxisUsage> axes(10, {1, 0x36});
        for (unsigned usage = 0x35; usage >= 0x30; --usage) axes.push_back({1, usage});
        const auto mappings = rcMappingsForHid(axes); QVERIFY(mappings);
        Snapshot snapshot; snapshot.connected = true; snapshot.axes.assign(16, 0);
        for (int channel = 0; channel < 16; ++channel) {
            QCOMPARE((*mappings)[channel].index, 15 - channel);
            for (int position : {-32768, 0, 32767}) {
                snapshot.axes.assign(16, 0); snapshot.axes[15 - channel] = position;
                const auto values = evaluateAll(*mappings, snapshot);
                for (int other = 0; other < 16; ++other)
                    QCOMPARE(int(values[other]), other != channel ? 1500 : position < 0 ? 1000 : position > 0 ? 2000 : 1500);
            }
        }
        axes[0].page = 2; QVERIFY(!rcMappingsForHid(axes)); axes[0].page = 1;
        std::swap(axes[10], axes[11]); QVERIFY(!rcMappingsForHid(axes));
    }
    void hidLogicalRanges() {
        QCOMPARE(normalizeHidValue(0, 16, 0, 2047), -32768);
        QCOMPARE(normalizeHidValue(2047, 16, 0, 2047), 32767);
        QVERIFY(std::abs(normalizeHidValue(1024, 16, 0, 2047)) < 32);
        QCOMPARE(normalizeHidValue(65535, 16, -32768, 32767), -1);
        QCOMPARE(normalizeHidValue(32768, 16, -32768, 32767), -32768);
        QCOMPARE(normalizeHidValue(32767, 16, -32768, 32767), 32767);
        QCOMPARE(normalizeHidValue(5000, 16, 0, 2047), 32767);
        QCOMPARE(normalizeHidValue(0, 16, 1, 1), 0);
    }
    void safeDefaults() {
        auto mappings = defaultMappings(); Snapshot s;
        auto values = evaluateAll(mappings, s);
        for (int i = 0; i < 16; ++i) QCOMPARE(int(values[i]), i == 0 || i == 1 || i == 3 ? 1500 : 1000);
        s.connected = true; s.axes = {32767};
        QCOMPARE(evaluate(mappings[4], s).value, 1000);
        QVERIFY(!evaluate(mappings[4], s).live);
    }
    void analogAndCalibration() {
        Mapping m; m.source = Source::Axis; Snapshot s{true, {-32768}, {}, {}};
        QCOMPARE(evaluate(m, s).value, 1000);
        s.axes[0] = 0; QCOMPARE(evaluate(m, s).value, 1500);
        s.axes[0] = 32767; QCOMPARE(evaluate(m, s).value, 2000);
        m.inverted = true; QCOMPARE(evaluate(m, s).value, 1000);
        m.inverted = false; m.minimum = -10000; m.center = 500; m.maximum = 20000;
        s.axes[0] = 500; QCOMPARE(evaluate(m, s).value, 1500);
        s.axes[0] = 32767; QCOMPARE(evaluate(m, s).value, 2000);
        m.center = 30000; QVERIFY(!evaluate(m, s).live);
        m.safe = 1200; QCOMPARE(evaluate(m, s).value, 1200);
    }
    void switchModels() {
        Mapping m; m.source = Source::Axis; m.mode = Mode::ThreePosition;
        Snapshot s{true, {-32768}, {false, false}, {0}};
        QCOMPARE(evaluate(m, s).value, 1000); s.axes[0] = 0; QCOMPARE(evaluate(m, s).value, 1500);
        s.axes[0] = 32767; QCOMPARE(evaluate(m, s).value, 2000);
        m.mode = Mode::TwoPosition; s.axes[0] = -1; QCOMPARE(evaluate(m, s).value, 1000);
        s.axes[0] = 1; QCOMPARE(evaluate(m, s).value, 2000);
        m.source = Source::Button; QCOMPARE(evaluate(m, s).value, 1000);
        s.buttons[0] = true; QCOMPARE(evaluate(m, s).value, 2000);
        m.source = Source::ButtonPair; m.mode = Mode::ThreePosition;
        QCOMPARE(evaluate(m, s).value, 1000); s.buttons[0] = false; QCOMPARE(evaluate(m, s).value, 1500);
        s.buttons[1] = true; QCOMPARE(evaluate(m, s).value, 2000);
        s.buttons[0] = true; m.safe = 1100; m.inverted = true;
        QCOMPARE(evaluate(m, s).value, 1100); QVERIFY(!evaluate(m, s).live);
        s.connected = false; QCOMPARE(evaluate(m, s).value, 1100);
    }
    void hatModels() {
        Mapping m; m.source = Source::Hat; m.mode = Mode::ThreePosition;
        Snapshot s{true, {}, {}, {SDL_HAT_UP}};
        QCOMPARE(evaluate(m, s).value, 1000); s.hats[0] = SDL_HAT_CENTERED; QCOMPARE(evaluate(m, s).value, 1500);
        s.hats[0] = SDL_HAT_DOWN; QCOMPARE(evaluate(m, s).value, 2000);
        m.inverted = true; QCOMPARE(evaluate(m, s).value, 1000);
        s.hats[0] = SDL_HAT_UP | SDL_HAT_DOWN; QVERIFY(!evaluate(m, s).live);
        m.mode = Mode::TwoPosition; m.inverted = false; s.hats[0] = SDL_HAT_DOWN;
        QCOMPARE(evaluate(m, s).value, 2000); s.hats[0] = SDL_HAT_UP; QCOMPARE(evaluate(m, s).value, 1000);
    }
    void independentSixteenAndMissingInputs() {
        auto mappings = defaultMappings(); Snapshot s; s.connected = true;
        for (int i = 0; i < 16; ++i) { mappings[i].source = Source::Axis; mappings[i].index = i; s.axes.push_back(-32768 + i * 4000); }
        const auto values = evaluateAll(mappings, s);
        for (int i = 1; i < 16; ++i) QVERIFY(values[i] > values[i - 1]);
        s.axes.resize(8); auto lost = evaluateAll(mappings, s);
        for (int i = 8; i < 16; ++i) QCOMPARE(int(lost[i]), 1000);
        s.connected = false; QCOMPARE(int(evaluateAll(mappings, s)[2]), 1000);
    }
    void profilesAndCorruption() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Profiles profiles(directory.path() + "/profiles.json");
        auto mappings = defaultMappings(); mappings[0].source = Source::Axis; mappings[0].index = 7; mappings[0].inverted = true;
        mappings[4].functionText = "ARM / озброєння";
        QString error; QVERIFY(profiles.save("device-a", mappings, &error)); QVERIFY(error.isEmpty());
        auto loaded = profiles.load("device-a", &error); QCOMPARE(loaded[0].index, 7); QVERIFY(loaded[0].inverted);
        QCOMPARE(loaded[4].functionText, mappings[4].functionText);
        auto legacy = toJson(mappings[4]); legacy.remove("functionText"); Mapping compatible;
        QVERIFY(fromJson(legacy, compatible)); QVERIFY(compatible.functionText.isEmpty());
        mappings[0].index = 6;
        QVERIFY2(profiles.save("device-a", mappings, &error), qPrintable(error));
        QCOMPARE(profiles.load("device-a")[0].index, 6);
        QCOMPARE(profiles.load("device-a")[4].functionText, mappings[4].functionText);
        QVERIFY(profiles.save("device-b", defaultMappings(), &error));
        QCOMPARE(profiles.load("device-a")[0].index, 6);
        QCOMPARE(profiles.load("device-b")[0].source, Source::Unassigned);
        auto invalid = toJson(mappings[0]); invalid["minimum"] = 99999; Mapping rejected;
        QVERIFY(!fromJson(invalid, rejected));
        QFile file(profiles.path()); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken JSON"); file.close();
        error.clear(); QCOMPARE(profiles.load("device-a", &error)[0].source, Source::Unassigned); QVERIFY(!error.isEmpty());
    }
    void virtualSdlDevice() {
        Controller controller;
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        SDL_VirtualJoystickDesc description;
        SDL_INIT_INTERFACE(&description);
        description.type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
        description.naxes = 16; description.nbuttons = 32; description.nhats = 1;
        description.name = "FPV GCS automated test device";
        const auto id = SDL_AttachVirtualJoystick(&description);
        QVERIFY2(id != 0, SDL_GetError()); QVERIFY(controller.select(id));
        SDL_Joystick* joystick = SDL_OpenJoystick(id); QVERIFY(joystick);
        for (int i = 0; i < 16; ++i) QVERIFY(SDL_SetJoystickVirtualAxis(joystick, i, Sint16(-32768 + i * 4000)));
        QVERIFY(SDL_SetJoystickVirtualButton(joystick, 31, true));
        QVERIFY(SDL_SetJoystickVirtualHat(joystick, 0, SDL_HAT_LEFT));
        controller.update(); const auto snapshot = controller.snapshot();
        QCOMPARE(snapshot.axes.size(), size_t(16)); QCOMPARE(snapshot.buttons.size(), size_t(32));
        QCOMPARE(snapshot.axes[15], -32768 + 15 * 4000); QVERIFY(snapshot.buttons[31]); QCOMPARE(snapshot.hats[0], int(SDL_HAT_LEFT));
        auto mappings = defaultMappings();
        for (int i = 0; i < 16; ++i) { mappings[i].source = Source::Axis; mappings[i].index = i; }
        const auto values = evaluateAll(mappings, snapshot); QCOMPARE(int(values[0]), 1000); QVERIFY(values[15] > values[14]);
        QVERIFY(SDL_DetachVirtualJoystick(id)); controller.update(); QVERIFY(!controller.snapshot().connected);
        QCOMPARE(int(evaluateAll(mappings, controller.snapshot())[2]), 1000);
        SDL_CloseJoystick(joystick);
    }
    void learnSwitchRegardlessOfAnalogDefault() {
        std::array<Snapshot, 3> samples;
        for (auto& s : samples) { s.connected = true; s.axes.resize(8); s.buttons.resize(24); s.buttons[4] = true; }
        samples[0].buttons[16] = true;
        samples[1].buttons[17] = true;
        samples[2].buttons[18] = true;
        auto result = learnControl(samples, true, Mode::Analog, defaultMappings()[8]);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.mapping.source, Source::SwitchPositions);
        QCOMPARE(result.mapping.mode, Mode::ThreePosition);
        QCOMPARE(result.mapping.switchInputs, std::vector<int>({16, 17, 18}));
        for (int p = 0; p < 3; ++p) QCOMPARE(evaluate(result.mapping, samples[p]).value, 1000 + p * 500);
        // Other switches do not participate in this learned physical switch.
        samples[1].buttons[4] = false; samples[1].buttons[23] = true;
        QCOMPARE(evaluate(result.mapping, samples[1]).value, 1500);
        samples[1].buttons[18] = true; QVERIFY(!evaluate(result.mapping, samples[1]).live);
        Mapping restored; QVERIFY(fromJson(toJson(result.mapping), restored));
        QCOMPARE(evaluate(restored, samples[2]).value, 2000);
    }
    void learnActiveLowTwoPositionSwitch() {
        std::array<Snapshot, 3> samples;
        for (auto& s : samples) { s.connected = true; s.axes.resize(8); s.buttons.resize(24); }
        samples[0].buttons[22] = true;
        auto result = learnControl(samples, false, Mode::TwoPosition, defaultMappings()[15]);
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(evaluate(result.mapping, samples[0]).value, 1000);
        QCOMPARE(evaluate(result.mapping, samples[2]).value, 2000);
        samples[1] = samples[0];
        result = learnControl(samples, true, Mode::Analog, defaultMappings()[15]);
        QVERIFY(result.ok); QCOMPARE(result.mapping.mode, Mode::TwoPosition);
    }
    void learnAxisAndHatByPhysicalPositions() {
        std::array<Snapshot, 3> samples;
        for (auto& s : samples) { s.connected = true; s.axes.resize(8); s.buttons.resize(24); }
        samples[0].axes[7] = 30000; samples[1].axes[7] = -100; samples[2].axes[7] = -30000;
        auto result = learnControl(samples, true, Mode::Analog, defaultMappings()[7]);
        QVERIFY(result.ok); QVERIFY(result.mapping.inverted);
        for (int p = 0; p < 3; ++p) QCOMPARE(evaluate(result.mapping, samples[p]).value, 1000 + p * 500);
        for (auto& s : samples) { s.axes.assign(8, 0); s.hats.resize(1); }
        samples[0].hats[0] = SDL_HAT_RIGHT; samples[1].hats[0] = SDL_HAT_CENTERED; samples[2].hats[0] = SDL_HAT_LEFT;
        result = learnControl(samples, true, Mode::ThreePosition, defaultMappings()[9]);
        QVERIFY(result.ok); QCOMPARE(result.mapping.source, Source::HatPositions);
        for (int p = 0; p < 3; ++p) QCOMPARE(evaluate(result.mapping, samples[p]).value, 1000 + p * 500);
        Mapping restored; QVERIFY(fromJson(toJson(result.mapping), restored));
    }
    void learnAllSixteenWithEightAxesAndEightSwitches() {
        Snapshot baseline; baseline.connected = true; baseline.axes.resize(8); baseline.buttons.resize(24);
        for (int i = 0; i < 8; ++i) baseline.buttons[i * 3] = true;
        auto mappings = defaultMappings();
        for (int i = 0; i < 8; ++i) { mappings[i].source = Source::Axis; mappings[i].index = i; }
        for (int i = 0; i < 8; ++i) {
            std::array<Snapshot, 3> samples{baseline, baseline, baseline};
            for (int p = 0; p < 3; ++p) {
                samples[p].buttons[i * 3] = false; samples[p].buttons[i * 3 + p] = true;
            }
            auto result = learnControl(samples, true, Mode::ThreePosition, mappings[8 + i]);
            QVERIFY2(result.ok, qPrintable(result.error)); mappings[8 + i] = result.mapping;
        }
        for (int i = 0; i < 8; ++i) {
            auto snapshot = baseline; snapshot.buttons[i * 3] = false; snapshot.buttons[i * 3 + 2] = true;
            snapshot.axes[i] = 32767;
            auto values = evaluateAll(mappings, snapshot);
            for (int j = 0; j < 8; ++j) {
                QCOMPARE(int(values[j]), j == i ? 2000 : 1500);
                QCOMPARE(int(values[8 + j]), j == i ? 2000 : 1000);
            }
        }
    }
    void ambiguousOrInvalidLearningFailsClosed() {
        std::array<Snapshot, 3> samples;
        for (auto& s : samples) { s.connected = true; s.axes.resize(8); s.buttons.resize(24); }
        samples[2].axes[0] = 30000; samples[2].axes[1] = 30000;
        QVERIFY(!learnControl(samples, true, Mode::Analog, {}).ok);
        for (auto& s : samples) s.axes.assign(8, 0);
        QVERIFY(!learnControl(samples, true, Mode::ThreePosition, {}).ok);
        samples[0].connected = false; QVERIFY(!learnControl(samples, true, Mode::Analog, {}).ok);
        Mapping invalid; auto json = toJson(defaultMappings()[8]); json["source"] = int(Source::SwitchPositions);
        QVERIFY(!fromJson(json, invalid));
    }
};
QTEST_GUILESS_MAIN(Tests)
#include "tests.moc"
