#include "capture.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolButton>
#include <QGroupBox>

class VideoApi {
public:
    QTcpServer server;
    QJsonObject state, configured;
    VideoApi() {
        server.listen(QHostAddress::LocalHost, 0);
        qputenv("FPV_VIDEO_API_URL", QByteArray("http://127.0.0.1:") + QByteArray::number(server.serverPort()));
        QJsonArray controls;
        for (int i = 0; i < 35; ++i) controls.append(QJsonObject{{"name", QString("control_%1").arg(i)},
            {"type", "int"}, {"min", 0}, {"max", 100}, {"step", 1}, {"default", 50}, {"value", 50}, {"writable", true}});
        state = {{"active", "usb-a"}, {"devices", QJsonArray{QJsonObject{{"id", "usb-a"}, {"name", "USB capture"}, {"available", true}}}},
            {"detail", QJsonObject{{"controls", controls}, {"formats", QJsonArray{QJsonObject{{"pixel", "YUYV"}, {"sizes", QJsonArray{
                QJsonObject{{"width", 640}, {"height", 480}, {"rates", QJsonArray{"30.000"}}},
                QJsonObject{{"width", 320}, {"height", 240}, {"rates", QJsonArray{"15.000"}}}}}}}},
                {"actual", QJsonObject{{"pixel", "YUYV"}, {"width", 640}, {"height", 480}, {"fps", "30.000"}}}}}};
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    const int end = request.indexOf("\r\n\r\n"); if (end < 0) return;
                    int length = 0;
                    for (auto line : request.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (request.size() < end + 4 + length || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    QJsonObject response = state;
                    QByteArray code = "200 OK";
                    if (request.startsWith("POST /configure ")) {
                        configured = QJsonDocument::fromJson(request.mid(end+4, length)).object();
                        auto detail = state.value("detail").toObject(); detail["actual"] = configured.value("format"); state["detail"] = detail;
                        state["operations"] = QJsonObject{{"configured", QJsonObject{{"done", true}, {"error", ""}}}};
                        response = {{"operation", "configured"}}; code = "202 Accepted";
                    }
                    const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + code + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

class CaptureTests : public QObject {
    Q_OBJECT
private slots:
    void captureFormatSendsTheSelectedTuple() {
        VideoApi api;
        fpv::CaptureBar bar; bar.show();
        QTRY_COMPARE(bar.findChild<QComboBox*>()->currentData().toString(), QString("usb-a"));
        QTest::mouseClick(bar.findChild<QToolButton*>(), Qt::LeftButton);
        auto* dialog = bar.findChild<QDialog*>(); QVERIFY(dialog);
        for (auto* group : dialog->findChildren<QGroupBox*>()) QVERIFY(group->title() != "H.264 Encoding");
        auto* resolution = dialog->findChild<QComboBox*>("captureResolution"); QVERIFY(resolution);
        resolution->setCurrentIndex(1);
        QCOMPARE(dialog->findChild<QSpinBox*>("captureWidth")->value(), 320);
        QCOMPARE(dialog->findChild<QSpinBox*>("captureHeight")->value(), 240);
        QCOMPARE(dialog->findChild<QComboBox*>("captureFps")->currentText(), QString("15.000"));
        auto* apply = dialog->findChild<QPushButton*>("applyCaptureFormat"); QVERIFY(apply);
        QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_VERIFY(!api.configured.isEmpty());
        const auto format = api.configured.value("format").toObject();
        QCOMPARE(format.value("pixel").toString(), QString("YUYV"));
        QCOMPARE(format.value("width").toInt(), 320);
        QCOMPARE(format.value("height").toInt(), 240);
        QCOMPARE(format.value("fps").toString(), QString("15.000"));
        QVERIFY(!api.configured.contains("encoding"));
    }

    void imageReadbackPreservesScrollAndPendingFormat() {
        VideoApi api;
        fpv::CaptureBar bar; bar.show();
        QTRY_COMPARE(bar.findChild<QComboBox*>()->currentData().toString(), QString("usb-a"));
        QTest::mouseClick(bar.findChild<QToolButton*>(), Qt::LeftButton);
        auto* dialog = bar.findChild<QDialog*>(); QVERIFY(dialog);
        auto* scroll = dialog->findChild<QScrollArea*>(); QVERIFY(scroll);
        auto* original = scroll->widget();
        dialog->findChild<QComboBox*>("captureResolution")->setCurrentIndex(1);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 100);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()/2);
        const int position = scroll->verticalScrollBar()->value();
        auto detail = api.state.value("detail").toObject();
        auto controls = detail.value("controls").toArray(); auto spec = controls[25].toObject();
        spec["value"] = 72; controls[25] = spec; detail["controls"] = controls; api.state["detail"] = detail;
        QTRY_COMPARE(dialog->findChild<QWidget*>("imageControl_control_25")->findChild<QSpinBox*>()->value(), 72);
        QCOMPARE(scroll->widget(), original);
        QCOMPARE(scroll->verticalScrollBar()->value(), position);
        QCOMPARE(dialog->findChild<QSpinBox*>("captureWidth")->value(), 320);
        QCOMPARE(dialog->findChild<QComboBox*>("captureFps")->currentText(), QString("15.000"));
    }
};
QTEST_MAIN(CaptureTests)
#include "capture_tests.moc"
