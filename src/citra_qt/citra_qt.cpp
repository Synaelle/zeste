// Copyright 2024-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <clocale>
#include <format>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QSysInfo>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui>
#include <QtWidgets>
#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#ifdef __APPLE__
#include <unistd.h> // for chdir
#endif
#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#include <QAbstractNativeEventFilter>
#else
#include <getopt.h>
#endif
#ifdef __unix__
#include <QVariant>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QtDBus>
#include "common/linux/gamemode.h"
#endif
#include "citra_meta/common_strings.h"
#include "citra_qt/aboutdialog.h"
#include "citra_qt/applets/mii_selector.h"
#include "citra_qt/applets/swkbd.h"
#include "citra_qt/bootmanager.h"
#include "citra_qt/camera/qt_multimedia_camera.h"
#include "citra_qt/camera/still_image_camera.h"
#include "citra_qt/citra_qt.h"
#include "citra_qt/compatibility_list.h"
#include "citra_qt/configuration/config.h"
#include "citra_qt/configuration/configure_dialog.h"
#include "citra_qt/configuration/configure_per_game.h"
#include "citra_qt/configuration/configure_system.h"
#include "citra_qt/debugger/console.h"
#include "citra_qt/debugger/graphics/graphics.h"
#include "citra_qt/debugger/graphics/graphics_breakpoints.h"
#include "citra_qt/debugger/graphics/graphics_cmdlists.h"
#include "citra_qt/debugger/graphics/graphics_surface.h"
#include "citra_qt/debugger/graphics/graphics_tracing.h"
#include "citra_qt/debugger/graphics/graphics_vertex_shader.h"
#include "citra_qt/debugger/ipc/recorder.h"
#include "citra_qt/debugger/lle_service_modules.h"
#if MICROPROFILE_ENABLED
#include "citra_qt/debugger/profiler.h"
#endif
#include "citra_qt/debugger/registers.h"
#include "citra_qt/debugger/wait_tree.h"
#ifdef ENABLE_DISCORD_RPC
#include "citra_qt/discord.h"
#endif
#ifdef ENABLE_FFMPEG
#include "citra_qt/dumping/dumping_dialog.h"
#endif
#include "citra_qt/game_list.h"
#include "citra_qt/hotkeys.h"
#include "citra_qt/loading_screen.h"
#include "citra_qt/movie/movie_play_dialog.h"
#include "citra_qt/movie/movie_record_dialog.h"
#include "citra_qt/multiplayer/state.h"
#include "citra_qt/qt_image_interface.h"
#include "citra_qt/qt_swizzle.h"
#include "citra_qt/uisettings.h"
#include "common/play_time_manager.h"
#include "citra_qt/util/clickable_label.h"
#include "citra_qt/util/graphics_device_info.h"
#include "citra_qt/util/util.h"
#include "citra_qt/zeste_bug_report.h"
#include "citra_qt/zeste_update.h"
#include "common/arch.h"
#include "common/common_paths.h"
#include "common/dynamic_library.h"
#include "common/file_util.h"
#include "common/literals.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#if CITRA_ARCH(x86_64)
#include "common/x64/cpu_detect.h"
#endif
#include "common/settings.h"
#include "common/string_util.h"
#include "common/zstd_compression.h"
#include "core/arm/exception_handler.h"
#include "core/core.h"
#include "core/dumping/backend.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/frontend/applets/default_applets.h"
#include "core/file_sys/cia_container.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "core/system_titles.h"
#include "audio_core/sink_details.h"
#include "core/zeste_automation.h"
#include "core/zeste_keyboard.h"
#include "core/zeste_net.h"
#include "core/rpc/udp_server.h"
#include "core/zeste_summary.h"
#include "core/zeste_core.h"
#include "common/zeste_crash.h"
#include "common/zeste_run.h"
#include "input_common/main.h"
#include "ui_main.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

#ifdef __APPLE__
#include "common/apple_authorization.h"
#include "common/apple_utils.h"
Q_IMPORT_PLUGIN(QDarwinCameraPermissionPlugin);
#endif

#ifdef ENABLE_DISCORD_RPC
#include "citra_qt/discord_impl.h"
#endif

#ifdef QT_STATICPLUGIN
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#endif

#ifdef HAVE_SDL2
#include <SDL.h>
#endif

constexpr int default_mouse_timeout = 2500;

/**
 * "Callouts" are one-time instructional messages shown to the user. In the config settings, there
 * is a bitfield "callout_flags" options, used to track if a message has already been shown to the
 * user. This is 32-bits - if we have more than 32 callouts, we should retire and recycle old ones.
 */

const int GMainWindow::max_recent_files_item;

// There is a bug in the QT implementation on MSYS2 builds
// that cause corners to appear when the app is switched to
// fullscreen. The following code aims to fix that issue
// until it is addressed upstream. It works by manually
// disabling corners through the DWM API.
// TODO(PabloMK7): Remove once the upstream bug is solved.
#if defined(_WIN32) && !defined(_MSC_VER)
#define NEEDS_ROUND_CORNERS_FIX
#endif

#ifdef NEEDS_ROUND_CORNERS_FIX
#include <dwmapi.h>
class WindowCornerManager {
public:
    static WindowCornerManager& instance() {
        static WindowCornerManager inst;
        return inst;
    }

    void blockRoundedCorners(QWidget* widget, bool block) {
        HWND hwnd = reinterpret_cast<HWND>(widget->winId());
        DWORD pref;

        if (block) {
            pref = DWMWCP_DEFAULT;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref,
                                                sizeof(pref)))) {
                original_prefs[hwnd] = pref;
            } else {
                original_prefs[hwnd] = DWMWCP_DEFAULT;
            }

            pref = DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        } else {
            auto it = original_prefs.find(hwnd);
            if (it == original_prefs.end())
                return;

            pref = it->second;

            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

            original_prefs.erase(it);
        }
    }

private:
    WindowCornerManager() = default;
    ~WindowCornerManager() = default;

    std::unordered_map<HWND, DWORD> original_prefs;
};
#endif

static QString PrettyProductName() {
#ifdef _WIN32
    // After Windows 10 Version 2004, Microsoft decided to switch to a different notation: 20H2
    // With that notation change they changed the registry key used to denote the current version
    QSettings windows_registry(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"),
        QSettings::NativeFormat);
    const QString release_id = windows_registry.value(QStringLiteral("ReleaseId")).toString();
    if (release_id == QStringLiteral("2009")) {
        const u32 current_build = windows_registry.value(QStringLiteral("CurrentBuild")).toUInt();
        const QString display_version =
            windows_registry.value(QStringLiteral("DisplayVersion")).toString();
        const u32 ubr = windows_registry.value(QStringLiteral("UBR")).toUInt();
        const u32 version = current_build >= 22000 ? 11 : 10;
        return QStringLiteral("Windows %1 Version %2 (Build %3.%4)")
            .arg(QString::number(version), display_version, QString::number(current_build),
                 QString::number(ubr));
    }
#endif
    return QSysInfo::prettyProductName();
}

void GMainWindow::ShowCommandOutput(std::string title, std::string message) {
    bool output_captured = false;
#ifdef _WIN32
    // Zeste: run from a script (stdout piped or redirected to a file), print the text
    // instead of opening a dialog.
    const DWORD out_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    output_captured = out_type == FILE_TYPE_PIPE || out_type == FILE_TYPE_DISK;
#endif
    if (headless_mode || output_captured) {
        if (output_captured && !headless_mode && title == "Help") {
            std::cout << message << std::endl;
            return;
        }
        std::cout << title << ": " << message << std::endl;
        LOG_INFO(Frontend, "{}: {}", title, message);
        return;
    }
#ifdef _WIN32
    boost::replace_all(message, " ", "\u00a0"); // Non-breaking space
    boost::replace_all(message, "-", "\u2011"); // Non-breaking hyphen
    QMessageBox::information(this, QString::fromStdString(title), QString::fromStdString(message));
#else
    std::cout << message << std::endl;
#endif
}

bool IsPrereleaseBuild() {
    return ((strstr(Common::g_build_fullname, "alpha") != NULL) ||
            (strstr(Common::g_build_fullname, "beta") != NULL) ||
            (strstr(Common::g_build_fullname, "rc") != NULL) ||
            (strstr(Common::g_build_fullname, "test") != NULL));
}


/// Zeste: status bar look, added after every theme. Items get no frame (the default style draws a
/// sunken one around each), groups are split by thin dividers, the buttons get a soft hover.
static QString ZesteStatusBarStyle() {
    return QStringLiteral(R"(
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0px 6px; }
QFrame#ZesteStatusDivider { background-color: rgba(128, 128, 128, 90); border: none; }
QPushButton#TogglableStatusBarButton, QPushButton#GraphicsAPIStatusBarButton {
    border: none; border-radius: 3px; padding: 2px 8px; margin: 0px 1px;
}
QPushButton#TogglableStatusBarButton:hover, QPushButton#GraphicsAPIStatusBarButton:hover {
    border: none; background-color: rgba(128, 128, 128, 45);
}
)");
}

/// Zeste: menus homebrew testing does not need. Hidden rather than removed, so nothing that
/// refers to their actions breaks.
void GMainWindow::ZesteHideUnusedMenus() {
    ui->menu_Multiplayer->menuAction()->setVisible(false);
    ui->menu_Movie->menuAction()->setVisible(false);
    // Screen layouts: Default, Single Screen, Side by Side and Separate Windows stay.
    for (QAction* action : {ui->action_Screen_Layout_Large_Screen,
                            ui->action_Screen_Layout_Hybrid_Screen,
                            ui->action_Screen_Layout_Custom_Layout,
                            ui->menu_Small_Screen_Position->menuAction()}) {
        action->setVisible(false);
    }
}

static QFrame* ZesteStatusDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("ZesteStatusDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedSize(1, 14);
    return divider;
}

/// Zeste headless: the widget behaves as shown (GL contexts, layouts, screenshots) but never
/// reaches the screen, and never takes the focus or the foreground (an invisible window in the
/// foreground made the taskbar appear over full-screen applications).
static void MakeHeadless(QWidget* widget) {
    widget->setAttribute(Qt::WA_DontShowOnScreen);
    widget->setAttribute(Qt::WA_ShowWithoutActivating);
    widget->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    widget->setFocusPolicy(Qt::NoFocus);
}

GMainWindow::GMainWindow(Core::System& system_)
    : ui{std::make_unique<Ui::MainWindow>()}, system{system_}, movie{system.Movie()},
      config{std::make_unique<QtConfig>()}, emu_thread{nullptr} {
    // Zeste: every process logs into its own run folder (log, report, run.txt), so concurrent
    // instances never rotate, lock or overwrite each other's files.
    {
        const QStringList arguments = QApplication::arguments();
        const std::string folder =
            Zeste::Run::Begin(arguments.join(QChar::fromLatin1(' ')).toStdString());
        if (headless_mode && !folder.empty()) {
            std::cout << "Zeste run folder: " << folder << std::endl;
        }
    }
    Common::Log::Initialize();
    Common::Log::Start();

    if (headless_mode) {
        // Must be set before anything shows the window (the constructor boots the game).
        MakeHeadless(this);
    } else {
        Debugger::ToggleConsole();
    }

    QStringList args = QApplication::arguments();
    QString game_path;
    std::optional<bool> fullscreen_override;
    for (int i = 1; i < args.size(); ++i) {
        // Preserves drag/drop functionality
        if (args.size() == 2 && !args[1].startsWith(QChar::fromLatin1('-'))) {
            game_path = args[1];
            break;
        }

        // Dump video
        if (args[i] == QStringLiteral("--dump-video") || args[i] == QStringLiteral("-d")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            video_dumping_path = args[++i];
            video_dumping_on_start = true;
            continue;
        }

        // Launch game in fullscreen mode
        if (args[i] == QStringLiteral("--fullscreen") || args[i] == QStringLiteral("-f")) {
            fullscreen_override = true;
            continue;
        }

        // Enable GDB stub
        if (args[i] == QStringLiteral("--gdbport") || args[i] == QStringLiteral("-g")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            gdbport_from_arg = strtoul(args[++i].toLatin1(), NULL, 0);
            continue;
        }

        if (args[i] == QStringLiteral("--help") || args[i] == QStringLiteral("-h")) {
            ShowCommandOutput("Help", fmt::format(Common::help_string, args[0].toStdString()));
            exit(0);
        }

        if (args[i] == QStringLiteral("--install") || args[i] == QStringLiteral("-i")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            Service::AM::InstallStatus result = Service::AM::InstallCIA(args[++i].toStdString());
            if (result != Service::AM::InstallStatus::Success) {
                std::string failure_reason;

                if (result == Service::AM::InstallStatus::ErrorFailedToOpenFile)
                    failure_reason = "Unable to open file.";

                if (result == Service::AM::InstallStatus::ErrorFileNotFound)
                    failure_reason = "File not found.";

                if (result == Service::AM::InstallStatus::ErrorAborted)
                    failure_reason = "Install was aborted.";

                if (result == Service::AM::InstallStatus::ErrorInvalid)
                    failure_reason = "CIA is invalid.";

                if (result == Service::AM::InstallStatus::ErrorEncrypted)
                    failure_reason = "CIA is encrypted.";

                std::string failure_string = "Failed to install CIA: " + failure_reason;
                ShowCommandOutput("Failure", failure_string);
                exit((int)result +
                     2); // 2 is added here to avoid stepping on the toes of
                         // exit codes 1 and 2 which have pre-established conventional meanings
            }
            ShowCommandOutput("Success", "Installed CIA successfully.");
            exit(0);
        }

        if (args[i] == QStringLiteral("--movie-play") || args[i] == QStringLiteral("-p")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            movie_playback_path = args[++i];
            movie_playback_on_start = true;
            continue;
        }

        if (args[i] == QStringLiteral("--movie-record") || args[i] == QStringLiteral("-r")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            movie_record_path = args[++i];
            movie_record_on_start = true;
            continue;
        }

        if (args[i] == QStringLiteral("--movie-record-author") || args[i] == QStringLiteral("-a")) {
            if (i >= args.size() - 1 || args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                continue;
            }
            movie_record_author = args[++i];
            continue;
        }

        if (args[i] == QStringLiteral("--multiplayer") || args[i] == QStringLiteral("-m")) {
            std::cout << "Warning: The --multiplayer option is not yet implemented for the Qt "
                         "frontend; Ignoring."
                      << std::endl;
            if (i < args.size() - 1 && !args[i + 1].startsWith(QChar::fromLatin1('-'))) {
                i++;
            }
            continue;
        }

        if (args[i] == QStringLiteral("--version") || args[i] == QStringLiteral("-v")) {
            const std::string version_string = std::string("Zeste (Azahar fork) ") + Common::g_zeste_display_version;
            ShowCommandOutput("Version", version_string);
            exit(0);
        }

        // Launch game in windowed mode
        if (args[i] == QStringLiteral("--windowed") || args[i] == QStringLiteral("-w")) {
            fullscreen_override = false;
            continue;
        }

        // Zeste hardware-accuracy options. They are not written back to the config file.
        if (args[i] == QStringLiteral("--strict")) {
            Settings::SetZesteStrictPreset(true);
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--zeste") && i + 1 < args.size()) {
            for (const QString& name : args[++i].split(QChar::fromLatin1(','))) {
                if (!Settings::ApplyZesteOptionByName(name.trimmed().toStdString())) {
                    ShowCommandOutput("Zeste", "Unknown Zeste option: " + name.toStdString());
                }
            }
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--seed") && i + 1 < args.size()) {
            Settings::values.zeste_preempt_seed = args[++i].toUInt(nullptr, 0);
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--break-on-first")) {
            Settings::values.zeste_break_on_first = true;
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--no-dialogs")) {
            Settings::values.zeste_no_modal_dialogs = true;
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        // Zeste automation
        if (args[i] == QStringLiteral("--headless")) {
            continue; // Detected in LaunchQtFrontend, before the window is created.
        }
        if (args[i] == QStringLiteral("--script") && i + 1 < args.size()) {
            if (!Zeste::Automation::LoadScript(args[++i].toStdString())) {
                ShowCommandOutput("Zeste", "Invalid automation script (see the log)");
                exit(1);
            }
            continue;
        }
        if (args[i] == QStringLiteral("--quit-after") && i + 1 < args.size()) {
            // Frames at 60 Hz ("1800"), or seconds with an "s" ("30s").
            const std::string when = args[++i].toStdString();
            if (!Zeste::Automation::LoadScriptText("@" + when + " quit 0", "--quit-after")) {
                ShowCommandOutput("Zeste", "Invalid --quit-after value: " + when);
                exit(1);
            }
            continue;
        }
        if (args[i] == QStringLiteral("--load-state") && i + 1 < args.size()) {
            // Loaded once the game has booted (same game build and same Zeste build only).
            Zeste::Automation::LoadScriptText("@1 load-state " + args[++i].toStdString(),
                                              "--load-state");
            continue;
        }
        if (args[i] == QStringLiteral("--speed") && i + 1 < args.size()) {
            const QString speed = args[++i].toLower();
            bool ok = true;
            const int percent = speed == QStringLiteral("unlimited") ? 0 : speed.toInt(&ok);
            if (!ok || percent < 0) {
                ShowCommandOutput("Zeste", "Invalid --speed: " + speed.toStdString() +
                                               " (a percentage or 'unlimited')");
                exit(1);
            }
            Settings::values.frame_limit = static_cast<double>(percent);
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--profile")) {
            // Sampling CPU profiler for the whole run, report in the run folder (profile.md).
            // An optional number sets the interval in microseconds of emulated time.
            std::string interval;
            if (i + 1 < args.size() && args[i + 1].toUInt() > 0) {
                interval = " " + args[++i].toStdString();
            }
            Zeste::Automation::LoadScriptText("@1 profile-start" + interval, "--profile");
            continue;
        }
        if (args[i] == QStringLiteral("--elf") && i + 1 < args.size()) {
            Zeste::Summary::SetElfPath(args[++i].toStdString());
            continue;
        }
        if (args[i] == QStringLiteral("--renderer") && i + 1 < args.size()) {
            const QString api = args[++i].toLower();
            if (api == QStringLiteral("software")) {
                Settings::values.graphics_api = Settings::GraphicsAPI::Software;
            } else if (api == QStringLiteral("opengl")) {
                Settings::values.graphics_api = Settings::GraphicsAPI::OpenGL;
            } else if (api == QStringLiteral("vulkan")) {
#ifdef ENABLE_VULKAN
                Settings::values.graphics_api = Settings::GraphicsAPI::Vulkan;
#else
                std::cout << "Zeste: Vulkan is not built, using OpenGL" << std::endl;
                Settings::values.graphics_api = Settings::GraphicsAPI::OpenGL;
#endif
            } else {
                ShowCommandOutput("Zeste", "Unknown renderer: " + api.toStdString());
                exit(1);
            }
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--install-cia") && i + 1 < args.size()) {
            // Installs without booting (repeatable), e.g. before --home-menu.
            if (ZesteInstallCiaForBoot(args[++i]).isEmpty()) {
                ShowCommandOutput("Zeste", "Could not install the CIA (see the log)");
                exit(1);
            }
            continue;
        }
        if (args[i] == QStringLiteral("--console") && i + 1 < args.size()) {
            const QString model = args[++i].toLower();
            if (model != QStringLiteral("new3ds") && model != QStringLiteral("old3ds")) {
                ShowCommandOutput("Zeste", "--console takes new3ds or old3ds");
                exit(1);
            }
            Settings::values.is_new_3ds.SetGlobal(true);
            Settings::values.is_new_3ds = model == QStringLiteral("new3ds");
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--region") && i + 1 < args.size()) {
            static const QStringList regions{
                QStringLiteral("jpn"), QStringLiteral("usa"), QStringLiteral("eur"),
                QStringLiteral("aus"), QStringLiteral("chn"), QStringLiteral("kor"),
                QStringLiteral("twn")};
            const QString region = args[++i].toLower();
            const int index = region == QStringLiteral("auto") ? -1 : regions.indexOf(region);
            if (index < 0 && region != QStringLiteral("auto")) {
                ShowCommandOutput("Zeste", "--region takes auto, jpn, usa, eur, aus, chn, kor or twn");
                exit(1);
            }
            Settings::values.region_value.SetGlobal(true);
            Settings::values.region_value = index;
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--country") && i + 1 < args.size()) {
            const int code = ConfigureSystem::CountryCodeFromName(args[++i]);
            if (code < 0) {
                ShowCommandOutput("Zeste", "Unknown --country: " + args[i].toStdString() +
                                               " (an English name like France, or a code)");
                exit(1);
            }
            Zeste::SetCountryOverride(code);
            continue;
        }
        if (args[i] == QStringLiteral("--offline")) {
            Zeste::Net::SetOffline(true);
            continue;
        }
        if (args[i] == QStringLiteral("--home-menu")) {
            // Boots the HOME Menu installed in the NAND (needs system files from a console).
            zeste_home_menu = true;
            static const QStringList regions{
                QStringLiteral("jpn"), QStringLiteral("usa"), QStringLiteral("eur"),
                QStringLiteral("aus"), QStringLiteral("chn"), QStringLiteral("kor"),
                QStringLiteral("twn")};
            if (i + 1 < args.size() && regions.contains(args[i + 1].toLower())) {
                zeste_home_menu_region = static_cast<int>(regions.indexOf(args[++i].toLower()));
            }
            continue;
        }
        if (args[i] == QStringLiteral("--boot-cia") && i + 1 < args.size()) {
            const QString installed = ZesteInstallCiaForBoot(args[++i]);
            if (installed.isEmpty()) {
                ShowCommandOutput("Zeste", "Could not install the CIA (see the log)");
                exit(1);
            }
            game_path = installed;
            continue;
        }
        if (args[i] == QStringLiteral("--rpc-port") && i + 1 < args.size()) {
            // Implies --rpc. 0 = a free port, printed on stdout ("Zeste RPC port: N").
            bool ok = false;
            const uint port = args[++i].toUInt(&ok);
            if (!ok || port > 65535) {
                ShowCommandOutput("Zeste", "--rpc-port takes a port number (0 = a free one)");
                exit(1);
            }
            Core::RPC::SetUdpPort(static_cast<u16>(port));
            Settings::values.enable_rpc_server = true;
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--rpc")) {
            Settings::values.enable_rpc_server = true;
            QtConfig::zeste_command_line_override = true;
            continue;
        }
        if (args[i] == QStringLiteral("--zeste-report") && i + 1 < args.size()) {
            Zeste::SetReportBasePath(args[++i].toStdString());
            Settings::values.zeste_write_report = true;
            continue;
        }

        // Launch game at path
        if (i == args.size() - 1 && !args[i].startsWith(QChar::fromLatin1('-'))) {
            game_path = args[i];
            continue;
        }
    }

    if (zeste_home_menu) {
        // Region: the one given, else --region / the setting, else the first one installed.
        int region = zeste_home_menu_region;
        if (region < 0 && Settings::values.region_value.GetValue() >= 0) {
            region = Settings::values.region_value.GetValue();
        }
        for (int r = 0; region < 0 && r < 7; ++r) {
            if (FileUtil::Exists(Core::GetHomeMenuNcchPath(static_cast<u32>(r)))) {
                region = r;
            }
        }
        const std::string path =
            region >= 0 ? Core::GetHomeMenuNcchPath(static_cast<u32>(region)) : std::string{};
        if (path.empty() || !FileUtil::Exists(path)) {
            ShowCommandOutput("Zeste", "--home-menu: no HOME Menu installed for that region "
                                       "(it needs the system files from a console: File > Set "
                                       "Up System Files)");
            exit(1);
        }
        LOG_INFO(Frontend, "Zeste: booting the HOME Menu ({})", path);
        game_path = QString::fromStdString(path);
    }

    if (QtConfig::zeste_command_line_override) {
        // Settings given on the command line apply to this run only: never write them back.
        QtConfig::read_only = true;
        LOG_INFO(Frontend, "Zeste: settings overridden on the command line; the config file "
                           "will not be written during this session");
    }
    if (headless_mode) {
        if (game_path.isEmpty()) {
            std::cerr << "--headless needs an application to boot" << std::endl;
            exit(1);
        }
        QtConfig::read_only = true;
        Settings::values.zeste_no_modal_dialogs = true;
        Settings::values.instant_debug_log = true;
        Settings::values.output_type = AudioCore::SinkType::Null;
        if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
            Settings::values.layout_option = Settings::LayoutOption::Default;
        }
        UISettings::values.single_window_mode = true;
        UISettings::values.fullscreen = false;
        UISettings::values.confirm_before_closing = false;
        fullscreen_override = false;
        headless_exit_on_stop = true;
        LOG_INFO(Frontend, "Zeste headless mode: no window, null audio, no dialogs");
    }
    Zeste::Automation::SetQuitHandler([this](int code) {
        QMetaObject::invokeMethod(
            this,
            [this, code] {
                // The first quit wins: a second one (a script `quit` right after a wait-log
                // timeout) used to leave the event loop before the window closed, so input
                // shutdown ran at process exit and crashed.
                if (zeste_quit_requested) {
                    return;
                }
                zeste_quit_requested = true;
                headless_exit_code = code;
                headless_exit_on_stop = true;
                if (emu_thread) {
                    ShutdownGame();
                } else {
                    close();
                    QCoreApplication::exit(code);
                }
            },
            Qt::QueuedConnection);
    });

#ifdef __unix__
    SetGamemodeEnabled(Settings::values.enable_gamemode.GetValue());
#endif

    // register types to use in slots and signals
    qRegisterMetaType<std::size_t>("std::size_t");
    qRegisterMetaType<Service::AM::InstallStatus>("Service::AM::InstallStatus");

    // Register CameraFactory
    qt_cameras = std::make_shared<Camera::QtMultimediaCameraHandlerFactory>();
    Camera::RegisterFactory("image", std::make_unique<Camera::StillImageCameraFactory>());
    Camera::RegisterFactory("qt", std::make_unique<Camera::QtMultimediaCameraFactory>(qt_cameras));

    system.RegisterInfoLEDColorChanged([this]() { emit InfoLEDColorChanged(); });

    LoadTranslation();

    if (Settings::values.pica_debugging) {
        Pica::g_debug_context = Pica::DebugContext::Construct();
    } else {
        Pica::g_debug_context.reset();
    }
    setAcceptDrops(true);
    ui->setupUi(this);
    statusBar()->hide();
    ZesteHideUnusedMenus();
#ifndef ENABLE_VULKAN
    // Zeste: Vulkan is not built; a config from an older version that picked it gets OpenGL.
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Vulkan) {
        Settings::values.graphics_api = Settings::GraphicsAPI::OpenGL;
    }
