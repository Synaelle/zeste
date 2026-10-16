// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste real CPU exceptions: unmapped accesses (including stack overflows below a stack) raise a
// data abort that is dispatched to the application's handler (libctru threadOnException),
// instead of being logged and ignored. Prefetch aborts (jumping to unmapped code) likewise.
//
// Dispatch follows the ABI that libctru's threadOnException relies on: TLS+0x40 = handler,
// TLS+0x44 = handler stack top (1: run on the faulting stack), TLS+0x48 = exception data buffer
// (0: push it on the handler stack, 1: push it on the faulting stack). The handler is entered in
// user mode with r0 = ERRF_ExceptionInfo*, r1 = CpuRegisters* and the chosen stack.
//
// Precise dispatch needs the dynarmic JIT (the default): it is configured to stop right after
// the faulting access with the PC on the faulting instruction. With the interpreters the abort
// is reported and emulation stops (as the process would die on hardware without a handler).

#pragma once

#include "common/common_types.h"

namespace Core {
class ARM_Interface;
class System;
} // namespace Core

namespace Zeste::Exceptions {

bool Enabled();

/// Called by the memory system on an unmapped access. Returns true when Zeste takes over (the
/// access is reported and an abort is raised), false to keep Azahar's behaviour.
bool OnUnmappedAccess(Core::System& system, VAddr vaddr, u32 size, bool write);

/// Set by the JIT around guest loads/stores: only those can raise a data abort (HLE services and
/// timing events touching bad addresses keep Azahar's behaviour).
void SetGuestAccess(bool active);

/// Instruction fetches also go through the unmapped-access path; they are prefetch aborts and are
/// handled by OnPrefetchAbort instead. The JIT brackets code reads with this.
void SetCodeFetch(bool active);

/// Called by the JIT (on the emulation thread) after it stopped for a pending data abort.
void DispatchPending(Core::System& system, Core::ARM_Interface& core);

/// Called by the JIT when it cannot fetch an instruction. Returns true when handled.
bool OnPrefetchAbort(Core::System& system, Core::ARM_Interface& core, VAddr pc);

/// True when a data abort is waiting for the given core.
bool HasPending(u32 core_id);

} // namespace Zeste::Exceptions
