// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <vector>
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exception_handler.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/zeste_core.h"
#include "core/zeste_exceptions.h"

namespace Zeste::Exceptions {

namespace {

constexpr std::size_t MaxCores = 4;

/// ARM11 FSR values: translation fault (page), bit 11 = write (WnR).
constexpr u32 FsrTranslationFaultPage = 0x7;
constexpr u32 FsrPermissionFaultPage = 0xF;
constexpr u32 FsrWrite = 1u << 11;

enum class Type : u8 {
    PrefetchAbort = 0,
    DataAbort = 1,
};

struct Pending {
    bool active = false;
    Type type = Type::DataAbort;
    u32 far = 0;
    u32 fsr = 0;
    u32 size = 0;
};

std::array<Pending, MaxCores> g_pending{};
thread_local bool g_code_fetch = false;
thread_local bool g_guest_access = false;

constexpr u32 ExceptionInfoSize = 0x18;   // ERRF_ExceptionInfo
constexpr u32 CpuRegistersSize = 0x44;    // CpuRegisters: r0-r12, sp, lr, pc, cpsr
constexpr u32 ExceptionDataSize = ExceptionInfoSize + CpuRegistersSize;
constexpr u32 TlsHandler = 0x40;
constexpr u32 TlsStackTop = 0x44;
constexpr u32 TlsExceptionData = 0x48;
constexpr u32 RunOnFaultingStack = 1;
constexpr u32 WriteDataToHandlerStack = 0;
constexpr u32 WriteDataToFaultingStack = 1;
constexpr u32 CpsrThumb = 1u << 5;
constexpr u32 CpsrItMask = 0x0600FC00;
constexpr u32 CpsrModeUser = 0x10;

CrashInfo g_crash;

struct ReadOnlyRange {
    u32 process_id;
    VAddr start;
    u32 size;
};
std::vector<ReadOnlyRange> g_read_only;

/// Fills g_crash: registers, and a scan of the faulting thread's stack for return addresses.
void CaptureCrash(Core::System& system, Core::ARM_Interface& core, const Pending& p) {
    auto& memory = system.Memory();
    const auto process = system.Kernel().GetCurrentProcess();
    CrashInfo c;
    c.valid = true;
    c.type = p.type == Type::DataAbort ? "Data abort" : "Prefetch abort";
    c.fault_address = p.far;
    for (u32 i = 0; i < 15; ++i) {
        c.regs[i] = core.GetReg(static_cast<int>(i));
    }
    c.regs[15] = core.GetPC();
    c.cpsr = core.GetCPSR();
    if (process && process->codeset) {
        const auto& code = process->codeset->CodeSegment();
        c.code_start = code.addr;
        c.code_end = code.addr + code.size;
    }
    const auto valid = [&](VAddr a) { return process && memory.IsValidVirtualAddress(*process, a); };
    const auto in_code = [&](u32 v) { return v >= c.code_start + 4 && v < c.code_end; };
    // A return address follows a call: ARM BL/BLX (imm) or BLX (reg); Thumb BL/BLX pair or
    // BLX (reg).
    const auto after_call = [&](u32 v) {
        if (v & 1) {
            const VAddr ret = v & ~1u;
            if (valid(ret - 4)) {
                const u16 hi = memory.Read16(ret - 4);
                if ((hi & 0xF800) == 0xF000) {
                    return true;
                }
            }
            return valid(ret - 2) && (memory.Read16(ret - 2) & 0xFF87) == 0x4780;
        }
        if ((v & 3) != 0 || !valid(v - 4)) {
            return false;
        }
        const u32 insn = memory.Read32(v - 4);
        return (insn & 0x0F000000) == 0x0B000000 || (insn & 0xFE000000) == 0xFA000000 ||
               (insn & 0x0FFFFFF0) == 0x012FFF30;
    };
    if (c.code_end != 0) {
        const u32 sp = c.regs[13];
        for (u32 i = 0; i < 2048 && c.stack_returns.size() < 24; ++i) {
            const VAddr a = sp + i * 4;
            if (!valid(a)) {
                break;
            }
            const u32 v = memory.Read32(a);
            if (in_code(v & ~1u) && after_call(v)) {
                c.stack_returns.push_back(v);
            }
        }
    }
    g_crash = std::move(c);
}

bool IsStackAccess(VAddr vaddr, u32 sp) {
    return vaddr < sp + 0x1000 && static_cast<u64>(vaddr) + 0x10000 >= sp;
}

/// The process would die on hardware (no handler): report and stop like Azahar's exception
/// handler does. The PC is moved past the faulting instruction so "Continue" behaves like Azahar.
void StopWithoutHandler(Core::System& system, Core::ARM_Interface& core, const Pending& p) {
    CaptureCrash(system, core, p);
    const u32 cpsr = core.GetCPSR();
    if (p.type == Type::DataAbort) {
        core.SetPC(core.GetPC() + ((cpsr & CpsrThumb) ? 2 : 4));
    }
    LOG_CRITICAL(Zeste,
                 "{} at 0x{:08X} with no exception handler installed: the process would be "
                 "terminated on hardware (Luma shows its exception screen)",
                 p.type == Type::DataAbort ? "Data abort" : "Prefetch abort", p.far);
    Core::LogException(system, p.type == Type::DataAbort
                                   ? ((p.fsr & FsrWrite) ? Core::ExceptionType::UnmappedWrite
                                                         : Core::ExceptionType::UnmappedRead)
                                   : Core::ExceptionType::PrefetchAbort);
    system.PrepareReschedule();
}

void Dispatch(Core::System& system, Core::ARM_Interface& core, const Pending& p) {
    auto& memory = system.Memory();
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return;
    }
    const VAddr tls = core.GetCP15Register(CP15_THREAD_URO);
    const auto read_tls = [&](u32 offset) -> u32 {
        return memory.IsValidVirtualAddress(*process, tls + offset) ? memory.Read32(tls + offset)
                                                                      : 0;
    };
    const u32 handler = tls != 0 ? read_tls(TlsHandler) : 0;
    if (handler == 0) {
        StopWithoutHandler(system, core, p);
        return;
    }
    const u32 stack_top = read_tls(TlsStackTop);
    const u32 data_ptr = read_tls(TlsExceptionData);