#endif

    default_theme_paths = QIcon::themeSearchPaths();
    UpdateUITheme();

#ifdef ENABLE_DISCORD_RPC
    SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
    discord_rpc->Update(false);
#endif

    play_time_manager = std::make_unique<PlayTime::PlayTimeManager>();

    Network::Init();

    movie.SetPlaybackCompletionCallback([this] {
        QMetaObject::invokeMethod(this, "OnMoviePlaybackCompleted", Qt::BlockingQueuedConnection);
    });

    InitializeWidgets();
    InitializeDebugWidgets();
    InitializeRecentFileMenuActions();
    InitializeSaveStateMenuActions();
    InitializeHotkeys();

    SetDefaultUIGeometry();
    RestoreUIState();

    ui->action_Dump_Video->setChecked(video_dumping_on_start);
    if (fullscreen_override) {
        ui->action_Fullscreen->setChecked(*fullscreen_override);
    }
    ui->action_Close_Movie->setEnabled(movie_playback_on_start || movie_record_on_start);

    ConnectAppEvents();
    ConnectMenuEvents();
    ZesteUpdateStrictUI();
    ConnectWidgetEvents();

    LOG_INFO(Frontend, "Zeste (Azahar fork) Version: {} | {}-{}", Common::g_zeste_display_version, Common::g_scm_branch,
             Common::g_scm_desc);
    std::string build_variant = Common::g_build_variant;
    if (build_variant == "") {
        build_variant = "N/A";
    }
    LOG_INFO(Frontend, "Build Variant: {}", build_variant);
#if CITRA_ARCH(x86_64)
    const auto& caps = Common::GetCPUCaps();
    std::string cpu_string = caps.cpu_string;
    if (caps.avx || caps.avx2 || caps.avx512) {
        cpu_string += " | AVX";
        if (caps.avx512) {
            cpu_string += "512";
        } else if (caps.avx2) {
            cpu_string += '2';
        }
        if (caps.fma) {
            cpu_string += " | FMA";
        }
    }
    LOG_INFO(Frontend, "Host CPU: {}", cpu_string);
#endif
    LOG_INFO(Frontend, "Host OS: {}", PrettyProductName().toStdString());
    const auto& mem_info = Common::GetMemInfo();
    using namespace Common::Literals;
    LOG_INFO(Frontend, "Host RAM: {:.2f} GiB", mem_info.total_physical_memory / f64{1_GiB});
    LOG_INFO(Frontend, "Host Swap: {:.2f} GiB", mem_info.total_swap_memory / f64{1_GiB});
    UpdateWindowTitle();

    QIcon azahar_icon = QIcon(QString::fromStdString(":/icons/default/256x256/zeste.png"));
    render_window->setWindowIcon(azahar_icon);
    secondary_window->setWindowIcon(azahar_icon);

    show();

    // If the theme is updated before show() it doesn't apply properly, so do it again here.
    // We still need the earlier call to make sure that the icons are set up correctly in the
    // game list.
    UpdateUITheme();

#ifdef __APPLE__
    if (AppleUtils::IsRunningFromTerminal()) {
        QMessageBox::warning(
            this, tr("Warning"),
            tr("The `azahar` executable is being run directly rather than via the Azahar.app "
               "bundle.\n\n"
               "When run this way, the app may be missing certain functionality such as camera "
               "emulation.\n\n"
               "It is recommended to instead run Azahar using the `open` command, e.g.:\n"
               "`open ./Azahar.app`"));
    }
#endif

    // Zeste: compares with the latest release on GitHub (at most every 6 hours, cached).
    if (UISettings::values.check_for_update_on_start) {
        ZesteCheckForUpdates(false);
    }


    mouse_hide_timer.setInterval(default_mouse_timeout);
    connect(&mouse_hide_timer, &QTimer::timeout, this, &GMainWindow::HideMouseCursor);
    connect(ui->menubar, &QMenuBar::hovered, this, &GMainWindow::OnMouseActivity);

#ifdef ENABLE_OPENGL
    gl_renderer = GetOpenGLRenderer();
#endif

#ifdef ENABLE_VULKAN
    physical_devices = GetVulkanPhysicalDevices();
#endif

    if (!game_path.isEmpty()) {
        BootGame(game_path);
        if (headless_mode && !emu_thread) {
            // Boot failed: nothing to run, leave as soon as the event loop starts.
            LOG_CRITICAL(Frontend, "Zeste headless: boot failed, exiting with code 1");
            headless_boot_failed = true;
        }
    }
}

GMainWindow::~GMainWindow() {
    // Will get automatically deleted otherwise
    if (!render_window->parent()) {
        delete render_window;
    }

    Pica::g_debug_context.reset();
    Network::Shutdown();
}

void GMainWindow::InitializeWidgets() {
    render_window = new GRenderWindow(this, emu_thread.get(), system, false);
    secondary_window = new GRenderWindow(this, emu_thread.get(), system, true);
    render_window->hide();
    secondary_window->hide();
    secondary_window->setParent(nullptr);
    if (headless_mode) {
        // Widgets behave as shown (GL contexts, layouts, screenshots) but never reach the screen.
        MakeHeadless(render_window);
        MakeHeadless(secondary_window);
    }

#ifndef ENABLE_FFMPEG
    ui->action_Dump_Video->setVisible(false);
#endif
    game_list = new GameList(*play_time_manager, this);
    ui->horizontalLayout->addWidget(game_list);

    game_list_placeholder = new GameListPlaceholder(this);
    ui->horizontalLayout->addWidget(game_list_placeholder);
    game_list_placeholder->setVisible(false);

    loading_screen = new LoadingScreen(this);
    loading_screen->hide();
    ui->horizontalLayout->addWidget(loading_screen);
    connect(loading_screen, &LoadingScreen::Hidden, this, [&] {
        loading_screen->Clear();
        if (emulation_running) {
            render_window->show();
            if (!headless_mode) {
                render_window->setFocus();
                render_window->activateWindow();
            }
        }
    });

    InputCommon::Init();
    multiplayer_state = new MultiplayerState(system, this, game_list->GetModel(),
                                             ui->action_Leave_Room, ui->action_Show_Room);
    multiplayer_state->setVisible(false);

    UpdateBootHomeMenuState();

    // Create status bar
    message_label = new QLabel();
    // Configured separately for left alignment
    message_label->setFrameStyle(QFrame::NoFrame);
    message_label->setContentsMargins(4, 0, 4, 0);
    message_label->setAlignment(Qt::AlignLeft);
    statusBar()->addPermanentWidget(message_label, 1);

    progress_bar = new QProgressBar();
    progress_bar->hide();
    statusBar()->addPermanentWidget(progress_bar);

    loading_shaders_label = new QLabel();

    artic_traffic_label = new QLabel();
    artic_traffic_label->setToolTip(
        tr("Current Artic traffic speed. Higher values indicate bigger transfer loads."));

    emu_speed_label = new QLabel();
    emu_speed_label->setToolTip(tr("Current emulation speed. Values higher or lower than 100% "
                                   "indicate emulation is running faster or slower than a 3DS."));
    game_fps_label = new QLabel();
    game_fps_label->setToolTip(tr("How many frames per second the app is currently displaying. "
                                  "This will vary from app to app and scene to scene."));
    emu_frametime_label = new QLabel();
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a 3DS frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    for (auto& label : {loading_shaders_label, artic_traffic_label, emu_speed_label, game_fps_label,
                        emu_frametime_label}) {
        label->setVisible(false);
        label->setFrameStyle(QFrame::NoFrame);
        label->setContentsMargins(4, 0, 4, 0);
        statusBar()->addPermanentWidget(label);
    }

    // Setup Graphics API button
    graphics_api_button = new QPushButton();
    graphics_api_button->setObjectName(QStringLiteral("GraphicsAPIStatusBarButton"));
    graphics_api_button->setFocusPolicy(Qt::NoFocus);
    UpdateAPIIndicator();

    connect(graphics_api_button, &QPushButton::clicked, this, [this] { UpdateAPIIndicator(true); });

    statusBar()->insertPermanentWidget(0, graphics_api_button);

    // Zeste: strict hardware mode on/off (what --strict sets).
    zeste_strict_button = new QPushButton(tr("STRICT"));
    zeste_strict_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    zeste_strict_button->setFocusPolicy(Qt::NoFocus);
    zeste_strict_button->setCheckable(true);
    connect(zeste_strict_button, &QPushButton::clicked, this, &GMainWindow::ZesteSetStrict);
    statusBar()->insertPermanentWidget(0, zeste_strict_button);

    volume_popup = new QWidget(this);
    volume_popup->setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::Popup);
    volume_popup->setLayout(new QVBoxLayout());
    volume_popup->setMinimumWidth(200);

    volume_slider = new QSlider(Qt::Horizontal);
    volume_slider->setObjectName(QStringLiteral("volume_slider"));
    volume_slider->setMaximum(100);
    volume_slider->setPageStep(5);
    connect(volume_slider, &QSlider::valueChanged, this, [this](int percentage) {
        Settings::values.audio_muted = false;
        const auto value = static_cast<float>(percentage) / volume_slider->maximum();
        Settings::values.volume.SetValue(value);
        UpdateVolumeUI();
    });
    volume_popup->layout()->addWidget(volume_slider);

    volume_button = new QPushButton();
    volume_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    volume_button->setFocusPolicy(Qt::NoFocus);
    volume_button->setCheckable(true);
    UpdateVolumeUI();
    connect(volume_button, &QPushButton::clicked, this, [&] {
        UpdateVolumeUI();
        volume_popup->setVisible(!volume_popup->isVisible());
        QRect rect = volume_button->geometry();
        QPoint bottomLeft = statusBar()->mapToGlobal(rect.topLeft());
        bottomLeft.setY(bottomLeft.y() - volume_popup->geometry().height());
        volume_popup->setGeometry(QRect(bottomLeft, QSize(rect.width(), rect.height())));
    });
    statusBar()->insertPermanentWidget(1, volume_button);
    // Zeste: [STRICT VOLUME API] | message ... [speed fps frame]
    statusBar()->insertPermanentWidget(3, ZesteStatusDivider(this));

    // Zeste: multiplayer is hidden (local wireless still works through the room code).
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusText());
    statusBar()->addPermanentWidget(multiplayer_state->GetStatusIcon());
    multiplayer_state->GetStatusText()->hide();
    multiplayer_state->GetStatusIcon()->hide();

    notification_led = new LedWidget();
    notification_led->setToolTip(tr("Emulated notification LED"));
    statusBar()->addPermanentWidget(notification_led);
    connect(this, &GMainWindow::InfoLEDColorChanged, this, [this] {
        auto led_color = system.GetInfoLEDColor();
        notification_led->setColor(QColor(led_color.r(), led_color.g(), led_color.b()));
    });

    statusBar()->setVisible(true);

    // Item frames are removed by ZesteStatusBarStyle (part of the app stylesheet).

    QActionGroup* actionGroup_ScreenLayouts = new QActionGroup(this);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Default);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Single_Screen);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Large_Screen);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Side_by_Side);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Separate_Windows);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Hybrid_Screen);
    actionGroup_ScreenLayouts->addAction(ui->action_Screen_Layout_Custom_Layout);

    QActionGroup* actionGroup_SmallPositions = new QActionGroup(this);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_TopRight);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_MiddleRight);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_BottomRight);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_TopLeft);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_MiddleLeft);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_BottomLeft);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_Above);
    actionGroup_SmallPositions->addAction(ui->action_Small_Screen_Below);
}

void GMainWindow::InitializeDebugWidgets() {
    if (Pica::g_debug_context) {
        connect(ui->action_Create_Pica_Surface_Viewer, &QAction::triggered, this,
                &GMainWindow::OnCreateGraphicsSurfaceViewer);
    } else {
        ui->action_Create_Pica_Surface_Viewer->setEnabled(false);
    }

    QMenu* debug_menu = ui->menu_View_Debugging;

#if MICROPROFILE_ENABLED
    microProfileDialog = new MicroProfileDialog(this);
    microProfileDialog->hide();
    debug_menu->addAction(microProfileDialog->toggleViewAction());
#endif

    registersWidget = new RegistersWidget(system, this);
    addDockWidget(Qt::RightDockWidgetArea, registersWidget);
    registersWidget->hide();
    debug_menu->addAction(registersWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, registersWidget,
            &RegistersWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, registersWidget,
            &RegistersWidget::OnEmulationStopping);

    if (Pica::g_debug_context) {
        graphicsWidget = new GPUCommandStreamWidget(system, this);
        addDockWidget(Qt::RightDockWidgetArea, graphicsWidget);
        graphicsWidget->hide();
        debug_menu->addAction(graphicsWidget->toggleViewAction());

        graphicsCommandsWidget = new GPUCommandListWidget(system, this);
        addDockWidget(Qt::RightDockWidgetArea, graphicsCommandsWidget);
        graphicsCommandsWidget->hide();
        debug_menu->addAction(graphicsCommandsWidget->toggleViewAction());

        graphicsBreakpointsWidget = new GraphicsBreakPointsWidget(Pica::g_debug_context, this);
        addDockWidget(Qt::RightDockWidgetArea, graphicsBreakpointsWidget);
        graphicsBreakpointsWidget->hide();
        debug_menu->addAction(graphicsBreakpointsWidget->toggleViewAction());

        graphicsVertexShaderWidget =
            new GraphicsVertexShaderWidget(system, Pica::g_debug_context, this);
        addDockWidget(Qt::RightDockWidgetArea, graphicsVertexShaderWidget);
        graphicsVertexShaderWidget->hide();
        debug_menu->addAction(graphicsVertexShaderWidget->toggleViewAction());

        graphicsTracingWidget = new GraphicsTracingWidget(system, Pica::g_debug_context, this);
        addDockWidget(Qt::RightDockWidgetArea, graphicsTracingWidget);
        graphicsTracingWidget->hide();
        debug_menu->addAction(graphicsTracingWidget->toggleViewAction());
        connect(this, &GMainWindow::EmulationStarting, graphicsTracingWidget,
                &GraphicsTracingWidget::OnEmulationStarting);
        connect(this, &GMainWindow::EmulationStopping, graphicsTracingWidget,
                &GraphicsTracingWidget::OnEmulationStopping);
    }

    waitTreeWidget = new WaitTreeWidget(system, this);
    addDockWidget(Qt::LeftDockWidgetArea, waitTreeWidget);
    waitTreeWidget->hide();
    debug_menu->addAction(waitTreeWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            &WaitTreeWidget::OnEmulationStopping);

    lleServiceModulesWidget = new LLEServiceModulesWidget(this);
    addDockWidget(Qt::RightDockWidgetArea, lleServiceModulesWidget);
    lleServiceModulesWidget->hide();
    debug_menu->addAction(lleServiceModulesWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting,
            [this] { lleServiceModulesWidget->setDisabled(true); });
    connect(this, &GMainWindow::EmulationStopping, waitTreeWidget,
            [this] { lleServiceModulesWidget->setDisabled(false); });

    ipcRecorderWidget = new IPCRecorderWidget(system, this);
    addDockWidget(Qt::RightDockWidgetArea, ipcRecorderWidget);
    ipcRecorderWidget->hide();
    debug_menu->addAction(ipcRecorderWidget->toggleViewAction());
    connect(this, &GMainWindow::EmulationStarting, ipcRecorderWidget,
            &IPCRecorderWidget::OnEmulationStarting);
}

void GMainWindow::InitializeRecentFileMenuActions() {
    for (int i = 0; i < max_recent_files_item; ++i) {
        actions_recent_files[i] = new QAction(this);
        actions_recent_files[i]->setVisible(false);
        connect(actions_recent_files[i], &QAction::triggered, this, &GMainWindow::OnMenuRecentFile);

        ui->menu_recent_files->addAction(actions_recent_files[i]);
    }
    ui->menu_recent_files->addSeparator();
    QAction* action_clear_recent_files = new QAction(this);
    action_clear_recent_files->setText(tr("Clear Recent Files"));
    connect(action_clear_recent_files, &QAction::triggered, this, [this] {
        UISettings::values.recent_files.clear();
        UpdateRecentFiles();
    });
    ui->menu_recent_files->addAction(action_clear_recent_files);

    UpdateRecentFiles();
}

void GMainWindow::InitializeSaveStateMenuActions() {
    for (u32 i = 0; i < Core::SaveStateSlotCount; ++i) {
        actions_load_state[i] = new QAction(this);
        actions_load_state[i]->setData(i);
        connect(actions_load_state[i], &QAction::triggered, this, &GMainWindow::OnLoadState);
        if (i > 0)
            ui->menu_Load_State->addAction(actions_load_state[i]);
        actions_save_state[i] = new QAction(this);
        actions_save_state[i]->setData(i);
        connect(actions_save_state[i], &QAction::triggered, this, &GMainWindow::OnSaveState);
        if (i > 0)
            ui->menu_Save_State->addAction(actions_save_state[i]);
    }

    connect(ui->action_Load_from_Newest_Slot, &QAction::triggered, this, [this] {
        UpdateSaveStates();
        if (newest_slot != 0) {
            actions_load_state[newest_slot]->trigger();
        }
    });
    connect(ui->action_Save_to_Oldest_Slot, &QAction::triggered, this, [this] {
        UpdateSaveStates();
        actions_save_state[oldest_slot]->trigger();
    });

    // Quick save / load uses slot
    connect(ui->action_Quick_Save, &QAction::triggered, this, [this] {
        UpdateSaveStates();
        actions_save_state[0]->trigger();
    });
    connect(ui->action_Quick_Load, &QAction::triggered, this, [this] {
        UpdateSaveStates();
        actions_load_state[0]->trigger();
    });

    connect(ui->menu_Load_State->menuAction(), &QAction::hovered, this,
            &GMainWindow::UpdateSaveStates);
    connect(ui->menu_Save_State->menuAction(), &QAction::hovered, this,
            &GMainWindow::UpdateSaveStates);

    UpdateSaveStates();
}

