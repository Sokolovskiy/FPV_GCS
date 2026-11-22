#pragma once
#include "channel.h"
#include <QString>
#include <QList>
#include <memory>
#include <optional>
namespace fpv {
struct HidAxisUsage { unsigned page = 0, usage = 0; int arrayElement = -1; };
struct HidDevice {
    unsigned id = 0;
    unsigned vendor = 0, product = 0;
    QString name, path;
    int axes = 0;
    std::vector<HidAxisUsage> axisUsages;
};
// Explicit USB layouts: 9 Desktop + 7 Simulation, or 6 Desktop + 10 Sliders.
std::optional<Mappings> rcMappingsForHid(const std::vector<HidAxisUsage>& axes);
std::optional<std::uint32_t> packedHidValue(const std::vector<std::uint8_t>& bytes, int element, int bits);
int normalizeHidValue(std::uint32_t raw, int bits, std::int64_t minimum, std::int64_t maximum);
bool hidReportsFresh(const std::vector<bool>& seen, const std::vector<std::int64_t>& received, std::int64_t now);
class RawHid {
public:
    RawHid();
    ~RawHid();
    QList<HidDevice> devices();
    bool select(unsigned id);
    void pump();
    Snapshot snapshot() const;
    HidDevice selectedDevice() const;
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
