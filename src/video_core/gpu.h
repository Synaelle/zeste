// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <boost/serialization/access.hpp>

#include "core/hle/result.h"
#include "core/hle/service/gsp/gsp_interrupt.h"

namespace Service::GSP {
struct Command;
struct FrameBufferInfo;
} // namespace Service::GSP

namespace Core {
class System;
}

namespace Pica {
class DebugContext;
class PicaCore;
struct RegsLcd;
union ColorFill;
struct DisplayTransferConfig;
struct MemoryFillConfig;
} // namespace Pica

namespace Frontend {
class EmuWindow;
}

namespace VideoCore {

/// Measured on hardware to be 2240568 timer cycles or 4481136 ARM11 cycles
constexpr u64 FRAME_TICKS = 4481136ull;

class GraphicsDebugger;
class RendererBase;
class RightEyeDisabler;

/**
 * The GPU class is the high level interface to the video_core for core services.
 */
class GPU {
public:
    explicit GPU(Core::System& system, Frontend::EmuWindow& emu_window,
                 Frontend::EmuWindow* secondary_window);
    ~GPU();

    /// Sets the function to call for signalling GSP interrupts.
    void SetInterruptHandler(Service::GSP::InterruptHandler handler);

    /// Zeste: called at every emulated VBlank, after the frame was presented (automation).
    void SetVBlankHook(std::function<void()> hook);

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(PAddr addr, u32 size);

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(PAddr addr, u32 size);

    /// Flushes and invalidates all memory in the rasterizer cache and removes any leftover state.
    void ClearAll(bool flush);

    /// Executes the provided GSP command.
    Result Execute(const Service::GSP::Command& command);

    /// Updates GPU display framebuffer configuration using the specified parameters.
    void SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info);

    /// Sets the LCD color fill configuration for the top and bottom screens.
    void SetColorFill(const Pica::ColorFill& fill);

    /// Reads a word from the GPU virtual address.
    u32 ReadReg(VAddr addr);

    /// Writes the provided value to the GPU virtual address.
    void WriteReg(VAddr addr, u32 data);

    /// Returns a mutable reference to the renderer.
    [[nodiscard]] VideoCore::RendererBase& Renderer();
    [[nodiscard]] bool HasRenderer() const;

    /// Returns a mutable reference to the PICA GPU.
    [[nodiscard]] Pica::PicaCore& PicaCore();

    /// Returns an immutable reference to the PICA GPU.
    [[nodiscard]] const Pica::PicaCore& PicaCore() const;

    /// Returns a mutable reference to the GSP command debugger.
    [[nodiscard]] GraphicsDebugger& Debugger();

    RightEyeDisabler& GetRightEyeDisabler() {
        return *right_eye_disabler;
    }

    void ApplyPerProgramSettings(u64 program_ID);

    /// Recreates the renderer (for GL context reset in libretro)
    void RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window);

    /// Releases the renderer (for GL context destroy in libretro)
    void ReleaseRenderer();

    /// Zeste (asynchronous GX transfers): true when the GSP command just executed is still
    /// running (a deferred transfer or fill, or, with the PICA timing model, its P3D). GSP must
    /// not start the next queued command before then; the GPU calls GSP_GPU::ZesteResumeQueue
    /// when it is done.
    bool ZesteCommandBusy();

private:
    void SubmitCmdList(u32 index);

    // Interrupt index must be 0 or 1 to signal the relative PSC interrupt.
    void MemoryFill(u32 index, u32 intr_index);

    void MemoryTransfer();

    /// Performs a display transfer or texture copy now; returns its modelled duration (ns).
    u64 PerformTransfer(Pica::DisplayTransferConfig& config);

    /// Performs a memory fill now; returns its modelled duration (ns).
    u64 PerformMemoryFill(const Pica::MemoryFillConfig& config);

    // Zeste asynchronous GX transfers (zeste_async_gx): the operation is performed at its
    // modelled completion time instead of at the register write.
    struct ZesteGx;
    std::unique_ptr<ZesteGx> zeste_gx;
    void ZesteDeferTransfer();
    void ZesteDeferMemoryFill(u32 index, u32 intr_index);
    void ZesteCompleteOp(u64 id);
    void ZesteOnP3D(u64 delay_ns);

    void VBlankCallback(uintptr_t user_data, s64 cycles_late);

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

    std::unique_ptr<RightEyeDisabler> right_eye_disabler;

private:
    friend class RightEyeDisabler;
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::function<void()> vblank_hook;

    /// Translates the address of a GPU buffer as the GSP module does, or returns std::nullopt when
    /// the buffer is not in GPU visible memory.
    std::optional<PAddr> VirtualToPhysicalAddress(VAddr addr, u32 size);
};

} // namespace VideoCore
