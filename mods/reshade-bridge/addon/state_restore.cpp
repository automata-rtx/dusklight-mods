#include <d3d12.h>

#include "state_restore.hpp"

using namespace reshade::api;

namespace drb_addon {
namespace {

thread_local int t_pause = 0;

bool is_compute(shader_stage stages) { return (stages & shader_stage::compute) != 0; }

CommandListState* state_of(command_list* cmd_list) {
    if (t_pause != 0 || cmd_list->get_device()->get_api() != device_api::d3d12) {
        return nullptr;
    }
    CommandListState* state = cmd_list->get_private_data<CommandListState>();
    // Command lists created before the add-on loaded never saw init_command_list.
    return state != nullptr ? state : cmd_list->create_private_data<CommandListState>();
}

BindPointState& bind_point(CommandListState& state, shader_stage stages, pipeline_layout layout) {
    BindPointState& bp = is_compute(stages) ? state.compute : state.graphics;
    if (layout.handle != bp.root_signature) {
        bp.reset(layout.handle); // a new root signature invalidates every root argument
    }
    return bp;
}

RootArgument& reset_arg(BindPointState& bp, uint32_t index, RootKind kind) {
    RootArgument& a = bp.arg(index);
    if (a.kind != kind) {
        a = RootArgument{};
        a.kind = kind;
    }
    return a;
}

// --- Events (Dawn's recording, through ReShade's command-list proxy) -----------------------------

void on_init_command_list(command_list* cmd_list) {
    if (cmd_list->get_device()->get_api() == device_api::d3d12 && cmd_list->get_private_data<CommandListState>() == nullptr) {
        cmd_list->create_private_data<CommandListState>();
    }
}

void on_destroy_command_list(command_list* cmd_list) {
    if (cmd_list->get_private_data<CommandListState>() != nullptr) {
        cmd_list->destroy_private_data<CommandListState>();
    }
}

void on_reset_command_list(command_list* cmd_list) {
    if (CommandListState* s = state_of(cmd_list)) {
        s->graphics.reset(0);
        s->compute.reset(0);
    }
}

// Also fired with no tables by SetGraphicsRootSignature / SetComputeRootSignature.
void on_bind_descriptor_tables(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t first,
    uint32_t count, const descriptor_table* tables, uint32_t, const uint32_t*) {
    CommandListState* s = state_of(cmd_list);
    if (s == nullptr) {
        return;
    }
    if (count == 0) {
        (is_compute(stages) ? s->compute : s->graphics).reset(layout.handle);
        return;
    }
    BindPointState& bp = bind_point(*s, stages, layout);
    for (uint32_t i = 0; i < count; ++i) {
        reset_arg(bp, first + i, RootKind::Table).value = tables[i].handle;
    }
}

void on_push_constants(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
    uint32_t first, uint32_t count, const void* values) {
    CommandListState* s = state_of(cmd_list);
    if (s == nullptr || values == nullptr) {
        return;
    }
    RootArgument& a = reset_arg(bind_point(*s, stages, layout), param, RootKind::Constants);
    if (a.constants.size() < first + count) {
        a.constants.resize(first + count, 0);
        a.constants_set.resize(first + count, 0);
    }
    const auto* v = static_cast<const uint32_t*>(values);
    for (uint32_t i = 0; i < count; ++i) {
        a.constants[first + i] = v[i];
        a.constants_set[first + i] = 1;
    }
}

// Root CBV/SRV/UAV. ReShade reports a CBV as a buffer range and an SRV/UAV as the raw GPU virtual
// address (d3d12_command_list.cpp, Set*Root*View).
void on_push_descriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
    const descriptor_table_update& update) {
    CommandListState* s = state_of(cmd_list);
    if (s == nullptr || update.descriptors == nullptr || update.count != 1) {
        return;
    }
    BindPointState& bp = bind_point(*s, stages, layout);
    switch (update.type) {
    case descriptor_type::constant_buffer: {
        const auto* range = static_cast<const buffer_range*>(update.descriptors);
        RootArgument& a = reset_arg(bp, param, RootKind::Cbv);
        a.cbv_resource = range->buffer.handle;
        a.cbv_offset = range->offset;
        break;
    }
    case descriptor_type::buffer_shader_resource_view:
    case descriptor_type::acceleration_structure:
        reset_arg(bp, param, RootKind::Srv).value = *static_cast<const uint64_t*>(update.descriptors);
        break;
    case descriptor_type::buffer_unordered_access_view:
        reset_arg(bp, param, RootKind::Uav).value = *static_cast<const uint64_t*>(update.descriptors);
        break;
    default:
        break;
    }
}

void on_init_device(device* dev) {
    if (dev->get_api() != device_api::d3d12) {
        return;
    }
    DeviceRestoreObjects& ro = *dev->create_private_data<DeviceRestoreObjects>();
    descriptor_range ranges[2];
    ranges[0].count = 1;
    ranges[0].visibility = shader_stage::pixel;
    ranges[0].type = descriptor_type::shader_resource_view;
    ranges[1].count = 1;
    ranges[1].visibility = shader_stage::pixel;
    ranges[1].type = descriptor_type::sampler;
    const pipeline_layout_param params[2] = {pipeline_layout_param(1, &ranges[0]), pipeline_layout_param(1, &ranges[1])};
    ro.valid = dev->create_pipeline_layout(2, params, &ro.layout) &&
               dev->allocate_descriptor_tables(1, ro.layout, 0, &ro.tables[0]) &&
               dev->allocate_descriptor_tables(1, ro.layout, 1, &ro.tables[1]);
    if (!ro.valid) {
        reshade::log::message(reshade::log::level::warning,
            "Dusklight bridge: could not create the objects that restore descriptor heaps; the bridge will not run");
    }
}

void on_destroy_device(device* dev) {
    DeviceRestoreObjects* ro = dev->get_private_data<DeviceRestoreObjects>();
    if (ro == nullptr) {
        return;
    }
    for (const descriptor_table& t : ro->tables) {
        if (t.handle != 0) {
            dev->free_descriptor_tables(1, &t);
        }
    }
    if (ro->layout.handle != 0) {
        dev->destroy_pipeline_layout(ro->layout);
    }
    dev->destroy_private_data<DeviceRestoreObjects>();
}

void apply_root_arguments(ID3D12GraphicsCommandList* list, const BindPointState& bp, bool compute) {
    for (uint32_t i = 0; i < bp.args.size(); ++i) {
        const RootArgument& a = bp.args[i];
        switch (a.kind) {
        case RootKind::Table: {
            const D3D12_GPU_DESCRIPTOR_HANDLE h = {a.value};
            compute ? list->SetComputeRootDescriptorTable(i, h) : list->SetGraphicsRootDescriptorTable(i, h);
            break;
        }
        case RootKind::Constants: {
            // Contiguous runs of the values Dawn set.
            const uint32_t n = static_cast<uint32_t>(a.constants.size());
            for (uint32_t first = 0; first < n;) {
                if (a.constants_set[first] == 0) {
                    ++first;
                    continue;
                }
                uint32_t end = first;
                while (end < n && a.constants_set[end] != 0) {
                    ++end;
                }
                compute ? list->SetComputeRoot32BitConstants(i, end - first, &a.constants[first], first)
                        : list->SetGraphicsRoot32BitConstants(i, end - first, &a.constants[first], first);
                first = end;
            }
            break;
        }
        case RootKind::Cbv: {
            auto* buffer = reinterpret_cast<ID3D12Resource*>(a.cbv_resource);
            if (buffer == nullptr) {
                break;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS va = buffer->GetGPUVirtualAddress() + a.cbv_offset;
            compute ? list->SetComputeRootConstantBufferView(i, va) : list->SetGraphicsRootConstantBufferView(i, va);
            break;
        }
        case RootKind::Srv:
            compute ? list->SetComputeRootShaderResourceView(i, a.value) : list->SetGraphicsRootShaderResourceView(i, a.value);
            break;
        case RootKind::Uav:
            compute ? list->SetComputeRootUnorderedAccessView(i, a.value) : list->SetGraphicsRootUnorderedAccessView(i, a.value);
            break;
        case RootKind::None:
            break;
        }
    }
}

} // namespace

RootArgument& BindPointState::arg(uint32_t index) {
    if (args.size() <= index) {
        args.resize(index + 1);
    }
    return args[index];
}

void BindPointState::reset(uint64_t signature) {
    root_signature = signature;
    args.clear();
}

TrackingPause::TrackingPause() { ++t_pause; }
TrackingPause::~TrackingPause() { --t_pause; }

void register_state_tracking() {
    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
    reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
    reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
    reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
}

void unregister_state_tracking() {
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
    reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
    reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
    reshade::unregister_event<reshade::addon_event::push_constants>(on_push_constants);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
}

void restore_state(command_list* cmd_list) {
    CommandListState* s = cmd_list->get_private_data<CommandListState>();
    DeviceRestoreObjects* ro = cmd_list->get_device()->get_private_data<DeviceRestoreObjects>();
    if (s == nullptr || ro == nullptr || !ro->valid) {
        return;
    }
    TrackingPause pause;

    // 1. Descriptor heaps. Bind one table from each of ReShade's shader-visible heaps so that both
    //    current heaps differ from Dawn's, then bind no tables: ReShade's command list then sets the
    //    heaps Dawn last set (d3d12_impl_command_list.cpp, bind_descriptor_tables2) and the given
    //    root signature. With no root signature tracked, the add-on's own stands in; Dawn has not
    //    applied one in this command list then and sets its own before it binds anything.
    cmd_list->bind_descriptor_tables(shader_stage::all_graphics, ro->layout, 0, 2, ro->tables);
    const pipeline_layout graphics = {s->graphics.root_signature != 0 ? s->graphics.root_signature : ro->layout.handle};
    const pipeline_layout compute = {s->compute.root_signature != 0 ? s->compute.root_signature : ro->layout.handle};
    cmd_list->bind_descriptor_tables(shader_stage::all_graphics, graphics, 0, 0, nullptr);
    cmd_list->bind_descriptor_tables(shader_stage::all_compute, compute, 0, 0, nullptr);

    // 2. Root arguments, natively: ReShade's API cannot express root SRV/UAV addresses, and the
    //    native calls fire no events.
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    apply_root_arguments(list, s->graphics, false);
    apply_root_arguments(list, s->compute, true);
}

} // namespace drb_addon