    const u32 faulting_sp = core.GetReg(13);
    const u32 sp_base = stack_top == RunOnFaultingStack ? faulting_sp : stack_top;
    u32 data_addr;
    u32 new_sp;
    if (data_ptr == WriteDataToHandlerStack) {
        data_addr = (sp_base - ExceptionDataSize) & ~7u;
        new_sp = data_addr;
    } else if (data_ptr == WriteDataToFaultingStack) {
        data_addr = (faulting_sp - ExceptionDataSize) & ~7u;
        new_sp = stack_top == RunOnFaultingStack ? data_addr : sp_base;
    } else {
        data_addr = data_ptr;
        new_sp = sp_base;
    }
    if (!memory.IsValidVirtualAddress(*process, data_addr) ||
        !memory.IsValidVirtualAddress(*process, data_addr + ExceptionDataSize - 1)) {
        LOG_CRITICAL(Zeste,
                     "exception handler 0x{:08X} installed but its exception data at 0x{:08X} is "
                     "not writable (stack overflowed into the handler stack?)",
                     handler, data_addr);
        StopWithoutHandler(system, core, p);
        return;
    }

    // ERRF_ExceptionInfo
    memory.Write32(data_addr + 0x00, static_cast<u32>(p.type));
    memory.Write32(data_addr + 0x04, p.fsr);
    memory.Write32(data_addr + 0x08, p.far);
    memory.Write32(data_addr + 0x0C, core.GetVFPSystemReg(VFP_FPEXC));
    memory.Write32(data_addr + 0x10, 0);
    memory.Write32(data_addr + 0x14, 0);
    // CpuRegisters
    const VAddr regs_addr = data_addr + ExceptionInfoSize;
    for (int i = 0; i < 15; ++i) {
        memory.Write32(regs_addr + i * 4, core.GetReg(i));
    }
    memory.Write32(regs_addr + 15 * 4, core.GetPC());
    memory.Write32(regs_addr + 16 * 4, core.GetCPSR());

    LOG_WARNING(Zeste,
                "{} at 0x{:08X} (PC 0x{:08X}) dispatched to the exception handler 0x{:08X} "
                "(exception data at 0x{:08X}, sp 0x{:08X})",
                p.type == Type::DataAbort ? "Data abort" : "Prefetch abort", p.far, core.GetPC(),
                handler, data_addr, new_sp);

    core.SetReg(0, data_addr);
    core.SetReg(1, regs_addr);
    core.SetReg(13, new_sp);
    u32 cpsr = (core.GetCPSR() & ~(CpsrThumb | CpsrItMask | 0x1F)) | CpsrModeUser;
    if (handler & 1) {
        cpsr |= CpsrThumb;
    }
    core.SetCPSR(cpsr);
    core.SetPC(handler & ~1u);
}

} // Anonymous namespace

bool Enabled() {
    return Settings::values.zeste_cpu_exceptions.GetValue();
}

void SetCodeFetch(bool active) {
    g_code_fetch = active;
}

