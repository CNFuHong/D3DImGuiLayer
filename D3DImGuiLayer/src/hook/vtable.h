#pragma once
#ifndef d3dimgui_vtable_h_
#define d3dimgui_vtable_h_

namespace d3d_imgui_layer::hook
{
// 一个 swapchain 实现里我们要的三个入口
struct swapchain_slots
{
    void* present        = nullptr; // vtable 8
    void* present1       = nullptr; // vtable 22 只在 IDXGISwapChain1 之后存在
    void* resize_buffers = nullptr; // vtable 13
};

// 造一个临时 d3d11 device 和 swapchain 只为了把 vtable 地址读出来
// 不碰游戏对象 所以不需要任何偏移
bool resolve_d3d11(swapchain_slots& out);

// 同上 d3d12 额外产出 command queue 的 ExecuteCommandLists vtable 10
bool resolve_d3d12(swapchain_slots& out, void*& execute_command_lists);
}

#endif