void GMainWindow::InitializeHotkeys() {
    hotkey_registry.LoadHotkeys();
    hotkey_registry.buttonMonitor.start(16);
    LOG_DEBUG(Frontend, "Initializing hotkeys");
    const QString main_window = QStringLiteral("Main Window");
    const QString fullscreen = QStringLiteral("Fullscreen");

    // QAction Hotkeys
    const auto link_action_shortcut = [&](QAction* action, const QString& action_name,
                                          const bool primary_only = false,
                                          const bool auto_repeat = false) {
        static const QString main_window = QStringLiteral("Main Window");
        auto context = hotkey_registry.GetShortcutContext(main_window, action_name);
        auto shortcut = hotkey_registry.GetKeySequence(main_window, action_name);
        action->setShortcut(shortcut);
        action->setShortcutContext(context);
        action->setAutoRepeat(auto_repeat);
        this->addAction(action);
        // handle the shortcuts that are different per-screen
        if (context == Qt::WidgetShortcut) {
            render_window->addAction(action);
            if (!primary_only) {
                secondary_window->addAction(action);
            }
        }
        hotkey_registry.SetAction(main_window, action_name, action);
    };

    link_action_shortcut(ui->action_Load_File, QStringLiteral("Load File"));
    link_action_shortcut(ui->action_Load_Amiibo, QStringLiteral("Load Amiibo"));
    link_action_shortcut(ui->action_Remove_Amiibo, QStringLiteral("Remove Amiibo"));
    link_action_shortcut(ui->action_Exit, QStringLiteral("Exit Zeste"));
    link_action_shortcut(ui->action_Restart, QStringLiteral("Restart Emulation"));
    link_action_shortcut(ui->action_Pause, QStringLiteral("Continue/Pause Emulation"));
    link_action_shortcut(ui->action_Stop, QStringLiteral("Stop Emulation"));
    link_action_shortcut(ui->action_Configure, QStringLiteral("Configure"));
    link_action_shortcut(ui->action_Configure_Current_Game,
                         QStringLiteral("Configure Current Application"));
    link_action_shortcut(ui->action_Show_Filter_Bar, QStringLiteral("Toggle Filter Bar"));
    link_action_shortcut(ui->action_Show_Status_Bar, QStringLiteral("Toggle Status Bar"));
    link_action_shortcut(ui->action_Fullscreen, fullscreen);
    link_action_shortcut(ui->action_Capture_Screenshot, QStringLiteral("Capture Screenshot"));
    link_action_shortcut(ui->action_Debug_Pause, QStringLiteral("Debug Pause"));
    link_action_shortcut(ui->action_Debug_Resume, QStringLiteral("Debug Resume"));
    link_action_shortcut(ui->action_Debug_Step, QStringLiteral("Debug Step"), false, true);
    link_action_shortcut(ui->action_Debug_Unschedule_All, QStringLiteral("Debug Unschedule All"));
    link_action_shortcut(ui->action_Debug_Schedule_All, QStringLiteral("Debug Schedule All"));
    link_action_shortcut(ui->action_Screen_Layout_Swap_Screens, QStringLiteral("Swap Screens"));
    link_action_shortcut(ui->action_Screen_Layout_Upright_Screens,
                         QStringLiteral("Rotate Screens Upright"));
    link_action_shortcut(ui->action_Advance_Frame, QStringLiteral("Advance Frame"));
    link_action_shortcut(ui->action_Load_from_Newest_Slot,
                         QStringLiteral("Load from Newest Non-Quicksave Slot"));
    link_action_shortcut(ui->action_Save_to_Oldest_Slot,
                         QStringLiteral("Save to Oldest Non-Quicksave Slot"));
    link_action_shortcut(ui->action_Quick_Save, QStringLiteral("Quick Save"));
    link_action_shortcut(ui->action_Quick_Load, QStringLiteral("Quick Load"));
    link_action_shortcut(ui->action_View_Lobby, QStringLiteral("Multiplayer Browse Public Rooms"));
    link_action_shortcut(ui->action_Start_Room, QStringLiteral("Multiplayer Create Room"));
    link_action_shortcut(ui->action_Connect_To_Room,
                         QStringLiteral("Multiplayer Direct Connect to Room"));
    link_action_shortcut(ui->action_Show_Room, QStringLiteral("Multiplayer Show Current Room"));
    link_action_shortcut(ui->action_Leave_Room, QStringLiteral("Multiplayer Leave Room"));

    // QShortcut Hotkeys
    const auto connect_shortcut = [&](const QString& action_name, const auto& function) {
        const auto* hotkey = hotkey_registry.GetHotkey(main_window, action_name, this);
        connect(hotkey, &QShortcut::activated, this, function);
    };

    connect_shortcut(QStringLiteral("Toggle Screen Layout"), &GMainWindow::ToggleScreenLayout);
    connect_shortcut(QStringLiteral("Exit Fullscreen"), [&] {
        if (emulation_running) {
            if (secondary_window->isActiveWindow()) {
                secondary_window->showNormal();
            } else {
                ui->action_Fullscreen->setChecked(false);
                ToggleFullscreen();
            }
        }
    });

    connect_shortcut(QStringLiteral("Toggle Per-Application Speed"), [&] {
        if (!hotkey_registry
                 .GetKeySequence(QStringLiteral("Main Window"), QStringLiteral("Toggle Turbo Mode"))
                 .isEmpty()) {
            return;
        }
        Settings::values.frame_limit.SetGlobal(!Settings::values.frame_limit.UsingGlobal());
        UpdateStatusBar();
    });
    connect_shortcut(QStringLiteral("Toggle Texture Dumping"),
                     [&] { Settings::values.dump_textures = !Settings::values.dump_textures; });
    connect_shortcut(QStringLiteral("Toggle Custom Textures"),
                     [&] { Settings::values.custom_textures = !Settings::values.custom_textures; });

    connect_shortcut(QStringLiteral("Toggle Turbo Mode"),
                     [&] { GMainWindow::SetTurboEnabled(!GMainWindow::IsTurboEnabled()); });

    connect_shortcut(QStringLiteral("Increase Speed Limit"), [&] { AdjustSpeedLimit(true); });
    connect_shortcut(QStringLiteral("Decrease Speed Limit"), [&] { AdjustSpeedLimit(false); });

    connect_shortcut(QStringLiteral("Audio Mute/Unmute"), &GMainWindow::OnMute);
    connect_shortcut(QStringLiteral("Audio Volume Down"), &GMainWindow::OnDecreaseVolume);
    connect_shortcut(QStringLiteral("Audio Volume Up"), &GMainWindow::OnIncreaseVolume);

    // We use "static" here in order to avoid capturing by lambda due to a MSVC bug, which makes the
    // variable hold a garbage value after this function exits
    static constexpr u16 FACTOR_3D_STEP = 5;
    connect_shortcut(QStringLiteral("Decrease 3D Factor"), [this] {
        const auto factor_3d = Settings::values.factor_3d.GetValue();
        if (factor_3d > 0) {
            if (factor_3d % FACTOR_3D_STEP != 0) {
                Settings::values.factor_3d = factor_3d - (factor_3d % FACTOR_3D_STEP);
            } else {
                Settings::values.factor_3d = factor_3d - FACTOR_3D_STEP;
            }
            UpdateStatusBar();
        }
    });
    connect_shortcut(QStringLiteral("Increase 3D Factor"), [this] {
        const auto factor_3d = Settings::values.factor_3d.GetValue();
        if (factor_3d < 255) {
            if (factor_3d % FACTOR_3D_STEP != 0) {
                Settings::values.factor_3d =
                    factor_3d + FACTOR_3D_STEP - (factor_3d % FACTOR_3D_STEP);
            } else {
                Settings::values.factor_3d = factor_3d + FACTOR_3D_STEP;
            }
            UpdateStatusBar();
        }
    });
}

void GMainWindow::SetDefaultUIGeometry() {
    // geometry: 55% of the window contents are in the upper screen half, 45% in the lower half
    const QRect screenRect = screen()->geometry();

    const int w = screenRect.width() * 2 / 3;
    const int h = screenRect.height() / 2;
    const int x = (screenRect.x() + screenRect.width()) / 2 - w / 2;
    const int y = (screenRect.y() + screenRect.height()) / 2 - h * 55 / 100;

    setGeometry(x, y, w, h);
}

void GMainWindow::RestoreUIState() {
    restoreGeometry(UISettings::values.geometry);
    restoreState(UISettings::values.state);
    render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
    secondary_window->restoreGeometry(UISettings::values.secondarywindow_geometry);
#if MICROPROFILE_ENABLED
    microProfileDialog->restoreGeometry(UISettings::values.microprofile_geometry);
    microProfileDialog->setVisible(UISettings::values.microprofile_visible.GetValue());
#endif

    game_list->LoadInterfaceLayout();

    ui->action_Single_Window_Mode->setChecked(UISettings::values.single_window_mode.GetValue());
    ToggleWindowMode();

    ui->action_Fullscreen->setChecked(UISettings::values.fullscreen.GetValue());
    SyncMenuUISettings();

    ui->action_Display_Dock_Widget_Headers->setChecked(
        UISettings::values.display_titlebar.GetValue());
    OnDisplayTitleBars(ui->action_Display_Dock_Widget_Headers->isChecked());

    ui->action_Show_Filter_Bar->setChecked(UISettings::values.show_filter_bar.GetValue());
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());

    ui->action_Show_Status_Bar->setChecked(UISettings::values.show_status_bar.GetValue());
    statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
}

void GMainWindow::OnAppFocusStateChanged(Qt::ApplicationState state) {
    if (state != Qt::ApplicationHidden && state != Qt::ApplicationInactive &&
        state != Qt::ApplicationActive) {
        LOG_DEBUG(Frontend, "ApplicationState unusual flag: {} ", state);
    }
    if (!emulation_running) {
        return;
    }
    if (UISettings::values.pause_when_in_background) {
        if (emu_thread->IsRunning() &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            auto_paused = true;
            OnPauseGame();
        } else if (!emu_thread->IsRunning() && auto_paused && state == Qt::ApplicationActive) {
            auto_paused = false;
            OnResumeGame(false);
        }
    }
    if (UISettings::values.mute_when_in_background) {
        if (!Settings::values.audio_muted &&
            (state & (Qt::ApplicationHidden | Qt::ApplicationInactive))) {
            Settings::values.audio_muted = true;
            auto_muted = true;
        } else if (auto_muted && state == Qt::ApplicationActive) {
            Settings::values.audio_muted = false;
            auto_muted = false;
        }
        UpdateVolumeUI();
    }
}

bool GApplicationEventFilter::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::FileOpen) {
        emit FileOpen(static_cast<QFileOpenEvent*>(event));
        return true;
    }
    return false;
}

void GMainWindow::ConnectAppEvents() {
    const auto filter = new GApplicationEventFilter();
    QGuiApplication::instance()->installEventFilter(filter);

    connect(filter, &GApplicationEventFilter::FileOpen, this, &GMainWindow::OnFileOpen);
}

void GMainWindow::ConnectWidgetEvents() {
    connect(game_list, &GameList::GameChosen, this, &GMainWindow::OnGameListLoadFile);
    connect(game_list, &GameList::OpenDirectory, this, &GMainWindow::OnGameListOpenDirectory);
    connect(game_list, &GameList::OpenFolderRequested, this, &GMainWindow::OnGameListOpenFolder);
    connect(game_list, &GameList::RemovePlayTimeRequested, this,
            &GMainWindow::OnGameListRemovePlayTimeData);
    connect(game_list, &GameList::CreateShortcut, this, &GMainWindow::OnGameListCreateShortcut);
    connect(game_list, &GameList::DumpRomFSRequested, this, &GMainWindow::OnGameListDumpRomFS);
    connect(game_list, &GameList::AddDirectory, this, &GMainWindow::OnGameListAddDirectory);
    connect(game_list_placeholder, &GameListPlaceholder::AddDirectory, this,
            &GMainWindow::OnGameListAddDirectory);
    connect(game_list, &GameList::ShowList, this, &GMainWindow::OnGameListShowList);
    connect(game_list, &GameList::PopulatingCompleted, this,
            [this] { multiplayer_state->UpdateGameList(game_list->GetModel()); });
#ifdef ENABLE_DEVELOPER_OPTIONS
    connect(game_list, &GameList::StartingLaunchStressTest, this,
            &GMainWindow::StartLaunchStressTest);
#endif

    connect(game_list, &GameList::OpenPerGameGeneralRequested, this,
            &GMainWindow::OnGameListOpenPerGameProperties);

    connect(this, &GMainWindow::EmulationStarting, render_window,
            &GRenderWindow::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, render_window,
            &GRenderWindow::OnEmulationStopping);
    connect(this, &GMainWindow::EmulationStarting, secondary_window,
            &GRenderWindow::OnEmulationStarting);
    connect(this, &GMainWindow::EmulationStopping, secondary_window,
            &GRenderWindow::OnEmulationStopping);

    connect(&status_bar_update_timer, &QTimer::timeout, this, &GMainWindow::UpdateStatusBar);

    connect(this, &GMainWindow::UpdateProgress, this, &GMainWindow::OnUpdateProgress);
    connect(this, &GMainWindow::CIAInstallReport, this, &GMainWindow::OnCIAInstallReport);
    connect(this, &GMainWindow::CIAInstallFinished, this, &GMainWindow::OnCIAInstallFinished);
    connect(this, &GMainWindow::UpdateThemedIcons, multiplayer_state,
            &MultiplayerState::UpdateThemedIcons);
}

void GMainWindow::ConnectMenuEvents() {
    // Zeste menu (inserted before Help).
    auto* zeste_menu = new QMenu(tr("&Zeste"), this);
    menuBar()->insertMenu(ui->menu_Help->menuAction(), zeste_menu);
    connect(zeste_menu->addAction(tr("&Hardware accuracy settings...")), &QAction::triggered,
            this, [this] {
                configure_open_zeste = true;
                OnConfigure();
            });
    zeste_menu->addSeparator();
    zeste_strict_action = zeste_menu->addAction(tr("&Strict hardware mode"));
    zeste_strict_action->setCheckable(true);
    connect(zeste_strict_action, &QAction::triggered, this, &GMainWindow::ZesteSetStrict);
    connect(zeste_menu->addAction(tr("&Azahar behaviour (disable all)")), &QAction::triggered,
            this, [this] {
                Settings::SetZesteStrictPreset(false);
                config->Save();
                ZesteUpdateStrictUI();
                statusBar()->showMessage(tr("Zeste options disabled"), 5000);
            });
    // Playtest report (only for the titles Zeste::BugReport knows).
    auto* report_action = new QAction(this);
    report_action->setShortcut(QKeySequence(Zeste::BugReport::Shortcut()));
    report_action->setShortcutContext(Qt::ApplicationShortcut);
    connect(report_action, &QAction::triggered, this, &GMainWindow::ZesteOpenPlaytestReport);
    addAction(report_action);
    render_window->addAction(report_action);
    secondary_window->addAction(report_action);
    const auto connect_menu = [&](QAction* action, const auto& event_fn,
                                  QAction::MenuRole role = QAction::NoRole) {
        action->setMenuRole(role);
        connect(action, &QAction::triggered, this, event_fn);
        // Add actions to this window so that hiding menus in fullscreen won't disable them
        addAction(action);
        // Add actions to the render window so that they work outside of single window mode
        render_window->addAction(action);
    };

    // File
    connect_menu(ui->action_Load_File, &GMainWindow::OnMenuLoadFile);
    connect_menu(ui->action_Install_CIA, &GMainWindow::OnMenuInstallCIA);
    connect_menu(ui->action_Connect_Artic, &GMainWindow::OnMenuConnectArticBase);
    connect_menu(ui->action_Setup_System_Files, &GMainWindow::OnMenuSetUpSystemFiles);
    for (u32 region = 0; region < Core::NUM_SYSTEM_TITLE_REGIONS; region++) {
        connect_menu(ui->menu_Boot_Home_Menu->actions().at(region),
                     [this, region] { OnMenuBootHomeMenu(region); });
    }
    connect_menu(ui->action_Exit, &QMainWindow::close, QAction::QuitRole);
    connect_menu(ui->action_Load_Amiibo, &GMainWindow::OnLoadAmiibo);
    connect_menu(ui->action_Remove_Amiibo, &GMainWindow::OnRemoveAmiibo);
    connect_menu(ui->action_Open_Citra_Folder, &GMainWindow::OnOpenCitraFolder);
    connect_menu(ui->action_Open_NAND_Folder, &GMainWindow::OnOpenNANDFolder);
    connect_menu(ui->action_Open_SDMC_Folder, &GMainWindow::OnOpenSDMCFolder);

    // Emulation
    connect_menu(ui->action_Pause, &GMainWindow::OnPauseContinueGame);
    connect_menu(ui->action_Stop, &GMainWindow::OnStopGame);
    connect_menu(ui->action_Restart, [this] { BootGame(QString(game_path)); });
    connect_menu(ui->action_Configure, &GMainWindow::OnConfigure, QAction::PreferencesRole);
    connect_menu(ui->action_Configure_Current_Game, &GMainWindow::OnConfigurePerGame);

    // View
    connect_menu(ui->action_Single_Window_Mode, &GMainWindow::ToggleWindowMode);
    connect_menu(ui->action_Display_Dock_Widget_Headers, &GMainWindow::OnDisplayTitleBars);
    connect_menu(ui->action_Show_Filter_Bar, &GMainWindow::OnToggleFilterBar);
    connect(ui->action_Show_Status_Bar, &QAction::triggered, statusBar(), &QStatusBar::setVisible);

    // Multiplayer
    connect(ui->action_View_Lobby, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnViewLobby);
    connect(ui->action_Start_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCreateRoom);
    connect(ui->action_Leave_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnCloseRoom);
    connect(ui->action_Connect_To_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnDirectConnectToRoom);
    connect(ui->action_Show_Room, &QAction::triggered, multiplayer_state,
            &MultiplayerState::OnOpenNetworkRoom);

    connect_menu(ui->action_Fullscreen, &GMainWindow::ToggleFullscreen);
    connect_menu(ui->action_Screen_Layout_Default, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Single_Screen, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Large_Screen, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Hybrid_Screen, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Side_by_Side, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Separate_Windows, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Custom_Layout, &GMainWindow::ChangeScreenLayout);
    connect_menu(ui->action_Screen_Layout_Swap_Screens, &GMainWindow::OnSwapScreens);
    connect_menu(ui->action_Screen_Layout_Upright_Screens, &GMainWindow::OnRotateScreens);
    connect_menu(ui->action_Small_Screen_TopRight, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_MiddleRight, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_BottomRight, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_TopLeft, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_MiddleLeft, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_BottomLeft, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_Above, &GMainWindow::ChangeSmallScreenPosition);
    connect_menu(ui->action_Small_Screen_Below, &GMainWindow::ChangeSmallScreenPosition);

    // Movie
    connect_menu(ui->action_Record_Movie, &GMainWindow::OnRecordMovie);
    connect_menu(ui->action_Play_Movie, &GMainWindow::OnPlayMovie);
    connect_menu(ui->action_Close_Movie, &GMainWindow::OnCloseMovie);
    connect_menu(ui->action_Save_Movie, &GMainWindow::OnSaveMovie);
    connect_menu(ui->action_Movie_Read_Only_Mode,
                 [this](bool checked) { movie.SetReadOnly(checked); });
    connect_menu(ui->action_Advance_Frame, [this] {
        if (emulation_running && system.frame_limiter.IsFrameAdvancing()) {
            system.frame_limiter.AdvanceFrame();
        }
    });
    connect_menu(ui->action_Capture_Screenshot, &GMainWindow::OnCaptureScreenshot);
#ifdef ENABLE_FFMPEG
    connect_menu(ui->action_Dump_Video, &GMainWindow::OnDumpVideo);
#endif

    // Tools debug
    connect_menu(ui->action_Debug_Pause, [this] {
        if (emu_thread) {
            emu_thread->SetRunning(false);
        }
    });
    connect_menu(ui->action_Debug_Resume, [this] {
        if (emu_thread) {
            emu_thread->SetRunning(true);
        }
    });
    connect_menu(ui->action_Debug_Step, [this] {
        if (emu_thread) {
            emu_thread->ExecStep();
        }
    });
    connect_menu(ui->action_Debug_Unschedule_All,
                 [this] { system.DebugUnscheduleAllThreadsFromFrontend(true); });
    connect_menu(ui->action_Debug_Schedule_All,
                 [this] { system.DebugUnscheduleAllThreadsFromFrontend(false); });

    // Tools

    // Help
    connect_menu(ui->action_Open_Log_Folder, []() {
        QString path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect_menu(ui->action_Check_For_Updates, [this]() { ZesteCheckForUpdates(true); });
    connect_menu(ui->action_About, &GMainWindow::OnMenuAboutCitra, QAction::AboutRole);
}

void GMainWindow::UpdateMenuState() {
    const bool is_paused =
        !emu_thread || !emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing();

    const std::array running_actions{
        ui->action_Stop,
        ui->action_Restart,
        ui->action_Configure_Current_Game,
        ui->action_Load_Amiibo,
        ui->action_Remove_Amiibo,
        ui->action_Pause,
        ui->action_Advance_Frame,
    };

    for (QAction* action : running_actions) {
        action->setEnabled(emulation_running);
    }

    ui->action_Capture_Screenshot->setEnabled(emulation_running);
    ui->action_Advance_Frame->setEnabled(emulation_running && is_paused);

    if (emulation_running && is_paused) {
        ui->action_Pause->setText(tr("Continue"));
    } else {
        ui->action_Pause->setText(tr("Pause"));
    }
}

void GMainWindow::OnDisplayTitleBars(bool show) {
    QList<QDockWidget*> widgets = findChildren<QDockWidget*>();

    if (show) {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(nullptr);
            if (old) {
                delete old;
            }
        }
    } else {
        for (QDockWidget* widget : widgets) {
            QWidget* old = widget->titleBarWidget();
            widget->setTitleBarWidget(new QWidget());
            if (old) {
                delete old;
            }
        }
    }
}

#if defined(HAVE_SDL2) && defined(__unix__) && !defined(__APPLE__)
static std::optional<QDBusObjectPath> HoldWakeLockLinux(u32 window_id = 0) {
    if (!QDBusConnection::sessionBus().isConnected()) {
        return {};
    }
    // reference: https://flatpak.github.io/xdg-desktop-portal/#gdbus-org.freedesktop.portal.Inhibit
    QDBusInterface xdp(QStringLiteral("org.freedesktop.portal.Desktop"),
                       QStringLiteral("/org/freedesktop/portal/desktop"),
                       QStringLiteral("org.freedesktop.portal.Inhibit"));
    if (!xdp.isValid()) {
        LOG_WARNING(Frontend, "Couldn't connect to XDP D-Bus endpoint");
        return {};
    }
    QVariantMap options = {};
    //: TRANSLATORS: This string is shown to the user to explain why Citra needs to prevent the
    //: computer from sleeping
    options.insert(QString::fromLatin1("reason"),
                   QCoreApplication::translate("GMainWindow", "Zeste is running an application"));
    // 0x4: Suspend lock; 0x8: Idle lock
    QDBusReply<QDBusObjectPath> reply =
        xdp.call(QString::fromLatin1("Inhibit"),
                 QString::fromLatin1("x11:") + QString::number(window_id, 16), 12U, options);

    if (reply.isValid()) {
        return reply.value();
    }
    LOG_WARNING(Frontend, "Couldn't read Inhibit reply from XDP: {}",
                reply.error().message().toStdString());
    return {};
}

static void ReleaseWakeLockLinux(const QDBusObjectPath& lock) {
    if (!QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    QDBusInterface unlocker(QString::fromLatin1("org.freedesktop.portal.Desktop"), lock.path(),
                            QString::fromLatin1("org.freedesktop.portal.Request"));
    unlocker.call(QString::fromLatin1("Close"));
}
#endif // __unix__

void GMainWindow::PreventOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#elif defined(HAVE_SDL2)
    SDL_DisableScreenSaver();
#if defined(__unix__) && !defined(__APPLE__)
    auto reply = HoldWakeLockLinux(winId());
    if (reply) {
        wake_lock = std::move(reply.value());
    }
#endif // defined(__unix__) && !defined(__APPLE__)
#endif // _WIN32
}

void GMainWindow::AllowOSSleep() {
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(HAVE_SDL2)
    SDL_EnableScreenSaver();
#if defined(__unix__) && !defined(__APPLE__)
    if (!wake_lock.path().isEmpty()) {
        ReleaseWakeLockLinux(wake_lock);
    }
#endif // defined(__unix__) && !defined(__APPLE__)
#endif // _WIN32
}

bool GMainWindow::LoadROM(const QString& filename) {
    // Shutdown previous session if the emu thread is still active...
    if (emu_thread) {
        ShutdownGame();
    }

    if (!render_window->InitRenderTarget() || !secondary_window->InitRenderTarget()) {
        LOG_CRITICAL(Frontend, "Failed to initialize render targets!");
        return false;
    }

    const auto scope = render_window->Acquire();

    if (!UISettings::values.inserted_cartridge.GetValue().empty()) {
        system.InsertCartridge(UISettings::values.inserted_cartridge.GetValue());
    }

    const Core::System::ResultStatus result{
        system.Load(*render_window, filename.toStdString(), secondary_window)};

    if (result != Core::System::ResultStatus::Success &&
        Settings::values.zeste_no_modal_dialogs.GetValue()) {
        // Zeste: headless / --no-dialogs runs never open the load error dialogs.
        LOG_CRITICAL(Frontend, "Failed to load {} (result {}), no-dialogs mode",
                     filename.toStdString(), static_cast<u32>(result));
        std::cerr << "Failed to load " << filename.toStdString() << " (result "
                  << static_cast<u32>(result) << ")" << std::endl;
        return false;
    }

    if (result != Core::System::ResultStatus::Success) {
        QString invalid_format = tr("Invalid application format");
        QString invalid_format_description =
            tr("The application file format not supported.<br>Please make sure you are using one "
               "of the compatible file formats:<ul><li>Cartridge images: "
               "<b>.cci/.zcci/.bcci/.3ds</b></li><li>Installable archives: "
               "<b>.cia/.zcia/.bcia</b></li><li>Homebrew titles: <b>.3dsx/.z3dsx</b></li><li>NCCH "
               "containers: <b>.cxi/.zcxi/.bcxi/.app</b></li><li>ELF files: "
               "<b>.elf/.axf</b></li></ul>");

        switch (result) {
        case Core::System::ResultStatus::ErrorGetLoader:
            LOG_CRITICAL(Frontend, "Failed to obtain loader for {}", filename.toStdString());
            QMessageBox::critical(this, invalid_format, invalid_format_description);
            break;

        case Core::System::ResultStatus::ErrorSystemMode:
            LOG_CRITICAL(Frontend, "Failed to load application!");
            QMessageBox::critical(this, invalid_format, invalid_format_description);
            break;

        case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted: {
            QMessageBox::critical(this, tr("Encrypted application"),
                                  tr("Encrypted applications are not supported.<br/>"
                                     "<a "
                                     "href='https://azahar-emu.org/blog/game-loading-changes/'>"
                                     "Please check our blog for more info.</a>"));
            break;
        }
        case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
            QMessageBox::critical(this, invalid_format, invalid_format_description);
            break;

        case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
            QMessageBox::critical(this, tr("Unsupported application"),
                                  tr("GBA Virtual Console is not supported by Zeste."));
            break;

        case Core::System::ResultStatus::ErrorArticDisconnected:
            QMessageBox::critical(
                this, tr("Artic Server"),
                tr(fmt::format(
                       "An error has occurred whilst communicating with the Artic Server.\n{}",
                       system.GetStatusDetails())
                       .c_str()));
            break;
        case Core::System::ResultStatus::ErrorN3DSApplication:
            QMessageBox::critical(this, tr("Invalid system mode"),
                                  tr("New 3DS exclusive applications cannot be loaded while Zeste "
                                     "emulates an Old 3DS (Configure > Zeste > Hardware accuracy "
                                     "> Console)."));
            break;
        case Core::System::ResultStatus::ErrorLoader:
            QMessageBox::critical(this, tr("Generic load error"),
                                  tr("A generic load error occurred while loading the "
                                     "application.<br/>Please check the log for more details."));
            break;
        case Core::System::ResultStatus::ErrorLoader_ErrorPatches:
            QMessageBox::critical(this, tr("Error applying patches"),
                                  tr("A generic error occurred while applying a patch to the "
                                     "application.<br/>Please check the log for more details."));
            break;
        case Core::System::ResultStatus::ErrorLoader_ErrorPatchesInvalidTitle:
            QMessageBox::critical(
                this, tr("Error applying patches"),
                tr("Failed to apply a patch because it is designed for a different "
                   "application.<br/>Please make sure you are using the patches for "
                   "the right application, region and version."));
            break;
        default:
            QMessageBox::critical(
                this, tr("Error while loading application"),
                tr("An unknown error occurred.<br/>Please see the log for more details."));
            break;
        }
        return false;
    }

    std::string title;
    system.GetAppLoader().ReadTitle(title, true);
    game_title = QString::fromStdString(title);
    UpdateWindowTitle();

    u64 title_id;
    system.GetAppLoader().ReadProgramId(title_id);

    game_path = filename;
    game_title_id = title_id;

    return true;
}

QString GMainWindow::ZesteInstallCiaForBoot(const QString& cia_path) {
    const std::string path = cia_path.toStdString();
    u64 title_id = 0;
    {
        FileUtil::IOFile file(path, "rb");
        FileSys::CIAContainer cia;
        if (!file.IsOpen() || cia.Load(&file) != Loader::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "--boot-cia: {} is not a readable CIA", path);
            return {};
        }
        title_id = cia.GetTitleMetadata().GetTitleID();
    }
    const auto status = Service::AM::InstallCIA(path);
    if (status != Service::AM::InstallStatus::Success) {
        LOG_CRITICAL(Frontend, "--boot-cia: installing {} failed (status {})", path,
                     static_cast<u32>(status));
        return {};
    }
    const auto media = Service::AM::GetTitleMediaType(title_id);
    const std::string content = Service::AM::GetTitleContentPath(media, title_id);
    LOG_INFO(Frontend, "--boot-cia: installed {:016X}, booting {}", title_id, content);
    return QString::fromStdString(content);
}

