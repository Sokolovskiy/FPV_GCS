#include "video.h"
#include <QVBoxLayout>
#include <QTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QImage>
#include <QColor>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <windows.h>

namespace fpv {
namespace {
class NativeVideoSurface final : public QWidget {
public:
    NativeVideoSurface() : m_placeholder(":/video/fatum.jpg") {}
    QPaintEngine* paintEngine() const override { return nullptr; }
    void setVideoAvailable(bool available) {
        m_available = available;
        if (!available) update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (m_available) return; // Direct3D11 owns live video pixels.
        const auto handle = reinterpret_cast<HWND>(winId());
        RECT bounds;
        GetClientRect(handle, &bounds);
        const auto dc = GetDC(handle);
        const auto color = m_placeholder.isNull() ? QColor(Qt::black) : m_placeholder.pixelColor(0, 0);
        const auto brush = CreateSolidBrush(RGB(color.red(), color.green(), color.blue()));
        FillRect(dc, &bounds, brush);
        DeleteObject(brush);
        if (!m_placeholder.isNull()) {
            const QSize pixels(bounds.right - bounds.left, bounds.bottom - bounds.top);
            if (m_cachedSize != pixels) {
                m_cachedSize = pixels;
                m_scaled = m_placeholder.scaled(pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGB32);
            }
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = m_scaled.width();
            bitmap.bmiHeader.biHeight = -m_scaled.height(); // Top-down Qt image.
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            // Keep the existing native surface: live D3D11 video still renders
            // directly to its HWND, without Qt frame copies or overlay widgets.
            SetDIBitsToDevice(dc, (pixels.width() - m_scaled.width()) / 2, (pixels.height() - m_scaled.height()) / 2,
                DWORD(m_scaled.width()), DWORD(m_scaled.height()), 0, 0, 0, UINT(m_scaled.height()),
                m_scaled.constBits(), &bitmap, DIB_RGB_COLORS);
        }
        ReleaseDC(handle, dc);
    }
private:
    bool m_available = false;
    QImage m_placeholder, m_scaled;
    QSize m_cachedSize;
};
constexpr auto pipelineDescription =
    "udpsrc address=192.168.144.3 port=%1 reuse=false timeout=1000000000 "
    "caps=\"application/x-rtp,media=(string)video,encoding-name=(string)H264,payload=(int)96,clock-rate=(int)90000\" "
    "! rtpjitterbuffer latency=30 drop-on-latency=true "
    "! rtph264depay ! h264parse ! decodebin "
    "! d3d11videosink name=video sync=false enable-last-sample=false force-aspect-ratio=true";
}

void VideoWorker::run() {
    // Everything that can initialize a codec, wait for UDP, or stop GStreamer
    // runs here. GStreamer owns its streaming threads; Qt never receives frames.
    const auto logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(logDir);
    QFile log(logDir + "/video.log");
    const bool logOpened = log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    auto report = [this, &log, logOpened](bool connected, const QString& detail) {
        if (logOpened) {
            log.write((QDateTime::currentDateTime().toString(Qt::ISODate) + " " +
            (connected ? "VIDEO CONNECTED " : "NO SIGNAL ") + detail + "\n").toUtf8());
            log.flush();
        }
        emit statusChanged(connected, detail);
    };
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        report(false, error ? QString::fromUtf8(error->message) : "GStreamer initialization failed");
        g_clear_error(&error);
        return;
    }
    const auto description = QString::fromLatin1(pipelineDescription).arg(m_port).toUtf8();
    while (!isInterruptionRequested()) {
        GstElement* pipeline = gst_parse_launch(description.constData(), &error);
        if (!pipeline || error) {
            report(false, error ? QString::fromUtf8(error->message) : "Pipeline creation failed");
            g_clear_error(&error);
            if (pipeline) gst_object_unref(pipeline);
            return;
        }
        auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "video");
        // HWND is created on the GUI thread before starting this worker.
        // Explicit sink + handle before PLAYING prevents a separate video window.
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), guintptr(m_handle));
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), FALSE);
        auto* bus = gst_element_get_bus(pipeline);
        report(false, QString::fromUtf8(description));
        bool failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
        if (failed) report(false, "Could not start video pipeline");
        QElapsedTimer clock;
        clock.start();
        qint64 lastFrame = -1;
        guint64 lastRendered = 0;
        bool connected = false;
        bool udpTimedOut = false;
        while (!isInterruptionRequested() && !failed) {
            auto* message = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND,
                GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_ELEMENT));
            if (message) {
                const auto* structure = gst_message_get_structure(message);
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
                    if (structure && gst_structure_has_name(structure, "GstUDPSrcTimeout") && !udpTimedOut) {
                        udpTimedOut = true;
                        report(false, QString("No UDP packets at 192.168.144.3:%1 for 1 second").arg(m_port));
                    }
                } else {
                    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                        gchar* debug = nullptr;
                        gst_message_parse_error(message, &error, &debug);
                        report(false, QString::fromUtf8(error->message));
                        g_clear_error(&error);
                        g_free(debug);
                    } else report(false, "End of video stream");
                    failed = true;
                }
                gst_message_unref(message);
                if (failed) break;
            }
            // BaseSink's rendered counter verifies decoded frames reached the renderer.
            // Poll only metadata; no pad probes, frame copies or per-frame UI callbacks.
            GstStructure* stats = nullptr;
            g_object_get(sink, "stats", &stats, nullptr);
            guint64 rendered = 0;
            if (stats) {
                gst_structure_get_uint64(stats, "rendered", &rendered);
                gst_structure_free(stats);
            }
            if (rendered != lastRendered) {
                lastRendered = rendered; lastFrame = clock.elapsed(); udpTimedOut = false;
            }
            const bool fresh = lastFrame >= 0 && clock.elapsed() - lastFrame < 1000;
            if (fresh != connected) {
                connected = fresh;
                report(connected, connected ? QString("H.264/RTP/UDP :%1 · Direct3D11").arg(m_port) : "No rendered frames for 1 second");
            }
            // A restarted sender can change RTP sequence/timestamps or codec state.
            // Recreate the receiver only while stalled, leaving live latency unchanged.
            const auto idleMs = lastFrame < 0 ? clock.elapsed() : clock.elapsed() - lastFrame;
            if (idleMs >= (lastFrame < 0 ? 10000 : 5000)) {
                report(false, "No decoded video; restarting video receiver");
                break;
            }
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        // Retry stream/device errors without a busy loop. Keep shutdown interruptible.
        if (failed) for (int i = 0; i < 20 && !isInterruptionRequested(); ++i) msleep(50);
    }
}

VideoPanel::VideoPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* surface = new NativeVideoSurface;
    surface->setMinimumSize(320, 240);
    surface->setStyleSheet("background:black;");
    surface->setAttribute(Qt::WA_NativeWindow);
    surface->setAttribute(Qt::WA_PaintOnScreen);
    surface->setAttribute(Qt::WA_NoSystemBackground);
    layout->addWidget(surface, 1);
    m_worker = new VideoWorker(quintptr(surface->winId()), this);
    connect(m_worker, &VideoWorker::statusChanged, this, &VideoPanel::statusChanged);
    connect(m_worker, &VideoWorker::statusChanged, this, [surface](bool connected, const QString&) {
        surface->setVideoAvailable(connected);
    });
    connect(m_worker, &QThread::finished, this, [this] { if (m_stopping) emit stopped(); });
    QTimer::singleShot(0, this, [this] { if (!m_stopping) m_worker->start(); });
}

VideoPanel::~VideoPanel() {
    m_worker->requestInterruption();
    m_worker->wait();
}

bool VideoPanel::stop() {
    m_stopping = true;
    m_worker->requestInterruption();
    return !m_worker->isRunning();
}
}
