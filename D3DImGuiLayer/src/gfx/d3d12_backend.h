#pragma once
#ifndef d3dimgui_d3d12_backend_h_
#define d3dimgui_d3d12_backend_h_

#include "gfx/backend.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <vector>

struct ImGui_ImplDX12_InitInfo;

namespace d3d_imgui_layer::gfx
{
class d3d12_backend final : public backend
{
public:
    ~d3d12_backend() override;

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
    struct frame_context
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE render_target{};
        std::uint64_t fence_value = 0;
    };

    static void allocate_srv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* gpu_handle);
    static void free_srv(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle);

    bool read_desc();
    bool create_descriptor_heaps();
    bool create_frame_resources();
    bool bind_imgui();
    void destroy_frame_resources();
    bool wait_frame(frame_context& frame);
    bool wait_gpu(unsigned long timeout_ms);
    void discard_frame_resources();

    // 同 d3d11 只借不攥 引用留在宿主手里 换链时才放得干净
    IDXGISwapChain3* swapchain_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> render_target_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;

    std::vector<frame_context> frames_;
    std::vector<bool> srv_slots_;
    std::mutex srv_mutex_;

    HANDLE fence_event_ = nullptr;
    DXGI_FORMAT render_target_format_ = DXGI_FORMAT_UNKNOWN;
    unsigned int render_target_descriptor_size_ = 0;
    unsigned int srv_descriptor_size_ = 0;
    unsigned int buffer_index_ = 0;
    unsigned int buffer_count_ = 0;
    std::uint64_t next_fence_value_ = 0;
    bool imgui_bound_ = false;
};
}

#endif