void GMainWindow::BootGame(const QString& filename) {
    if (emu_thread) {
        ShutdownGame();
    }

    // NOTE: We could make this not hardcoded, but it's a temp thing anyway so probs not worth. -OS
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Vulkan &&
        Settings::values.texture_filter.GetValue() == Settings::TextureFilter::Anime4K) {
        const auto answer = Settings::values.zeste_no_modal_dialogs.GetValue()
                                ? QMessageBox::Yes
                                : QMessageBox::warning(
            this, tr("Incompatible texture filter"),
            tr("The selected texture filter (Anime4K) is not compatible with the Vulkan "
               "renderer.\nThe texture filter will be set to None.\nProceed?"),
            QMessageBox::Yes | QMessageBox::No);

        if (answer == QMessageBox::No) {
            return;
        }
        if (answer == QMessageBox::Yes) {
            Settings::values.texture_filter = Settings::TextureFilter::NoFilter;
        }
    }

    const bool is_artic = filename.startsWith(QString::fromStdString("articbase:/")) ||
                          filename.startsWith(QString::fromStdString("articinio:/")) ||
                          filename.startsWith(QString::fromStdString("articinin:/"));

    if (!is_artic && filename.endsWith(QStringLiteral(".cia")) &&
        Settings::values.zeste_no_modal_dialogs.GetValue()) {
        // Zeste: install and boot, without the "must be installed" question.
        const QString installed = ZesteInstallCiaForBoot(filename);
        if (!installed.isEmpty()) {
            BootGame(installed);
        }
        return;
    }
    if (!is_artic && filename.endsWith(QStringLiteral(".cia"))) {
        const auto answer = QMessageBox::question(
            this, tr("CIA must be installed before usage"),
            tr("Before using this CIA, you must install it. Do you want to install it now?"),
            QMessageBox::Yes | QMessageBox::No);

        if (answer == QMessageBox::Yes)
            InstallCIA(QStringList(filename));

        return;
    }

    show_artic_label = is_artic;

    LOG_INFO(Frontend, "Zeste starting...");
    if (!is_artic) {
        StoreRecentFile(filename); // Put the filename on top of the list
    }

    if (movie_record_on_start) {
        movie.PrepareForRecording();
    }
    if (movie_playback_on_start) {
        movie.PrepareForPlayback(movie_playback_path.toStdString());
    }

    const std::string path = filename.toStdString();
    auto loader = Loader::GetLoader(path);

    u64 title_id{0};
    Loader::ResultStatus res =
        loader ? loader->ReadProgramId(title_id) : Loader::ResultStatus::Error;

    if (Loader::ResultStatus::Success == res) {
        // Load per game settings
        const std::string name{is_artic ? "" : FileUtil::GetFilename(filename.toStdString())};
        const std::string config_file_name =
            title_id == 0 ? name : fmt::format("{:016X}", title_id);
        LOG_INFO(Frontend, "Loading per application config file for title {}", config_file_name);
        QtConfig per_game_config(config_file_name, QtConfig::ConfigType::PerGameConfig);
    }

    // Artic Server cannot accept a client multiple times, so multiple loaders are not
    // possible. Instead register the app loader early and do not create it again on system load.
    if (loader && !loader->SupportsMultipleInstancesForSameFile()) {
        system.RegisterAppLoaderEarly(loader);
    }

    // Override GDB settings if emulator was launched with
    // GDB port option.
    if (gdbport_from_arg != -1) {
        system.SetGDBPortOverride(gdbport_from_arg);
        system.SetDebugNextProcessFlag();
    }

    system.ApplySettings();

    Settings::LogSettings();

    // Save configurations
    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    config->Save();

    if (!LoadROM(filename)) {
        render_window->ReleaseRenderTarget();
        secondary_window->ReleaseRenderTarget();
        return;
    }

    // Set everything up
    if (movie_record_on_start) {
        movie.StartRecording(movie_record_path.toStdString(), movie_record_author.toStdString());
        movie_record_on_start = false;
        movie_record_path.clear();
        movie_record_author.clear();
    }
    if (movie_playback_on_start) {
        movie.StartPlayback(movie_playback_path.toStdString());
        movie_playback_on_start = false;
        movie_playback_path.clear();
    }

    ui->action_Advance_Frame->setEnabled(false);

#ifdef ENABLE_FFMPEG
    if (video_dumping_on_start) {
        StartVideoDumping(video_dumping_path);
        video_dumping_on_start = false;
        video_dumping_path.clear();
    }
#endif()

    // Register debug widgets
    if (graphicsWidget && graphicsWidget->isVisible()) {
        graphicsWidget->Register();
    }

    // Create and start the emulation thread
    emu_thread = std::make_unique<EmuThread>(system, *render_window);
    emit EmulationStarting(emu_thread.get());
    emu_thread->start();

    connect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    connect(render_window, &GRenderWindow::MouseActivity, this, &GMainWindow::OnMouseActivity);
    connect(secondary_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    connect(secondary_window, &GRenderWindow::MouseActivity, this, &GMainWindow::OnMouseActivity);

    // BlockingQueuedConnection is important here, it makes sure we've finished refreshing our views
    // before the CPU continues
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, registersWidget,
            &RegistersWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeEntered, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeEntered, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, registersWidget,
            &RegistersWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);
    connect(emu_thread.get(), &EmuThread::DebugModeLeft, waitTreeWidget,
            &WaitTreeWidget::OnDebugModeLeft, Qt::BlockingQueuedConnection);

    connect(emu_thread.get(), &EmuThread::LoadProgress, loading_screen,
            &LoadingScreen::OnLoadProgress, Qt::QueuedConnection);
    connect(emu_thread.get(), &EmuThread::SwitchDiskResources, this,
            &GMainWindow::OnSwitchDiskResources, Qt::QueuedConnection);
    connect(emu_thread.get(), &EmuThread::HideLoadingScreen, loading_screen,
            &LoadingScreen::OnLoadComplete);

    // Update the GUI
    registersWidget->OnDebugModeEntered();
    if (ui->action_Single_Window_Mode->isChecked()) {
        game_list->hide();
        game_list_placeholder->hide();
    }
    status_bar_update_timer.start(1000);

    if (UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
        setMouseTracking(true);
    }

    loading_screen->Prepare(system.GetAppLoader());
    loading_screen->show();

    emulation_running = true;
    if (!Settings::values.is_new_3ds.GetValue()) {
        // Zeste: the Old 3DS side is experimental (no hardware to check it against).
        if (headless_mode) {
            std::cout << "Zeste: Old 3DS mode is experimental (not checked against a real Old "
                         "3DS)"
                      << std::endl;
        } else {
            statusBar()->showMessage(tr("Old 3DS mode is experimental: not checked against a "
                                        "real Old 3DS"),
                                     10000);
        }
    }
    if (ui->action_Fullscreen->isChecked()) {
        ShowFullscreen();
    }

    OnResumeGame(true);
}

void GMainWindow::ShutdownGame() {
    if (!emulation_running) {
        return;
    }

    if (ui->action_Fullscreen->isChecked()) {
        HideFullscreen();
    }

#ifdef ENABLE_FFMPEG
    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        game_shutdown_delayed = true;
        OnStopVideoDumping();
        return;
    }
#endif

    AllowOSSleep();

#ifdef ENABLE_DISCORD_RPC
    discord_rpc->Pause();
#endif

    emu_thread->RequestStop();

    // Release emu threads from any breakpoints
    // This belongs after RequestStop() and before wait() because if emulation stops on a GPU
    // breakpoint after (or before) RequestStop() is called, the emulation would never be able
    // to continue out to the main loop and terminate. Thus wait() would hang forever.
    // TODO(bunnei): This function is not thread safe, but it's being used as if it were
    if (Pica::g_debug_context) {
        Pica::g_debug_context->ClearBreakpoints();
    }

    // Unregister debug widgets
    if (graphicsWidget && graphicsWidget->isVisible()) {
        graphicsWidget->Unregister();
    }

    // Frame advancing must be cancelled in order to release the emu thread from waiting
    system.frame_limiter.SetFrameAdvancing(false);

    emit EmulationStopping();

    // Wait for emulation thread to complete and delete it
    emu_thread->wait();
    emu_thread = nullptr;

    system.EjectCartridge();

    OnCloseMovie();

#ifdef ENABLE_DISCORD_RPC
    discord_rpc->Update(false);
#endif
#ifdef __unix__
    Common::Linux::StopGamemode();
#endif

    // The emulation is stopped, so closing the window or not does not matter anymore
    disconnect(render_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);
    disconnect(secondary_window, &GRenderWindow::Closed, this, &GMainWindow::OnStopGame);

    render_window->hide();
    secondary_window->hide();
    loading_screen->hide();
    loading_screen->Clear();

    if (game_list->IsEmpty()) {
        game_list_placeholder->show();
    } else {
        game_list->show();
    }
    game_list->SetFilterFocus();

    setMouseTracking(false);

    // Disable status bar updates
    status_bar_update_timer.stop();
    message_label_used_for_movie = false;
    show_artic_label = false;
    loading_shaders_label->setVisible(false);
    artic_traffic_label->setVisible(false);
    emu_speed_label->setVisible(false);
    game_fps_label->setVisible(false);
    emu_frametime_label->setVisible(false);
    notification_led->setColor(QColor(0, 0, 0));

    UpdateSaveStates();

    emulation_running = false;

    game_title.clear();
    UpdateWindowTitle();

    game_path.clear();
    game_title_id = 0;

    // Update the GUI
    UpdateMenuState();

    // When closing the game, destroy the GLWindow to clear the context after the game is closed
    render_window->ReleaseRenderTarget();
    secondary_window->ReleaseRenderTarget();

    if (headless_exit_on_stop) {
        LOG_INFO(Frontend, "Zeste: emulation stopped, exiting with code {}", headless_exit_code);
        // Leave through the normal close path (input and multiplayer shutdown), then exit.
        QMetaObject::invokeMethod(
            this,
            [this] {
                close();
                QCoreApplication::exit(headless_exit_code);
            },
            Qt::QueuedConnection);
    }
}

#ifdef ENABLE_DEVELOPER_OPTIONS
void GMainWindow::StartLaunchStressTest(const QString& game_path) {
    QThreadPool::globalInstance()->start([this, game_path] {
        do {
            ui->action_Stop->trigger();
            emit game_list->GameChosen(game_path);
            QThread::sleep(2);
        } while (emulation_running);
    });
}
#endif

void GMainWindow::StoreRecentFile(const QString& filename) {
    UISettings::values.recent_files.prepend(filename);
    UISettings::values.recent_files.removeDuplicates();
    while (UISettings::values.recent_files.size() > max_recent_files_item) {
        UISettings::values.recent_files.removeLast();
    }

    UpdateRecentFiles();
}

void GMainWindow::UpdateRecentFiles() {
    const int num_recent_files =
        std::min(static_cast<int>(UISettings::values.recent_files.size()), max_recent_files_item);

    for (int i = 0; i < num_recent_files; i++) {
        const QString text = QStringLiteral("&%1. %2").arg(i + 1).arg(
            QFileInfo(UISettings::values.recent_files[i]).fileName());
        actions_recent_files[i]->setText(text);
        actions_recent_files[i]->setData(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setToolTip(UISettings::values.recent_files[i]);
        actions_recent_files[i]->setVisible(true);
    }

    for (int j = num_recent_files; j < max_recent_files_item; ++j) {
        actions_recent_files[j]->setVisible(false);
    }

    // Enable the recent files menu if the list isn't empty
    ui->menu_recent_files->setEnabled(num_recent_files != 0);
}

void GMainWindow::UpdateSaveStates() {
    if (!system.IsPoweredOn()) {
        ui->menu_Load_State->setEnabled(false);
        ui->menu_Save_State->setEnabled(false);
        return;
    }

    ui->menu_Load_State->setEnabled(true);
    ui->menu_Save_State->setEnabled(true);
    ui->action_Load_from_Newest_Slot->setEnabled(false);

    oldest_slot = newest_slot = 1;
    oldest_slot_time = std::numeric_limits<u64>::max();
    newest_slot_time = 0;

    u64 title_id;
    if (system.GetAppLoader().ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        return;
    }
    auto savestates = Core::ListSaveStates(title_id, movie.GetCurrentMovieID());
    for (u32 i = 0; i < Core::SaveStateSlotCount; ++i) {
        actions_load_state[i]->setEnabled(false);
        if (i == 0) {
            actions_load_state[i]->setText(tr("Quick Load"));
            actions_save_state[i]->setText(tr("Quick Save"));
        } else {
            actions_load_state[i]->setText(tr("Slot %1").arg(i));
            actions_save_state[i]->setText(tr("Slot %1").arg(i));
        }
    }
    for (const auto& savestate : savestates) {
        if (savestate.slot >= Core::SaveStateSlotCount) {
            continue;
        }
        actions_load_state[savestate.slot]->setEnabled(true);
        if (savestate.slot == 0) {
            const auto text = QStringLiteral("%2")
                                  .arg(QDateTime::fromSecsSinceEpoch(savestate.time)
                                           .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")))
                                  .trimmed();
            ui->action_Quick_Save->setText(tr("Quick Save - %1").arg(text).trimmed());
            ui->action_Quick_Load->setText(tr("Quick Load - %1").arg(text).trimmed());
            continue;
        }
        const auto text = tr("Slot %1 - %2")
                              .arg(savestate.slot)
                              .arg(QDateTime::fromSecsSinceEpoch(savestate.time)
                                       .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")))
                              .trimmed();

        actions_load_state[savestate.slot]->setText(text);
        actions_save_state[savestate.slot]->setText(text);

        ui->action_Load_from_Newest_Slot->setEnabled(true);
        if (savestate.time > newest_slot_time) {
            newest_slot = savestate.slot;
            newest_slot_time = savestate.time;
        }
        if (savestate.time < oldest_slot_time) {
            oldest_slot = savestate.slot;
            oldest_slot_time = savestate.time;
        }
    }
    // Value as 1 because quicksave slot is not used for this calculation
    for (u32 i = 1; i < Core::SaveStateSlotCount; ++i) {
        if (!actions_load_state[i]->isEnabled()) {
            // Prefer empty slot
            oldest_slot = i;
            oldest_slot_time = 0;
            break;
        }
    }
}

void GMainWindow::OnGameListLoadFile(QString game_path) {
    if (ConfirmChangeGame()) {
        BootGame(game_path);
    }
}

void GMainWindow::OnGameListOpenFolder(u64 data_id, GameListOpenTarget target) {
    std::string path;
    std::string open_target;

    switch (target) {
    case GameListOpenTarget::SAVE_DATA: {
        open_target = "Save Data";
        std::string sdmc_dir = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
        path = FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(sdmc_dir, data_id);
        break;
    }
    case GameListOpenTarget::EXT_DATA: {
        open_target = "Extra Data";
        std::string sdmc_dir = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
        path = FileSys::GetExtDataPathFromId(sdmc_dir, data_id);
        break;
    }
    case GameListOpenTarget::APPLICATION: {
        open_target = "Application";
        auto media_type = Service::AM::GetTitleMediaType(data_id);
        path = Service::AM::GetTitlePath(media_type, data_id) + "content/";
        break;
    }
    case GameListOpenTarget::UPDATE_DATA: {
        open_target = "Update Data";
        path = Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, data_id + 0xe00000000) +
               "content/";
        break;
    }
    case GameListOpenTarget::TEXTURE_DUMP: {
        open_target = "Dumped Textures";
        path = fmt::format("{}textures/{:016X}/",
                           FileUtil::GetUserPath(FileUtil::UserPath::DumpDir), data_id);
        break;
    }
    case GameListOpenTarget::TEXTURE_LOAD: {
        open_target = "Custom Textures";
        path = fmt::format("{}textures/{:016X}/",
                           FileUtil::GetUserPath(FileUtil::UserPath::LoadDir), data_id);
        break;
    }
    case GameListOpenTarget::MODS: {
        open_target = "Mods";
        path = fmt::format("{}mods/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::LoadDir),
                           data_id);
        break;
    }
    case GameListOpenTarget::DLC_DATA: {
        open_target = "DLC Data";
        path = fmt::format("{}Nintendo 3DS/00000000000000000000000000000000/"
                           "00000000000000000000000000000000/title/0004008c/{:08x}/content/",
                           FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), data_id);
        break;
    }
    case GameListOpenTarget::SHADER_CACHE: {
        open_target = "Shader Cache";
        path = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir);
        break;
    }
    default:
        LOG_ERROR(Frontend, "Unexpected target {}", static_cast<int>(target));
        return;
    }

    QString qpath = QString::fromStdString(path);

    QDir dir(qpath);
    if (!dir.exists()) {
        QMessageBox::critical(
            this, tr("Error Opening %1 Folder").arg(QString::fromStdString(open_target)),
            tr("Folder does not exist!"));
        return;
    }

    LOG_INFO(Frontend, "Opening {} path for data_id={:016x}", open_target, data_id);

    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath));
}

void GMainWindow::OnGameListRemovePlayTimeData(u64 program_id) {
    if (QMessageBox::question(this, tr("Remove Play Time Data"), tr("Reset play time?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    play_time_manager->ResetProgramPlayTime(program_id);
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

bool GMainWindow::CreateShortcutLink(const std::filesystem::path& shortcut_path,
                                     const std::string& comment,
                                     const std::filesystem::path& icon_path,
                                     const std::string& command, const std::string& arguments,
                                     const std::string& categories, const std::string& keywords,
                                     const std::string& name, const bool& skip_tryexec) try {
#if defined(__linux__) || defined(__FreeBSD__) // Linux and FreeBSD
    std::filesystem::path shortcut_path_full = shortcut_path / (name + ".desktop");
    std::ofstream shortcut_stream(shortcut_path_full, std::ios::binary | std::ios::trunc);
    if (!shortcut_stream.is_open()) {
        LOG_ERROR(Frontend, "Failed to create shortcut");
        return false;
    }
    // TODO: Migrate fmt::print to std::print in futures STD C++ 23.
    fmt::print(shortcut_stream, "[Desktop Entry]\n");
    fmt::print(shortcut_stream, "Type=Application\n");
    fmt::print(shortcut_stream, "Version=1.0\n");
    fmt::print(shortcut_stream, "Name={}\n", name);
    if (!comment.empty()) {
        fmt::print(shortcut_stream, "Comment={}\n", comment);
    }
    if (std::filesystem::is_regular_file(icon_path)) {
        fmt::print(shortcut_stream, "Icon={}\n", icon_path.string());
    }
    if (!skip_tryexec) {
        fmt::print(shortcut_stream, "TryExec={}\n", command);
    }
    fmt::print(shortcut_stream, "Exec={} {}\n", command, arguments);
    if (!categories.empty()) {
        fmt::print(shortcut_stream, "Categories={}\n", categories);
    }
    if (!keywords.empty()) {
        fmt::print(shortcut_stream, "Keywords={}\n", keywords);
    }
    return true;
#elif defined(_WIN32) // Windows
    HRESULT hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        LOG_ERROR(Frontend, "CoInitialize failed");
        return false;
    }
    SCOPE_EXIT({ CoUninitialize(); });
    IShellLinkW* ps1 = nullptr;
    IPersistFile* persist_file = nullptr;
    SCOPE_EXIT({
        if (persist_file != nullptr) {
            persist_file->Release();
        }
        if (ps1 != nullptr) {
            ps1->Release();
        }
    });
    HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                    reinterpret_cast<void**>(&ps1));
    if (FAILED(hres)) {
        LOG_ERROR(Frontend, "Failed to create IShellLinkW instance");
        return false;
    }
    hres = ps1->SetPath(Common::UTF8ToUTF16W(command).data());
    if (FAILED(hres)) {
        LOG_ERROR(Frontend, "Failed to set path");
        return false;
    }
    if (!arguments.empty()) {
        hres = ps1->SetArguments(Common::UTF8ToUTF16W(arguments).data());
        if (FAILED(hres)) {
            LOG_ERROR(Frontend, "Failed to set arguments");
            return false;
        }
    }
    if (!comment.empty()) {
        hres = ps1->SetDescription(Common::UTF8ToUTF16W(comment).data());
        if (FAILED(hres)) {
            LOG_ERROR(Frontend, "Failed to set description");
            return false;
        }
    }
    if (std::filesystem::is_regular_file(icon_path)) {
        hres = ps1->SetIconLocation(icon_path.c_str(), 0);
        if (FAILED(hres)) {
            LOG_ERROR(Frontend, "Failed to set icon location");
            return false;
        }
    }
    hres = ps1->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist_file));
    if (FAILED(hres)) {
        LOG_ERROR(Frontend, "Failed to get IPersistFile interface");
        return false;
    }
    hres = persist_file->Save(
        std::filesystem::path{shortcut_path / (Common::UTF8ToUTF16W(name) + L".lnk")}.c_str(),
        TRUE);
    if (FAILED(hres)) {
        LOG_ERROR(Frontend, "Failed to save shortcut");
        return false;
    }
    return true;
#else                 // Unsupported platform
    return false;
#endif
} catch (const std::exception& e) {
    LOG_ERROR(Frontend, "Failed to create shortcut: {}", e.what());
    return false;
}

