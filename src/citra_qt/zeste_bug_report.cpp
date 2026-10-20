// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <vector>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "citra_qt/zeste_bug_report.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/zeste_report.h"
#include "common/zeste_run.h"

namespace Zeste::BugReport {

namespace {

struct Title {
    u64 id;
    const char* logs; ///< log folder, relative to the emulated SD card
};

constexpr std::array<Title, 1> Titles{{
    {0x000400000F6D3E00, "openmw/logs/"}, // OpenMW 3DS
}};

const Title* Find(u64 title_id) {
    for (const auto& title : Titles) {
        if (title.id == title_id) {
            return &title;
        }
    }
    return nullptr;
}

/// Minimal zip writer: deflated entries (zlib's deflate through qCompress), CRC-32, UTF-8 names.
class ZipWriter {
public:
    void Add(const QString& name, const QByteArray& data) {
        const QByteArray utf8_name = name.toUtf8();
        // qCompress: 4-byte size, then a zlib stream (2-byte header, deflate data, Adler-32).
        const QByteArray zlib = qCompress(data, 9);
        QByteArray deflated = zlib.size() > 10 ? zlib.mid(6, zlib.size() - 10) : QByteArray{};
        u16 method = 8;
        if (deflated.isEmpty() || deflated.size() >= data.size()) {
            deflated = data; // stored
            method = 0;
        }
        Entry entry{utf8_name, Crc32(data), static_cast<u32>(deflated.size()),
                    static_cast<u32>(data.size()), static_cast<u32>(out.size()), method};
        // Local file header
        Put32(0x04034B50);
        Put16(20);
        Put16(0x0800); // UTF-8 names
        Put16(method);
        Put16(dos_time);
        Put16(dos_date);
        Put32(entry.crc);
        Put32(entry.compressed);
        Put32(entry.size);
        Put16(static_cast<u16>(utf8_name.size()));
        Put16(0);
        out += utf8_name;
        out += deflated;
        entries.push_back(entry);
    }

    QByteArray Finish() {
        const u32 directory_offset = static_cast<u32>(out.size());
        for (const auto& e : entries) {
            Put32(0x02014B50);
            Put16(20);
            Put16(20);
            Put16(0x0800);
            Put16(e.method);
            Put16(dos_time);
            Put16(dos_date);
            Put32(e.crc);
            Put32(e.compressed);
            Put32(e.size);
            Put16(static_cast<u16>(e.name.size()));
            Put16(0);
            Put16(0);
            Put16(0);
            Put16(0);
            Put32(0);
            Put32(e.offset);
            out += e.name;
        }
        const u32 directory_size = static_cast<u32>(out.size()) - directory_offset;
        Put32(0x06054B50);
        Put16(0);
        Put16(0);
        Put16(static_cast<u16>(entries.size()));
        Put16(static_cast<u16>(entries.size()));
        Put32(directory_size);
        Put32(directory_offset);
        Put16(0);
        return out;
    }

