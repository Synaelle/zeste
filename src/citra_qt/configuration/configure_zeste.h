// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

/// "Zeste / Hardware accuracy" settings tab. Every option defaults to off (Azahar behaviour).
class ConfigureZeste : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureZeste(bool is_powered_on, QWidget* parent = nullptr);
    ~ConfigureZeste() override;

    void ApplyConfiguration();
    void RetranslateUI();
    void SetConfiguration();

private:
    void SetStrictPreset(bool enabled);

    bool is_powered_on;

    QCheckBox* strict_pica;
    QCheckBox* cache_detection;
    QCheckBox* hw_memory_layout;
    QCheckBox* cpu_exceptions;
    QCheckBox* kernel_fixes;
    QComboBox* preempt_mode;
    QSpinBox* preempt_seed;
    QCheckBox* strict_priority;
    QCheckBox* pica_timing;
    QCheckBox* async_gx;
    QCheckBox* cpu_speed_model;
    QCheckBox* sd_latency;
    QCheckBox* break_on_first;
    QSpinBox* report_log_limit;
    QCheckBox* write_report;
    QCheckBox* no_modal_dialogs;
    QLabel* boot_note;
};
