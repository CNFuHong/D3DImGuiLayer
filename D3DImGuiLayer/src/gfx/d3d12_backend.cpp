#include "gfx/d3d12_backend.h"

#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

#include <algorithm>

namespace d3d_imgui_layer::gfx
{
d3d12_backend::~d3d12_backend()
{
    release();
}

unsigned int d3d12_backend::api() const
{
    return d3dimgui_graphics_d3d12;
}

const char* d3d12_backend::name() const
{
    return "direct3d 12";
}

void* d3d12_backend::native_device() const
{
    return device_.Get();
}

bool d3d12_backend::read_desc()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain_->GetDesc(&desc)) || desc.BufferCount == 0)
        return false;

    render_target_format_ = desc.BufferDesc.Format;
    buffer_count_ = desc.BufferCount;
    return true;
}

bool d3d12_backend::initialize(void* surface, void* auxiliary)
{
    auto* swapchain = static_cast<IDXGISwapChain*>(surface);
    auto* queue = static_cast<ID3D12CommandQueue*>(auxiliary);
    // 队列拿不到就等着 上层下一帧还会重试 这是 dx12 唯一需要蹲点的东西
    if (swapchain == nullptr || queue == nullptr)
        return false;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> probe;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&probe))) || probe == nullptr)
    {
        log_error("swapchain is not idxgiswapchain3, cannot run d3d12 backend");
        return false;
    }
    // 借一下探测接口 探完就放 真正留存的只有裸指针
    swapchain_ = probe.Get();

    if (FAILED(swapchain_->GetDevice(IID_PPV_ARGS(&device_))))
    {
        release();
        return false;
    }

    queue_ = queue;

    if (!read_desc())
    {
        release();
        return false;
    }

    frames_.resize(buffer_count_);

    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) ||
        !create_descriptor_heaps() ||
        !create_frame_resources())
    {
        release();
        return false;
    }

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
             frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&command_list_))))
    {
        release();
        return false;
    }
    command_list_->Close();

    if (!bind_imgui())
    {
        release();
        return false;
    }

    log_info("d3d12 backend ready, buffers=%u format=%u queue=%p",
        buffer_count_, static_cast<unsigned>(render_target_format_), queue_.Get());
    return true;
}

bool d3d12_backend::create_descriptor_heaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = static_cast<UINT>(frames_.size());
    if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&render_target_heap_))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = d3dimgui_srv_slots;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_))))
        return false;

    render_target_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv_slots_.assign(d3dimgui_srv_slots, false);
    return true;
}

bool d3d12_backend::create_frame_resources()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = render_target_heap_->GetCPUDescriptorHandleForHeapStart();

    for (UINT index = 0; index < static_cast<UINT>(frames_.size()); ++index)
    {
        frame_context& frame = frames_[index];
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                 IID_PPV_ARGS(&frame.allocator))) ||
            FAILED(swapchain_->GetBuffer(index, IID_PPV_ARGS(&frame.backbuffer))))
        {
            return false;
        }

        frame.render_target = handle;
        device_->CreateRenderTargetView(frame.backbuffer.Get(), nullptr, frame.render_target);
        handle.ptr += render_target_descriptor_size_;
    }

    return true;
}

bool d3d12_backend::bind_imgui()
{
    ImGui_ImplDX12_InitInfo info{};
    info.Device = device_.Get();
    info.CommandQueue = queue_.Get();
    info.NumFramesInFlight = static_cast<int>(frames_.size());
    info.RTVFormat = render_target_format_;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = this;
    info.SrvDescriptorHeap = srv_heap_.Get();
    info.SrvDescriptorAllocFn = allocate_srv;
    info.SrvDescriptorFreeFn = free_srv;

    imgui_bound_ = ImGui_ImplDX12_Init(&info);
    return imgui_bound_;
}

