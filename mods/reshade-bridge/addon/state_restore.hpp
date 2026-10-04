// Puts Dawn's Direct3D 12 command-list state back after the add-on has recorded ReShade's work into
// the middle of it.
//
// What needs restoring, and why only this (dawn/native/d3d12/CommandBufferD3D12.cpp at the pinned
// aurora's Dawn): the add-on runs between WebGPU passes, never inside one. Every pass Dawn records
// afterwards sets its own pipeline state, primitive topology, render targets, viewport, scissor,
// blend factor, stencil reference, vertex and index buffers. What Dawn does NOT set again is what
// it tracks across the passes of a command buffer:
//   - the descriptor heaps (set once per command buffer, DescriptorHeapState),
//   - the graphics and compute root signatures (BindGroupStateTracker skips SetRootSignature while
//     the pipeline layout is unchanged, AreLayoutsCompatible),
//   - the root arguments bound under them: descriptor tables, root constants, and root
//     CBV/SRV/UAV addresses (a bind group that is set again unchanged is not re-applied).
// ReShade's render_technique changes all of these and, on D3D12, restores none of them (its
// capture_state/apply_state handle D3D9 to D3D11 and OpenGL only).
//
// So this file tracks those three things per command list from ReShade's events (fired by its
// command-list proxy as Dawn records) and re-applies them natively afterwards. The descriptor heaps
// cannot be read back; ReShade's own command list remembers the application's pair and puts it back
// when bind_descriptor_tables is called with no tables, provided both bound heaps differ from the
// application's. restore_state() first binds two tables of its own (one from each of ReShade's
// shader-visible heaps) to guarantee that, then makes that call.

#pragma once

#include <cstdint>
#include <vector>

#include <reshade.hpp>

namespace drb_addon {

enum class RootKind : uint8_t { None, Table, Constants, Cbv, Srv, Uav };

struct RootArgument {
    RootKind kind = RootKind::None;
    uint64_t value = 0;           // GPU descriptor handle (Table), GPU virtual address (Srv/Uav)
    uint64_t cbv_resource = 0;    // Cbv: buffer resource (ID3D12Resource*) and offset into it
    uint64_t cbv_offset = 0;
    std::vector<uint32_t> constants; // Constants: values by 32-bit offset
    std::vector<uint8_t> constants_set;
};

struct BindPointState {
    uint64_t root_signature = 0; // ID3D12RootSignature*, 0 = none set in this command list
    std::vector<RootArgument> args;

    RootArgument& arg(uint32_t index);
    void reset(uint64_t signature);
};

// Per command list (ReShade private data).
class __declspec(uuid("6f0d3a52-9c1e-4b7a-a6d4-3b2e8f71c5d9")) CommandListState {
public:
    BindPointState graphics;
    BindPointState compute;
};

// Per D3D12 device: a root signature and two descriptor tables of the add-on's own, used to force
// ReShade's heaps to be the current ones (see the header comment).
class __declspec(uuid("1a7e4c90-5b3d-4e2f-8c61-d09f2b7a4e38")) DeviceRestoreObjects {
public:
    reshade::api::pipeline_layout layout = {0};
    reshade::api::descriptor_table tables[2] = {};
    bool valid = false;
};

void register_state_tracking();
void unregister_state_tracking();

// While a scope like this is alive on a thread, the trackers ignore that thread's events (the
// add-on's own recording must not be mistaken for Dawn's).
struct TrackingPause {
    TrackingPause();
    ~TrackingPause();
};

// Re-applies the descriptor heaps, root signatures and root arguments Dawn last set on `cmd_list`.
void restore_state(reshade::api::command_list* cmd_list);

} // namespace drb_addon
