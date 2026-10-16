// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <QDialog>

class QPlainTextEdit;

/// Zeste > Inaccuracy report: a non-modal window with the live inaccuracy report (what the
/// enabled hardware-accuracy checks found so far), and "bug report" export (report + environment
/// + the end of the log) for sharing.
class ZesteReportDialog : public QDialog {
    Q_OBJECT

public:
    explicit ZesteReportDialog(QWidget* parent = nullptr);

    void Refresh();

    /// Report + environment + the last lines of the log, as Markdown.
    static QString BuildBugReport();

private:
    void SaveReport(bool bug_report);

    QPlainTextEdit* text;
};
