// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <optional>
#include <vector>
#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "core/zeste_cache.h"
#include "core/zeste_gx.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/sw_blitter.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"

namespace VideoCore {

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

class DelayGenerator {
private:
    DelayGenerator() = default;

    // Average transfer speed based on measurements taken from real
    // hardware. 4 different modes have been taken into consideration:
    // RAM -> RAM, RAM -> VRAM, VRAM -> RAM and VRAM -> VRAM.
    // Furthermore, measurements are split into DMA transfers and tex
    // copies. For simplicity, we will assume fills are as fast as
    // texture copies.

    static constexpr double mibps_to_ns_per_byte(double mib_per_sec) {
        return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
    }

    static constexpr std::array<std::array<double, 4>, 2> speed_mibps = {
        {{
             190.0, // DMA RAMTORAM
             310.0, // DMA RAMTOVRAM
             380.0, // DMA VRAMTORAM
             380.0, // DMA VRAMTOVRAM
         },
         {
             450.0,  // TEX RAMTORAM
             3100.0, // TEX RAMTOVRAM
             5400.0, // TEX VRAMTORAM
             5400.0, // TEX VRAMTOVRAM
         }}};

public:
    enum class CopyMode {
        RAMTORAM,
        RAMTOVRAM,
        VRAMTORAM,
        VRAMTOVRAM,
    };

    static CopyMode GetCopyMode(bool input_vram, bool output_vram) {
        if (!input_vram && !output_vram) {
            return CopyMode::RAMTORAM;
        } else if (!input_vram && output_vram) {
            return CopyMode::RAMTOVRAM;
        } else if (input_vram && !output_vram) {
            return CopyMode::VRAMTORAM;
        } else {
            return CopyMode::VRAMTOVRAM;
        }
    }

    static u64 CalculateDelayNanoseconds(CopyMode mode, bool is_textre, size_t size) {
        double base_ns_per_byte =
            mibps_to_ns_per_byte(speed_mibps[is_textre][static_cast<u32>(mode)]);

        return static_cast<u64>(size * base_ns_per_byte);
    }
};

namespace {

u64 TransferDelayNanoseconds(Pica::DisplayTransferConfig config) {
    const u32 size = config.is_texture_copy
                         ? config.texture_copy.size
                         : config.input_width * config.input_height *
                               BytesPerPixel(config.input_format);
    return DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true, size);
}

u64 FillDelayNanoseconds(const Pica::MemoryFillConfig& config) {
    // Treat fill as texture transfer from VRAM
    return DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
        config.GetEndAddress() - config.GetStartAddress());
}

/// Reports a transfer source the CPU wrote without a data cache flush (cache detection).
void CheckTransferSourceFlushed(Memory::MemorySystem& memory,
                                const Pica::DisplayTransferConfig& config) {
    if (config.is_texture_copy) {
        Zeste::Cache::CheckDeviceRead(memory, config.GetPhysicalInputAddress(),
                                      config.texture_copy.size, "texture copy source");
    } else {
        Zeste::Cache::CheckDeviceRead(
            memory, config.GetPhysicalInputAddress(),
            config.input_width * config.input_height * BytesPerPixel(config.input_format),
            "display transfer source");
    }
}

/// Bytes a display transfer or texture copy writes, from its output address.
u32 TransferOutputSpan(const Pica::DisplayTransferConfig& config) {
    if (!config.is_texture_copy) {
        return config.output_width * config.output_height * BytesPerPixel(config.output_format);
    }
    const u32 size = config.texture_copy.size & ~15u;
    const u32 gap = config.texture_copy.output_gap * 16;
    const u32 width = gap == 0 ? size : config.texture_copy.output_width * 16;
    if (width == 0) {
        return size;
    }
    const u32 lines = (size + width - 1) / width;
    return lines * (width + gap) - gap;
}

} // Anonymous namespace

/// Zeste asynchronous GX transfers: time the GSP module takes to handle a GX command before the
/// operation starts. This is Azahar's hardware-measured GSP request time ("Every gsp request
/// takes a constant amount of time to be processed", sync_delay_nanoseconds in gsp_gpu.cpp, used
/// by "Simulate 3DS GPU timings"). The hardware calibration needs it: v2's GPU copies of 256 and
/// 375 KB were still unfinished after two IPC round trips (~100 us), while the speed table alone
/// gives 46-68 us.
constexpr u64 ZesteGspRequestNanoseconds = 300 * 1000;