// Messages in pre-defined message boxes for less code spaghetti
bool GMainWindow::CreateShortcutMessagesGUI(QWidget* parent, int message,
                                            const QString& game_title) {
    int result = 0;
    QMessageBox::StandardButtons buttons;
    switch (message) {
    case GMainWindow::CREATE_SHORTCUT_MSGBOX_FULLSCREEN_PROMPT:
        buttons = QMessageBox::Yes | QMessageBox::No;
        result = QMessageBox::information(
            parent, tr("Create Shortcut"),
            tr("Do you want to launch the application in fullscreen?"), buttons);
        return result == QMessageBox::Yes;
    case GMainWindow::CREATE_SHORTCUT_MSGBOX_SUCCESS:
        QMessageBox::information(parent, tr("Create Shortcut"),
                                 tr("Successfully created a shortcut to %1").arg(game_title));
        return false;
    case GMainWindow::CREATE_SHORTCUT_MSGBOX_APPIMAGE_VOLATILE_WARNING:
        buttons = QMessageBox::StandardButton::Ok | QMessageBox::StandardButton::Cancel;
        result =
            QMessageBox::warning(this, tr("Create Shortcut"),
                                 tr("This will create a shortcut to the current AppImage. This may "
                                    "not work well if you update. Continue?"),
                                 buttons);
        return result == QMessageBox::Ok;
    default:
        buttons = QMessageBox::Ok;
        QMessageBox::critical(parent, tr("Create Shortcut"),
                              tr("Failed to create a shortcut to %1").arg(game_title), buttons);
        return false;
    }
}

bool GMainWindow::MakeShortcutIcoPath(const u64 program_id, const std::string_view game_file_name,
                                      std::filesystem::path& out_icon_path) {
    // Get path to Citra icons directory & icon extension
    std::string ico_extension = "png";
#if defined(_WIN32)
    out_icon_path = FileUtil::GetUserPath(FileUtil::UserPath::IconsDir);
    ico_extension = "ico";
#elif defined(__linux__) || defined(__FreeBSD__)
    out_icon_path = FileUtil::GetUserDirectory("XDG_DATA_HOME") + "/icons/hicolor/256x256/";
#endif
    // Create icons directory if it doesn't exist
    if (!FileUtil::CreateFullPath(out_icon_path.string())) {
        QMessageBox::critical(
            this, tr("Create Icon"),
            tr("Cannot create icon file. Path \"%1\" does not exist and cannot be created.")
                .arg(QString::fromStdString(out_icon_path.string())),
            QMessageBox::StandardButton::Ok);
        out_icon_path.clear();
        return false;
    }

    // Create icon file path
    out_icon_path /= (program_id == 0 ? fmt::format("citra-{}.{}", game_file_name, ico_extension)
                                      : fmt::format("citra-{:016X}.{}", program_id, ico_extension));
    return true;
}

void GMainWindow::OnGameListCreateShortcut(u64 program_id, const std::string& game_path,
                                           GameListShortcutTarget target) {
    std::string citra_command{};
    bool skip_tryexec = false;
    const char* env_flatpak_id = getenv("FLATPAK_ID");
    if (env_flatpak_id) {
        citra_command = fmt::format("flatpak run {}", env_flatpak_id);
        skip_tryexec = true;
    } else {
        // Get path to Citra executable
        const QStringList args = QApplication::arguments();
        citra_command = args[0].toStdString();
        // If relative path, make it an absolute path
        if (citra_command.c_str()[0] == '.') {
            citra_command = FileUtil::GetCurrentDir().value_or("") + DIR_SEP + citra_command;
        }
    }

    // Shortcut path
    std::filesystem::path shortcut_path{};
    if (target == GameListShortcutTarget::Desktop) {
        shortcut_path =
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).toStdString();
    } else if (target == GameListShortcutTarget::Applications) {
        shortcut_path = GetApplicationsDirectory();
    }

    // Icon path and title
    if (!std::filesystem::exists(shortcut_path)) {
        CreateShortcutMessagesGUI(this, CREATE_SHORTCUT_MSGBOX_ERROR, {});
        LOG_ERROR(Frontend, "Invalid shortcut target");
        return;
    }

    // Get title from game file
    const auto loader = Loader::GetLoader(game_path);
    std::string game_title = fmt::format("{:016X}", program_id);
    if (loader->ReadTitle(game_title, false) != Loader::ResultStatus::Success) {
        game_title = fmt::format("{:016x}", program_id);
    }

    // Delete illegal characters from title
    const std::string illegal_chars = "<>:\"/\\|?*.";
    for (auto it = game_title.rbegin(); it != game_title.rend(); ++it) {
        if (illegal_chars.find(*it) != std::string::npos) {
            game_title.erase(it.base() - 1);
        }
    }

    // Get icon from game file
    std::vector<u8> icon_image_file;
    if (loader->ReadIcon(icon_image_file, false) != Loader::ResultStatus::Success) {
        LOG_WARNING(Frontend, "Could not read icon from {:s}", game_path);
    }

    const QPixmap pixmap = GetQPixmapFromSMDH(icon_image_file);
    const QImage icon_data = pixmap.toImage();
    std::filesystem::path out_icon_path;
    if (MakeShortcutIcoPath(program_id, game_title, out_icon_path)) {
        if (!SaveIconToFile(out_icon_path, icon_data)) {
            LOG_ERROR(Frontend, "Could not write icon to file");
        }
    }

    const auto qt_game_title = QString::fromStdString(game_title);
#if defined(__linux__)
    // Special case for AppImages
    // Warn once if we are making a shortcut to a volatile AppImage
    const std::string appimage_ending =
        std::string(Common::g_scm_rev).substr(0, 9).append(".AppImage");
    if (citra_command.ends_with(appimage_ending) && !UISettings::values.shortcut_already_warned) {
        if (CreateShortcutMessagesGUI(this, CREATE_SHORTCUT_MSGBOX_APPIMAGE_VOLATILE_WARNING,
                                      qt_game_title)) {
            return;
        }
        UISettings::values.shortcut_already_warned = true;
    }
#endif // __linux__
    // Create shortcut
    std::string arguments = fmt::format("\"{:s}\"", game_path);
    if (CreateShortcutMessagesGUI(this, CREATE_SHORTCUT_MSGBOX_FULLSCREEN_PROMPT, qt_game_title)) {
        arguments = "-f " + arguments;
    }
    const std::string comment = fmt::format("Start {:s} with Zeste", game_title);
    const std::string categories = "Game;Emulator;Qt;";
    const std::string keywords = "3ds;Nintendo;";

    if (CreateShortcutLink(shortcut_path, comment, out_icon_path, citra_command, arguments,
                           categories, keywords, game_title, skip_tryexec)) {
        CreateShortcutMessagesGUI(this, CREATE_SHORTCUT_MSGBOX_SUCCESS, qt_game_title);
        return;
    }
    CreateShortcutMessagesGUI(this, CREATE_SHORTCUT_MSGBOX_ERROR, qt_game_title);
}

void GMainWindow::OnGameListDumpRomFS(QString game_path, u64 program_id) {
    auto* dialog = new QProgressDialog(tr("Dumping..."), tr("Cancel"), 0, 0, this);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowFlags(dialog->windowFlags() &
                           ~(Qt::WindowCloseButtonHint | Qt::WindowContextHelpButtonHint));
    dialog->setCancelButton(nullptr);
    dialog->setMinimumDuration(0);
    dialog->setValue(0);

    const auto base_path = fmt::format(
        "{}romfs/{:016X}", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir), program_id);
    const auto update_path =
        fmt::format("{}romfs/{:016X}", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir),
                    program_id | 0x0004000e00000000);
    using FutureWatcher = QFutureWatcher<std::pair<Loader::ResultStatus, Loader::ResultStatus>>;
    auto* future_watcher = new FutureWatcher(this);
    connect(future_watcher, &FutureWatcher::finished, this,
            [this, dialog, base_path, update_path, future_watcher] {
                dialog->hide();
                const auto& [base, update] = future_watcher->result();
                if (base != Loader::ResultStatus::Success) {
                    QMessageBox::critical(
                        this, QStringLiteral("Zeste"),
                        tr("Could not dump base RomFS.\nRefer to the log for details."));
                    return;
                }
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(base_path)));
                if (update == Loader::ResultStatus::Success) {
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(QString::fromStdString(update_path)));
                }
            });

    auto future = QtConcurrent::run([game_path, base_path, update_path] {
        std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(game_path.toStdString());
        return std::make_pair(loader->DumpRomFS(base_path), loader->DumpUpdateRomFS(update_path));
    });
    future_watcher->setFuture(future);
}

