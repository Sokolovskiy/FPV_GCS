#include "window.h"
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
#include <glib.h>
#include <windows.h>
#include <mmsystem.h>
#include <QDebug>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/app/fatum.ico"));
    const auto appDir = QCoreApplication::applicationDirPath();
    // Update GStreamer's CRT environment as well (the SDK uses MSVC, Qt uses MinGW).
    g_setenv("PATH", (QDir::toNativeSeparators(appDir).toUtf8() + ";" + qgetenv("PATH")).constData(), TRUE);
    if (QDir(appDir + "/gstreamer-1.0").exists()) {
        g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", (appDir + "/gstreamer-1.0").toUtf8().constData(), TRUE);
        g_setenv("GST_PLUGIN_SCANNER_1_0", (appDir + "/gst-plugin-scanner.exe").toUtf8().constData(), TRUE);
    }
    QCoreApplication::setOrganizationName("FPVGCS");
    QCoreApplication::setApplicationName("FPV GCS");
    QCoreApplication::setApplicationVersion("0.1.10");
    const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);
    QLockFile instance(dataDir + "/application.lock");
    if (!instance.tryLock(0)) return 0;
    // Preserve the 10 ms RC timer when Windows 11 occludes/minimizes this window.
    PROCESS_POWER_THROTTLING_STATE timing{};
    timing.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    timing.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    timing.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &timing, sizeof(timing)))
        qWarning() << "RC timer power policy failed:" << GetLastError();
    const bool timerResolution = timeBeginPeriod(1) == TIMERR_NOERROR;
    if (!timerResolution) qWarning() << "RC timer resolution request failed";
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [timerResolution] {
        if (timerResolution) timeEndPeriod(1);
    });
    app.setStyle("Fusion"); app.setFont(QFont("Segoe UI", 10));
    app.setStyleSheet(R"(
        QWidget { background:#101923; color:#e6edf4; }
        QLabel#title { font-size:27px; font-weight:700; letter-spacing:1px; }
        QLabel#subtitle { color:#9aa9ba; font-size:11px; }
        QLabel#localBadge { color:#79c6ff; background:#182d40; border:1px solid #294c68; border-radius:5px; padding:7px; font-size:10px; }
        QTableWidget { background:#131f2c; alternate-background-color:#172535; border:1px solid #29394b; border-radius:6px; }
        QHeaderView::section { background:#1b2a3a; color:#9fb4c9; border:0; padding:8px; font-size:11px; }
        QPushButton { background:#22374b; border:1px solid #35516c; border-radius:4px; padding:5px 10px; }
        QPushButton:hover { background:#30516d; }
        QPushButton:pressed { background:#19334b; }
        QPushButton:disabled { color:#647486; border-color:#253341; }
        QComboBox, QSpinBox { background:#1c2c3c; border:1px solid #35516c; border-radius:4px; padding:5px; }
        QLineEdit { background:#1c2c3c; border:1px solid #35516c; border-radius:4px; padding:5px; }
        QComboBox QAbstractItemView { background:#1c2c3c; selection-background-color:#315a78; }
        QCheckBox::indicator { width:14px; height:14px; border:1px solid #637f98; border-radius:3px; background:#152536; }
        QCheckBox::indicator:checked { background:#5ab8f4; border:2px solid #c6e8ff; }
        QCheckBox::indicator:disabled { border-color:#354455; background:#15202b; }
        QProgressBar { background:#29394b; border:0; border-radius:5px; }
        QProgressBar::chunk { background:#5ab8f4; border-radius:5px; }
        QGroupBox { border:1px solid #35516c; border-radius:4px; margin-top:12px; padding-top:14px; }
        QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px; }
        QPlainTextEdit { background:#131f2c; border:1px solid #35516c; font-family:Consolas; }
        QToolTip { background:#22374b; color:white; border:1px solid #35516c; }
    )");
    fpv::Window window; window.showMaximized(); return app.exec();
}