/// Zeste asynchronous GX transfers: operations waiting for their modelled completion.
struct GPU::ZesteGx {
    struct Op {
        u64 id;
        bool is_fill;
        Pica::DisplayTransferConfig transfer; ///< Registers at the trigger
        Pica::MemoryFillConfig fill;          ///< Registers at the trigger
        u32 fill_index;
        std::optional<Service::GSP::InterruptId> interrupt;
        u64 group; ///< Fills of one two-buffer GSP command: the last one signals
        s64 done_ticks;
    };

    Core::TimingEventType* complete_event = nullptr;
    Core::TimingEventType* resume_event = nullptr;
    std::vector<Op> ops;
    u64 next_id = 1;
    u64 next_group = 1;
    s64 transfer_free = 0;           ///< The transfer engine is busy until then
    std::array<s64, 2> fill_free{};  ///< Each fill unit is busy until then
    s64 command_done = 0;            ///< Completion of the GSP command being executed
    u64 fill_group = 0;              ///< Set while a two-buffer memory fill command runs
    bool in_gsp_command = false;     ///< The trigger comes from a GSP command (not a register)

    /// When an operation triggered now can start on an engine that is busy until `engine_free`.
    s64 StartTicks(s64 now, s64 engine_free) const {
        const s64 request = in_gsp_command ? nsToCycles(ZesteGspRequestNanoseconds) : 0;
        return std::max(now + request, engine_free);
    }

    void Schedule(GPU& gpu, Op op, PAddr dst, u32 dst_size, const char* what) {
        auto& impl = *gpu.impl;
        const s64 now = impl.timing.GetTicks();
        op.id = next_id++;
        command_done = std::max(command_done, op.done_ticks);
        impl.timing.ScheduleEvent(op.done_ticks - now, complete_event, op.id);
        ::Zeste::Gx::AddPending(impl.system, op.id, dst, dst_size, what, op.done_ticks);
        ops.push_back(op);
    }
};