void d3d12_backend::allocate_srv(ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* gpu_handle)
{
    d3d12_backend* self = static_cast<d3d12_backend*>(info->UserData);
    std::lock_guard lock(self->srv_mutex_);

    const auto free_slot = std::find(self->srv_slots_.begin(), self->srv_slots_.end(), false);
    if (free_slot == self->srv_slots_.end())
    {
        // 槽位不够时给回 0 号 会覆盖字体图集 但至少不会崩 真出现就调大 srv_slots
        log_warn("srv heap exhausted, raise d3dimgui_srv_slots");
        cpu_handle->ptr = self->srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
        gpu_handle->ptr = self->srv_heap_->GetGPUDescriptorHandleForHeapStart().ptr;
        return;
    }

    const std::size_t index = static_cast<std::size_t>(std::distance(self->srv_slots_.begin(), free_slot));
    self->srv_slots_[index] = true;

    *cpu_handle = self->srv_heap_->GetCPUDescriptorHandleForHeapStart();
    *gpu_handle = self->srv_heap_->GetGPUDescriptorHandleForHeapStart();
    cpu_handle->ptr += index * self->srv_descriptor_size_;
    gpu_handle->ptr += index * self->srv_descriptor_size_;
}

void d3d12_backend::free_srv(ImGui_ImplDX12_InitInfo* info,
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    d3d12_backend* self = static_cast<d3d12_backend*>(info->UserData);
    std::lock_guard lock(self->srv_mutex_);

    const D3D12_CPU_DESCRIPTOR_HANDLE start = self->srv_heap_->GetCPUDescriptorHandleForHeapStart();
    if (cpu_handle.ptr < start.ptr || self->srv_descriptor_size_ == 0)
        return;

    const std::size_t index = (cpu_handle.ptr - start.ptr) / self->srv_descriptor_size_;
    if (index < self->srv_slots_.size())
        self->srv_slots_[index] = false;
}

void d3d12_backend::new_frame()
{
    if (swapchain_ != nullptr)
        buffer_index_ = swapchain_->GetCurrentBackBufferIndex();

    ImGui_ImplDX12_NewFrame();
}

// 等不到就返回 false 让这一帧不画 绝不能长时间堵在宿主的渲染线程上
bool d3d12_backend::wait_frame(frame_context& frame)
{
    if (frame.fence_value == 0 || fence_->GetCompletedValue() >= frame.fence_value)
        return true;

    if (FAILED(fence_->SetEventOnCompletion(frame.fence_value, fence_event_)))
        return false;

    if (WaitForSingleObject(fence_event_, 250) == WAIT_OBJECT_0)
        return true;

    log_warn("gpu did not retire frame %u in time, skipping this overlay frame",
        (unsigned)frame.fence_value);
    return false;
}

// 等到 gpu 把最后一个 fence 走完才返回 true
// 返回 false 说明队列不回话 调用方要么丢帧 要么放弃释放对象
bool d3d12_backend::wait_gpu(unsigned long timeout_ms)
{
    if (queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr)
        return queue_ == nullptr;

    const std::uint64_t value = ++next_fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), value)))
        return false;
    if (fence_->GetCompletedValue() >= value)
        return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fence_event_)))
        return false;

    return WaitForSingleObject(fence_event_, timeout_ms) == WAIT_OBJECT_0;
}

void d3d12_backend::render(ImDrawData* draw_data)
{
    if (swapchain_ == nullptr || command_list_ == nullptr || frames_.empty() || draw_data == nullptr)
        return;

    if (buffer_index_ >= static_cast<unsigned int>(frames_.size()))
        return;

    frame_context& frame = frames_[buffer_index_];
    if (!wait_frame(frame))
        return;

    if (FAILED(frame.allocator->Reset()) || FAILED(command_list_->Reset(frame.allocator.Get(), nullptr)))
        return;

    D3D12_RESOURCE_BARRIER to_target{};
    to_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_target.Transition.pResource = frame.backbuffer.Get();
    to_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list_->ResourceBarrier(1, &to_target);

    command_list_->OMSetRenderTargets(1, &frame.render_target, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    command_list_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw_data, command_list_.Get());

    D3D12_RESOURCE_BARRIER to_present = to_target;
    std::swap(to_present.Transition.StateBefore, to_present.Transition.StateAfter);
    command_list_->ResourceBarrier(1, &to_present);

    if (FAILED(command_list_->Close()))
        return;

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);

    const std::uint64_t value = ++next_fence_value_;
    if (SUCCEEDED(queue_->Signal(fence_.Get(), value)))
        frame.fence_value = value;
}