void GMainWindow::OnGameListOpenDirectory(const QString& directory) {
    QString path;
    if (directory == QStringLiteral("INSTALLED")) {
        path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) +
                                      "Nintendo "
                                      "3DS/00000000000000000000000000000000/"
                                      "00000000000000000000000000000000/title/00040000");
    } else if (directory == QStringLiteral("SYSTEM")) {
        path = QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir) +
                                      "00000000000000000000000000000000/title/00040010");
    } else {
        path = directory;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::critical(this, tr("Error Opening %1").arg(path), tr("Folder does not exist!"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void GMainWindow::OnGameListAddDirectory() {
    const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Directory"));
    if (dir_path.isEmpty())
        return;
    UISettings::GameDir game_dir{dir_path, false, true};
    if (!UISettings::values.game_dirs.contains(game_dir)) {
        UISettings::values.game_dirs.append(game_dir);
        game_list->PopulateAsync(UISettings::values.game_dirs);
    } else {
        LOG_WARNING(Frontend, "Selected directory is already in the application list");
    }
}

void GMainWindow::OnGameListShowList(bool show) {
    if (emulation_running && ui->action_Single_Window_Mode->isChecked())
        return;
    game_list->setVisible(show);
    game_list_placeholder->setVisible(!show);
};

void GMainWindow::OnGameListOpenPerGameProperties(const QString& file) {
    const auto loader = Loader::GetLoader(file.toStdString());

    u64 title_id{};
    if (!loader || loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        QMessageBox::information(this, tr("Properties"),
                                 tr("The application properties could not be loaded."));
        return;
    }

    OpenPerGameConfiguration(title_id, file);
}

void GMainWindow::OnMenuLoadFile() {
    const QString extensions = QStringLiteral("*.").append(
        GameList::supported_file_extensions.join(QStringLiteral(" *.")));
    const QString file_filter = tr("3DS Executable") + QStringLiteral(" (%1)").arg(extensions) +
                                QStringLiteral(";;") + tr("All Files") + QStringLiteral(" (*.*)");
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Load File"), UISettings::values.roms_path, file_filter);

    if (filename.isEmpty()) {
        return;
    }

    UISettings::values.roms_path = QFileInfo(filename).path();
    BootGame(filename);
}

void GMainWindow::OnMenuSetUpSystemFiles() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Set Up System Files"));

    QVBoxLayout layout(&dialog);

    QLabel label_description(
        tr("<p>Zeste needs console unique data and firmware files from a real console to be "
           "able to use some of its features.<br>Such files and data can be set up with the <a "
           "href=https://github.com/azahar-emu/ArticSetupTool>Azahar "
           "Artic Setup Tool</a><br>Notes:<ul><li><b>This operation will install console unique "
           "data to Zeste, do not share your user or nand folders<br>after performing the setup "
           "process!</b></li><li>While doing the setup process, Zeste will link to the console "
           "running the setup tool. You can unlink the<br>console later from the System tab in the "
           "emulator configuration menu.</li><li>Do not go online with both Zeste and your 3DS "
           "console at the same time after setting up system files,<br>as it could cause "
           "issues.</li><li>Old 3DS setup is needed for the New 3DS setup to work (doing both "
           "setup modes is recommended).</li><li>Both setup modes will work regardless of the "
           "model of the console running the setup tool.</li></ul><hr></p>"),
        &dialog);
    label_description.setOpenExternalLinks(true);
    layout.addWidget(&label_description);

    QHBoxLayout layout_h(&dialog);
    layout.addLayout(&layout_h);

    QLabel label_enter(tr("Enter Azahar Artic Setup Tool address:"), &dialog);

    layout_h.addWidget(&label_enter);

    QLineEdit textInput(UISettings::values.last_artic_base_addr, &dialog);
    layout_h.addWidget(&textInput);

    QLabel label_select(QStringLiteral("<br>") + tr("Choose setup mode:"), &dialog);
    layout.addWidget(&label_select);

    std::pair<bool, bool> install_state = Core::AreSystemTitlesInstalled();

    QRadioButton radio1(&dialog);
    QRadioButton radio2(&dialog);
    QString new3dsSetupString = tr("New 3DS setup");
    QString old3dsSetupString = tr("Old 3DS setup");
    QString availableIcon = QStringLiteral("(\u2139\uFE0F) ");
    QString unavailableIcon = QStringLiteral("(\u26A0) ");
    QString installedIcon = QStringLiteral("(\u2705) ");
    if (!install_state.first) {
        radio1.setChecked(true);

        radio1.setText(availableIcon + old3dsSetupString);
        radio1.setToolTip(tr("Setup is possible."));

        radio2.setText(unavailableIcon + new3dsSetupString);
        radio2.setToolTip(tr("Old 3DS setup is required first."));
        radio2.setEnabled(false);
    } else {
        radio1.setText(installedIcon + old3dsSetupString);
        radio1.setToolTip(tr("Setup completed."));

        if (!install_state.second) {
            radio2.setChecked(true);

            radio2.setText(availableIcon + new3dsSetupString);
            radio2.setToolTip(tr("Setup is possible."));
        } else {
            radio1.setChecked(true);

            radio2.setText(installedIcon + new3dsSetupString);
            radio2.setToolTip(tr("Setup completed."));
        }
    }
    layout.addWidget(&radio1);
    layout.addWidget(&radio2);

    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout.addWidget(&buttonBox);

    int res = dialog.exec();
    if (res == QDialog::Accepted) {
        bool is_o3ds = radio1.isChecked();
        if ((is_o3ds && install_state.first) || (!is_o3ds && install_state.second)) {
            QMessageBox::StandardButton answer =
                QMessageBox::question(this, tr("Set Up System Files"),
                                      tr("The system files for the selected mode are already set "
                                         "up.\nReinstall the files anyway?"),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        Core::UninstallSystemFiles(is_o3ds ? Core::SystemTitleSet::Old3ds
                                           : Core::SystemTitleSet::New3ds);
        QString addr = textInput.text();
        UISettings::values.last_artic_base_addr = addr;
        BootGame(QString::fromStdString(is_o3ds ? "articinio://" : "articinin://").append(addr));
    }
}

void GMainWindow::OnMenuInstallCIA() {
    QStringList filepaths = QFileDialog::getOpenFileNames(
        this, tr("Load Files"), UISettings::values.roms_path,
        tr("3DS Installation File") + QStringLiteral(" (*.cia *.zcia *.bcia);;") + tr("All Files") +
            QStringLiteral(" (*.*)"));

    if (filepaths.isEmpty()) {
        return;
    }

    UISettings::values.roms_path = QFileInfo(filepaths[0]).path();
    InstallCIA(filepaths);
}

void GMainWindow::OnMenuConnectArticBase() {
    bool ok = false;
    auto res = QInputDialog::getText(this, tr("Connect to Artic Base"),
                                     tr("Enter Artic Base server address:"), QLineEdit::Normal,
                                     UISettings::values.last_artic_base_addr, &ok);
    if (ok) {
        UISettings::values.last_artic_base_addr = res;
        BootGame(QString::fromStdString("articbase://").append(res));
    }
}

void GMainWindow::OnMenuBootHomeMenu(u32 region) {
    BootGame(QString::fromStdString(Core::GetHomeMenuNcchPath(region)));
}

void GMainWindow::InstallCIA(QStringList filepaths) {
    ui->action_Install_CIA->setEnabled(false);
    game_list->SetDirectoryWatcherEnabled(false);

    emit UpdateProgress(0, 0);

    (void)QtConcurrent::run([this, filepaths] {
        Service::AM::InstallStatus status;
        const auto cia_progress = [&](std::size_t written, std::size_t total) {
            emit UpdateProgress(written, total);
        };
        for (const auto& current_path : filepaths) {
            status = Service::AM::InstallCIA(current_path.toStdString(), cia_progress);
            emit CIAInstallReport(status, current_path);
        }
        emit CIAInstallFinished();
    });
}

void GMainWindow::OnUpdateProgress(std::size_t written, std::size_t total) {
    if (written == 0 and total == 0) {
        progress_bar->show();
        progress_bar->setValue(0);
        progress_bar->setMaximum(INT_MAX);
    }
    progress_bar->setValue(
        static_cast<int>(INT_MAX * (static_cast<double>(written) / static_cast<double>(total))));
}

void GMainWindow::OnCIAInstallReport(Service::AM::InstallStatus status, QString filepath) {
    QString filename = QFileInfo(filepath).fileName();
    switch (status) {
    case Service::AM::InstallStatus::Success:
        this->statusBar()->showMessage(tr("%1 has been installed successfully.").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        QMessageBox::critical(this, tr("Unable to open File"),
                              tr("Could not open %1").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorAborted:
        QMessageBox::critical(
            this, tr("Installation aborted"),
            tr("The installation of %1 was aborted. Please see the log for more details")
                .arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorInvalid:
        QMessageBox::critical(this, tr("Invalid File"), tr("%1 is not a valid CIA").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorEncrypted:
        QMessageBox::critical(this, tr("CIA Encrypted"),
                              tr("Your CIA file is encrypted.<br/>"
                                 "<a "
                                 "href='https://azahar-emu.org/blog/game-loading-changes/'>"
                                 "Please check our blog for more info.</a>"));
        break;
    case Service::AM::InstallStatus::ErrorFileNotFound:
        QMessageBox::critical(this, tr("Unable to find File"),
                              tr("Could not find %1").arg(filename));
        break;
    case Service::AM::InstallStatus::ErrorCannotCheckBundle:
        break;
    }
}

void GMainWindow::OnCIAInstallFinished() {
    progress_bar->hide();
    progress_bar->setValue(0);
    game_list->SetDirectoryWatcherEnabled(true);
    ui->action_Install_CIA->setEnabled(true);
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

void GMainWindow::UninstallTitles(
    const std::vector<std::tuple<Service::FS::MediaType, u64, QString>>& titles) {
    if (titles.empty()) {
        return;
    }

    // Select the first title in the list as representative.
    const auto first_name = std::get<QString>(titles[0]);

    QProgressDialog progress(tr("Uninstalling '%1'...").arg(first_name), tr("Cancel"), 0,
                             static_cast<int>(titles.size()), this);
    progress.setWindowModality(Qt::WindowModal);

    QFutureWatcher<void> future_watcher;
    QObject::connect(&future_watcher, &QFutureWatcher<void>::finished, &progress,
                     &QProgressDialog::reset);
    QObject::connect(&progress, &QProgressDialog::canceled, &future_watcher,
                     &QFutureWatcher<void>::cancel);
    QObject::connect(&future_watcher, &QFutureWatcher<void>::progressValueChanged, &progress,
                     &QProgressDialog::setValue);

    auto failed = false;
    QString failed_name;

    const auto uninstall_title = [&future_watcher, &failed, &failed_name](const auto& title) {
        const auto name = std::get<QString>(title);
        const auto media_type = std::get<Service::FS::MediaType>(title);
        const auto program_id = std::get<u64>(title);

        const auto result = Service::AM::UninstallProgram(media_type, program_id);
        if (result.IsError()) {
            LOG_ERROR(Frontend, "Failed to uninstall '{}': 0x{:08X}", name.toStdString(),
                      result.raw);
            failed = true;
            failed_name = name;
            future_watcher.cancel();
        }
    };

    future_watcher.setFuture(QtConcurrent::map(titles, uninstall_title));
    progress.exec();
    future_watcher.waitForFinished();

    if (failed) {
        QMessageBox::critical(this, QStringLiteral("Zeste"),
                              tr("Failed to uninstall '%1'.").arg(failed_name));
    } else if (!future_watcher.isCanceled()) {
        QMessageBox::information(this, QStringLiteral("Zeste"),
                                 tr("Successfully uninstalled '%1'.").arg(first_name));
        emit InstalledTitlesChanged();
    }
}

void GMainWindow::OnMenuRecentFile() {
    QAction* action = qobject_cast<QAction*>(sender());
    ASSERT(action);

    const QString filename = action->data().toString();
    if (QFileInfo::exists(filename)) {
        BootGame(filename);
    } else {
        // Display an error message and remove the file from the list.
        QMessageBox::information(this, tr("File not found"),
                                 tr("File \"%1\" not found").arg(filename));

        UISettings::values.recent_files.removeOne(filename);
        UpdateRecentFiles();
    }
}

void GMainWindow::OnResumeGame(bool first_start) {
    qt_cameras->ResumeCameras();

    PreventOSSleep();

    emu_thread->SetRunning(true);
    system.frame_limiter.SetFrameAdvancing(false);
    graphics_api_button->setEnabled(false);
    qRegisterMetaType<Core::System::ResultStatus>("Core::System::ResultStatus");
    qRegisterMetaType<std::string>("std::string");
    connect(emu_thread.get(), &EmuThread::ErrorThrown, this, &GMainWindow::OnCoreError);

    UpdateMenuState();

    play_time_manager->SetProgramId(game_title_id);
    play_time_manager->Start();

    if (first_start) {
#ifdef ENABLE_DISCORD_RPC
        discord_rpc->Update(true);
#endif
    }

#ifdef __unix__
    Common::Linux::StartGamemode();
#endif

    UpdateSaveStates();
    UpdateStatusButtons();
}

void GMainWindow::OnRestartGame() {
    if (!system.IsPoweredOn()) {
        return;
    }
    // Make a copy since BootGame edits game_path
    BootGame(QString(game_path));
}

void GMainWindow::OnPauseGame() {
    system.frame_limiter.SetFrameAdvancing(true);
    qt_cameras->PauseCameras();

    play_time_manager->Stop();

    UpdateMenuState();
    AllowOSSleep();

#ifdef __unix__
    Common::Linux::StopGamemode();
#endif
}

void GMainWindow::OnPauseContinueGame() {
    if (emulation_running) {
        if (emu_thread->IsRunning() && !system.frame_limiter.IsFrameAdvancing()) {
            OnPauseGame();
        } else {
            OnResumeGame(false);
        }
    }
}

void GMainWindow::OnStopGame() {
    SetTurboEnabled(false);

    play_time_manager->Stop();
    // Update game list to show new play time
    game_list->PopulateAsync(UISettings::values.game_dirs);

    ShutdownGame();
    graphics_api_button->setEnabled(true);
    Settings::RestoreGlobalState(false);
    UpdateStatusButtons();
}

void GMainWindow::OnLoadComplete() {
    loading_screen->OnLoadComplete();
    UpdateSecondaryWindowVisibility();
}

void GMainWindow::ToggleFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (secondary_window->isVisible() && secondary_window->isActiveWindow()) {
        // undo the action and fullscreen secondary manually
        ui->action_Fullscreen->toggle();
        ToggleSecondaryFullscreen();
    } else {
        if (ui->action_Fullscreen->isChecked()) {
            ShowFullscreen();
        } else {
            HideFullscreen();
        }
    }
}

void GMainWindow::ToggleSecondaryFullscreen() {
    if (!emulation_running) {
        return;
    }
    if (secondary_window->isFullScreen()) {
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(secondary_window, false);
#endif
        secondary_window->restoreGeometry(UISettings::values.secondarywindow_geometry);
        secondary_window->showNormal();
    } else {
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(secondary_window, true);
#endif
        UISettings::values.secondarywindow_geometry = secondary_window->saveGeometry();
        LOG_INFO(Frontend, "Attempting to fullscreen secondary window");
        secondary_window->showFullScreen();
    }
}

void GMainWindow::ShowFullscreen() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        ui->menubar->hide();
        statusBar()->hide();
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(this, true);
#endif
        showFullScreen();
    } else {
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(render_window, true);
#endif
        render_window->showFullScreen();
    }
}

void GMainWindow::HideFullscreen() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        statusBar()->setVisible(ui->action_Show_Status_Bar->isChecked());
        ui->menubar->show();
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(this, false);
#endif
        showNormal();
        restoreGeometry(UISettings::values.geometry);
    } else {
#ifdef NEEDS_ROUND_CORNERS_FIX
        WindowCornerManager::instance().blockRoundedCorners(render_window, false);
#endif
        render_window->showNormal();
        render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
    }
}

void GMainWindow::ToggleWindowMode() {
    if (ui->action_Single_Window_Mode->isChecked()) {
        // Render in the main window...
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
        ui->horizontalLayout->addWidget(render_window);
        render_window->setFocusPolicy(Qt::StrongFocus);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->setFocus();
            game_list->hide();
        }

    } else {
        // Render in a separate window...
        ui->horizontalLayout->removeWidget(render_window);
        render_window->setParent(nullptr);
        if (emulation_running) {
            render_window->setVisible(true);
            render_window->restoreGeometry(UISettings::values.renderwindow_geometry);
            game_list->show();
        }
    }
}

void GMainWindow::UpdateSecondaryWindowVisibility() {
    if (!emulation_running) {
        return;
    }
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        secondary_window->restoreGeometry(UISettings::values.secondarywindow_geometry);
        secondary_window->show();
    } else {
        UISettings::values.secondarywindow_geometry = secondary_window->saveGeometry();
        secondary_window->hide();
    }
    // make sure focus is on primary window whenever this changes
    if (headless_mode) {
        // Zeste: never activate an invisible window (it took the foreground from full-screen apps).
        return;
    }
    if (UISettings::values.single_window_mode.GetValue()) {
        QApplication::setActiveWindow(this);
    } else {
        QApplication::setActiveWindow(render_window);
    }
}

void GMainWindow::ChangeScreenLayout() {
    Settings::LayoutOption new_layout = Settings::LayoutOption::Default;
    if (ui->action_Screen_Layout_Default->isChecked()) {
        new_layout = Settings::LayoutOption::Default;
    } else if (ui->action_Screen_Layout_Single_Screen->isChecked()) {
        new_layout = Settings::LayoutOption::SingleScreen;
    } else if (ui->action_Screen_Layout_Large_Screen->isChecked()) {
        new_layout = Settings::LayoutOption::LargeScreen;
        ui->menu_Small_Screen_Position->setEnabled(true);
    } else if (ui->action_Screen_Layout_Hybrid_Screen->isChecked()) {
        new_layout = Settings::LayoutOption::HybridScreen;
    } else if (ui->action_Screen_Layout_Side_by_Side->isChecked()) {
        new_layout = Settings::LayoutOption::SideScreen;
    } else if (ui->action_Screen_Layout_Separate_Windows->isChecked()) {
        new_layout = Settings::LayoutOption::SeparateWindows;
    } else if (ui->action_Screen_Layout_Custom_Layout->isChecked()) {
        new_layout = Settings::LayoutOption::CustomLayout;
    }

    Settings::values.layout_option = new_layout;
    SyncMenuUISettings();
    system.ApplySettings();
    UpdateSecondaryWindowVisibility();
}

void GMainWindow::ChangeSmallScreenPosition() {
    Settings::SmallScreenPosition new_position = Settings::SmallScreenPosition::BottomRight;

    if (ui->action_Small_Screen_TopRight->isChecked()) {
        new_position = Settings::SmallScreenPosition::TopRight;
    } else if (ui->action_Small_Screen_MiddleRight->isChecked()) {
        new_position = Settings::SmallScreenPosition::MiddleRight;
    } else if (ui->action_Small_Screen_BottomRight->isChecked()) {
        new_position = Settings::SmallScreenPosition::BottomRight;
    } else if (ui->action_Small_Screen_TopLeft->isChecked()) {
        new_position = Settings::SmallScreenPosition::TopLeft;
    } else if (ui->action_Small_Screen_MiddleLeft->isChecked()) {
        new_position = Settings::SmallScreenPosition::MiddleLeft;
    } else if (ui->action_Small_Screen_BottomLeft->isChecked()) {
        new_position = Settings::SmallScreenPosition::BottomLeft;
    } else if (ui->action_Small_Screen_Above->isChecked()) {
        new_position = Settings::SmallScreenPosition::AboveLarge;
    } else if (ui->action_Small_Screen_Below->isChecked()) {
        new_position = Settings::SmallScreenPosition::BelowLarge;
    }

    Settings::values.small_screen_position = new_position;
    SyncMenuUISettings();
    system.ApplySettings();
    UpdateSecondaryWindowVisibility();
}

bool GMainWindow::IsTurboEnabled() {
    return turbo_mode_active;
}

void GMainWindow::SetTurboEnabled(bool state) {
    turbo_mode_active = state;
    GMainWindow::ReloadTurbo();
}

void GMainWindow::ReloadTurbo() {
    if (IsTurboEnabled()) {
        Settings::temporary_frame_limit = Settings::values.turbo_limit.GetValue();
        Settings::is_temporary_frame_limit = true;
    } else {
        Settings::is_temporary_frame_limit = false;
    }

    UpdateStatusBar();
}

// TODO: This should probably take in something more descriptive than a bool. -OS
void GMainWindow::AdjustSpeedLimit(bool increase) {
    const int SPEED_LIMIT_STEP = 5;
    auto active_limit =
        IsTurboEnabled() ? &Settings::values.turbo_limit : &Settings::values.frame_limit;
    const auto active_limit_value = active_limit->GetValue();

    if (increase) {
        if (active_limit_value < 995) {
            active_limit->SetValue(active_limit_value + SPEED_LIMIT_STEP);
        }
    } else {
        if (active_limit_value > SPEED_LIMIT_STEP) {
            active_limit->SetValue(active_limit_value - SPEED_LIMIT_STEP);
        }
    }

    if (IsTurboEnabled()) {
        ReloadTurbo();
    }

    UpdateStatusBar();
}

void GMainWindow::ToggleScreenLayout() {
    const Settings::LayoutOption new_layout = []() {
        const Settings::LayoutOption current_layout = Settings::values.layout_option.GetValue();
        std::vector<Settings::LayoutOption> layouts_to_cycle =
            Settings::values.layouts_to_cycle.GetValue();
        const auto current_pos =
            distance(layouts_to_cycle.begin(),
                     std::find(layouts_to_cycle.begin(), layouts_to_cycle.end(), current_layout));
        // if the layouts_to_cycle setting has somehow been
        // cleared out, add just default back in
        if (layouts_to_cycle.size() == 0) {
            layouts_to_cycle.push_back(Settings::LayoutOption::Default);
        }
        if (current_pos >= layouts_to_cycle.size() - 1) {
            // either this layout wasn't found or it was last so move to the beginning
            return layouts_to_cycle[0];
        } else {
            return layouts_to_cycle[current_pos + 1];
        }
    }();

    Settings::values.layout_option = new_layout;
    SyncMenuUISettings();
    system.ApplySettings();
    UpdateSecondaryWindowVisibility();
}

void GMainWindow::OnSwapScreens() {
    Settings::values.swap_screen = ui->action_Screen_Layout_Swap_Screens->isChecked();
    system.ApplySettings();
}

void GMainWindow::OnRotateScreens() {
    Settings::values.upright_screen = ui->action_Screen_Layout_Upright_Screens->isChecked();
    system.ApplySettings();
}

void GMainWindow::TriggerSwapScreens() {
    ui->action_Screen_Layout_Swap_Screens->trigger();
}

void GMainWindow::TriggerRotateScreens() {
    ui->action_Screen_Layout_Upright_Screens->trigger();
}

void GMainWindow::OnSaveState() {
    if (!system.IsPoweredOn()) {
        return;
    }

    QAction* action = qobject_cast<QAction*>(sender());
    ASSERT(action);

    system.SendSignal(Core::System::Signal::Save, action->data().toUInt());
    system.frame_limiter.AdvanceFrame();
    newest_slot = action->data().toUInt();
}

void GMainWindow::OnLoadState() {
    if (!system.IsPoweredOn()) {
        return;
    }

    QAction* action = qobject_cast<QAction*>(sender());
    ASSERT(action);

    if (UISettings::values.save_state_warning) {
        QMessageBox::warning(
            this, tr("Savestates"),
            tr("Warning: Savestates are NOT a replacement for in-application saves, "
               "and are not meant to be reliable.\n\nUse at your own risk!"));
        UISettings::values.save_state_warning = false;
        config->Save();
    }

    system.SendSignal(Core::System::Signal::Load, action->data().toUInt());
    system.frame_limiter.AdvanceFrame();
}

void GMainWindow::OnConfigure() {
    game_list->SetDirectoryWatcherEnabled(false);
    Settings::SetConfiguringGlobal(true);
    ConfigureDialog configureDialog(this, hotkey_registry, system, gl_renderer, physical_devices,
                                    !multiplayer_state->IsHostingPublicRoom());
    connect(&configureDialog, &ConfigureDialog::LanguageChanged, this,
            &GMainWindow::OnLanguageChanged);
    if (configure_open_zeste) {
        configure_open_zeste = false;
        configureDialog.ShowZestePage();
    }
    auto old_theme = UISettings::values.theme;
    const int old_input_profile_index = Settings::values.current_input_profile_index;
    const auto old_input_profiles = Settings::values.input_profiles;
    const auto old_touch_from_button_maps = Settings::values.touch_from_button_maps;
#ifdef ENABLE_DISCORD_RPC
    const bool old_discord_presence = UISettings::values.enable_discord_presence.GetValue();
#endif
#ifdef __unix__
    const bool old_gamemode = Settings::values.enable_gamemode.GetValue();
#endif
    auto result = configureDialog.exec();
    game_list->SetDirectoryWatcherEnabled(true);
    if (result == QDialog::Accepted) {
        configureDialog.ApplyConfiguration();
        InitializeHotkeys();
        if (UISettings::values.theme != old_theme) {
            UpdateUITheme();
        }
#ifdef ENABLE_DISCORD_RPC
        if (UISettings::values.enable_discord_presence.GetValue() != old_discord_presence) {
            SetDiscordEnabled(UISettings::values.enable_discord_presence.GetValue());
            discord_rpc->Update(system.IsPoweredOn());
        }
#endif
#ifdef __unix__
        if (Settings::values.enable_gamemode.GetValue() != old_gamemode) {
            SetGamemodeEnabled(Settings::values.enable_gamemode.GetValue());
        }
#endif
        if (!multiplayer_state->IsHostingPublicRoom())
            multiplayer_state->UpdateCredentials();
        emit UpdateThemedIcons();
        SyncMenuUISettings();
        game_list->RefreshGameDirectory();
        config->Save();
        if (UISettings::values.hide_mouse && emulation_running) {
            setMouseTracking(true);
            mouse_hide_timer.start();
        } else {
            setMouseTracking(false);
        }
        ReloadTurbo();
        UpdateSecondaryWindowVisibility();
        UpdateBootHomeMenuState();
        UpdateStatusButtons();
    } else {
        Settings::values.input_profiles = old_input_profiles;
        Settings::values.touch_from_button_maps = old_touch_from_button_maps;
        Settings::LoadProfile(old_input_profile_index);
    }
    ZesteUpdateStrictUI();
}

void GMainWindow::OnLoadAmiibo() {
    if (!emu_thread || !emu_thread->IsRunning()) [[unlikely]] {
        return;
    }

    Service::SM::ServiceManager& sm = system.ServiceManager();
    auto nfc = sm.GetService<Service::NFC::Module::Interface>("nfc:u");
    if (nfc == nullptr) {
        return;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (nfc->IsTagActive()) {
        QMessageBox::warning(this, tr("Error opening amiibo data file"),
                             tr("A tag is already in use."));
        return;
    }

    if (!nfc->IsSearchingForAmiibos()) {
        QMessageBox::warning(this, tr("Error opening amiibo data file"),
                             tr("Application is not looking for amiibos."));
        return;
    }

    const QString extensions{QStringLiteral("*.bin")};
    const QString file_filter = tr("Amiibo File") + QStringLiteral(" (%1);;").arg(extensions) +
                                tr("All Files") + QStringLiteral(" (*.*)");
    const QString filename = QFileDialog::getOpenFileName(this, tr("Load Amiibo"), {}, file_filter);

    if (filename.isEmpty()) {
        return;
    }

    LoadAmiibo(filename);
}

void GMainWindow::LoadAmiibo(const QString& filename) {
    Service::SM::ServiceManager& sm = system.ServiceManager();
    auto nfc = sm.GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) [[unlikely]] {
        return;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (!nfc->LoadAmiibo(filename.toStdString())) {
        QMessageBox::warning(this, tr("Error opening amiibo data file"),
                             tr("Unable to open amiibo file \"%1\" for reading.").arg(filename));
        return;
    }

    ui->action_Remove_Amiibo->setEnabled(true);
}

void GMainWindow::OnRemoveAmiibo() {
    Service::SM::ServiceManager& sm = system.ServiceManager();
    auto nfc = sm.GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) [[unlikely]] {
        return;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    nfc->RemoveAmiibo();
    ui->action_Remove_Amiibo->setEnabled(false);
}

void GMainWindow::OnOpenCitraFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::UserDir))));
}

void GMainWindow::OnOpenNANDFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::NANDDir))));
}

void GMainWindow::OnOpenSDMCFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir))));
}

void GMainWindow::OnToggleFilterBar() {
    game_list->SetFilterVisible(ui->action_Show_Filter_Bar->isChecked());
    if (ui->action_Show_Filter_Bar->isChecked()) {
        game_list->SetFilterFocus();
    } else {
        game_list->ClearFilter();
    }
}

void GMainWindow::OnCreateGraphicsSurfaceViewer() {
    auto graphicsSurfaceViewerWidget =
        new GraphicsSurfaceWidget(system, Pica::g_debug_context, this);
    addDockWidget(Qt::RightDockWidgetArea, graphicsSurfaceViewerWidget);
    // TODO: Maybe graphicsSurfaceViewerWidget->setFloating(true);
    graphicsSurfaceViewerWidget->show();
}

void GMainWindow::OnRecordMovie() {
    MovieRecordDialog dialog(this, system);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    movie_record_on_start = true;
    movie_record_path = dialog.GetPath();
    movie_record_author = dialog.GetAuthor();

    if (emulation_running) { // Restart game
        BootGame(QString(game_path));
    }
    ui->action_Close_Movie->setEnabled(true);
    ui->action_Save_Movie->setEnabled(true);
}

void GMainWindow::OnPlayMovie() {
    MoviePlayDialog dialog(this, game_list, system);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    movie_playback_on_start = true;
    movie_playback_path = dialog.GetMoviePath();
    BootGame(dialog.GetGamePath());

    ui->action_Close_Movie->setEnabled(true);
    ui->action_Save_Movie->setEnabled(false);
}

void GMainWindow::OnCloseMovie() {
    if (movie_record_on_start) {
        QMessageBox::information(this, tr("Record Movie"), tr("Movie recording cancelled."));
        movie_record_on_start = false;
        movie_record_path.clear();
        movie_record_author.clear();
    } else {
        const bool was_running = emu_thread && emu_thread->IsRunning();
        if (was_running) {
            OnPauseGame();
        }

        const bool was_recording = movie.GetPlayMode() == Core::Movie::PlayMode::Recording;
        movie.Shutdown();
        if (was_recording) {
            QMessageBox::information(this, tr("Movie Saved"),
                                     tr("The movie is successfully saved."));
        }

        if (was_running) {
            OnResumeGame(false);
        }
    }

    ui->action_Close_Movie->setEnabled(false);
    ui->action_Save_Movie->setEnabled(false);
}

void GMainWindow::OnSaveMovie() {
    const bool was_running = emu_thread && emu_thread->IsRunning();
    if (was_running) {
        OnPauseGame();
    }

    if (movie.GetPlayMode() == Core::Movie::PlayMode::Recording) {
        movie.SaveMovie();
        QMessageBox::information(this, tr("Movie Saved"), tr("The movie is successfully saved."));
    } else {
        LOG_ERROR(Frontend, "Tried to save movie while movie is not being recorded");
    }

    if (was_running) {
        OnResumeGame(false);
    }
}

void GMainWindow::OnCaptureScreenshot() {
    if (!emu_thread) [[unlikely]] {
        return;
    }

    const bool was_running = emu_thread->IsRunning();

    if (was_running || (QMessageBox::question(this, tr("Application will unpause"),
                                              tr("The application will be unpaused, and the next "
                                                 "frame will be captured. Is this okay?"),
                                              QMessageBox::Yes | QMessageBox::No,
                                              QMessageBox::No) == QMessageBox::Yes)) {
        if (was_running) {
            OnPauseGame();
        }
        std::string path = UISettings::values.screenshot_path.GetValue();
        if (!FileUtil::IsDirectory(path)) {
            if (!FileUtil::CreateFullPath(path)) {
                QMessageBox::information(
                    this, tr("Invalid Screenshot Directory"),
                    tr("Cannot create specified screenshot directory. Screenshot "
                       "path is set back to its default value."));
                path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
                path.append("screenshots/");
                UISettings::values.screenshot_path = path;
            };
        }

        static QRegularExpression expr(QStringLiteral("[\\/:?\"<>|]"));
        const std::string filename = game_title.remove(expr).toStdString();
        const std::string timestamp = QDateTime::currentDateTime()
                                          .toString(QStringLiteral("dd.MM.yy_hh.mm.ss.z"))
                                          .toStdString();
        path.append(fmt::format("/{}_{}.png", filename, timestamp));
        auto* const screenshot_window =
            secondary_window->HasFocus() ? secondary_window : render_window;
        screenshot_window->CaptureScreenshot(
            UISettings::values.screenshot_resolution_factor.GetValue(),
            QString::fromStdString(path));
        OnResumeGame(false);
    }
}

#ifdef ENABLE_FFMPEG
void GMainWindow::OnDumpVideo() {
    if (ui->action_Dump_Video->isChecked()) {
        OnStartVideoDumping();
    } else {
        OnStopVideoDumping();
    }
}
#endif

#ifdef ENABLE_FFMPEG
void GMainWindow::OnStartVideoDumping() {
    DumpingDialog dialog(this, system);
    if (dialog.exec() != QDialog::DialogCode::Accepted) {
        ui->action_Dump_Video->setChecked(false);
        return;
    }
    const auto path = dialog.GetFilePath();
    if (emulation_running) {
        StartVideoDumping(path);
    } else {
        video_dumping_on_start = true;
        video_dumping_path = path;
    }
}

void GMainWindow::StartVideoDumping(const QString& path) {
    auto& renderer = system.GPU().Renderer();
    const auto layout{Layout::FrameLayoutFromResolutionScale(renderer.GetResolutionScaleFactor())};

    auto dumper = std::make_shared<VideoDumper::FFmpegBackend>(renderer);
    if (dumper->StartDumping(path.toStdString(), layout)) {
        system.RegisterVideoDumper(dumper);
    } else {
        QMessageBox::critical(
            this, QStringLiteral("Zeste"),
            tr("Could not start video dumping.<br>Please ensure that the video encoder is "
               "configured correctly.<br>Refer to the log for details."));
        ui->action_Dump_Video->setChecked(false);
    }
}

void GMainWindow::OnStopVideoDumping() {
    ui->action_Dump_Video->setChecked(false);

    if (video_dumping_on_start) {
        video_dumping_on_start = false;
        video_dumping_path.clear();
    } else {
        auto dumper = system.GetVideoDumper();
        if (!dumper || !dumper->IsDumping()) {
            return;
        }

        game_paused_for_dumping = emu_thread->IsRunning();
        OnPauseGame();

        auto future = QtConcurrent::run([dumper] { dumper->StopDumping(); });
        auto* future_watcher = new QFutureWatcher<void>(this);
        connect(future_watcher, &QFutureWatcher<void>::finished, this, [this] {
            if (game_shutdown_delayed) {
                game_shutdown_delayed = false;
                ShutdownGame();
            } else if (game_paused_for_dumping) {
                game_paused_for_dumping = false;
                OnResumeGame(false);
            }
        });
        future_watcher->setFuture(future);
    }
}
#endif

