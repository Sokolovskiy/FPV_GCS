#pragma once
#include "controller.h"
#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
namespace fpv {
class MappingDialog : public QDialog {
public:
    MappingDialog(int channel, Mapping mapping, Controller& controller, QWidget* parent);
    Mapping mapping() const;
private:
    Controller& m_controller;
    Mapping m_mapping;
    QComboBox* m_mode;
    QLineEdit* m_name;
    QSpinBox* m_safe;
    QCheckBox* m_invert;
    QLabel *m_hint, *m_previewText;
    QProgressBar* m_preview;
    void learn();
    void updatePreview();
};
}
