// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste playtest reports, for specific homebrew titles only: a key opens a small window (bug or
// suggestion, description, screenshot taken when the key was pressed), then the report is saved
// as report_<timestamp>.zip with report.json, screenshot.png and the game's log folder.

#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include "common/common_types.h"

class QCheckBox;
class QComboBox;
class QPlainTextEdit;

namespace Zeste::BugReport {

/// Titles that get playtest reports, and where each keeps its logs on the SD card.
bool IsEnabledFor(u64 title_id);

/// The key that opens the report window.
QString Shortcut();

class ReportDialog : public QDialog {
public:
    ReportDialog(QWidget* parent, const QImage& screenshot);

    bool IsSuggestion() const;
    QString Description() const;
    bool AttachScreenshot() const;

private:
    QComboBox* kind;
    QPlainTextEdit* description;
    QCheckBox* attach;
};

/// Asks where to save, then writes the zip. Returns the path written, or "" (cancelled or failed;
/// failures are shown to the user).
QString SaveReport(QWidget* parent, const ReportDialog& dialog, const QImage& screenshot,
                   const QString& game_title, u64 title_id);

} // namespace Zeste::BugReport