void GMainWindow::UpdateStatusBar() {
    if (!emu_thread) [[unlikely]] {
        status_bar_update_timer.stop();
        return;
    }

    // Update movie status
    const u64 current = movie.GetCurrentInputIndex();
    const u64 total = movie.GetTotalInputCount();
    const auto play_mode = movie.GetPlayMode();
    if (play_mode == Core::Movie::PlayMode::Recording) {
        message_label->setText(tr("Recording %1").arg(current));
        message_label_used_for_movie = true;
        ui->action_Save_Movie->setEnabled(true);
    } else if (play_mode == Core::Movie::PlayMode::Playing) {
        message_label->setText(tr("Playing %1 / %2").arg(current).arg(total));
        message_label_used_for_movie = true;
        ui->action_Save_Movie->setEnabled(false);
    } else if (play_mode == Core::Movie::PlayMode::MovieFinished) {
        message_label->setText(tr("Movie Finished"));
        message_label_used_for_movie = true;
        ui->action_Save_Movie->setEnabled(false);
    } else if (message_label_used_for_movie) { // Clear the label if movie was just closed
        message_label->setText(QString{});
        message_label_used_for_movie = false;
        ui->action_Save_Movie->setEnabled(false);
    }

    auto results = system.GetAndResetPerfStats();

    if (show_artic_label) {
        const bool do_mb = results.artic_transmitted >= (1000.0 * 1000.0);
        const double value = do_mb ? (results.artic_transmitted / (1000.0 * 1000.0))
                                   : (results.artic_transmitted / 1000.0);
        static const std::array<std::pair<Core::PerfStats::PerfArticEventBits, QString>, 5>
            perf_events = {
                std::make_pair(Core::PerfStats::PerfArticEventBits::ARTIC_SHARED_EXT_DATA,
                               tr("(Accessing SharedExtData)")),
                std::make_pair(Core::PerfStats::PerfArticEventBits::ARTIC_SYSTEM_SAVE_DATA,
                               tr("(Accessing SystemSaveData)")),
                std::make_pair(Core::PerfStats::PerfArticEventBits::ARTIC_BOSS_EXT_DATA,
                               tr("(Accessing BossExtData)")),
                std::make_pair(Core::PerfStats::PerfArticEventBits::ARTIC_EXT_DATA,
                               tr("(Accessing ExtData)")),
                std::make_pair(Core::PerfStats::PerfArticEventBits::ARTIC_SAVE_DATA,
                               tr("(Accessing SaveData)")),
            };

        const QString unit = do_mb ? QStringLiteral("MB/s") : QStringLiteral("KB/s");
        QString event{};
        for (auto p : perf_events) {
            if (results.artic_events.Get(p.first)) {
                event = QString::fromStdString(" ") + p.second;
                break;
            }
        }

        static const std::array label_color = {QStringLiteral(""), QStringLiteral("#eed202"),
                                               QStringLiteral("#ff3333")};

        int style_index;

        if (value > 200.0) {
            style_index = 2;
        } else if (value > 125.0) {
            style_index = 1;
        } else {
            style_index = 0;
        }

        QString style_sheet;
        if (!label_color[style_index].isEmpty()) {
            style_sheet = QStringLiteral("QLabel { color: %0; }").arg(label_color[style_index]);
        }

        artic_traffic_label->setText(
            tr("Artic Traffic: %1 %2%3").arg(value, 0, 'f', 0).arg(unit).arg(event));
        artic_traffic_label->setStyleSheet(style_sheet);
    }

    if (Settings::GetFrameLimit() == 0) {
        emu_speed_label->setText(tr("Speed: %1%").arg(results.emulation_speed * 100.0, 0, 'f', 0));
    } else {
        emu_speed_label->setText(tr("Speed: %1% / %2%")
                                     .arg(results.emulation_speed * 100.0, 0, 'f', 0)
                                     .arg(Settings::GetFrameLimit()));
    }
    game_fps_label->setText(tr("App: %1 FPS").arg(results.game_fps, 0, 'f', 0));
    if (UISettings::values.show_advanced_frametime_info) {
        emu_frametime_label->setText(
            tr("Frame: %1 ms (GPU: [CMD: %2 ms, SWP: %3 ms], IPC: %4 ms, SVC: %5 ms, Rem: %6 ms)")
                .arg(results.time_vblank_interval * 1000.0, 2, 'f', 2)
                .arg(results.time_gpu * 1000.0, 2, 'f', 2)
                .arg(results.time_swap * 1000.0, 2, 'f', 2)
                .arg(results.time_hle_ipc * 1000.0, 2, 'f', 2)
                .arg(results.time_hle_svc * 1000.0, 2, 'f', 2)
                .arg(results.time_remaining * 1000.0, 2, 'f', 2));
    } else {
        emu_frametime_label->setText(
            tr("Frame: %1 ms").arg(results.time_vblank_interval * 1000.0, 2, 'f', 2));
    }

    if (show_artic_label) {
        artic_traffic_label->setVisible(true);
    }
    emu_speed_label->setVisible(true);
    game_fps_label->setVisible(true);
    emu_frametime_label->setVisible(true);
}

void GMainWindow::UpdateBootHomeMenuState() {
    const auto current_region = Settings::values.region_value.GetValue();
    for (u32 region = 0; region < Core::NUM_SYSTEM_TITLE_REGIONS; region++) {
        const auto path = Core::GetHomeMenuNcchPath(region);
        ui->menu_Boot_Home_Menu->actions().at(region)->setEnabled(
            (current_region == Settings::REGION_VALUE_AUTO_SELECT ||
             current_region == static_cast<int>(region)) &&
            !path.empty() && FileUtil::Exists(path));
    }
}

void GMainWindow::HideMouseCursor() {
    if (!emu_thread || !UISettings::values.hide_mouse.GetValue()) {
        mouse_hide_timer.stop();
        ShowMouseCursor();
        return;
    }
    render_window->setCursor(QCursor(Qt::BlankCursor));
    secondary_window->setCursor(QCursor(Qt::BlankCursor));
    if (UISettings::values.single_window_mode.GetValue()) {
        setCursor(QCursor(Qt::BlankCursor));
    }
}

void GMainWindow::ShowMouseCursor() {
    unsetCursor();
    render_window->unsetCursor();
    secondary_window->unsetCursor();
    if (emu_thread && UISettings::values.hide_mouse) {
        mouse_hide_timer.start();
    }
}

void GMainWindow::OnMute() {
    Settings::values.audio_muted = !Settings::values.audio_muted;
    UpdateVolumeUI();
}

void GMainWindow::OnDecreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume =
        static_cast<s32>(Settings::values.volume.GetValue() * volume_slider->maximum());
    int step = 5;
    if (current_volume <= 30) {
        step = 2;
    }
    if (current_volume <= 6) {
        step = 1;
    }
    const auto value =
        static_cast<float>(std::max(current_volume - step, 0)) / volume_slider->maximum();
    Settings::values.volume.SetValue(value);
    UpdateVolumeUI();
}

void GMainWindow::OnIncreaseVolume() {
    Settings::values.audio_muted = false;
    const auto current_volume =
        static_cast<s32>(Settings::values.volume.GetValue() * volume_slider->maximum());
    int step = 5;
    if (current_volume < 30) {
        step = 2;
    }
    if (current_volume < 6) {
        step = 1;
    }
    const auto value = static_cast<float>(current_volume + step) / volume_slider->maximum();
    Settings::values.volume.SetValue(value);
    UpdateVolumeUI();
}

void GMainWindow::UpdateVolumeUI() {
    const auto volume_value =
        static_cast<int>(Settings::values.volume.GetValue() * volume_slider->maximum());
    volume_slider->setValue(volume_value);
    if (Settings::values.audio_muted) {
        volume_button->setChecked(false);
        volume_button->setText(tr("VOLUME: MUTE"));
    } else {
        volume_button->setChecked(true);
        volume_button->setText(tr("VOLUME: %1%", "Volume percentage (e.g. 50%)").arg(volume_value));
    }
}

void GMainWindow::UpdateAPIIndicator(bool update) {
    static std::array graphics_apis = {QStringLiteral("SOFTWARE"), QStringLiteral("OPENGL"),
                                       QStringLiteral("VULKAN")};

    static std::array graphics_api_colors = {QStringLiteral("#3ae400"), QStringLiteral("#00ccdd"),
                                             QStringLiteral("#91242a")};

    u32 api_index = static_cast<u32>(Settings::values.graphics_api.GetValue());
    if (update) {
        api_index = (api_index + 1) % graphics_apis.size();
        // Skip past any disabled renderers.
#ifndef ENABLE_SOFTWARE_RENDERER
        if (api_index == static_cast<u32>(Settings::GraphicsAPI::Software)) {
            api_index = (api_index + 1) % graphics_apis.size();
        }
#endif
#ifndef ENABLE_OPENGL
        if (api_index == static_cast<u32>(Settings::GraphicsAPI::OpenGL)) {
            api_index = (api_index + 1) % graphics_apis.size();
        }
#endif
#ifndef ENABLE_VULKAN
        if (api_index == static_cast<u32>(Settings::GraphicsAPI::Vulkan)) {
            api_index = (api_index + 1) % graphics_apis.size();
        }
#else
        if (physical_devices.empty()) {
            if (api_index == static_cast<u32>(Settings::GraphicsAPI::Vulkan)) {
                api_index = (api_index + 1) % graphics_apis.size();
            }
        }
#endif
        Settings::values.graphics_api = static_cast<Settings::GraphicsAPI>(api_index);
    }

    const QString style_sheet = QStringLiteral("QPushButton { font-weight: bold; color: %0; }")
                                    .arg(graphics_api_colors[api_index]);

    graphics_api_button->setText(graphics_apis[api_index]);
    graphics_api_button->setStyleSheet(style_sheet);
}

void GMainWindow::UpdateStatusButtons() {
    UpdateAPIIndicator();
    UpdateVolumeUI();
}

void GMainWindow::OnMouseActivity() {
    ShowMouseCursor();
}

void GMainWindow::mouseMoveEvent([[maybe_unused]] QMouseEvent* event) {
    OnMouseActivity();
}

void GMainWindow::mousePressEvent([[maybe_unused]] QMouseEvent* event) {
    OnMouseActivity();
}

void GMainWindow::mouseReleaseEvent([[maybe_unused]] QMouseEvent* event) {
    OnMouseActivity();
}

void GMainWindow::showEvent([[maybe_unused]] QShowEvent* event) {
    game_list->LoadCompatibilityList();
    game_list->PopulateAsync(UISettings::values.game_dirs);
}

