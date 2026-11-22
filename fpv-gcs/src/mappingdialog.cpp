#include "mappingdialog.h"
#include "learning.h"
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
namespace fpv {
MappingDialog::MappingDialog(int channel, Mapping initial, Controller& controller, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_mapping(initial) {
    setWindowTitle(QString("CH%1 — %2: орган пульта").arg(channel + 1).arg(channelName(channel)));
    setMinimumWidth(590);
    auto* layout = new QVBoxLayout(this); layout->setSpacing(16);
    auto* intro = new QLabel("Призначте стік, регулятор або перемикач на цей RC-канал.\nНатисніть розпізнавання й задійте потрібний орган.");
    const auto radio = controller.radioMappings();
    const bool fromRadio = radio && initial.source == Source::Axis && initial.index == (*radio)[channel].index;
    if (fromRadio) intro->setText(QString("Автоматично отримується CH%1 пульта.\nПризначення стіка, регулятора чи перемикача вже задано в мікшері пульта. Повторне навчання не потрібне.").arg(channel + 1));
    intro->setWordWrap(true); layout->addWidget(intro);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(initial.label); m_name->setMaxLength(100); m_name->setPlaceholderText("Наприклад: SA, S1 або лівий стік");
    form->addRow("Назва органа", m_name);
    m_mode = new QComboBox;
    m_mode->addItems({"Стік / плавний регулятор", "Двопозиційний перемикач", "Трипозиційний перемикач"});
    m_mode->setItemText(0, "Значення з пульта (1000–2000)");
    m_mode->setCurrentIndex(int(initial.mode)); form->addRow("Тип органа", m_mode); layout->addLayout(form);
    auto* recognize = new QPushButton("Розпізнати орган пульта…"); recognize->setMinimumHeight(42);
    if (fromRadio) recognize->setText("Перепризначити рухом…");
    layout->addWidget(recognize); connect(recognize, &QPushButton::clicked, this, [this] { learn(); });
    m_hint = new QLabel; m_hint->setWordWrap(true); layout->addWidget(m_hint);
    auto* options = new QFormLayout;
    m_invert = new QCheckBox("Інверсія каналу"); m_invert->setChecked(initial.inverted); options->addRow(m_invert);
    m_safe = new QSpinBox; m_safe->setRange(1000, 2000); m_safe->setValue(initial.safe);
    options->addRow("Безпечне значення", m_safe); layout->addLayout(options);
    m_previewText = new QLabel; m_preview = new QProgressBar; m_preview->setRange(1000, 2000); m_preview->setTextVisible(false);
    layout->addWidget(m_previewText); layout->addWidget(m_preview);
    auto* buttons = new QDialogButtonBox;
    auto* apply = buttons->addButton("Застосувати", QDialogButtonBox::AcceptRole);
    auto* cancel = buttons->addButton("Скасувати", QDialogButtonBox::RejectRole);
    auto* clear = buttons->addButton("Прибрати призначення", QDialogButtonBox::ResetRole);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(clear, &QPushButton::clicked, this, [this] { m_mapping.source = Source::Unassigned; accept(); });
    connect(apply, &QPushButton::clicked, this, [this] {
        Mapping checked;
        if (!fromJson(toJson(mapping()), checked)) { QMessageBox::information(this, "Виберіть орган пульта", "Натисніть «Розпізнати орган пульта» і задійте потрібний орган."); return; }
        if (checked.source != Source::Unassigned) {
            const auto result = evaluate(checked, m_controller.snapshot());
            if (!result.live) { QMessageBox::information(this, "Перевірте положення", result.reason); return; }
        }
        accept();
    });
    layout->addWidget(buttons);
    auto* timer = new QTimer(this); timer->setInterval(20); connect(timer, &QTimer::timeout, this, [this] { updatePreview(); }); timer->start();
    updatePreview();
}
Mapping MappingDialog::mapping() const {
    Mapping result = m_mapping;
    result.mode = Mode(m_mode->currentIndex()); result.safe = m_safe->value();
    result.inverted = m_invert->isChecked(); result.label = m_name->text().trimmed();
    return result;
}
void MappingDialog::learn() {
    const QString identity = m_controller.identity();
    QDialog wizard(this); wizard.setWindowTitle("Задійте потрібний орган пульта"); wizard.setMinimumWidth(570);
    auto* layout = new QVBoxLayout(&wizard); layout->setSpacing(20);
    auto* instruction = new QLabel("Порухайте потрібним стіком, регулятором або перемикачем.\nКанал визначиться автоматично. Крайні положення не потрібні.");
    instruction->setWordWrap(true); layout->addWidget(instruction);
    auto* status = new QLabel("Очікування руху…"); status->setWordWrap(true); layout->addWidget(status);
    auto* retry = new QPushButton("Повторити розпізнавання"); retry->hide(); layout->addWidget(retry);
    auto* cancel = new QPushButton("Скасувати"); layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, &wizard, &QDialog::reject);
    m_controller.update();
    AxisMotionCapture capture(m_controller.snapshot());
    QElapsedTimer settling;
    bool blocked = false;
    connect(retry, &QPushButton::clicked, &wizard, [&] {
        m_controller.update(); capture = AxisMotionCapture(m_controller.snapshot());
        settling.invalidate(); blocked = false; retry->hide(); status->setText("Очікування руху…");
    });
    QTimer timer; timer.setInterval(10);
    connect(&timer, &QTimer::timeout, &wizard, [&] {
        if (m_controller.identity() != identity || !m_controller.snapshot().connected) {
            blocked = true; status->setText("Пульт відключено або змінено. Скасуйте й повторіть після підключення."); return;
        }
        if (blocked) return;
        m_controller.update(); capture.observe(m_controller.snapshot());
        const int axis = capture.detectedAxis();
        if (axis == -3) { blocked = true; status->setText("Набір входів змінився. Скасуйте й повторіть розпізнавання."); return; }
        if (axis == -2) {
            blocked = true; status->setText("Рухалися кілька каналів. Відпустіть органи керування, натисніть «Повторити» та рухайте лише один."); retry->show(); return;
        }
        if (axis < 0) return;
        if (!settling.isValid()) { settling.start(); status->setText("Канал виявлено…"); }
        // Collect short movements too, and reject a second axis before accepting.
        if (settling.elapsed() < 180) return;
        auto result = mapping();
        result.source = Source::Axis; result.index = axis;
        result.minimum = -32768; result.center = 0; result.maximum = 32767;
        result.switchInputs.clear(); result.switchStates = {}; result.hatStates = {1, 0, 4};
        result.label = QString("Вісь %1 пульта").arg(axis + 1);
        if (const auto radio = m_controller.radioMappings()) {
            for (const auto& input : *radio) if (input.index == axis) { result.label = input.label; break; }
        }
        m_mapping = result; m_name->setText(result.label);
        wizard.accept();
    });
    timer.start(); wizard.exec(); updatePreview();
}
void MappingDialog::updatePreview() {
    const auto current = mapping(); const auto result = evaluate(current, m_controller.snapshot());
    m_hint->setText(current.source == Source::Unassigned ? "Орган ще не призначений. Натисніть «Розпізнати орган пульта»." :
        QString("Призначено: %1. Порухайте цим органом і перевірте значення каналу.").arg(mappingDescription(current)));
    m_preview->setValue(result.value);
    m_previewText->setText(QString("RC-канал: %1 · %2").arg(result.value).arg(result.live ? "працює" : result.reason));
}
}