MICROPROFILE_DEFINE(GPU_DisplayTransfer, "GPU", "DisplayTransfer", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(GPU_CmdlistProcessing, "GPU", "Cmdlist Processing", MP_RGB(100, 255, 100));

GPU::GPU(Core::System& system, Frontend::EmuWindow& emu_window,
         Frontend::EmuWindow* secondary_window)
    : right_eye_disabler{std::make_unique<RightEyeDisabler>(*this)},
      impl{std::make_unique<Impl>(system, emu_window, secondary_window)} {
    impl->vblank_event = impl->timing.RegisterEvent(
        "GPU::VBlankCallback",
        [this](uintptr_t user_data, s64 cycles_late) { VBlankCallback(user_data, cycles_late); });
    impl->timing.ScheduleEvent(FRAME_TICKS, impl->vblank_event);

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    zeste_gx = std::make_unique<ZesteGx>();
    zeste_gx->complete_event = impl->timing.RegisterEvent(
        "Zeste::GxComplete", [this](uintptr_t id, s64) { ZesteCompleteOp(id); });
    zeste_gx->resume_event =
        impl->timing.RegisterEvent("Zeste::GxResumeQueue", [this](uintptr_t, s64) {
            if (auto gsp = impl->system.ServiceManager().GetService<Service::GSP::GSP_GPU>(
                    "gsp::Gpu")) {
                gsp->ZesteResumeQueue();
            }
        });
    Zeste::Gx::Reset();
}

GPU::~GPU() {
    Zeste::Gx::Reset();
}

constexpr Result ResultInvalidGpuBuffer(ErrorDescription::InvalidAddress, ErrorModule::GX,
                                        ErrorSummary::InvalidArgument,
                                        ErrorLevel::Usage); // 0xE0E02BF5
constexpr Result ResultUnknownGxCommand(ErrorDescription::NotImplemented, ErrorModule::GX,
                                        ErrorSummary::NotSupported,
                                        ErrorLevel::Usage); // 0xE0C02BF4

std::optional<PAddr> GPU::VirtualToPhysicalAddress(VAddr addr, u32 size) {
    // Address translation of the GSP module.
    // The linear heaps and the New 3DS extra memory must contain both the start,
    // and the end of the buffer, VRAM only the start. The ends of the
    // ranges are inclusive.
    const VAddr end = addr + size;
    const auto contains = [addr, end](VAddr range_start, VAddr range_end) {
        return addr >= range_start && addr <= range_end && end >= range_start && end <= range_end;
    };

    if (contains(Memory::LINEAR_HEAP_VADDR, Memory::LINEAR_HEAP_VADDR_END)) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (contains(Memory::NEW_LINEAR_HEAP_VADDR, Memory::NEW_LINEAR_HEAP_VADDR_END)) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (contains(Memory::N3DS_EXTRA_RAM_VADDR, Memory::N3DS_EXTRA_RAM_VADDR_END)) {
        return addr - Memory::N3DS_EXTRA_RAM_VADDR + Memory::N3DS_EXTRA_RAM_PADDR;
    }

    // TODO(PabloMK7): Find a better way to handle this.
    PAddr plg_fb_addr;
    if (contains(Memory::PLUGIN_3GX_FB_VADDR, Memory::PLUGIN_3GX_FB_VADDR_END) &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_GPU, "Invalid GPU buffer address 0x{:08X} (size 0x{:X})", addr, size);
    return std::nullopt;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->signal_interrupt = handler;
    // Zeste: the P3D delay of the PICA timing model tells when a command list completes.
    Service::GSP::InterruptHandler pica_handler = [this, handler](Service::GSP::InterruptId id,
                                                                  u64 delay_ns) {
        if (id == Service::GSP::InterruptId::P3D) {
            ZesteOnP3D(delay_ns);
        }
        handler(id, delay_ns);
    };
    impl->pica.SetInterruptHandler(pica_handler);
}

void GPU::FlushRegion(PAddr addr, u32 size) {
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    impl->rasterizer->InvalidateRegion(addr, size);
}

void GPU::ClearAll(bool flush) {
    impl->rasterizer->ClearAll(flush);
}

Result GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->pica.regs;
    Result result = ResultSuccess;
    zeste_gx->command_done = 0;
    zeste_gx->in_gsp_command = true;

    switch (command.id) {
    case CommandId::RequestDma: {
        impl->system.Memory().RasterizerFlushVirtualRegion(
            command.dma_request.source_address, command.dma_request.size, Memory::FlushMode::Flush);
        impl->system.Memory().RasterizerFlushVirtualRegion(command.dma_request.dest_address,
                                                           command.dma_request.size,
                                                           Memory::FlushMode::Invalidate);

        if (const auto dst = Zeste::Cache::LinearToPhysical(command.dma_request.dest_address)) {
            // Written by the DMA engine. The source side is not checked: whether GSP flushes it
            // itself is not known.
            Zeste::Cache::OnSystemWrite(impl->memory, *dst, command.dma_request.size);
        }
        // TODO(Subv): These memory accesses should not go through the application's memory mapping.
        // They should go through the GSP module's memory mapping.
        const auto process = impl->system.Kernel().GetCurrentProcess();
        impl->memory.CopyBlock(*process, command.dma_request.dest_address,
                               command.dma_request.source_address, command.dma_request.size);

        auto is_vram = [&](u32 addr) {
            return addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END;
        };

        u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(is_vram(command.dma_request.source_address),
                                        is_vram(command.dma_request.dest_address)),
            false, command.dma_request.size);

        impl->signal_interrupt(Service::GSP::InterruptId::DMA, delay);
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        auto& cmdbuffer = regs.internal.pipeline.command_buffer;

        if (params.do_flush) {
            // GX_ProcessCommandList with the flush flag: GSP flushes the list for the app.
            if (const auto list = Zeste::Cache::LinearToPhysical(params.address)) {
                Zeste::Cache::OnCpuFlush(impl->memory, *list, params.size);
            }
        }

        // Write to the command buffer GPU registers.
        // The GSP module uses address 0 when the buffer is not in GPU visible memory.
        const PAddr address = VirtualToPhysicalAddress(params.address, params.size).value_or(0);
        cmdbuffer.addr[0].Assign(address >> 3);
        cmdbuffer.size[0].Assign(params.size >> 3);
        cmdbuffer.trigger[0] = 1;

        // Trigger processing of the command list
        SubmitCmdList(0);
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        const auto translate = [this](u32 start, u32 end) -> std::optional<PAddr> {
            if (start >= end) {
                return std::nullopt;
            }
            return VirtualToPhysicalAddress(start, end - start);
        };

        // Write to the memory fill GPU registers.
        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = params.start1 != 0 && params.start2 != 0;
        zeste_gx->fill_group = has_both_bufs ? zeste_gx->next_group++ : 0;
        if (params.start1 != 0) {
            const auto start = translate(params.start1, params.end1);
            if (!start) {
                result = ResultInvalidGpuBuffer;
                break;
            }
            memfill[0].address_start = *start >> 3;
            memfill[0].address_end = (*start + params.end1 - params.start1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (params.start2 != 0) {
            const auto start = translate(params.start2, params.end2);
            if (!start) {
                result = ResultInvalidGpuBuffer;
                break;
            }
            memfill[1].address_start = *start >> 3;
            memfill[1].address_end = (*start + params.end2 - params.start2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        // The GSP module uses address 0 when the buffer is not in GPU visible memory (only the
        // first byte is checked).
        display_transfer.input_address =
            VirtualToPhysicalAddress(params.in_buffer_address, 1).value_or(0) >> 3;
        display_transfer.output_address =
            VirtualToPhysicalAddress(params.out_buffer_address, 1).value_or(0) >> 3;
        display_transfer.input_size = params.in_buffer_size;
        display_transfer.output_size = params.out_buffer_size;
        display_transfer.flags = params.flags;
        display_transfer.trigger.Assign(1);

        // Trigger the display transfer.
        MemoryTransfer();
        break;
    }
    case CommandId::TextureCopy: {
        auto& params = command.texture_copy;
        auto& texture_copy = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        // The GSP module uses address 0 when the buffer is not in GPU visible memory.
        texture_copy.input_address =
            VirtualToPhysicalAddress(params.in_buffer_address, params.size).value_or(0) >> 3;
        texture_copy.output_address =
            VirtualToPhysicalAddress(params.out_buffer_address, params.size).value_or(0) >> 3;
        texture_copy.texture_copy.size = params.size;
        texture_copy.texture_copy.input_size = params.in_width_gap;
        texture_copy.texture_copy.output_size = params.out_width_gap;
        texture_copy.flags = params.flags;
        texture_copy.trigger.Assign(1);

        // Trigger the texture copy.
        MemoryTransfer();
        break;
    }
    case CommandId::CacheFlush: {
        // Rasterizer flushing handled elsewhere in CPU read/write and other GPU handlers
        // Use command.cache_flush.regions to implement this handler
        for (const auto& region : command.cache_flush.regions) {
            if (region.size == 0) {
                continue;
            }
            if (const auto paddr = Zeste::Cache::LinearToPhysical(region.address)) {
                Zeste::Cache::OnCpuFlush(impl->memory, *paddr, region.size);
            }
        }
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Unknown command {:#08X}", command.id.Value());
        result = ResultUnknownGxCommand;
    }

    zeste_gx->fill_group = 0;
    zeste_gx->in_gsp_command = false;

    // Notify debugger that a GSP command was processed.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::GSPCommandProcessed, &command);
    }
    return result;
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    // The GSP module does not apply a framebuffer outside GPU visible memory. The right address
    // is used only for the top screen in stereoscopic mode and when it is not zero.
    // Otherwise the left address is used for both eyes.
    const auto phys_address_left = VirtualToPhysicalAddress(info.address_left, info.stride);
    if (!phys_address_left) {
        return;
    }

    // Update framebuffer properties.
    auto& framebuffer = impl->pica.regs.framebuffer_config[screen_id];
    if (screen_id == 0) {
        const bool stereoscopic = (info.format & 0x70) == 0x20;
        const PAddr phys_address_right =
            (!stereoscopic || info.address_right == 0)
                ? *phys_address_left
                : VirtualToPhysicalAddress(info.address_right, info.stride).value_or(0);
        if (info.active_fb == 0) {
            framebuffer.address_right1 = phys_address_right;
        } else {
            framebuffer.address_right2 = phys_address_right;
        }
    }
    if (info.active_fb == 0) {
        framebuffer.address_left1 = *phys_address_left;
    } else {
        framebuffer.address_left2 = *phys_address_left;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = (framebuffer.format & 0xFFFF0000) | (info.format & 0xFFFF);
    framebuffer.active_fb = info.shown_fb & 0xFF;

    // Notify debugger about the buffer swap.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::BufferSwapped, nullptr);
    }

    if (screen_id == 0) {
        MicroProfileFlip();
        impl->system.perf_stats->EndGameFrame();
        right_eye_disabler->ReportEndFrame();
    }
}

void GPU::SetColorFill(const Pica::ColorFill& fill) {
    impl->pica.regs_lcd.color_fill_top = fill;
    impl->pica.regs_lcd.color_fill_bottom = fill;
}

u32 GPU::ReadReg(VAddr addr) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        return impl->pica.regs_lcd[index];
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        return impl->pica.regs.reg_array[index];
    }
    default:
        UNREACHABLE_MSG("Read from unknown GPU address {:#08X}", addr);
    }
}

void GPU::WriteReg(VAddr addr, u32 data) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        impl->pica.regs_lcd[index] = data;
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        impl->pica.regs.reg_array[index] = data;

        // Handle registers that trigger GPU actions
        switch (index) {
        case GPU_REG_INDEX(memory_fill_config[0].trigger):
            MemoryFill(0, 0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, 1);
            break;
        case GPU_REG_INDEX(display_transfer_config.trigger):
            MemoryTransfer();
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0]):
            SubmitCmdList(0);
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[1]):
            SubmitCmdList(1);
            break;
        default:
            break;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("Write to unknown GPU address {:#08X}", addr);
    }
}

