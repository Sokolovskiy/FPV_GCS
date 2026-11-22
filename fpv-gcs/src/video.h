#pragma once
#include <QWidget>
#include <QThread>

namespace fpv {
class VideoWorker final : public QThread {
    Q_OBJECT
public:
    VideoWorker(quintptr handle, QObject* parent, quint16 port = 5602)
        : QThread(parent), m_handle(handle), m_port(port) {}
signals:
    void statusChanged(bool connected, const QString& detail);
protected:
    void run() override;
private:
    const quintptr m_handle;
    const quint16 m_port;
};

class VideoPanel final : public QWidget {
    Q_OBJECT
public:
    explicit VideoPanel(QWidget* parent = nullptr);
    ~VideoPanel() override;
    bool stop();
signals:
    void statusChanged(bool connected, const QString& detail);
    void stopped();
private:
    VideoWorker* m_worker;
    bool m_stopping = false;
};
}