void d3d12_backend::destroy_frame_resources()
{
    for (frame_context& frame : frames_)
    {
        frame.backbuffer.Reset();
        frame.allocator.Reset();
        frame.fence_value = 0;
    }
    frames_.clear();
    buffer_count_ = 0;
}

void d3d12_backend::pre_reset()
{
    // resize 之前必须让 gpu 停下来 热区要重建 带着引用释放就是设备误用
    if (!wait_gpu(2000))
        log_warn("queue did not retire before reset, continuing anyway");

    if (imgui_bound_)
    {
        ImGui_ImplDX12_Shutdown();
        imgui_bound_ = false;
    }

    command_list_.Reset();
    destroy_frame_resources();
    render_target_heap_.Reset();
}

bool d3d12_backend::post_reset()
{
    if (swapchain_ == nullptr || device_ == nullptr || !read_desc())
    {
        log_error("post reset lost the device or the desc");
        return false;
    }

    frames_.resize(buffer_count_);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = buffer_count_;
    if (FAILED(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&render_target_heap_))))
    {
        log_error("post reset could not recreate the rtv heap");
        return false;
    }

    if (!create_frame_resources())
    {
        log_error("post reset could not recreate frame resources");
        return false;
    }

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&command_list_))))
    {
        log_error("post reset could not recreate the command list");
        return false;
    }

    command_list_->Close();
    if (!bind_imgui())
    {
        log_error("post reset could not rebind imgui");
        return false;
    }

    log_info("d3d12 post reset done, buffers=%u", buffer_count_);
    return true;
}

unsigned int d3d12_backend::buffer_index() const
{
    return buffer_index_;
}

unsigned int d3d12_backend::buffer_count() const
{
    return buffer_count_;
}

void d3d12_backend::discard_frame_resources()
{
    // gpu 还没跑完的时候走这里 对象大概率还被队列引用着
    // 这时候 release 就是设备误用 运行时直接把宿主设备判死
    // 丢掉引用只泄漏几 mb 判死设备是把整个进程带走
    for (frame_context& frame : frames_)
    {
        frame.backbuffer.Detach();
        frame.allocator.Detach();
        frame.fence_value = 0;
    }
    frames_.clear();
    buffer_count_ = 0;
}

void d3d12_backend::release()
{
    const bool healthy = device_ == nullptr || SUCCEEDED(device_->GetDeviceRemovedReason());

    // 设备还活着就必须等最后一个 fence 走完 不等就是拿 in-flight 的资源去释放
    // 设备已经没了或者队列早就不在了 等下去只会把这条线程挂死 直接放手
    // 预算压到 400ms 拆资源可能跑在宿主的窗口线程上 超时就走泄漏而不是干等
    const bool retired = !healthy || wait_gpu(400);
    if (!retired)
        log_warn("queue did not retire in time, leaking d3d12 objects instead of freeing them");

    if (imgui_bound_)
    {
        ImGui_ImplDX12_Shutdown();
        imgui_bound_ = false;
    }

    if (retired)
    {
        command_list_.Reset();
        destroy_frame_resources();
        render_target_heap_.Reset();
        srv_heap_.Reset();
        fence_.Reset();
    }
    else
    {
        command_list_.Detach();
        discard_frame_resources();
        render_target_heap_.Detach();
        srv_heap_.Detach();
        fence_.Detach();
    }

    queue_.Reset();
    device_.Reset();
    swapchain_ = nullptr;
    srv_slots_.clear();
    buffer_index_ = 0;

    if (fence_event_ != nullptr)
    {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}
}