bool GPU::HasRenderer() const {
    return impl->renderer != nullptr;
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

Pica::PicaCore& GPU::PicaCore() {
    return impl->pica;
}

const Pica::PicaCore& GPU::PicaCore() const {
    return impl->pica;
}

GraphicsDebugger& GPU::Debugger() {
    return impl->gpu_debugger;
}

void GPU::ApplyPerProgramSettings(u64 program_ID) {
    auto hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_MULTIPLICATION, program_ID);
    bool use_accurate_mul = Settings::values.shaders_accurate_mul.GetValue();
    if (hack) {
        switch (hack->mode) {
        case Common::Hacks::HackAllowMode::DISALLOW:
            use_accurate_mul = false;
            break;
        case Common::Hacks::HackAllowMode::FORCE:
            use_accurate_mul = true;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->rasterizer->SetAccurateMul(use_accurate_mul);
}

void GPU::SubmitCmdList(u32 index) {
    // Check if a command list was triggered.
    auto& config = impl->pica.regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_CmdlistProcessing);

    // Forward command list processing to the PICA core.
    const PAddr addr = config.GetPhysicalAddress(index);
    const u32 size = config.GetSize(index);

    // Do not process the list if the memory range is not sane.
    auto& memory = impl->system.Memory();
    const u8* head = memory.GetPhysicalPointer(addr);
    if (!head || (size != 0 && memory.GetPhysicalPointer(addr + size - 1) != head + size - 1)) {
        LOG_ERROR(HW_GPU, "Invalid command list address 0x{:08X} (size 0x{:X})", addr, size);
        impl->signal_interrupt(Service::GSP::InterruptId::P3D, 0);
        config.trigger[index] = 0;
        return;
    }

    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
    config.trigger[index] = 0;
}

void GPU::MemoryFill(u32 index, u32 intr_index) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }

    if (Zeste::Gx::Enabled()) [[unlikely]] {
        ZesteDeferMemoryFill(index, intr_index);
        return;
    }

    // Perform memory fill.
    const u64 delay = PerformMemoryFill(config);

    // It seems that it won't signal interrupt if "address_start" is zero.
    // TODO: hwtest this
    if (config.GetStartAddress() != 0) {
        if (intr_index == 0) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC0, delay);
        } else if (intr_index == 1) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC1, delay);
        }
    }

    // Reset "trigger" flag and set the "finish" flag
    // This was confirmed to happen on hardware even if "address_start" is zero.
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

