#pragma once
#include "channel.h"

namespace fpv {
class Profiles {
public:
    explicit Profiles(QString path = {});
    Mappings load(const QString& identity, QString* error = nullptr) const;
    bool save(const QString& identity, const Mappings&, QString* error = nullptr);
    QString path() const { return m_path; }
private:
    QString m_path;
};
}