    explicit ZipWriter(const QDateTime& now) {
        const QDate d = now.date();
        const QTime t = now.time();
        dos_date = static_cast<u16>(((d.year() - 1980) << 9) | (d.month() << 5) | d.day());
        dos_time = static_cast<u16>((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    }

private:
    struct Entry {
        QByteArray name;
        u32 crc;
        u32 compressed;
        u32 size;
        u32 offset;
        u16 method;
    };

    static u32 Crc32(const QByteArray& data) {
        static const std::array<u32, 256> table = [] {
            std::array<u32, 256> t{};
            for (u32 i = 0; i < 256; ++i) {
                u32 c = i;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                }
                t[i] = c;
            }
            return t;
        }();
        u32 crc = 0xFFFFFFFFu;
        for (const char byte : data) {
            crc = table[(crc ^ static_cast<u8>(byte)) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    void Put16(u16 v) {
        out.append(static_cast<char>(v & 0xFF));
        out.append(static_cast<char>(v >> 8));
    }
    void Put32(u32 v) {
        Put16(static_cast<u16>(v & 0xFFFF));
        Put16(static_cast<u16>(v >> 16));
    }

    QByteArray out;
    std::vector<Entry> entries;
    u16 dos_date = 0;
    u16 dos_time = 0;
};

QString g_last_folder;

} // namespace

bool IsEnabledFor(u64 title_id) {
    return Find(title_id) != nullptr;
}

QString Shortcut() {
    return QStringLiteral("F12");
}

ReportDialog::ReportDialog(QWidget* parent, const QImage& screenshot) : QDialog(parent) {
    setWindowTitle(tr("Playtest report"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    kind = new QComboBox;
    kind->addItem(tr("Bug"));
    kind->addItem(tr("Suggestion"));
    form->addRow(tr("Bug or suggestion?"), kind);

    description = new QPlainTextEdit;
    description->setPlaceholderText(
        tr("What happened, or what would you change? What were you doing just before?"));
    description->setMinimumSize(420, 140);
    form->addRow(tr("Description"), description);
    layout->addLayout(form);

    attach = new QCheckBox(tr("Attach screenshot"));
    attach->setChecked(!screenshot.isNull());
    attach->setEnabled(!screenshot.isNull());
    layout->addWidget(attach);
    if (!screenshot.isNull()) {
        auto* preview = new QLabel;
        preview->setPixmap(QPixmap::fromImage(
            screenshot.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        preview->setAlignment(Qt::AlignCenter);
        connect(attach, &QCheckBox::toggled, preview, &QLabel::setEnabled);
        layout->addWidget(preview);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Save report..."));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(description, &QPlainTextEdit::textChanged, this, [this, buttons] {
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(!description->toPlainText().trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    description->setFocus();
}

bool ReportDialog::IsSuggestion() const {
    return kind->currentIndex() == 1;
}

QString ReportDialog::Description() const {
    return description->toPlainText().trimmed();
}

bool ReportDialog::AttachScreenshot() const {
    return attach->isEnabled() && attach->isChecked();
}

QString SaveReport(QWidget* parent, const ReportDialog& dialog, const QImage& screenshot,
                   const QString& game_title, u64 title_id) {
    const QDateTime now = QDateTime::currentDateTime();
    const QString name =
        QStringLiteral("report_%1.zip").arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss")));
    if (g_last_folder.isEmpty()) {
        g_last_folder = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    }
    const QString path = QFileDialog::getSaveFileName(
        parent, QObject::tr("Save the report"), QDir(g_last_folder).filePath(name),
        QObject::tr("Zip archive (*.zip)"));
    if (path.isEmpty()) {
        return {};
    }
    g_last_folder = QFileInfo(path).absolutePath();

    ZipWriter zip(now);
    QJsonObject json;
    json[QStringLiteral("type")] =
        dialog.IsSuggestion() ? QStringLiteral("suggestion") : QStringLiteral("bug");
    json[QStringLiteral("date")] = now.toString(Qt::ISODate);
    json[QStringLiteral("description")] = dialog.Description();
    json[QStringLiteral("game")] = game_title;
    json[QStringLiteral("title_id")] = QString::fromStdString(fmt::format("{:016X}", title_id));
    json[QStringLiteral("zeste_version")] = QString::fromUtf8(Common::g_zeste_version);

    if (dialog.AttachScreenshot()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        screenshot.save(&buffer, "PNG");
        zip.Add(QStringLiteral("screenshot.png"), png);
        json[QStringLiteral("screenshot")] = QStringLiteral("screenshot.png");
    }

    // Adds every file of `dir` (recursively) under `prefix` in the zip and lists it in `list`.
    const auto add_folder = [&zip](const QString& dir, const QString& prefix, QJsonArray& list) {
        constexpr qint64 MaxFileSize = 64ll << 20; // skip crash dumps and the like
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file_path = it.next();
            QFile file(file_path);
            if (file.size() > MaxFileSize || !file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QString name = prefix + QDir(dir).relativeFilePath(file_path);
            zip.Add(name, file.readAll());
            list.append(name);
        }
    };

    // The game's own logs.
    QJsonArray game_logs;
    if (const Title* title = Find(title_id)) {
        add_folder(QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) +
                                          title->logs),
                   QStringLiteral("game_logs/"), game_logs);
    }
    json[QStringLiteral("game_logs")] = game_logs;

    // Zeste's side: this run's folder (log, run.txt, profiles...) and the findings so far (the
    // findings report itself is only written when emulation stops).
    QJsonArray zeste_files;
    if (!Zeste::Run::Folder().empty()) {
        add_folder(QString::fromStdString(Zeste::Run::Folder()), QStringLiteral("zeste/"),
                   zeste_files);
    }
    zip.Add(QStringLiteral("zeste/findings.md"),
            QByteArray::fromStdString(Zeste::FormatMarkdown(
                fmt::format("{} ({:016X}), playtest report", game_title.toStdString(), title_id))));
    zeste_files.append(QStringLiteral("zeste/findings.md"));
    json[QStringLiteral("zeste_files")] = zeste_files;

    QJsonArray checks;
    const auto check = [&checks](bool on, const char* name) {
        if (on) {
            checks.append(QString::fromLatin1(name));
        }
    };
    const auto& v = Settings::values;
    check(v.zeste_strict_pica.GetValue(), "strict_pica");
    check(v.zeste_cpu_exceptions.GetValue(), "cpu_exceptions");
    check(v.zeste_kernel_fixes.GetValue(), "kernel_fixes");
    check(v.zeste_cache_detection.GetValue(), "cache_detection");
    check(v.zeste_hw_memory_layout.GetValue(), "hw_memory_layout");
    check(v.zeste_preempt_mode.GetValue() == 1, "preempt_svc");
    check(v.zeste_preempt_mode.GetValue() == 2, "preempt_random");
    check(v.zeste_strict_priority.GetValue(), "strict_priority");
    check(v.zeste_fs_checks.GetValue(), "fs_checks");
    json[QStringLiteral("zeste_checks")] = checks;
    zip.Add(QStringLiteral("report.json"), QJsonDocument(json).toJson(QJsonDocument::Indented));

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(zip.Finish()) < 0) {
        QMessageBox::warning(parent, QObject::tr("Playtest report"),
                             QObject::tr("Could not write %1: %2").arg(path, out.errorString()));
        return {};
    }
    LOG_INFO(Frontend, "Zeste: playtest report saved to {} ({} game logs, {} Zeste files)",
             path.toStdString(), game_logs.size(), zeste_files.size());
    return path;
}

} // namespace Zeste::BugReport