u64 GPU::PerformMemoryFill(const Pica::MemoryFillConfig& config) {
    if (Zeste::Cache::Enabled()) [[unlikely]] {
        // Written by the GPU: device reads trust it, CPU reads need an invalidate.
        const PAddr start = config.GetStartAddress();
        const PAddr end = config.GetEndAddress();
        if (start != 0 && end > start) {
            Zeste::Cache::OnDeviceWrite(impl->memory, start, end - start);
        }
    }
    if (!impl->rasterizer->AccelerateFill(config)) {
        impl->sw_blitter->MemoryFill(config);
    }
    return FillDelayNanoseconds(config);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }

    if (Zeste::Gx::Enabled()) [[unlikely]] {
        ZesteDeferTransfer();
        return;
    }

    if (Zeste::Cache::Enabled()) [[unlikely]] {
        CheckTransferSourceFlushed(impl->memory, config);
    }
    const u64 delay = PerformTransfer(config);

    // Complete transfer.
    config.trigger.Assign(0);
    impl->signal_interrupt(Service::GSP::InterruptId::PPF, delay);
}

u64 GPU::PerformTransfer(Pica::DisplayTransferConfig& config) {
    MICROPROFILE_SCOPE(GPU_DisplayTransfer);

    // Notify debugger about the display transfer.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::IncomingDisplayTransfer, nullptr);
    }

    if (Zeste::Cache::Enabled()) [[unlikely]] {
        Zeste::Cache::OnDeviceWrite(
            impl->memory, config.GetPhysicalOutputAddress(),
            config.is_texture_copy
                ? static_cast<u32>(config.texture_copy.size)
                : config.output_width * config.output_height * BytesPerPixel(config.output_format));
    }

    // Perform memory transfer
    if (config.is_texture_copy) {
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            impl->sw_blitter->TextureCopy(config);
        }
    } else {
        if (right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                           config.input_height)) {
            if (!impl->rasterizer->AccelerateDisplayTransfer(config)) {
                impl->sw_blitter->DisplayTransfer(config);
            }
        }
    }
    return TransferDelayNanoseconds(config);
}

