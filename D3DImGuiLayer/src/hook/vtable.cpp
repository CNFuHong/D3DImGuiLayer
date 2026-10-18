#include "hook/vtable.h"

#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iterator>

namespace
{
using Microsoft::WRL::ComPtr;
using d3d_imgui_layer::log_error;
using d3d_imgui_layer::log_warn;

// 槽位号是 com 的 abi 不是特征 微软改不了 会变的只有自己数错和头文件换版
// 这几个数由 slot_check.c 拿 sdk 的 c vtbl 结构在编译期重新算一遍核死
// 另外挑两个纯读取的方法做运行时自证 采完先证明这张表确实是它
constexpr size_t slot_present = 8;
constexpr size_t slot_present1 = 22;
constexpr size_t slot_resize_buffers = 13;
constexpr size_t slot_execute_command_lists = 10;
constexpr size_t slot_swapchain_get_desc = 12;
constexpr size_t slot_queue_get_desc = 18;

LRESULT CALLBACK dummy_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

// 只为拿到一个合法 hwnd 建完就拆 从不显示也不发消息循环
struct dummy_window
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND handle = nullptr;

    ~dummy_window()
    {
        if (handle != nullptr)
            DestroyWindow(handle);
        UnregisterClassW(d3dimgui_dummy_window_class, instance);
    }

    bool create()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dummy_window_proc;
        wc.hInstance = instance;
        wc.lpszClassName = d3dimgui_dummy_window_class;

        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            log_error("dummy window class failed, gle=%lu", GetLastError());
            return false;
        }

        handle = CreateWindowExW(0, d3dimgui_dummy_window_class, L"d3dimgui dummy",
            WS_OVERLAPPEDWINDOW, 0, 0, d3dimgui_dummy_size, d3dimgui_dummy_size,
            nullptr, nullptr, instance, nullptr);
        return handle != nullptr;
    }
};

void* slot_at(void* object, const size_t index)
{
    if (object == nullptr)
        return nullptr;
    void** table = *reinterpret_cast<void***>(object);
    if (table == nullptr)
        return nullptr;
    return table[index];
}

using swapchain_get_desc_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using queue_get_desc_fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, D3D12_COMMAND_QUEUE_DESC*);

// 走自己算出来的槽位去调 getdesc 而不是走编译器解析出来的接口
// 表认错人或者号数错都在这里露馅 不会拖到宿主进程里炸
__declspec(noinline) HRESULT probe_swapchain_desc(void* object, DXGI_SWAP_CHAIN_DESC* out)
{
    void* fn = slot_at(object, slot_swapchain_get_desc);
    if (fn == nullptr || out == nullptr)
        return E_POINTER;

    HRESULT result = E_FAIL;
    __try
    {
        result = reinterpret_cast<swapchain_get_desc_fn>(fn)(static_cast<IDXGISwapChain*>(object), out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = static_cast<HRESULT>(0xE0000000);
    }
    return result;
}

__declspec(noinline) bool probe_queue_desc(void* object, D3D12_COMMAND_QUEUE_DESC* out)
{
    void* fn = slot_at(object, slot_queue_get_desc);
    if (fn == nullptr || out == nullptr)
        return false;

    bool called = false;
    __try
    {
        reinterpret_cast<queue_get_desc_fn>(fn)(static_cast<ID3D12CommandQueue*>(object), out);
        called = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        called = false;
    }
    return called;
}
}

namespace d3d_imgui_layer::hook
{
bool resolve_d3d11(swapchain_slots& out)
{
    dummy_window window;
    if (!window.create())
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = d3dimgui_dummy_size;
    desc.BufferDesc.Height = d3dimgui_dummy_size;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window.handle;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen{};

    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
        &swapchain, &device, &chosen, &context);

#if d3dimgui_allow_warp
    if (FAILED(result))
    {
        // 没有可用 gpu 的服务或者远程会话走 warp 一样能拿到同一份 dxgi vtable
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
            &swapchain, &device, &chosen, &context);
    }
#endif

    if (FAILED(result) || swapchain == nullptr)
    {
        log_warn("dummy d3d11 device failed, hr=0x%08lx", static_cast<unsigned long>(result));
        return false;
    }

    DXGI_SWAP_CHAIN_DESC probed{};
    const HRESULT probe_hr = probe_swapchain_desc(swapchain.Get(), &probed);
    if (FAILED(probe_hr) || probed.BufferCount < 1 || probed.BufferCount > 8 ||
        probed.OutputWindow != window.handle)
    {
        log_error("d3d11 vtable self check failed, hr=0x%08lx buffers=%u window=%p",
            static_cast<unsigned long>(probe_hr), probed.BufferCount, probed.OutputWindow);
        return false;
    }

    out.present = slot_at(swapchain.Get(), slot_present);
    out.resize_buffers = slot_at(swapchain.Get(), slot_resize_buffers);

    ComPtr<IDXGISwapChain1> swapchain1;
    if (SUCCEEDED(swapchain.As(&swapchain1)))
        out.present1 = slot_at(swapchain1.Get(), slot_present1);

    return out.present != nullptr && out.resize_buffers != nullptr;
}

bool resolve_d3d12(swapchain_slots& out, void*& execute_command_lists)
{
    execute_command_lists = nullptr;

    dummy_window window;
    if (!window.create())
        return false;

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        log_warn("dummy d3d12 device unavailable");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))))
        return false;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.Width = d3dimgui_dummy_size;
    sc_desc.Height = d3dimgui_dummy_size;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window.handle, &sc_desc, nullptr,
             nullptr, &swapchain1)))
    {
        log_warn("dummy d3d12 swapchain failed");
        return false;
    }

    ComPtr<IDXGISwapChain> swapchain;
    if (FAILED(swapchain1.As(&swapchain)))
        return false;

    DXGI_SWAP_CHAIN_DESC probed{};
    const HRESULT probe_hr = probe_swapchain_desc(swapchain.Get(), &probed);
    if (FAILED(probe_hr) || probed.BufferCount < 1 || probed.BufferCount > 8 ||
        probed.OutputWindow != window.handle)
    {
        log_error("d3d12 vtable self check failed, hr=0x%08lx buffers=%u window=%p",
            static_cast<unsigned long>(probe_hr), probed.BufferCount, probed.OutputWindow);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_probe{};
    if (!probe_queue_desc(queue.Get(), &queue_probe) ||
        queue_probe.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        log_error("queue vtable self check failed, type=%d", static_cast<int>(queue_probe.Type));
        return false;
    }

    out.present = slot_at(swapchain.Get(), slot_present);
    out.resize_buffers = slot_at(swapchain.Get(), slot_resize_buffers);
    out.present1 = slot_at(swapchain1.Get(), slot_present1);
    execute_command_lists = slot_at(queue.Get(), slot_execute_command_lists);

    return out.present != nullptr && out.resize_buffers != nullptr &&
        execute_command_lists != nullptr;
}
}
