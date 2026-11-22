#include "profile.h"
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace fpv {
Profiles::Profiles(QString path) : m_path(path.isEmpty() ?
    QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/profiles.json" : path) {}
Mappings Profiles::load(const QString& identity, QString* error) const {
    auto defaults = defaultMappings();
    QFile file(m_path);
    if (!file.exists()) return defaults;
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return defaults; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc["version"].toInt() != 1) {
        if (error) *error = "Profile format is invalid; safe defaults are active.";
        return defaults;
    }
    const auto array = doc["devices"].toObject()[identity].toArray();
    if (array.isEmpty()) return defaults;
    if (array.size() != ChannelCount) { if (error) *error = "Profile must contain 16 channels."; return defaults; }
    for (int i = 0; i < ChannelCount; ++i) {
        if (!array[i].isObject() || !fromJson(array[i].toObject(), defaults[i])) {
            if (error) *error = "Invalid channel profile; safe defaults are active.";
            return defaultMappings();
        }
    }
    return defaults;
}
bool Profiles::save(const QString& identity, const Mappings& mappings, QString* error) {
    if (identity.isEmpty()) { if (error) *error = "No controller selected."; return false; }
    QJsonObject root{{"version", 1}, {"devices", QJsonObject{}}};
    QFile old(m_path);
    if (old.open(QIODevice::ReadOnly)) {
        const auto previous = QJsonDocument::fromJson(old.readAll());
        if (previous.isObject() && previous["version"].toInt() == 1) root = previous.object();
        old.close(); // Windows must release the destination before QSaveFile replaces it.
    }
    QJsonArray channels;
    for (const auto& mapping : mappings) channels.append(toJson(mapping));
    auto devices = root["devices"].toObject(); devices[identity] = channels; root["devices"] = devices;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile out(m_path);
    const auto bytes = QJsonDocument(root).toJson();
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        if (error) *error = out.errorString(); return false;
    }
    return true;
}
}
