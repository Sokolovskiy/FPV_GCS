#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QComboBox>
#include <QLabel>
#include <QDialog>
#include <QFormLayout>
#include <QPushButton>
#include <QTimer>
#include <functional>

namespace fpv {
class CaptureClient final : public QObject {
    Q_OBJECT
public:
    explicit CaptureClient(QObject* parent);
    QJsonObject state;
    bool online = false;
    bool busy() const { return m_posting || !m_operation.isEmpty(); }
    void request(const QString& action, QJsonObject payload = {});
signals:
    void changed();
    void failed(const QString& message);
private:
    QNetworkAccessManager m_network;
    QTimer m_timer;
    QString m_base, m_operation;
    bool m_polling = false, m_posting = false;
    qint64 m_started = 0;
    void poll();
};

class CaptureBar final : public QWidget {
    Q_OBJECT
public:
    explicit CaptureBar(QWidget* parent = nullptr);
    void videoStatus(bool connected, const QString& detail);
private:
    CaptureClient* m_client;
    QComboBox* m_devices;
    QLabel *m_status, *m_notice;
    QDialog* m_dialog = nullptr;
    void settings();
};
}
