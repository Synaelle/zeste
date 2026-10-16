// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include "citra_qt/configuration/configure_zeste.h"
#include "common/settings.h"

namespace {
QCheckBox* MakeCheck(QLayout* layout, const QString& text, const QString& tooltip) {
    auto* box = new QCheckBox(text);
    box->setToolTip(tooltip);
    layout->addWidget(box);
    return box;
}
} // Anonymous namespace

ConfigureZeste::ConfigureZeste(bool is_powered_on_, QWidget* parent)
    : QWidget(parent), is_powered_on{is_powered_on_} {
    auto* outer = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(content);

    auto* intro = new QLabel(
        tr("Zeste options make the emulator behave (or report) like a real New 3DS instead of "
           "silently doing the right thing. Everything is off by default, so applications run "
           "exactly as in Azahar. Reports go to the log (category <i>Zeste</i>) and to the "
           "inaccuracy report written in the log folder when emulation stops."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* preset_row = new QHBoxLayout;
    auto* preset_on = new QPushButton(tr("Strict hardware mode (enable all)"));
    auto* preset_off = new QPushButton(tr("Azahar behaviour (disable all)"));
    preset_row->addWidget(preset_on);
    preset_row->addWidget(preset_off);
    preset_row->addStretch();
    root->addLayout(preset_row);
    connect(preset_on, &QPushButton::clicked, this, [this] { SetStrictPreset(true); });
    connect(preset_off, &QPushButton::clicked, this, [this] { SetStrictPreset(false); });

    auto* gpu_group = new QGroupBox(tr("GPU (PICA200)"));
    auto* gpu = new QVBoxLayout(gpu_group);
    strict_pica = MakeCheck(
        gpu, tr("Strict PICA checks"),
        tr("Report, per draw, vertices outside the fixed-point rasterizer range, NaN/Inf data, "
           "near-plane clipped primitives with huge extents, primitives outside the framebuffer "
           "and invalid texture/buffer/address state. Forces the software vertex shader path."));
    fixed_point_raster = MakeCheck(
        gpu, tr("Hardware-like clipping (software renderer)"),
        tr("Software renderer only: clip only against the near plane (w > 0) and drop primitives "
           "whose screen coordinates overflow the fixed-point range, like the PICA200."));
    draw_wrapped = MakeCheck(
        gpu, tr("Draw wrapped primitives (software renderer)"),
        tr("Software renderer only: instead of dropping primitives outside the fixed-point range, "
           "draw them flat-shaded at their wrapped coordinates (signed 13.4 wrap, no scissor), "
           "writing wherever the model says the hardware writes, so the corruption becomes "
           "visible."));
    pica_timing = MakeCheck(
        gpu, tr("PICA timing model"),
        tr("Delay the P3D interrupt by an approximate PICA cycle cost (vertices, fragments, "
           "commands) so C3D_GetDrawingTime and frame rates are meaningful."));
    root->addWidget(gpu_group);

    auto* mem_group = new QGroupBox(tr("CPU and memory"));
    auto* mem = new QVBoxLayout(mem_group);
    cpu_exceptions = MakeCheck(
        mem, tr("Real CPU exceptions (data aborts)"),
        tr("Unmapped or protected accesses (including stack overflow below the main stack) raise "
           "a data abort dispatched to the application's handler (threadOnException) instead of "
           "being logged and ignored."));
    kernel_fixes = MakeCheck(
        mem, tr("Kernel fixes (svcGetProcessorID)"),
        tr("Implement SVCs that Azahar leaves unimplemented and that return garbage, such as "
           "svcGetProcessorID (0x11)."));
    cache_detection = MakeCheck(
        mem, tr("CPU data cache detection"),
        tr("Report GPU/DSP/display-transfer reads of memory the CPU wrote without "
           "GSPGPU_FlushDataCache, and CPU reads of GPU-written memory without an invalidate."));
    hw_memory_layout = MakeCheck(
        mem, tr("Hardware-like memory layout (applies at boot)"),
        tr("Place the APPLICATION linear heap at the same physical addresses as a New 3DS "
           "(above 0x28000000 in 178 MB mode) and report 124 MB for .3dsx files."));
    cpu_speed_model = MakeCheck(
        mem, tr("Model 268/804 MHz CPU speed"),
        tr("Make osSetSpeedupEnable / PTMSYSM_ConfigureNew3DSCPU change the emulated CPU speed "
           "(268 MHz when off, 804 MHz when on) instead of being ignored."));
    root->addWidget(mem_group);

    auto* sched_group = new QGroupBox(tr("Scheduling (race exposure)"));
    auto* sched = new QFormLayout(sched_group);
    preempt_mode = new QComboBox;
    preempt_mode->addItem(tr("Off (Azahar)"));
    preempt_mode->addItem(tr("Reschedule at every SVC"));
    preempt_mode->addItem(tr("Random preemption (seeded)"));
    preempt_mode->setToolTip(tr("Interleave threads at SVCs or at random points so unlocked "
                                "shared state breaks in the emulator too. Runs with the same "
                                "seed are reproducible."));
    sched->addRow(tr("Thread preemption"), preempt_mode);
    preempt_seed = new QSpinBox;
    preempt_seed->setRange(0, 0x7FFFFFFF);
    sched->addRow(tr("Seed"), preempt_seed);
    strict_priority = new QCheckBox(tr("Strict-priority starvation"));
    strict_priority->setToolTip(tr("A ready thread never runs while a higher-priority thread on "
                                   "the same core is runnable (no anti-starvation slack)."));
    sched->addRow(strict_priority);
    root->addWidget(sched_group);

    auto* sys_group = new QGroupBox(tr("System"));
    auto* sys = new QVBoxLayout(sys_group);
    sd_latency = MakeCheck(sys, tr("Hardware SD card latency"),
                           tr("Per-request SD card latency modelled on hardware measurements: "
                              "small reads and directory operations become slow."));
    root->addWidget(sys_group);

    auto* rep_group = new QGroupBox(tr("Reporting"));
    auto* rep = new QFormLayout(rep_group);
    break_on_first = new QCheckBox(tr("Break (pause) on the first report"));
    rep->addRow(break_on_first);
    report_log_limit = new QSpinBox;
    report_log_limit->setRange(1, 1000);
    report_log_limit->setToolTip(tr("Log lines per distinct issue before it is only counted."));
    rep->addRow(tr("Log lines per issue"), report_log_limit);
    write_report = new QCheckBox(tr("Write the inaccuracy report when emulation stops"));
    rep->addRow(write_report);
    no_modal_dialogs = new QCheckBox(tr("No modal error dialogs (automation)"));
    no_modal_dialogs->setToolTip(tr("Errors and exceptions are logged and emulation stops instead "
                                    "of waiting on a dialog."));
    rep->addRow(no_modal_dialogs);
    root->addWidget(rep_group);

    boot_note = new QLabel(tr("Some options only take effect when the next application boots."));
    boot_note->setWordWrap(true);
    root->addWidget(boot_note);
    root->addStretch();

    hw_memory_layout->setEnabled(!is_powered_on);

    SetConfiguration();
}

ConfigureZeste::~ConfigureZeste() = default;

void ConfigureZeste::SetStrictPreset(bool enabled) {
    strict_pica->setChecked(enabled);
    fixed_point_raster->setChecked(enabled);
    draw_wrapped->setChecked(enabled);
    pica_timing->setChecked(enabled);
    cpu_exceptions->setChecked(enabled);
    kernel_fixes->setChecked(enabled);
    cache_detection->setChecked(enabled);
    if (!is_powered_on) {
        hw_memory_layout->setChecked(enabled);
    }
    cpu_speed_model->setChecked(enabled);
    preempt_mode->setCurrentIndex(enabled ? 2 : 0);
    strict_priority->setChecked(enabled);
    sd_latency->setChecked(enabled);
}

void ConfigureZeste::SetConfiguration() {
    const auto& v = Settings::values;
    strict_pica->setChecked(v.zeste_strict_pica.GetValue());
    fixed_point_raster->setChecked(v.zeste_fixed_point_raster.GetValue());
    draw_wrapped->setChecked(v.zeste_draw_wrapped.GetValue());
    pica_timing->setChecked(v.zeste_pica_timing.GetValue());
    cpu_exceptions->setChecked(v.zeste_cpu_exceptions.GetValue());
    kernel_fixes->setChecked(v.zeste_kernel_fixes.GetValue());
    cache_detection->setChecked(v.zeste_cache_detection.GetValue());
    hw_memory_layout->setChecked(v.zeste_hw_memory_layout.GetValue());
    cpu_speed_model->setChecked(v.zeste_cpu_speed_model.GetValue());
    preempt_mode->setCurrentIndex(static_cast<int>(v.zeste_preempt_mode.GetValue()));
    preempt_seed->setValue(static_cast<int>(v.zeste_preempt_seed.GetValue()));
    strict_priority->setChecked(v.zeste_strict_priority.GetValue());
    sd_latency->setChecked(v.zeste_sd_latency.GetValue());
    break_on_first->setChecked(v.zeste_break_on_first.GetValue());
    report_log_limit->setValue(static_cast<int>(v.zeste_report_log_limit.GetValue()));
    write_report->setChecked(v.zeste_write_report.GetValue());
    no_modal_dialogs->setChecked(v.zeste_no_modal_dialogs.GetValue());
}

void ConfigureZeste::ApplyConfiguration() {
    auto& v = Settings::values;
    v.zeste_strict_pica = strict_pica->isChecked();
    v.zeste_fixed_point_raster = fixed_point_raster->isChecked();
    v.zeste_draw_wrapped = draw_wrapped->isChecked();
    v.zeste_pica_timing = pica_timing->isChecked();
    v.zeste_cpu_exceptions = cpu_exceptions->isChecked();
    v.zeste_kernel_fixes = kernel_fixes->isChecked();
    v.zeste_cache_detection = cache_detection->isChecked();
    v.zeste_hw_memory_layout = hw_memory_layout->isChecked();
    v.zeste_cpu_speed_model = cpu_speed_model->isChecked();
    v.zeste_preempt_mode = static_cast<u32>(preempt_mode->currentIndex());
    v.zeste_preempt_seed = static_cast<u32>(preempt_seed->value());
    v.zeste_strict_priority = strict_priority->isChecked();
    v.zeste_sd_latency = sd_latency->isChecked();
    v.zeste_break_on_first = break_on_first->isChecked();
    v.zeste_report_log_limit = static_cast<u32>(report_log_limit->value());
    v.zeste_write_report = write_report->isChecked();
    v.zeste_no_modal_dialogs = no_modal_dialogs->isChecked();
}

void ConfigureZeste::RetranslateUI() {
    // Strings are set in the constructor; Zeste ships untranslated.
}
