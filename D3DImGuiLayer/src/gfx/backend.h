#pragma once
#ifndef d3dimgui_backend_h_
#define d3dimgui_backend_h_

#include <d3d_imgui_layer/d3dimgui.h>

#include <dxgi.h>

#include <memory>

struct ImDrawData;
struct ID3D12CommandQueue;

namespace d3d_imgui_layer::gfx
{
// 一个后端只管三件事 绑定目标 提交帧 响应重建
// imgui 上下文由上层持有 后端只调 imgui 官方 backend 的那几个函数
class backend
{
public:
    virtual ~backend() = default;

    virtual unsigned int api() const = 0;
    virtual const char* name() const = 0;
    virtual void* native_device() const = 0;

    // surface 是提交用的那个东西 d3d11 和 d3d12 是一条交换链 opengl 是一个 hdc
    // auxiliary 只有 d3d12 用 是它的 direct 队列 拿不到就返回 false 让上层下一帧重试
    virtual bool initialize(void* surface, void* auxiliary) = 0;

    virtual void new_frame() = 0;
    virtual void render(ImDrawData* draw_data) = 0;

    // resizebuffers 前必须放掉所有指向旧 backbuffer 的资源
    virtual void pre_reset() = 0;
    virtual bool post_reset() = 0;

    virtual unsigned int buffer_index() const = 0;
    virtual unsigned int buffer_count() const = 0;

    virtual void release() = 0;
};

std::unique_ptr<backend> create_backend(unsigned int api);
}

#endif
