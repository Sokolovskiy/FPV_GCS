#include "channel.h"
#include <algorithm>
#include <cmath>
#include <QJsonArray>

namespace fpv {
bool commandsReady(const Mappings& mappings, const Snapshot& snapshot) {
    if (!snapshot.connected) return false;
    bool assigned = false;
    for (const auto& mapping : mappings) if (mapping.source != Source::Unassigned) {
        assigned = true;
        if (!evaluate(mapping, snapshot).live) return false;
    }
    return assigned;
}
Mappings defaultMappings() {
    Mappings result;
    for (int i = 0; i < ChannelCount; ++i)
        result[i].safe = (i == 0 || i == 1 || i == 3) ? 1500 : 1000;
    return result;
}
QString channelName(int i) {
    static const QStringList primary = {"Roll", "Pitch", "Throttle", "Yaw"};
    return i < 4 ? primary[i] : QString("AUX%1").arg(i - 3);
}
Value evaluate(const Mapping& m, const Snapshot& s) {
    const int safe = std::clamp(m.safe, 1000, 2000);
    auto fallback = [&](QString reason) { return Value{safe, false, reason}; };
    if (!s.connected) return fallback("Controller disconnected");
    if (m.source == Source::Unassigned) return fallback("Not assigned");
    double n = 0;
    auto button = [&](int index) { return index >= 0 && index < int(s.buttons.size()); };
    if (m.source == Source::Axis) {
        if (m.index < 0 || m.index >= int(s.axes.size())) return fallback("Input unavailable");
        if (!(m.minimum < m.center && m.center < m.maximum)) return fallback("Calibration invalid");
        const int raw = s.axes[m.index];
        n = raw >= m.center ? double(raw - m.center) / (m.maximum - m.center)
                            : double(raw - m.center) / (m.center - m.minimum);
        n = std::clamp(n, -1.0, 1.0);
        if (m.mode == Mode::TwoPosition) n = n >= 0 ? 1 : -1;
        else if (m.mode == Mode::ThreePosition) n = n < -.5 ? -1 : (n > .5 ? 1 : 0);
    } else if (m.source == Source::Button) {
        if (!button(m.index) || m.mode != Mode::TwoPosition) return fallback("Select a two-position input");
        n = s.buttons[m.index] ? 1 : -1;
    } else if (m.source == Source::ButtonPair) {
        if (!button(m.index) || !button(m.second) || m.index == m.second || m.mode != Mode::ThreePosition)
            return fallback("Select two different inputs");
        const bool low = s.buttons[m.index], high = s.buttons[m.second];
        if (low && high) return fallback("Conflicting switch positions");
        n = low ? -1 : high ? 1 : 0;
    } else if (m.source == Source::SwitchPositions) {
        if (m.switchInputs.empty() || m.mode == Mode::Analog) return fallback("Перепризначте перемикач");
        std::vector<bool> state;
        for (int input : m.switchInputs) {
            if (!button(input)) return fallback("Перемикач недоступний");
            state.push_back(s.buttons[input]);
        }
        if (state == m.switchStates[0]) n = -1;
        else if (state == m.switchStates[2]) n = 1;
        else if (m.mode == Mode::ThreePosition && state == m.switchStates[1]) n = 0;
        else return fallback("Нерозпізнане положення перемикача");
    } else if (m.source == Source::HatPositions) {
        if (m.index < 0 || m.index >= int(s.hats.size()) || m.mode == Mode::Analog) return fallback("Перемикач недоступний");
        const int state = s.hats[m.index];
        if (state == m.hatStates[0]) n = -1;
        else if (state == m.hatStates[2]) n = 1;
        else if (m.mode == Mode::ThreePosition && state == m.hatStates[1]) n = 0;
        else return fallback("Нерозпізнане положення перемикача");
    } else if (m.source == Source::Hat) {
        if (m.index < 0 || m.index >= int(s.hats.size()) || m.mode == Mode::Analog)
            return fallback("POV input unavailable");
        const int hat = s.hats[m.index];
        const bool low = (hat & m.lowDirection) != 0, high = (hat & m.highDirection) != 0;
        if (m.mode == Mode::TwoPosition) n = high ? 1 : -1;
        else {
            if (m.lowDirection == m.highDirection || (low && high)) return fallback("Conflicting POV positions");
            n = low ? -1 : high ? 1 : 0;
        }
    } else return fallback("Unknown source");
    if (m.inverted) n = -n;
    return {std::clamp(int(std::lround(1500 + 500 * n)), 1000, 2000), true, {}};
}
Channels evaluateAll(const Mappings& mappings, const Snapshot& snapshot) {
    Channels values{};
    for (int i = 0; i < ChannelCount; ++i) values[i] = evaluate(mappings[i], snapshot).value;
    return values;
}
QString mappingDescription(const Mapping& m) {
    if (m.source != Source::Unassigned && !m.label.trimmed().isEmpty()) return m.label.trimmed();
    if (m.source != Source::Unassigned) {
        if (m.mode == Mode::Analog) return "Стік / регулятор";
        return m.mode == Mode::TwoPosition ? "Перемикач · 2 положення" : "Перемикач · 3 положення";
    }
    switch (m.source) {
    case Source::Unassigned: return "Not assigned";
    case Source::Axis: return QString("Axis %1").arg(m.index + 1);
    case Source::Button: return QString("Switch input %1").arg(m.index + 1);
    case Source::ButtonPair: return QString("Switch inputs %1 / %2").arg(m.index + 1).arg(m.second + 1);
    case Source::Hat: return QString("POV %1").arg(m.index + 1);
    case Source::SwitchPositions:
    case Source::HatPositions: return "Перемикач";
    }
    return "Not assigned";
}
QJsonObject toJson(const Mapping& m) {
    QJsonArray inputs, states, hats;
    for (int input : m.switchInputs) inputs.append(input);
    for (const auto& state : m.switchStates) {
        QJsonArray values; for (bool value : state) values.append(value); states.append(values);
    }
    for (int state : m.hatStates) hats.append(state);
    return {{"mode", int(m.mode)}, {"source", int(m.source)}, {"index", m.index}, {"second", m.second},
        {"lowDirection", m.lowDirection}, {"highDirection", m.highDirection}, {"minimum", m.minimum},
        {"center", m.center}, {"maximum", m.maximum}, {"safe", m.safe}, {"inverted", m.inverted},
        {"label", m.label}, {"functionText", m.functionText}, {"switchInputs", inputs}, {"switchStates", states}, {"hatStates", hats}};
}
bool fromJson(const QJsonObject& o, Mapping& m) {
    auto number = [&](const char* key, int low, int high, int& target) {
        const auto v = o.value(key);
        if (!v.isDouble() || v.toDouble() != v.toInt() || v.toInt() < low || v.toInt() > high) return false;
        target = v.toInt(); return true;
    };
    Mapping candidate;
    int mode = 0, source = 0;
    if (!number("mode", 0, 2, mode) || !number("source", 0, 6, source) ||
        !number("index", 0, 4095, candidate.index) || !number("second", 0, 4095, candidate.second) ||
        !number("minimum", -32768, 32767, candidate.minimum) || !number("center", -32768, 32767, candidate.center) ||
        !number("maximum", -32768, 32767, candidate.maximum) || !number("safe", 1000, 2000, candidate.safe) ||
        !number("lowDirection", 1, 8, candidate.lowDirection) || !number("highDirection", 1, 8, candidate.highDirection) ||
        !o.value("inverted").isBool()) return false;
    candidate.mode = Mode(mode); candidate.source = Source(source); candidate.inverted = o["inverted"].toBool();
    if (o.contains("label") && (!o["label"].isString() || o["label"].toString().size() > 100)) return false;
    candidate.label = o["label"].toString();
    if (o.contains("functionText") && (!o["functionText"].isString() || o["functionText"].toString().size() > 100)) return false;
    candidate.functionText = o["functionText"].toString();
    if (candidate.source == Source::SwitchPositions) {
        if (candidate.mode == Mode::Analog || !o["switchInputs"].isArray() || !o["switchStates"].isArray()) return false;
        const auto inputs = o["switchInputs"].toArray(), states = o["switchStates"].toArray();
        if (inputs.isEmpty() || inputs.size() > 128 || states.size() != 3) return false;
        for (const auto& input : inputs) {
            if (!input.isDouble() || input.toDouble() != input.toInt() || input.toInt() < 0 || input.toInt() > 4095 ||
                std::find(candidate.switchInputs.begin(), candidate.switchInputs.end(), input.toInt()) != candidate.switchInputs.end()) return false;
            candidate.switchInputs.push_back(input.toInt());
        }
        for (int position = 0; position < 3; ++position) {
            if (!states[position].isArray()) return false;
            const auto state = states[position].toArray();
            if (position == 1 && candidate.mode == Mode::TwoPosition && state.isEmpty()) continue;
            if (state.size() != inputs.size()) return false;
            for (const auto& value : state) { if (!value.isBool()) return false; candidate.switchStates[position].push_back(value.toBool()); }
        }
        if (candidate.switchStates[0] == candidate.switchStates[2] || (candidate.mode == Mode::ThreePosition &&
            (candidate.switchStates[0] == candidate.switchStates[1] || candidate.switchStates[1] == candidate.switchStates[2]))) return false;
    }
    if (candidate.source == Source::HatPositions) {
        if (candidate.mode == Mode::Analog || !o["hatStates"].isArray() || o["hatStates"].toArray().size() != 3) return false;
        const auto states = o["hatStates"].toArray();
        for (int i = 0; i < 3; ++i) {
            if (!states[i].isDouble() || states[i].toDouble() != states[i].toInt() || states[i].toInt() < 0 || states[i].toInt() > 15) return false;
            candidate.hatStates[i] = states[i].toInt();
        }
        if (candidate.hatStates[0] == candidate.hatStates[2] || (candidate.mode == Mode::ThreePosition &&
            (candidate.hatStates[0] == candidate.hatStates[1] || candidate.hatStates[1] == candidate.hatStates[2]))) return false;
    }
    if (candidate.source == Source::Axis && !(candidate.minimum < candidate.center && candidate.center < candidate.maximum)) return false;
    if (candidate.source == Source::Button && candidate.mode != Mode::TwoPosition) return false;
    if (candidate.source == Source::ButtonPair && (candidate.mode != Mode::ThreePosition || candidate.index == candidate.second)) return false;
    auto direction = [](int x) { return x == 1 || x == 2 || x == 4 || x == 8; };
    if (!direction(candidate.lowDirection) || !direction(candidate.highDirection)) return false;
    if (candidate.source == Source::Hat && (candidate.mode == Mode::Analog ||
        (candidate.mode == Mode::ThreePosition && candidate.lowDirection == candidate.highDirection))) return false;
    m = candidate; return true;
}
}