void GPU::ZesteDeferTransfer() {
    // The trigger bit stays set (busy) until the transfer completes.
    auto& config = impl->pica.regs.display_transfer_config;
    auto& z = *zeste_gx;
    if (Zeste::Cache::Enabled()) [[unlikely]] {
        // The source must be flushed before the trigger, not before the completion.
        CheckTransferSourceFlushed(impl->memory, config);
    }
    ZesteGx::Op op{};
    op.is_fill = false;
    op.transfer = config;
    op.interrupt = Service::GSP::InterruptId::PPF;
    // One transfer engine: a transfer triggered while another runs starts after it.
    const s64 start = z.StartTicks(impl->timing.GetTicks(), z.transfer_free);
    op.done_ticks = start + nsToCycles(TransferDelayNanoseconds(config));
    z.transfer_free = op.done_ticks;
    z.Schedule(*this, op, config.GetPhysicalOutputAddress(), TransferOutputSpan(config),
               config.is_texture_copy ? "texture copy" : "display transfer");
}

void GPU::ZesteDeferMemoryFill(u32 index, u32 intr_index) {
    // The trigger bit stays set (busy) until the fill completes.
    auto& config = impl->pica.regs.memory_fill_config[index];
    auto& z = *zeste_gx;
    ZesteGx::Op op{};
    op.is_fill = true;
    op.fill = config;
    op.fill_index = index;
    // It seems that it won't signal interrupt if "address_start" is zero.
    if (config.GetStartAddress() != 0 && intr_index <= 1) {
        op.interrupt =
            intr_index == 0 ? Service::GSP::InterruptId::PSC0 : Service::GSP::InterruptId::PSC1;
    }
    op.group = z.fill_group;
    const s64 start = z.StartTicks(impl->timing.GetTicks(), z.fill_free[index]);
    op.done_ticks = start + nsToCycles(FillDelayNanoseconds(config));
    z.fill_free[index] = op.done_ticks;
    const PAddr dst = config.GetStartAddress();
    const PAddr dst_end = config.GetEndAddress();
    z.Schedule(*this, op, dst, dst_end > dst ? dst_end - dst : 0, "memory fill");
}

