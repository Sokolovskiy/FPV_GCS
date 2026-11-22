#include "learning.h"
#include <algorithm>
#include <cmath>

namespace fpv {
AxisMotionCapture::AxisMotionCapture(const Snapshot& baseline)
    : m_minimum(baseline.axes), m_maximum(baseline.axes), m_available(baseline.connected) {}
void AxisMotionCapture::observe(const Snapshot& sample) {
    if (!sample.connected || sample.axes.size() != m_minimum.size()) { m_available = false; return; }
    for (size_t i = 0; i < sample.axes.size(); ++i) {
        m_minimum[i] = std::min(m_minimum[i], sample.axes[i]);
        m_maximum[i] = std::max(m_maximum[i], sample.axes[i]);
    }
}
int AxisMotionCapture::detectedAxis() const {
    if (!m_available) return -3;
    int detected = -1;
    for (int i = 0; i < int(m_minimum.size()); ++i) {
        if (m_maximum[i] - m_minimum[i] < 2048) continue;
        if (detected >= 0) return -2;
        detected = i;
    }
    return detected;
}
LearningResult learnControl(const std::array<Snapshot, 3>& positions, bool middle, Mode requested, const Mapping& previous) {
    auto fail = [](QString error) { return LearningResult{false, {}, error}; };
    for (int p : {0, middle ? 1 : 0, 2}) {
        if (!positions[p].connected) return fail("Пульт відключений. Повторіть призначення.");
        if (positions[p].axes.size() != positions[0].axes.size() || positions[p].buttons.size() != positions[0].buttons.size() || positions[p].hats.size() != positions[0].hats.size())
            return fail("Під час навчання змінився пульт. Повторіть призначення.");
    }
    Mapping result = previous; result.mode = requested; result.inverted = false;
    result.switchInputs.clear(); result.switchStates = {};
    std::vector<int> axes, hats;
    for (int i = 0; i < int(positions[0].axes.size()); ++i) {
        const int a = positions[0].axes[i], b = positions[2].axes[i];
        if (std::abs(a - b) >= 1024) axes.push_back(i);
    }
    for (int i = 0; i < int(positions[0].buttons.size()); ++i)
        if (positions[0].buttons[i] != positions[2].buttons[i] || (middle && positions[0].buttons[i] != positions[1].buttons[i])) result.switchInputs.push_back(i);
    for (int i = 0; i < int(positions[0].hats.size()); ++i)
        if (positions[0].hats[i] != positions[2].hats[i] || (middle && positions[0].hats[i] != positions[1].hats[i])) hats.push_back(i);
    if (axes.size() > 1 || hats.size() > 1 || (!axes.empty() && (!hats.empty() || !result.switchInputs.empty())))
        return fail("Змінився більш ніж один орган керування. Рухайте лише один орган і повторіть.");
    if (!axes.empty()) {
        result.source = Source::Axis; result.index = axes[0];
        const int low = positions[0].axes[result.index], high = positions[2].axes[result.index];
        result.minimum = std::min(low, high); result.maximum = std::max(low, high);
        result.inverted = low > high;
        result.center = middle ? positions[1].axes[result.index] : (result.minimum + result.maximum) / 2;
        if (!(result.minimum < result.center && result.center < result.maximum)) {
            if (requested != Mode::Analog) return fail("Середнє положення не відрізняється від крайніх. Повторіть навчання.");
            // A physical two-position switch can itself be exposed as an axis.
            result.mode = Mode::TwoPosition; result.center = (result.minimum + result.maximum) / 2;
        }
    } else if (!result.switchInputs.empty()) {
        if (!hats.empty()) return fail("Рухайте лише один перемикач і повторіть.");
        result.source = Source::SwitchPositions;
        for (int p = 0; p < 3; ++p) {
            if (p == 1 && !middle) continue;
            for (int input : result.switchInputs) result.switchStates[p].push_back(positions[p].buttons[input]);
        }
        if (result.switchStates[0] == result.switchStates[2]) return fail("Положення 1000 і 2000 однакові. Переведіть перемикач в обидва крайні положення.");
        const bool three = middle && result.switchStates[1] != result.switchStates[0] && result.switchStates[1] != result.switchStates[2];
        if (requested == Mode::ThreePosition && !three) return fail("Для трипозиційного перемикача потрібні три різні положення.");
        result.mode = three ? Mode::ThreePosition : Mode::TwoPosition;
        if (!three) result.switchStates[1].clear();
    } else if (!hats.empty()) {
        result.source = Source::HatPositions; result.index = hats[0];
        result.hatStates[0] = positions[0].hats[result.index]; result.hatStates[2] = positions[2].hats[result.index];
        result.hatStates[1] = middle ? positions[1].hats[result.index] : 0;
        if (result.hatStates[0] == result.hatStates[2]) return fail("Крайні положення однакові. Повторіть навчання.");
        const bool three = middle && result.hatStates[1] != result.hatStates[0] && result.hatStates[1] != result.hatStates[2];
        if (requested == Mode::ThreePosition && !three) return fail("Потрібні три різні положення перемикача.");
        result.mode = three ? Mode::ThreePosition : Mode::TwoPosition;
    } else return fail("Зміни органа керування не виявлено. Переведіть його між крайніми положеннями і повторіть.");
    Mapping validated;
    if (!fromJson(toJson(result), validated)) return fail("Не вдалося зберегти положення органа. Повторіть навчання.");
    return {true, result, {}};
}
}
