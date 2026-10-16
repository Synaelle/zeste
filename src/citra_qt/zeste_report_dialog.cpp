// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include "citra_qt/zeste_report_dialog.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/zeste_report.h"

namespace {
constexpr int BugReportLogLines = 200;
} // Anonymous namespace

ZesteReportDialog::ZesteReportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Zeste inaccuracy report"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 600);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("What the enabled hardware-accuracy checks have found in this "
                                "session (behaviour that differs on a real New 3DS). The same "
                                "report is written to the log folder when emulation stops."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(text);

    auto* buttons = new QHBoxLayout;
    auto* refresh = new QPushButton(tr("Refresh"));
    auto* copy = new QPushButton(tr("Copy"));
    auto* save = new QPushButton(tr("Save report..."));
    auto* bug = new QPushButton(tr("Save bug report..."));
    bug->setToolTip(tr("Report, environment (build, settings) and the last %1 log lines")
                        .arg(BugReportLogLines));
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(refresh);
    buttons->addWidget(copy);
    buttons->addWidget(save);
    buttons->addWidget(bug);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(refresh, &QPushButton::clicked, this, &ZesteReportDialog::Refresh);
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(save, &QPushButton::clicked, this, [this] { SaveReport(false); });
    connect(bug, &QPushButton::clicked, this, [this] { SaveReport(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    Refresh();
}

void ZesteReportDialog::Refresh() {
    text->setPlainText(QString::fromStdString(Zeste::FormatMarkdown("current session")));
}

QString ZesteReportDialog::BuildBugReport() {
    std::string out = Zeste::FormatMarkdown("bug report");
    std::string log;
    const std::string log_path = FileUtil::GetUserPath(FileUtil::UserPath::LogDir) + LOG_FILE;
    FileUtil::ReadFileToString(true, log_path, log);
    std::size_t start = log.size();
    for (int lines = 0; start > 0 && lines <= BugReportLogLines; --start) {
        if (log[start - 1] == '\n') {
            ++lines;
        }
    }
    out += "\n## Log (last lines of " + log_path + ")\n\n```\n" + log.substr(start) + "\n```\n";
    return QString::fromStdString(out);
}

void ZesteReportDialog::SaveReport(bool bug_report) {
    const QString path = QFileDialog::getSaveFileName(
        this, bug_report ? tr("Save bug report") : tr("Save inaccuracy report"),
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::LogDir) +
                               (bug_report ? "zeste_bug_report.md" : "zeste_report.md")),
        tr("Markdown (*.md)"));
    if (path.isEmpty()) {
        return;
    }
    const QString content = bug_report ? BuildBugReport() : text->toPlainText();
    FileUtil::WriteStringToFile(true, path.toStdString(), content.toStdString());
}
