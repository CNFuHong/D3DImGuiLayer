#pragma once
#ifndef d3dimgui_d3d11_backend_h_
#define d3dimgui_d3d11_backend_h_

#include "gfx/backend.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vector>

namespace d3d_imgui_layer::gfx
{
class d3d11_backend final : public backend
{
public:
    ~d3d11_backend() override;

    unsigned int api() const override;
    const char* name() const override;
    void* native_device() const override;

    bool initialize(void* surface, void* auxiliary) override;
    void new_frame() override;
    void render(ImDrawData* draw_data) override;

    void pre_reset() override;
    bool post_reset() override;

    unsigned int buffer_index() const override;
    unsigned int buffer_count() const override;

    void release() override;

private:
    bool read_desc();
    unsigned int current_index() const;

    // 只借不攥 引用留在宿主手里
    // 游戏换全屏或者改分辨率时要重建这条链 我们多攥一个引用它就来不及放干净
    // 一个窗口同时只能挂一条 flip 链 放不掉就是 new 的时候收到 invalid_call
    IDXGISwapChain* swapchain_ = nullptr;
    IDXGISwapChain3* chain3_ = nullptr;   // 借来的 只有 flip 链才有值
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    unsigned int buffer_index_ = 0;
    unsigned int buffer_count_ = 1;
    bool flip_model_ = false;
    bool imgui_bound_ = false;
};
}

#endif
