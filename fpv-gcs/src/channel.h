#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <QString>
#include <QStringList>
#include <QJsonObject>

namespace fpv {
constexpr int ChannelCount = 16;
using Channels = std::array<std::uint16_t, ChannelCount>;
struct Snapshot {
    bool connected = false;
    std::vector<int> axes;
    std::vector<bool> buttons;
    std::vector<int> hats;
};
enum class Mode { Analog, TwoPosition, ThreePosition };
enum class Source { Unassigned, Axis, Button, ButtonPair, Hat, SwitchPositions, HatPositions };
struct Mapping {
    Mode mode = Mode::Analog;
    Source source = Source::Unassigned;
    int index = 0;
    int second = 1;
    int lowDirection = 1;
    int highDirection = 4;
    int minimum = -32768;
    int center = 0;
    int maximum = 32767;
    int safe = 1000;
    bool inverted = false;
    QString label;
    QString functionText;
    std::vector<int> switchInputs;
    std::array<std::vector<bool>, 3> switchStates;
    std::array<int, 3> hatStates{1, 0, 4};
};
struct Value { int value; bool live; QString reason; };
using Mappings = std::array<Mapping, ChannelCount>;
Mappings defaultMappings();
Value evaluate(const Mapping&, const Snapshot&);
Channels evaluateAll(const Mappings&, const Snapshot&);
bool commandsReady(const Mappings&, const Snapshot&);
QString channelName(int index);
QString mappingDescription(const Mapping&);
QJsonObject toJson(const Mapping&);
bool fromJson(const QJsonObject&, Mapping&);
}