bool GMainWindow::ShowExceptionDialog(Core::System::ResultStatus result,
                                      const std::string& details) {
    QDialog dialog(this);
    dialog.setWindowTitle(result == Core::System::ResultStatus::ErrorCoreExceptionRaised
                              ? tr("An exception occurred")
                              : tr("An invalid memory access occurred"));

    dialog.setMinimumSize(600, 500);

    auto* layout = new QVBoxLayout(&dialog);

    auto* label = new QLabel(
        result == Core::System::ResultStatus::ErrorCoreExceptionRaised
            ? tr("An exception occurred while executing the emulated application.")
            : tr("An invalid memory access occurred while executing the emulated application."));

    layout->addWidget(label);

    auto* textEdit = new QPlainTextEdit();
    textEdit->setPlainText(QString::fromStdString(details));
    textEdit->setReadOnly(true);
    QFont monoFont(QStringLiteral("Monospace"));
    monoFont.setStyleHint(QFont::TypeWriter);
    monoFont.setPointSize(10);
    textEdit->setFont(monoFont);
    textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(textEdit);

    auto* buttonLayout = new QHBoxLayout();

    auto* ignoreButton = new QPushButton(tr("Ignore for this Session"));

    QObject::connect(ignoreButton, &QPushButton::clicked,
                     []() { Core::SetIgnoreExceptionsForSession(true); });

    buttonLayout->addWidget(ignoreButton);
    buttonLayout->addStretch();

    auto* continueButton = new QPushButton(tr("Continue"));
    QObject::connect(continueButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttonLayout->addWidget(continueButton);

    auto* stopButton = new QPushButton(tr("Stop Emulation"));
    QObject::connect(stopButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttonLayout->addWidget(stopButton);

    layout->addLayout(buttonLayout);

    return dialog.exec() == QDialog::Accepted;
}

void GMainWindow::OnCoreError(Core::System::ResultStatus result, std::string details) {
    QString status_message;

    // Zeste: "break on first report" pauses emulation without a dialog.
    if (result == Core::System::ResultStatus::ZesteBreak) {
        if (emu_thread) {
            OnPauseGame();
            message_label->setText(tr("Zeste: paused on the first hardware-accuracy report"));
            message_label->setVisible(true);
            message_label_used_for_movie = false;
            statusBar()->showMessage(QString::fromStdString(details));
        }
        return;
    }

    // Zeste: a save state that cannot be saved or loaded is reported and emulation goes on.
    if (Settings::values.zeste_no_modal_dialogs.GetValue() &&
        (result == Core::System::ResultStatus::ErrorSavestate ||
         result == Core::System::ResultStatus::ErrorSavestateBuildMismatch)) {
        LOG_ERROR(Frontend, "Zeste: save state failed: {}", details);
        std::cout << "Zeste save state failed: " << details << std::endl;
        QTimer::singleShot(100, this, [this] {
            if (emu_thread) {
                emu_thread->SetRunning(true);
            }
        });
        return;
    }

    // Zeste: automated runs never wait on a modal dialog. Errors are logged; emulation stops on
    // anything that would otherwise ask the user.
    if (Settings::values.zeste_no_modal_dialogs.GetValue() &&
        result != Core::System::ResultStatus::ShutdownRequested) {
        LOG_CRITICAL(Frontend, "Core error {} (no-dialogs mode, stopping emulation): {}",
                     static_cast<u32>(result), details);
        headless_exit_code = 2; // Only used when the process exits with emulation.
        Zeste::Automation::SetExitReason(
            fmt::format("emulation error {}: {}", static_cast<u32>(result), details));
        // The emulation thread follows ErrorThrown with a blocking DebugModeEntered signal: let
        // the event loop deliver it before joining the thread in ShutdownGame (no dialog loop
        // does it for us here).
        QTimer::singleShot(250, this, [this] {
            if (emu_thread) {
                ShutdownGame();
            }
        });
        return;
    }

    // Handle exception dialogs separately
    if (result == Core::System::ResultStatus::ErrorCoreExceptionRaised) {
        if (ShowExceptionDialog(result, details)) {
            if (emu_thread) {
                ShutdownGame();
                return;
            }
        }

        if (emu_thread) {
            emu_thread->SetRunning(true);
            message_label->setText(status_message);
            message_label_used_for_movie = false;
        }
        return;
    }

    QString title, message;
    QMessageBox::Icon error_severity_icon;
    bool can_continue = true;
    if (result == Core::System::ResultStatus::ErrorSystemFiles) {
        const QString common_message =
            tr("%1 is missing. Please <a "
               "href='https://github.com/azahar-emu/azahar/wiki/Dumping-System-Files'>dump your "
               "system archives</a>.<br/>Continuing emulation may result in crashes and bugs.");

        if (!details.empty()) {
            message = common_message.arg(QString::fromStdString(details));
        } else {
            message = common_message.arg(tr("A system archive"));
        }

        title = tr("System Archive Not Found");
        status_message = tr("System Archive Missing");
        error_severity_icon = QMessageBox::Icon::Critical;
    } else if (result == Core::System::ResultStatus::ErrorSavestate) {
        title = tr("Save/load Error");
        message = QString::fromStdString(details);
        error_severity_icon = QMessageBox::Icon::Warning;
    } else if (result == Core::System::ResultStatus::ErrorArticDisconnected) {
        title = tr("Artic Server");
        message =
            tr(fmt::format("A communication error has occurred. The game will quit.\n{}", details)
                   .c_str());
        error_severity_icon = QMessageBox::Icon::Critical;
        can_continue = false;
    } else if (result == Core::System::ResultStatus::ErrorSavestateBuildMismatch) {
        title = tr("Savestate version mismatch");
        message = tr("Could not load the savestate: it was made with a different Zeste build "
                     "(<b>%1</b>). A savestate only loads in the build that made it.<br/><br/>To "
                     "keep your progress, open that build of Zeste, load the savestate there and "
                     "save in the game itself.")
                      .arg(QString::fromStdString(details));
        error_severity_icon = QMessageBox::Icon::Critical;
    } else {
        title = tr("Fatal Error");
        message = tr("A fatal error occurred. "
                     "Check the log for details."
                     "<br/>Continuing emulation may result in crashes and bugs.");
        status_message = tr("Fatal Error encountered");
        error_severity_icon = QMessageBox::Icon::Critical;
    }

    QMessageBox message_box;
    message_box.setWindowTitle(title);
    message_box.setText(message);
    message_box.setIcon(error_severity_icon);
    if (error_severity_icon == QMessageBox::Icon::Critical) {
        if (can_continue) {
            message_box.addButton(tr("Continue"), QMessageBox::RejectRole);
        }
        QPushButton* abort_button =
            message_box.addButton(tr("Quit Application"), QMessageBox::AcceptRole);
        if (result != Core::System::ResultStatus::ShutdownRequested)
            message_box.exec();

        if (!can_continue || result == Core::System::ResultStatus::ShutdownRequested ||
            message_box.clickedButton() == abort_button) {
            if (emu_thread) {
                ShutdownGame();
                return;
            }
        }
    } else {
        // This block should run when the error isn't too big of a deal
        // e.g. when a save state can't be saved or loaded
        message_box.addButton(tr("OK"), QMessageBox::RejectRole);
        message_box.exec();
    }

    // Only show the message if the game is still running.
    if (emu_thread) {
        emu_thread->SetRunning(true);
        message_label->setText(status_message);
        message_label_used_for_movie = false;
    }
}

void GMainWindow::OnMenuAboutCitra() {
    AboutDialog about{this};
    about.exec();
}

bool GMainWindow::ConfirmClose() {
    if (!emu_thread || !UISettings::values.confirm_before_closing) {
        return true;
    }

    QMessageBox::StandardButton answer =
        QMessageBox::question(this, QStringLiteral("Zeste"), tr("Would you like to exit now?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::closeEvent(QCloseEvent* event) {
    if (!ConfirmClose()) {
        event->ignore();
        return;
    }

    UpdateUISettings();
    game_list->SaveInterfaceLayout();
    hotkey_registry.SaveHotkeys();

    // Shutdown session if the emu thread is active...
    if (emu_thread) {
        ShutdownGame();
    }

    // Save settings in case they were changed from outside the configuration menu.
    config->Save();

    render_window->close();
    secondary_window->close();
    multiplayer_state->Close();
    InputCommon::Shutdown();
    QWidget::closeEvent(event);
}

static bool IsSingleFileDropEvent(const QMimeData* mime) {
    return mime->hasUrls() && mime->urls().length() == 1;
}

static const std::array<std::string, 13> AcceptedExtensions = {
    "cci",  "cxi",  "bin",   "3dsx", "app",  "elf", "axf",
    "zcci", "zcxi", "z3dsx", "3ds",  "bcci", "bcxi"};

static bool IsCorrectFileExtension(const QMimeData* mime) {
    const QString& filename = mime->urls().at(0).toLocalFile();
    return std::find(AcceptedExtensions.begin(), AcceptedExtensions.end(),
                     QFileInfo(filename).suffix().toStdString()) != AcceptedExtensions.end();
}

static bool IsAcceptableDropEvent(QDropEvent* event) {
    return IsSingleFileDropEvent(event->mimeData()) && IsCorrectFileExtension(event->mimeData());
}

void GMainWindow::AcceptDropEvent(QDropEvent* event) {
    if (IsAcceptableDropEvent(event)) {
        event->setDropAction(Qt::DropAction::LinkAction);
        event->accept();
    }
}

bool GMainWindow::DropAction(QDropEvent* event) {
    if (!IsAcceptableDropEvent(event)) {
        return false;
    }

    const QMimeData* mime_data = event->mimeData();
    const QString& filename = mime_data->urls().at(0).toLocalFile();

    if (emulation_running && QFileInfo(filename).suffix() == QStringLiteral("bin")) {
        // Amiibo
        LoadAmiibo(filename);
    } else {
        // Game
        if (ConfirmChangeGame()) {
            BootGame(filename);
        }
    }
    return true;
}

void GMainWindow::OnFileOpen(const QFileOpenEvent* event) {
    BootGame(event->file());
}

void GMainWindow::dropEvent(QDropEvent* event) {
    DropAction(event);
}

void GMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    AcceptDropEvent(event);
}

void GMainWindow::dragMoveEvent(QDragMoveEvent* event) {
    AcceptDropEvent(event);
}

bool GMainWindow::ConfirmChangeGame() {
    if (!emu_thread) [[unlikely]] {
        return true;
    }

    auto answer = QMessageBox::question(
        this, QStringLiteral("Zeste"),
        tr("The application is still running. Would you like to stop emulation?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer != QMessageBox::No;
}

void GMainWindow::filterBarSetChecked(bool state) {
    ui->action_Show_Filter_Bar->setChecked(state);
    emit(OnToggleFilterBar());
}

inline bool isDarkMode() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Use colorScheme for Qt 6.5 and later
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark;
#else
    // Fallback for Qt 6.4: Check the window palette
    QPalette palette = QGuiApplication::palette();
    return palette.color(QPalette::Window).lightness() < 128; // Rough check for dark mode
#endif
}

void GMainWindow::UpdateUITheme() {
    const QString icons_base_path = QStringLiteral(":/icons/");
    const QString default_system_theme = QStringLiteral("default");
    const QString light_icon_system_theme = QStringLiteral("default_with_light_icons");
    const QString colorful_icon_system_theme = QStringLiteral("colorful");

    QString default_theme;
    if (!isDarkMode()) {
        default_theme = default_system_theme;
    } else {
        default_theme = light_icon_system_theme;
    }

    const QString default_theme_path = icons_base_path + default_theme;

    // Zeste ships two themes: a config naming a removed one gets the system theme.
    if (std::none_of(UISettings::themes.begin(), UISettings::themes.end(), [](const auto& theme) {
            return UISettings::values.theme == QString::fromUtf8(theme.second);
        })) {
        UISettings::values.theme = default_system_theme;
    }
    const QString& current_theme = UISettings::values.theme;
    const bool is_default_theme = (current_theme == default_system_theme);
    QStringList theme_paths(default_theme_paths);

    if (is_default_theme || current_theme.isEmpty()) {
        const QString theme_uri(QStringLiteral(":default/style.qss"));
        QFile f(theme_uri);
        if (f.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&f);
            qApp->setStyleSheet(ts.readAll() + ZesteStatusBarStyle());
        } else {
            LOG_ERROR(Frontend,
                      "Unable to open default stylesheet, falling back to empty stylesheet");
            qApp->setStyleSheet(ZesteStatusBarStyle());
        }
        theme_paths.append(default_theme_path);
        QIcon::setThemeName(default_theme);
    } else {
        const QString theme_uri(QLatin1Char{':'} + current_theme + QStringLiteral("/style.qss"));
        QFile f(theme_uri);
        if (f.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&f);
            qApp->setStyleSheet(ts.readAll() + ZesteStatusBarStyle());
        } else {
            LOG_ERROR(Frontend, "Unable to set style, stylesheet file not found");
        }

        const QString current_theme_path = icons_base_path + current_theme;
        theme_paths.append({default_theme_path, current_theme_path});
        QIcon::setThemeName(current_theme);
    }

#ifdef _WIN32
    if (current_theme == default_system_theme || current_theme == light_icon_system_theme ||
        current_theme == colorful_icon_system_theme) {
        SetCustomWindowsScrollbar();
    }
#endif

    QIcon::setThemeSearchPaths(theme_paths);
}

void GMainWindow::LoadTranslation() {
    bool loaded = false;

    const QString lang_en = QStringLiteral("en");
    const QString languages_dir = QStringLiteral(":/languages/");

    // Workaround for incorrect Qt system language detection
    // TODO: Allow the "<System>" option to actually be selected rather than overriding the
    //       selected language option? Current behaviour is better than the issue it fixes,
    //       but not ideal.
    if (UISettings::values.language.isEmpty()) {
        QStringList languages;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        languages = QLocale::system().uiLanguages(QLocale::TagSeparator::Underscore);
#else
        languages = QLocale::system().uiLanguages();
        for (auto& lang : languages)
            lang.replace(u'-', u'_');
#endif
        for (const auto& lang : languages) {
            // If the first language found is English, no need to install any translation
            if (lang == lang_en) {
                UISettings::values.language = lang_en;
                return;
            }
            loaded = citraTranslator.load(lang, languages_dir);
            if (loaded) {
                UISettings::values.language = lang;
                break;
            }
        }
    }

    // If the selected language is English, no need to install any translation
    if (UISettings::values.language == lang_en) {
        return;
    }

    const QString qtbase_prefix = QStringLiteral("qtbase_");
    if (UISettings::values.language.isEmpty() && !loaded) {
        // Use the system's default locale
        qtTranslator.load(qtbase_prefix + QLocale::system().name(), {}, {},
                          QStringLiteral(":/languages/"));
        loaded = citraTranslator.load(QLocale::system(), {}, {}, QStringLiteral(":/languages/"));
    } else {
        // Otherwise load from the specified file
        qtTranslator.load(qtbase_prefix + UISettings::values.language,
                          QStringLiteral(":/languages/"));
        loaded = citraTranslator.load(UISettings::values.language, QStringLiteral(":/languages/"));
    }

    if (loaded) {
        qApp->installTranslator(&qtTranslator);
        qApp->installTranslator(&citraTranslator);
    } else {
        UISettings::values.language = lang_en;
    }
}

void GMainWindow::OnLanguageChanged(const QString& locale) {
    if (UISettings::values.language != QStringLiteral("en")) {
        qApp->removeTranslator(&qtTranslator);
        qApp->removeTranslator(&citraTranslator);
    }

    UISettings::values.language = locale;
    LoadTranslation();
    ui->retranslateUi(this);
    RetranslateStatusBar();
    UpdateWindowTitle();
}

void GMainWindow::OnConfigurePerGame() {
    u64 title_id{};
    system.GetAppLoader().ReadProgramId(title_id);
    OpenPerGameConfiguration(title_id, game_path);
}

void GMainWindow::OpenPerGameConfiguration(u64 title_id, const QString& file_name) {
    Settings::SetConfiguringGlobal(false);
    ConfigurePerGame dialog(this, title_id, file_name, gl_renderer, physical_devices, system);
    const auto result = dialog.exec();

    if (result != QDialog::Accepted) {
        Settings::RestoreGlobalState(system.IsPoweredOn());
        return;
    } else if (result == QDialog::Accepted) {
        dialog.ApplyConfiguration();
    }

    // Do not cause the global config to write local settings into the config file
    const bool is_powered_on = system.IsPoweredOn();
    Settings::RestoreGlobalState(system.IsPoweredOn());

    if (!is_powered_on) {
        config->Save();
    }

    UpdateStatusButtons();
}

void GMainWindow::OnMoviePlaybackCompleted() {
    OnPauseGame();
    QMessageBox::information(this, tr("Playback Completed"), tr("Movie playback completed."));
}


void GMainWindow::ZesteSetStrict(bool enabled) {
    Settings::SetZesteStrictPreset(enabled);
    if (!QtConfig::zeste_command_line_override) {
        config->Save();
    }
    ZesteUpdateStrictUI();
    statusBar()->showMessage(
        enabled ? (emulation_running ? tr("Zeste strict hardware mode on (some checks start at the "
                                          "next boot)")
                                     : tr("Zeste strict hardware mode on"))
                : tr("Zeste strict hardware mode off"),
        5000);
}

void GMainWindow::ZesteUpdateStrictUI() {
    const bool strict = Settings::IsZesteStrictPreset();
    if (zeste_strict_button) {
        zeste_strict_button->setChecked(strict);
        zeste_strict_button->setToolTip(
            strict ? tr("Strict hardware mode is on: every Zeste check is enabled. Click to turn "
                        "them all off.")
                   : Settings::IsAnyZesteOptionEnabled()
                         ? tr("Some Zeste checks are on. Click to enable strict hardware mode "
                              "(all of them).")
                         : tr("Zeste checks are off (Azahar behaviour). Click to enable strict "
                              "hardware mode."));
    }
    if (zeste_strict_action) {
        zeste_strict_action->setChecked(strict);
    }
}

void GMainWindow::ZesteOpenPlaytestReport() {
    if (headless_mode || !emulation_running || !emu_thread || zeste_report_open ||
        !Zeste::BugReport::IsEnabledFor(game_title_id)) {
        return;
    }
    zeste_report_open = true;
    const bool was_paused = system.frame_limiter.IsFrameAdvancing();

    // The screenshot is the next frame the game presents (the one on screen when the key was
    // pressed, give or take a frame); a timer opens the window without one if none comes.
    auto opened = std::make_shared<bool>(false);
    const auto open_window = [this, opened, was_paused](const QImage& screenshot) {
        if (*opened) {
            return;
        }
        *opened = true;
        if (!emulation_running) {
            zeste_report_open = false;
            return;
        }
        if (!was_paused) {
            OnPauseGame();
        }
        Zeste::BugReport::ReportDialog dialog(this, screenshot);
        if (dialog.exec() == QDialog::Accepted) {
            const QString saved = Zeste::BugReport::SaveReport(this, dialog, screenshot,
                                                               game_title, game_title_id);
            if (!saved.isEmpty()) {
                statusBar()->showMessage(tr("Report saved to %1").arg(saved), 8000);
            }
        }
        if (!was_paused && emulation_running) {
            OnResumeGame(false);
        }
        zeste_report_open = false;
    };
    render_window->CaptureScreenshotImage(open_window);
    QTimer::singleShot(2000, this, [open_window] { open_window(QImage{}); });
    if (was_paused) {
        system.frame_limiter.AdvanceFrame();
    }
}

void GMainWindow::ZesteCheckForUpdates(bool asked) {
    Zeste::Update::Check(this, asked, [this, asked](const Zeste::Update::Result& result) {
        const QString current = QString::fromUtf8(Common::g_zeste_version);
        if (headless_mode) {
            if (result.newer) {
                std::cout << "Zeste update available: " << result.latest.toStdString()
                          << " (this is " << current.toStdString()
                          << "): " << result.url.toStdString() << std::endl;
            }
            return;
        }
        if (result.newer) {
            auto* box = new QMessageBox(QMessageBox::Information, tr("Update Available"),
                                        tr("Zeste %1 is available (this is %2).\n"
                                           "Open the download page?")
                                            .arg(result.latest, current),
                                        QMessageBox::Yes | QMessageBox::No, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            connect(box, &QMessageBox::accepted, this, [url = result.url] {
                QDesktopServices::openUrl(QUrl(url));
            });
            box->open();
        } else if (asked) {
            QMessageBox::information(
                this, tr("Check for Updates"),
                result.error.isEmpty()
                    ? tr("Zeste %1 is the latest version.").arg(current)
                    : tr("Could not check for updates: %1").arg(result.error));
        }
    });
}

void GMainWindow::OnSwitchDiskResources(VideoCore::LoadCallbackStage stage, std::size_t value,
                                        std::size_t total, const std::string& object) {
    if (stage == VideoCore::LoadCallbackStage::Prepare) {
        loading_shaders_label->setText(QString());
        loading_shaders_label->setVisible(true);
    } else if (stage == VideoCore::LoadCallbackStage::Complete) {
        loading_shaders_label->setVisible(false);
    } else {
        loading_shaders_label->setText(
            loading_screen->GetStageTranslation(stage, value, total, object));
    }
}

void GMainWindow::UpdateWindowTitle() {
    QString full_name = QString::fromUtf8(Common::g_zeste_display_version);
    if (!game_title.isEmpty() && !Settings::values.is_new_3ds.GetValue()) {
        full_name += QStringLiteral(" (Old 3DS, experimental)");
    }

    if (game_title.isEmpty()) {
        setWindowTitle(QStringLiteral("Zeste %1").arg(full_name));
    } else {
        setWindowTitle(QStringLiteral("Zeste %1 | %2").arg(full_name, game_title));
        render_window->setWindowTitle(
            QStringLiteral("Zeste %1 | %2 | %3").arg(full_name, game_title, tr("Primary Window")));
        secondary_window->setWindowTitle(QStringLiteral("Zeste %1 | %2 | %3")
                                             .arg(full_name, game_title, tr("Secondary Window")));
    }
}

void GMainWindow::UpdateUISettings() {
    if (!ui->action_Fullscreen->isChecked()) {
        UISettings::values.geometry = saveGeometry();
        UISettings::values.renderwindow_geometry = render_window->saveGeometry();
    }
    if (!secondary_window->isFullScreen()) {
        UISettings::values.secondarywindow_geometry = secondary_window->saveGeometry();
    }
    UISettings::values.state = saveState();
#if MICROPROFILE_ENABLED
    UISettings::values.microprofile_geometry = microProfileDialog->saveGeometry();
    UISettings::values.microprofile_visible = microProfileDialog->isVisible();
#endif
    UISettings::values.single_window_mode = ui->action_Single_Window_Mode->isChecked();
    UISettings::values.fullscreen = ui->action_Fullscreen->isChecked();
    UISettings::values.display_titlebar = ui->action_Display_Dock_Widget_Headers->isChecked();
    UISettings::values.show_filter_bar = ui->action_Show_Filter_Bar->isChecked();
    UISettings::values.show_status_bar = ui->action_Show_Status_Bar->isChecked();
    UISettings::values.first_start = false;
}

void GMainWindow::SyncMenuUISettings() {
    ui->action_Screen_Layout_Default->setChecked(Settings::values.layout_option.GetValue() ==
                                                 Settings::LayoutOption::Default);
    ui->action_Screen_Layout_Single_Screen->setChecked(Settings::values.layout_option.GetValue() ==
                                                       Settings::LayoutOption::SingleScreen);
    ui->action_Screen_Layout_Large_Screen->setChecked(Settings::values.layout_option.GetValue() ==
                                                      Settings::LayoutOption::LargeScreen);
    ui->action_Screen_Layout_Hybrid_Screen->setChecked(Settings::values.layout_option.GetValue() ==
                                                       Settings::LayoutOption::HybridScreen);
    ui->action_Screen_Layout_Side_by_Side->setChecked(Settings::values.layout_option.GetValue() ==
                                                      Settings::LayoutOption::SideScreen);
    ui->action_Screen_Layout_Separate_Windows->setChecked(
        Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows);
    ui->action_Screen_Layout_Custom_Layout->setChecked(Settings::values.layout_option.GetValue() ==
                                                       Settings::LayoutOption::CustomLayout);
    ui->action_Screen_Layout_Swap_Screens->setChecked(Settings::values.swap_screen.GetValue());
    ui->action_Screen_Layout_Upright_Screens->setChecked(
        Settings::values.upright_screen.GetValue());

    ui->menu_Small_Screen_Position->setEnabled(Settings::values.layout_option.GetValue() ==
                                               Settings::LayoutOption::LargeScreen);

    ui->action_Small_Screen_TopRight->setChecked(
        Settings::values.small_screen_position.GetValue() ==
        Settings::SmallScreenPosition::TopRight);
    ui->action_Small_Screen_MiddleRight->setChecked(
        Settings::values.small_screen_position.GetValue() ==
        Settings::SmallScreenPosition::MiddleRight);
    ui->action_Small_Screen_BottomRight->setChecked(
        Settings::values.small_screen_position.GetValue() ==
        Settings::SmallScreenPosition::BottomRight);
    ui->action_Small_Screen_TopLeft->setChecked(Settings::values.small_screen_position.GetValue() ==
                                                Settings::SmallScreenPosition::TopLeft);
    ui->action_Small_Screen_MiddleLeft->setChecked(
        Settings::values.small_screen_position.GetValue() ==
        Settings::SmallScreenPosition::MiddleLeft);
    ui->action_Small_Screen_BottomLeft->setChecked(
        Settings::values.small_screen_position.GetValue() ==
        Settings::SmallScreenPosition::BottomLeft);
    ui->action_Small_Screen_Above->setChecked(Settings::values.small_screen_position.GetValue() ==
                                              Settings::SmallScreenPosition::AboveLarge);
    ui->action_Small_Screen_Below->setChecked(Settings::values.small_screen_position.GetValue() ==
                                              Settings::SmallScreenPosition::BelowLarge);
}

void GMainWindow::RetranslateStatusBar() {
    if (emu_thread)
        UpdateStatusBar();

    emu_speed_label->setToolTip(tr("Current emulation speed. Values higher or lower than 100% "
                                   "indicate emulation is running faster or slower than a 3DS."));
    game_fps_label->setToolTip(tr("How many frames per second the app is currently displaying. "
                                  "This will vary from app to app and scene to scene."));
    emu_frametime_label->setToolTip(
        tr("Time taken to emulate a 3DS frame, not counting framelimiting or v-sync. For "
           "full-speed emulation this should be at most 16.67 ms."));

    multiplayer_state->retranslateUi();
}

#ifdef ENABLE_DISCORD_RPC
void GMainWindow::SetDiscordEnabled([[maybe_unused]] bool state) {
    if (state) {
        discord_rpc = std::make_unique<DiscordRPC::DiscordImpl>(system);
    } else {
        discord_rpc = std::make_unique<DiscordRPC::NullImpl>();
    }
}
#endif

#ifdef __unix__
void GMainWindow::SetGamemodeEnabled(bool state) {
    if (emulation_running) {
        Common::Linux::SetGamemodeState(state);
    }
}
#endif

#ifdef main
#undef main
#endif

static Qt::HighDpiScaleFactorRoundingPolicy GetHighDpiRoundingPolicy() {
#ifdef _WIN32
    // For Windows, we want to avoid scaling artifacts on fractional scaling ratios.
    // This is done by setting the optimal scaling policy for the primary screen.

    // Create a temporary QApplication.
    int temp_argc = 0;
    char** temp_argv = nullptr;
    QApplication temp{temp_argc, temp_argv};

    // Get the current screen geometry.
    const QScreen* primary_screen = QGuiApplication::primaryScreen();
    if (!primary_screen) {
        return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
    }

    const QRect screen_rect = primary_screen->geometry();
    const qreal real_ratio = primary_screen->devicePixelRatio();
    const qreal real_width = std::trunc(screen_rect.width() * real_ratio);
    const qreal real_height = std::trunc(screen_rect.height() * real_ratio);

    // Recommended minimum width and height for proper window fit.
    // Any screen with a lower resolution than this will still have a scale of 1.
    constexpr qreal minimum_width = 1350.0;
    constexpr qreal minimum_height = 900.0;

    const qreal width_ratio = std::max(1.0, real_width / minimum_width);
    const qreal height_ratio = std::max(1.0, real_height / minimum_height);

    // Get the lower of the 2 ratios and truncate, this is the maximum integer scale.
    const qreal max_ratio = std::trunc(std::min(width_ratio, height_ratio));

    if (max_ratio > real_ratio) {
        return Qt::HighDpiScaleFactorRoundingPolicy::Round;
    } else {
        return Qt::HighDpiScaleFactorRoundingPolicy::Floor;
    }
#else
    // Other OSes should be better than Windows at fractional scaling.
    return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
#endif
}

#ifdef _WIN32
void GMainWindow::SetCustomWindowsScrollbar() {
    constexpr auto customScrollbarStyleTemplate = (R"(
        QScrollBar:vertical {{
            background: palette(window);
            width: 16px;
            margin: 0px;
            border-left: 1px solid palette({0});
        }}
        QScrollBar::handle:vertical {{
            background: palette({1});
            border: none;
            border-radius: 4px;
            margin: 3px;
        }}
        QScrollBar::handle:vertical:hover {{
            background: palette({0});
        }}
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
            border: none;
            height: 0px;
        }}
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{
            background: none;
        }}
    )");

    const auto darkCustomScrollbarStyle =
        QString::fromUtf8(std::format(customScrollbarStyleTemplate, "midlight", "light"));
    const auto lightCustomScrollbarStyle =
        QString::fromUtf8(std::format(customScrollbarStyleTemplate, "dark", "mid"));

    const bool isDarkMode = (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);

    if (isDarkMode) {
        setStyleSheet(darkCustomScrollbarStyle);
    } else {
        setStyleSheet(lightCustomScrollbarStyle);
    }
}
#endif

#ifdef _WIN32
/// Zeste headless (Windows): no Zeste window ever takes the foreground. A process launched by the
/// foreground application may take the foreground once, and Windows used it to activate one of
/// the invisible windows: a full-screen application behind lost the foreground and the taskbar
/// appeared over it. Every top-level window gets WS_EX_NOACTIVATE, and if the process is
/// activated anyway, the foreground goes back to the window that had it at start-up.
class HeadlessNoActivateFilter final : public QAbstractNativeEventFilter {
public:
    explicit HeadlessNoActivateFilter(HWND previous) : previous_foreground{previous} {}

    bool nativeEventFilter(const QByteArray&, void* message, qintptr* result) override {
        const MSG* msg = static_cast<const MSG*>(message);
        if (!msg->hwnd) {
            return false;
        }
        if (GetAncestor(msg->hwnd, GA_ROOT) == msg->hwnd) {
            const LONG_PTR ex_style = GetWindowLongPtrW(msg->hwnd, GWL_EXSTYLE);
            if (!(ex_style & WS_EX_NOACTIVATE)) {
                SetWindowLongPtrW(msg->hwnd, GWL_EXSTYLE, ex_style | WS_EX_NOACTIVATE);
            }
        }
        if (msg->message == WM_MOUSEACTIVATE) {
            *result = MA_NOACTIVATE;
            return true;
        }
        if (msg->message == WM_ACTIVATEAPP && msg->wParam && previous_foreground &&
            IsWindow(previous_foreground)) {
            SetForegroundWindow(previous_foreground);
        }
        return false;
    }

private:
    HWND previous_foreground;
};
#endif

int LaunchQtFrontend(int argc, char* argv[]) {
#ifdef __APPLE__
    // Ensure that the linker doesn't optimize qt_swizzle.mm out of existence.
    QtSwizzle::Dummy();
#endif

#if MICROPROFILE_ENABLED
    MicroProfileOnThreadCreate("Frontend");
    SCOPE_EXIT({ MicroProfileShutdown(); });
#endif

    // Init settings params
    QCoreApplication::setOrganizationName(QStringLiteral("Zeste"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("azahar_emu.org"));
    QCoreApplication::setApplicationName(QStringLiteral("Zeste"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.azahar_emu.Azahar"));

    auto rounding_policy = GetHighDpiRoundingPolicy();
    QApplication::setHighDpiScaleFactorRoundingPolicy(rounding_policy);

#ifdef __APPLE__
    auto bundle_dir = FileUtil::GetBundleDirectory();
    if (bundle_dir) {
        FileUtil::SetCurrentDir(bundle_dir.value() + "..");
    }
#endif

#ifdef ENABLE_OPENGL
    QCoreApplication::setAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif

    QApplication app(argc, argv);

#ifdef _WIN32
    // Zeste is portable: QFileDialog always saves its last folder and view in the registry
    // (HKCU\Software\QtProject\OrganizationDefaults\FileDialog, shared by every Qt program).
    // Snapshot it now and put it back as it was when Zeste exits.
    struct QtRegistryRestore {
        const QString root_path = QStringLiteral("HKEY_CURRENT_USER\\Software");
        bool had_qtproject = false;
        bool had_defaults = false;
        bool had_dialog = false;
        QVariantMap dialog;

        QtRegistryRestore() {
            QSettings root(root_path, QSettings::NativeFormat);
            had_qtproject = root.childGroups().contains(QStringLiteral("QtProject"));
            root.beginGroup(QStringLiteral("QtProject"));
            had_defaults = root.childGroups().contains(QStringLiteral("OrganizationDefaults"));
            root.beginGroup(QStringLiteral("OrganizationDefaults"));
            had_dialog = root.childGroups().contains(QStringLiteral("FileDialog"));
            root.beginGroup(QStringLiteral("FileDialog"));
            for (const QString& key : root.allKeys()) {
                dialog.insert(key, root.value(key));
            }
        }

        ~QtRegistryRestore() {
            QSettings root(root_path, QSettings::NativeFormat);
            if (!had_qtproject) {
                root.remove(QStringLiteral("QtProject"));
                return;
            }
            root.beginGroup(QStringLiteral("QtProject"));
            if (!had_defaults) {
                root.remove(QStringLiteral("OrganizationDefaults"));
                return;
            }
            root.beginGroup(QStringLiteral("OrganizationDefaults"));
            root.remove(QStringLiteral("FileDialog"));
            if (had_dialog) {
                root.beginGroup(QStringLiteral("FileDialog"));
                for (auto it = dialog.cbegin(); it != dialog.cend(); ++it) {
                    root.setValue(it.key(), it.value());
                }
            }
        }
    } qt_registry_restore;
#endif

    // Required when using .qrc resources from within a static library.
    // See https://doc.qt.io/qt-5/resources.html#using-resources-in-a-library
    Q_INIT_RESOURCE(theme_default);
    Q_INIT_RESOURCE(theme_qdarkstyle);
#ifdef ENABLE_QT_TRANSLATION
    Q_INIT_RESOURCE(languages);
#endif

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    auto& system{Core::System::GetInstance()};

    // Register Qt image interface
    system.RegisterImageInterface(std::make_shared<QtImageInterface>());

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            GMainWindow::headless_mode = true;
        }
    }

#ifdef _WIN32
    // Before any window exists: the window that has the foreground now keeps it.
    static HeadlessNoActivateFilter no_activate_filter{GetForegroundWindow()};
    if (GMainWindow::headless_mode) {
        app.installNativeEventFilter(&no_activate_filter);
    }
#endif
    // Zeste's own crashes: dump, stack and a summary in the run folder (Windows).
    Zeste::Crash::Install([](unsigned int code, const std::string& what) {
        if (!Zeste::Run::Folder().empty()) {
            Zeste::Run::Note("exit code", fmt::format("0x{:08X} (Zeste crashed)", code));
            Zeste::Summary::Write(Zeste::Run::Folder(), static_cast<int>(code), what);
        }
    });
    GMainWindow main_window(system);
    const auto finish_run = [](int code) {
        Zeste::Run::Note("exit code", std::to_string(code));
        if (!Zeste::Run::Folder().empty()) {
            Zeste::Summary::Write(Zeste::Run::Folder(), code,
                                  GMainWindow::headless_boot_failed ? "the game could not boot"
                                  : code == 2 ? "emulation stopped on an error (see the log)"
                                              : "emulation stopped");
        }
        if (GMainWindow::headless_mode && !Zeste::Run::Folder().empty()) {
            const std::string report = Zeste::Run::Folder() + "zeste_report.md";
            std::cout << "Zeste run folder: " << Zeste::Run::Folder() << std::endl;
            if (FileUtil::Exists(report)) {
                std::cout << "Zeste report: " << report << std::endl;
            }
            std::cout << "Zeste summary: " << Zeste::Run::Folder() << "summary.md" << std::endl;
            std::cout << "Zeste exit code: " << code << std::endl;
        }
        return code;
    };
    if (GMainWindow::headless_boot_failed) {
        main_window.close();
        return finish_run(1);
    }

    // Register frontend applets
    Frontend::RegisterDefaultApplets(system);

    if (GMainWindow::headless_mode) {
        // No dialogs: the keyboard is answered by script/RPC commands ("keyboard <text>"), the Mii
        // selector picks the default Mii (Azahar's default applet, registered above).
        Zeste::Keyboard::RegisterHeadless(system);
    } else {
        system.RegisterMiiSelector(std::make_shared<QtMiiSelector>(main_window));
        system.RegisterSoftwareKeyboard(std::make_shared<QtKeyboard>(main_window));
    }

#ifdef __APPLE__
    // Register microphone permission check.
    system.RegisterMicPermissionCheck(&AppleAuthorization::CheckAuthorizationForMicrophone);
#endif

    if (GMainWindow::headless_mode) {
        MakeHeadless(&main_window);
    }
    main_window.show();

    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &main_window,
                     &GMainWindow::OnAppFocusStateChanged);

    // Process any pending events before executing the app (prevents freeze-on–boot on macOS)
    app.processEvents();

    int result = app.exec();
    return finish_run(result);
}