void GPU::ZesteCompleteOp(u64 id) {
    auto& z = *zeste_gx;
    const auto it =
        std::find_if(z.ops.begin(), z.ops.end(), [id](const ZesteGx::Op& o) { return o.id == id; });
    if (it == z.ops.end()) {
        return; // Not ours any more (a savestate was loaded while it was pending)
    }
    ZesteGx::Op op = *it;
    z.ops.erase(it);
    Zeste::Gx::RemovePending(impl->system, id);

    const auto same_engine_pending = [&] {
        return std::any_of(z.ops.begin(), z.ops.end(), [&](const ZesteGx::Op& o) {
            return o.is_fill == op.is_fill && (!op.is_fill || o.fill_index == op.fill_index);
        });
    };
    if (op.is_fill) {
        PerformMemoryFill(op.fill);
        if (!same_engine_pending()) {
            auto& config = impl->pica.regs.memory_fill_config[op.fill_index];
            config.trigger.Assign(0);
            config.finished.Assign(1);
        }
    } else {
        PerformTransfer(op.transfer);
        if (!same_engine_pending()) {
            impl->pica.regs.display_transfer_config.trigger.Assign(0);
        }
    }

    // The interrupt fires once the data has landed; for a two-buffer fill, after both fills.
    if (op.interrupt && op.group != 0) {
        const auto other = std::find_if(z.ops.begin(), z.ops.end(), [&](const ZesteGx::Op& o) {
            return o.group == op.group;
        });
        if (other != z.ops.end()) {
            other->interrupt = op.interrupt;
            op.interrupt.reset();
        }
    }
    if (op.interrupt) {
        impl->signal_interrupt(*op.interrupt, 0);
    }
}

void GPU::ZesteOnP3D(u64 delay_ns) {
    // With the PICA timing model, a command list is done when its P3D interrupt fires.
    if (zeste_gx && Zeste::Gx::Enabled() && Settings::values.zeste_pica_timing.GetValue()) {
        zeste_gx->command_done = std::max(zeste_gx->command_done,
                                          impl->timing.GetTicks() + nsToCycles(delay_ns));
    }
}

bool GPU::ZesteCommandBusy() {
    const s64 now = impl->timing.GetTicks();
    if (zeste_gx->command_done <= now) {
        return false;
    }
    impl->timing.ScheduleEvent(zeste_gx->command_done - now, zeste_gx->resume_event);
    return true;
}

void GPU::SetVBlankHook(std::function<void()> hook) {
    vblank_hook = std::move(hook);
}

void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
    // Signal to GSP that GPU interrupt has occurred
    impl->signal_interrupt(Service::GSP::InterruptId::PDC0, 0);
    impl->signal_interrupt(Service::GSP::InterruptId::PDC1, 0);

    // Present renderered frame.
    impl->renderer->SwapBuffers();

    impl->pica.ZesteOnVBlank();
    if (vblank_hook) {
        vblank_hook();
    }

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(FRAME_TICKS - cycles_late, impl->vblank_event);
}

void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
    // Reset the renderer (this will destroy OpenGL resources)
    impl->renderer.reset();

    // Create a new renderer
    impl->renderer =
        VideoCore::CreateRenderer(emu_window, secondary_window, impl->pica, impl->system);
    impl->rasterizer = impl->renderer->Rasterizer();

    // Rebind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    // Update the sw_blitter with the new rasterizer
    impl->sw_blitter = std::make_unique<SwRenderer::SwBlitter>(impl->memory, impl->rasterizer);

    // Re-apply per-game configuration and reload disk shader cache
    u64 program_id{};
    impl->system.GetAppLoader().ReadProgramId(program_id);
    ApplyPerProgramSettings(program_id);
    if (Settings::values.use_disk_shader_cache) {
        impl->renderer->Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    }

    // Mark ALL GPU registers as dirty so current state gets uploaded to new renderer
    impl->pica.dirty_regs.SetAllDirty();

    // Also mark shader setups as dirty so uniforms get re-uploaded and
    // stale pointers to the old rasterizer's JIT cache are cleared.
    impl->pica.vs_setup.uniforms_dirty = true;
    impl->pica.vs_setup.cached_shader = nullptr;
    impl->pica.gs_setup.uniforms_dirty = true;
    impl->pica.gs_setup.cached_shader = nullptr;

    // Mark all cached LUT/table state in pica as dirty
    impl->pica.lighting.lut_dirty = impl->pica.lighting.LutAllDirty;
    impl->pica.fog.lut_dirty = true;
    impl->pica.proctex.table_dirty = impl->pica.proctex.TableAllDirty;
}

void GPU::ReleaseRenderer() {
    // Just reset the renderer to release OpenGL resources
    // Don't null out rasterizer pointer as it will become dangling
    impl->renderer.reset();
    impl->sw_blitter.reset();
    LOG_INFO(HW_GPU, "Renderer released for context destroy");
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    ar & impl->pica;
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore
