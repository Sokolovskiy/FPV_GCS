#pragma once
#include "channel.h"
#include <QUdpSocket>
#include <QElapsedTimer>

namespace fpv {
// Network byte order: magic, version, channel count, size, session, seq, monotonic us, CH1..16.
QByteArray encodeRcPacket(const Channels& channels, quint32 session, quint32 sequence, quint64 timeUs);
class UdpSender {
public:
    UdpSender();
    bool send(const Channels& channels, bool connected);
    QHostAddress address{QStringLiteral("192.168.144.20")};
    QHostAddress sourceAddress{QStringLiteral("192.168.144.3")};
    quint16 port = 14600;
    quint64 sent = 0;
    QString error;
private:
    QUdpSocket socket;
    QElapsedTimer clock;
    quint32 session, sequence = 0;
};
}
