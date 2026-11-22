#pragma once
#include "channel.h"
namespace fpv {
class AxisMotionCapture {
public:
    explicit AxisMotionCapture(const Snapshot& baseline);
    void observe(const Snapshot& sample);
    // -1: no movement, -2: ambiguous movement, -3: input unavailable.
    int detectedAxis() const;
private:
    std::vector<int> m_minimum, m_maximum;
    bool m_available;
};
struct LearningResult { bool ok = false; Mapping mapping; QString error; };
LearningResult learnControl(const std::array<Snapshot, 3>& positions, bool hasMiddle, Mode requested, const Mapping& previous);
}
