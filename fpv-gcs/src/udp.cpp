#include "udp.h"
#include <QDataStream>
#include <QRandomGenerator>
#include <algorithm>

namespace fpv {
QByteArray encodeRcPacket(const Channels& channels, quint32 session, quint32 sequence, quint64 timeUs) {
    if (std::any_of(channels.begin(), channels.end(), [](auto value) { return value < 1000 || value > 2000; })) return {};
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.writeRawData("FRC1", 4);
    stream << quint8(1) << quint8(ChannelCount) << quint16(56) << session << sequence << timeUs;
    for (auto value : channels) stream << quint16(value);
    return packet;
}
UdpSender::UdpSender() : session(QRandomGenerator::global()->generate()) { clock.start(); }
bool UdpSender::send(const Channels& channels, bool connected) {
    error.clear();
    if (!connected) return false;
    // Retry when Ethernet appears late; never send RC through the Wi-Fi address.
    if (socket.state() == QAbstractSocket::UnconnectedState &&
        !socket.bind(sourceAddress, 0)) {
        error = socket.errorString(); return false;
    }
    const auto packet = encodeRcPacket(channels, session, sequence, clock.nsecsElapsed() / 1000);
    if (packet.isEmpty()) { error = "Invalid RC values"; return false; }
    if (socket.writeDatagram(packet, address, port) != packet.size()) {
        error = socket.errorString(); socket.abort(); return false;
    }
    ++sequence; ++sent; return true;
}
}
