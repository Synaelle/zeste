// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "common/zeste_crash.h"

#ifdef _WIN32
#include <cstdio>
#include <fmt/format.h>
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/zeste_run.h"

#pragma comment(lib, "dbghelp.lib")
#endif

namespace Zeste::Crash {

#ifdef _WIN32
namespace {

std::function<void(unsigned int, const std::string&)> g_on_crash;
LONG g_entered = 0;

std::string ModuleOffset(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return "?";
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring name{path};
    if (const auto slash = name.find_last_of(L"\\/"); slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }
    return fmt::format("{}+0x{:X}", Common::UTF16ToUTF8(name),
                       address - reinterpret_cast<DWORD64>(module));
}

std::string Symbol(HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string text = ModuleOffset(address);
    if (SymFromAddr(process, address, &displacement, symbol)) {
        text += fmt::format(" {}+0x{:X}", symbol->Name, displacement);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
            text += fmt::format(" ({}:{})", line.FileName, line.LineNumber);
        }
    }
    return text;
}

std::string StackTrace(HANDLE process, const CONTEXT& original) {
    CONTEXT context = original;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    std::string text;
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        text += fmt::format("  #{:<2} 0x{:016X} {}\n", i, frame.AddrPC.Offset,
                            Symbol(process, frame.AddrPC.Offset));
    }
    return text;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&g_entered, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH; // crashed inside the handler
    }
    const auto* record = info->ExceptionRecord;
    const auto code = static_cast<unsigned int>(record->ExceptionCode);
    const auto address = reinterpret_cast<DWORD64>(record->ExceptionAddress);
    std::string what = fmt::format("zeste.exe crashed: exception 0x{:08X} at {} on thread {}",
                                   code, ModuleOffset(address), GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto kind = record->ExceptionInformation[0];
        what += fmt::format(" ({} 0x{:X})",
                            kind == 0   ? "reading"
                            : kind == 1 ? "writing"
                                        : "executing",
                            record->ExceptionInformation[1]);
    }

    const std::string folder = Zeste::Run::Folder();
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // zeste.pdb sits next to zeste.exe; dbghelp does not look there by itself.
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring exe_dir{exe_path};
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of(L"\\/"));
    const bool symbols =
        SymInitializeW(process, exe_dir.empty() ? nullptr : exe_dir.c_str(), TRUE) != FALSE;
    const std::string stack = StackTrace(process, *info->ContextRecord);
    const std::string crash_site = symbols ? Symbol(process, address) : ModuleOffset(address);

    if (!folder.empty()) {
        const std::wstring dump_path = Common::UTF8ToUTF16W(folder + "zeste_crash.dmp");
        const HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(process, GetCurrentProcessId(), file,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                         MiniDumpWithIndirectlyReferencedMemory |
                                                         MiniDumpWithUnloadedModules),
                              &exception, nullptr, nullptr);
            CloseHandle(file);
        }
        if (FILE* text = _wfopen(Common::UTF8ToUTF16W(folder + "zeste_crash.txt").c_str(), L"w")) {
            std::fprintf(text, "%s\ncrash site: %s\n\nstack of the crashing thread:\n%s\n%s\n",
                         what.c_str(), crash_site.c_str(), stack.c_str(),
                         "zeste_crash.dmp: open it in Visual Studio or WinDbg with zeste.pdb "
                         "next to zeste.exe for every thread.");
            std::fclose(text);
        }
    }
    LOG_CRITICAL(Frontend, "{}\n{}", what, stack);
    std::fprintf(stdout, "Zeste crashed: %s\nZeste crash report: %szeste_crash.txt\n",
                 what.c_str(), folder.c_str());
    std::fflush(stdout);
    if (g_on_crash) {
        g_on_crash(code, what + ": " + crash_site);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install(std::function<void(unsigned int, const std::string&)> on_crash) {
    g_on_crash = std::move(on_crash);
    SetUnhandledExceptionFilter(OnUnhandledException);
}

#else

void Install(std::function<void(unsigned int, const std::string&)>) {}

#endif

} // namespace Zeste::Crash