void SetGuestAccess(bool active) {
    g_guest_access = active;
}

const CrashInfo& LastCrash() {
    return g_crash;
}

bool HasPending(u32 core_id) {
    return core_id < MaxCores && g_pending[core_id].active;
}

void AddReadOnlyRange(u32 process_id, VAddr start, u32 size) {
    g_read_only.push_back({process_id, start, size});
}

void ClearReadOnlyRanges(u32 process_id) {
    std::erase_if(g_read_only,
                  [process_id](const ReadOnlyRange& r) { return r.process_id == process_id; });
}

bool OnReadOnlyWrite(Core::System& system, VAddr vaddr, u32 size) {
    if (!Enabled() || !g_guest_access || g_read_only.empty() || !system.IsPoweredOn() ||
        !system.HasRunningCore()) {
        return false;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return false;
    }
    const bool read_only = std::any_of(g_read_only.begin(), g_read_only.end(), [&](const auto& r) {
        return r.process_id == process->process_id && vaddr >= r.start &&
               vaddr - r.start < r.size;
    });
    if (!read_only) {
        return false;
    }
    auto& core = system.GetRunningCore();
    const u32 id = core.GetID();
    if (id < MaxCores && !g_pending[id].active) {
        g_pending[id] = {true, Type::DataAbort, vaddr, FsrPermissionFaultPage | FsrWrite, size};
    }
    if (!core.ZesteRequestAbortHalt()) {
        g_pending[id].active = false;
        return false;
    }
    return true;
}

bool OnUnmappedAccess(Core::System& system, VAddr vaddr, u32 size, bool write) {
    if (!Enabled() || g_code_fetch || !g_guest_access || !system.IsPoweredOn() ||
        !system.HasRunningCore()) {
        return false;
    }
    auto& core = system.GetRunningCore();
    const u32 id = core.GetID();
    if (id < MaxCores && !g_pending[id].active) {
        g_pending[id] = {true, Type::DataAbort, vaddr,
                         FsrTranslationFaultPage | (write ? FsrWrite : 0), size};
    }
    if (!core.ZesteRequestAbortHalt()) {
        g_pending[id].active = false;
        return false;
    }
    return true;
}

void DispatchPending(Core::System& system, Core::ARM_Interface& core) {
    const u32 id = core.GetID();
    if (id >= MaxCores || !g_pending[id].active) {
        return;
    }
    const Pending p = g_pending[id];
    g_pending[id].active = false;

    // Reported here, where the JIT has synchronised the PC to the faulting instruction.
    const bool write = (p.fsr & FsrWrite) != 0;
    const u32 sp = core.GetReg(13);
    const bool stack = IsStackAccess(p.far, sp);
    const bool permission = (p.fsr & 0xF) == FsrPermissionFaultPage;
    if (permission) {
        ReportF(Category::DataAbort, fmt::format("ro@{:08X}", core.GetPC()),
                "write of {} bytes at 0x{:08X} by the instruction at 0x{:08X}: the exheader maps "
                "this memory read-only (for VRAM: \"1f000000-1f5fffff:r\"), so the CPU may not "
                "write it: data abort (permission fault) on hardware. Write it with the GPU (a "
                "DMA or texture copy from linear memory), or map it writable in the .rsf",
                p.size, p.far, core.GetPC());
    } else {
        ReportF(stack ? Category::StackOverflow : Category::DataAbort,
                fmt::format("{}@{:08X}", write ? "w" : "r", core.GetPC()),
                "{} of {} bytes at unmapped 0x{:08X} by the instruction at 0x{:08X}{}: data abort "
                "on hardware",
                write ? "write" : "read", p.size, p.far, core.GetPC(),
                stack ? fmt::format(" (sp=0x{:08X}: stack overflow below the thread's stack)", sp)
                      : "");
    }

    if (ConsumeBreakRequest()) {
        // "Break on first": leave the PC on the faulting instruction. It faults again (and is
        // dispatched) when emulation resumes.
        return;
    }
    Dispatch(system, core, p);
}

bool OnPrefetchAbort(Core::System& system, Core::ARM_Interface& core, VAddr pc) {
    if (!Enabled()) {
        return false;
    }
    ReportF(Category::DataAbort, fmt::format("x@{:08X}", pc),
            "instruction fetch from unmapped 0x{:08X} (lr=0x{:08X}): prefetch abort on hardware",
            pc, core.GetReg(14));
    core.SetPC(pc);
    if (ConsumeBreakRequest()) {
        return true;
    }
    Dispatch(system, core, {true, Type::PrefetchAbort, pc, FsrTranslationFaultPage});
    return true;
}

} // namespace Zeste::Exceptions
