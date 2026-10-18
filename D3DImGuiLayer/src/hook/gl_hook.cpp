#include "hook/gl_hook.h"

#include "core/layer.h"
#include "core/log.h"
#include "hook/dxgi_hook.h"

#include <MinHook.h>

#include <atomic>
#include <mutex>

namespace
{
using d3d_imgui_layer::log_error;
using d3d_imgui_layer::log_info;
using d3d_imgui_layer::log_warn;
using d3d_imgui_layer::process_exiting;

using swap_buffers_fn = BOOL(WINAPI*)(HDC);

constexpr wchar_t gl_module_name[] = L"opengl32.dll";

swap_buffers_fn original_swap = nullptr;
void* target_swap = nullptr;
std::mutex state_mutex;
std::atomic<unsigned long> in_flight{0};
std::atomic<bool> stopping{true};
std::atomic<bool> hooked{false};

// 宿主什么时候自己把 opengl32 装进来我们决定不了 注入得早的就只能等
// 不去替它 LoadLibrary 那等于凭空给宿主塞了一个渲染后端
HANDLE watcher = nullptr;
std::atomic<bool> watcher_stop{false};

bool install_locked();

bool wgl_export(void** out)
{
    HMODULE opengl = GetModuleHandleW(gl_module_name);
    if (opengl == nullptr)
        return false;

    void* address = reinterpret_cast<void*>(GetProcAddress(opengl, "wglSwapBuffers"));
    if (address == nullptr)
    {
        log_warn("wglSwapBuffers not exported, opengl path unavailable");
        return false;
    }
    *out = address;
    return true;
}

DWORD WINAPI watch_opengl(LPVOID)
{
    for (;;)
    {
        // 200ms 一次 挂上或者框架停了就退出去 不占宿主的线程
        for (int spin = 0; spin < 20; ++spin)
        {
            if (watcher_stop.load(std::memory_order_acquire))
                return 0;
            if (process_exiting() || !d3d_imgui_layer::layer::instance().running())
                return 0;
            Sleep(10);
        }

        void* address = nullptr;
        if (!wgl_export(&address))
            continue;

        std::lock_guard lock(state_mutex);
        if (watcher_stop.load(std::memory_order_acquire) || hooked.load(std::memory_order_acquire))
            return 0;
        const bool bound = install_locked();
        if (bound)
            log_info("opengl32 showed up late, wgl hook now active");
        return 0;
    }
}

void start_watcher()
{
    if (watcher != nullptr)
        return;

    watcher_stop.store(false, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, &watch_opengl, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        log_warn("could not start the opengl32 waiter, gl path stays idle");
        return;
    }
    watcher = thread;
    log_info("waiting for opengl32 to be loaded, poll 200ms");
}

void stop_watcher()
{
    if (watcher == nullptr)
        return;

    watcher_stop.store(true, std::memory_order_release);
    // 线程体在我们的镜像里 卸载之前必须确认它真的走了
    const DWORD waited = WaitForSingleObject(watcher, 2000);
    if (waited == WAIT_OBJECT_0)
        CloseHandle(watcher);
    else
        log_warn("opengl32 waiter did not exit in time, keeping it and never unmapping");
    watcher = nullptr;
}

BOOL WINAPI hook_swap_buffers(HDC device_context)
{
    // 计数是给 shutdown 等的 摘钩之后还要拆后端 有线程站在里面就是踩空
    in_flight.fetch_add(1, std::memory_order_acq_rel);

    if (!stopping.load(std::memory_order_acquire))
    {
        __try
        {
            d3d_imgui_layer::layer::instance().draw_gl(device_context);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            log_error("wgl detour raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
        }
    }

    in_flight.fetch_sub(1, std::memory_order_acq_rel);

    if (original_swap == nullptr)
        return FALSE;
    return original_swap(device_context);
}

bool install_locked()
{
    if (hooked.load(std::memory_order_acquire))
        return true;

    void* address = nullptr;
    if (!wgl_export(&address))
        return false;

    target_swap = address;
    // 没有 dxgi 钩子的时候没人初始化 minhook gl 自己得把它的状态先立起来
    if (!d3d_imgui_layer::hook::ensure_minhook())
        return false;

    const MH_STATUS created = MH_CreateHook(target_swap, reinterpret_cast<void*>(&hook_swap_buffers),
        reinterpret_cast<void**>(&original_swap));
    if (created != MH_OK)
    {
        log_error("create wgl hook on %p failed: %s", target_swap, MH_StatusToString(created));
        target_swap = nullptr;
        original_swap = nullptr;
        return false;
    }

    // 先放开再启用 中间这段就算有 swap 进来也只会走一次不画
    stopping.store(false, std::memory_order_release);

    const MH_STATUS enabled = MH_EnableHook(target_swap);
    if (enabled != MH_OK)
    {
        log_error("enable wgl hook failed: %s", MH_StatusToString(enabled));
        MH_RemoveHook(target_swap);
        original_swap = nullptr;
        target_swap = nullptr;
        stopping.store(true, std::memory_order_release);
        return false;
    }

    hooked.store(true, std::memory_order_release);
    log_info("wglSwapBuffers hook installed, target=%p", target_swap);
    return true;
}
}

namespace d3d_imgui_layer::hook::gl
{
bool install()
{
    std::lock_guard lock(state_mutex);
    if (install_locked())
        return true;

    // 装不上只有一种情况是暂时的 就是模块还没进来 那就派个等的
    if (GetModuleHandleW(gl_module_name) == nullptr)
        start_watcher();
    return false;
}

void shutdown()
{
    stop_watcher();

    std::lock_guard lock(state_mutex);
    if (!hooked.load(std::memory_order_acquire))
        return;

    stopping.store(true, std::memory_order_release);
    MH_DisableHook(target_swap);

    // 在飞的调用没出去之前不能 remove 更不能碰后端 等满五秒还不干净就只摘不拆
    for (int spin = 0; in_flight.load(std::memory_order_acquire) != 0 && spin < 5000; ++spin)
        Sleep(1);
    if (in_flight.load(std::memory_order_acquire) != 0)
        log_warn("a wgl call is still inside the detour, leaving the backend alone");

    MH_RemoveHook(target_swap);
    target_swap = nullptr;
    original_swap = nullptr;
    hooked.store(false, std::memory_order_release);
}

bool installed()
{
    return hooked.load(std::memory_order_acquire);
}

HWND surface_window(HDC device_context)
{
    if (device_context == nullptr)
        return nullptr;

    HWND window = WindowFromDC(device_context);
    if (window == nullptr)
        return nullptr;

    HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr)
        root = window;
    if (IsWindow(root) == FALSE || IsIconic(root) != FALSE || IsWindowVisible(root) == FALSE)
        return nullptr;
    return root;
}
}
