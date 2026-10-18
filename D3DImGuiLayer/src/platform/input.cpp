#include "platform/input.h"

#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <deque>
#include <mutex>

// imgui 官方把这条声明留在 #if 0 里 要求使用方自己抄一份进 cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam);

namespace
{
// 消息只能排队 不能在窗口过程里直接喂 imgui
// imgui 的 win32 handler 处理按键时会调 SetCapture ReleaseCapture 这类
// 会同步把消息再派回同一个窗口的 api 直接调就是让自己重入下面这把锁
// msvc 的 std::mutex 认出同线程重入会抛 std::system_error 宿主当场死
// 所以窗口线程只入队 喂给 imgui 的动作全部放到渲染线程做
struct queued_message
{
    HWND window;
    unsigned int message;
    unsigned long long wparam;
    long long lparam;
};

constexpr size_t max_queued_messages = 4096;

std::mutex g_queue_mutex;
std::deque<queued_message> g_queue;

// 渲染线程每帧刷一次快照给窗口线程判断要不要吞 晚一帧可以接受
std::atomic<bool> g_imgui_wants_input{false};

std::atomic<HWND> g_window{nullptr};
WNDPROC g_original_proc = nullptr;
bool g_window_unicode = true;

std::atomic<bool> g_capturing{false};
std::atomic<bool> g_cursor_control{d3dimgui_hook_cursor != 0};
std::atomic<int> g_toggle_key{d3dimgui_toggle_key};
std::atomic<int> g_unload_key{d3dimgui_unload_key};
void (*g_toggle_handler)() = nullptr;
void (*g_unload_handler)() = nullptr;
d3d_imgui_layer::input::destroy_fn g_destroy_handler = nullptr;

// 光标记账 游戏线程和渲染线程都会碰到 单独一把叶锁
// 用递归版是因为 impl 退回真 showcursor 时会再绕进同一个钩子里 普通锁重入就是抛 system_error
std::recursive_mutex g_cursor_mutex;
int g_cursor_lift = 0;
d3d_imgui_layer::input::show_cursor_fn g_show_cursor = &ShowCursor;

bool is_classic_input(const UINT message)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        return true;
    default:
        return false;
    }
}

// 回调都是我们自己的代码 跑在宿主的窗口线程上 出问题也不能把宿主带走
__declspec(noinline) void run_destroy_handler()
{
    if (g_destroy_handler == nullptr)
        return;
    __try
    {
        g_destroy_handler();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d3d_imgui_layer::log_error("destroy handler raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

__declspec(noinline) void run_toggle_handler()
{
    if (g_toggle_handler == nullptr)
        return;
    __try
    {
        g_toggle_handler();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d3d_imgui_layer::log_error("toggle handler raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

__declspec(noinline) void run_unload_handler()
{
    if (g_unload_handler == nullptr)
        return;
    __try
    {
        g_unload_handler();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        d3d_imgui_layer::log_error("unload handler raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

LRESULT CALLBACK subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    // 先把原窗口过程抄一份在手里 下面 wm_destroy 的回调里可能就把子类化摘了
    // 那样 g_original_proc 会被清空 这条消息就被我们吞掉 宿主再也收不到 wm_destroy
    WNDPROC previous = g_original_proc;

    if (message == WM_DESTROY)
        run_destroy_handler();

    // 两个功能键要在吞消息之前判 不然菜单打开时按 end 永远传不到这里
    const int key = g_toggle_key.load(std::memory_order_relaxed);
    if (key != 0 && message == WM_KEYUP && wparam == static_cast<WPARAM>(key))
    {
        run_toggle_handler();
        return 0;
    }

    const int unload = g_unload_key.load(std::memory_order_relaxed);
    if (unload != 0 && message == WM_KEYUP && wparam == static_cast<WPARAM>(unload))
    {
        run_unload_handler();
        return 0;
    }

    {
        std::lock_guard lock(g_queue_mutex);
        if (g_queue.size() < max_queued_messages)
            g_queue.push_back(queued_message{window, message,
                static_cast<unsigned long long>(wparam), static_cast<long long>(lparam)});
    }

    // 吞不吞只看原子快照 这里绝不能碰 imgui 的任何东西
    if ((g_capturing.load(std::memory_order_relaxed) ||
         g_imgui_wants_input.load(std::memory_order_relaxed)) && is_classic_input(message))
        return 0;

    if (previous == nullptr)
        return DefWindowProcW(window, message, wparam, lparam);

    if (g_window_unicode)
        return CallWindowProcW(previous, window, message, wparam, lparam);
    return CallWindowProcA(previous, window, message, wparam, lparam);
}
}

namespace d3d_imgui_layer::input
{
bool attach(HWND window)
{
    if (window == nullptr)
        return false;

    if (g_window.load(std::memory_order_relaxed) == window && g_original_proc != nullptr)
        return true;

    detach();

    g_window_unicode = IsWindowUnicode(window) != FALSE;

    LONG_PTR previous = 0;
    LONG_PTR installed = 0;
    if (g_window_unicode)
    {
        previous = GetWindowLongPtrW(window, GWLP_WNDPROC);
        installed = SetWindowLongPtrW(window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(subclass_proc));
    }
    else
    {
        previous = GetWindowLongPtrA(window, GWLP_WNDPROC);
        installed = SetWindowLongPtrA(window, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(subclass_proc));
    }

    if (previous == 0 && GetLastError() != 0)
    {
        log_error("subclass window %p failed, gle=%lu", window, GetLastError());
        return false;
    }

    g_original_proc = reinterpret_cast<WNDPROC>(previous);
    g_window.store(window, std::memory_order_release);

    // 读回来确认真的挂上了 从别的线程给窗口换 wndproc 是会静默失败的
    const LONG_PTR now = g_window_unicode
        ? GetWindowLongPtrW(window, GWLP_WNDPROC)
        : GetWindowLongPtrA(window, GWLP_WNDPROC);
    const DWORD owning = GetWindowThreadProcessId(window, nullptr);
    // 挂上了就别报 gle 那值是上一个 api 留下的 报出来只会让人以为出了事
    if (now == reinterpret_cast<LONG_PTR>(&subclass_proc))
        log_info("subclassed hwnd=%p unicode=%d owner_thread=%u self=%u",
            window, g_window_unicode ? 1 : 0, owning, GetCurrentThreadId());
    else
        log_error("subclass did not stick, hwnd=%p gle=%lu", window, GetLastError());
    return now == reinterpret_cast<LONG_PTR>(&subclass_proc);
}

void detach()
{
    {
        std::lock_guard lock(g_queue_mutex);
        g_queue.clear();
    }
    g_imgui_wants_input.store(false, std::memory_order_release);

    HWND window = g_window.exchange(nullptr, std::memory_order_acq_rel);
    if (window == nullptr)
        return;

    if (g_original_proc != nullptr && IsWindow(window) != FALSE)
    {
        if (g_window_unicode)
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_proc));
        else
            SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_proc));
    }

    g_original_proc = nullptr;
    restore_cursor();
}

HWND attached_window()
{
    return g_window.load(std::memory_order_acquire);
}

void new_frame()
{
    std::deque<queued_message> batch;
    {
        std::lock_guard lock(g_queue_mutex);
        batch.swap(g_queue);
    }

    // 只有这里碰 imgui 的输入接口 全在渲染线程 窗口线程只负责入队
    for (const queued_message& item : batch)
        ImGui_ImplWin32_WndProcHandler(item.window, item.message,
            static_cast<WPARAM>(item.wparam), static_cast<LPARAM>(item.lparam));

    ImGui_ImplWin32_NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    g_imgui_wants_input.store(io.WantCaptureMouse || io.WantCaptureKeyboard,
        std::memory_order_release);
}

void set_capturing(const bool capturing)
{
    const bool previous = g_capturing.exchange(capturing, std::memory_order_acq_rel);
    if (previous == capturing)
        return;

    if (capturing)
    {
        if (g_cursor_control.load(std::memory_order_relaxed))
            force_cursor_visible();
    }
    else
    {
        restore_cursor();
    }
}

bool is_capturing()
{
    return g_capturing.load(std::memory_order_acquire);
}

void set_cursor_control(const bool enabled)
{
    g_cursor_control.store(enabled, std::memory_order_release);
    if (!enabled)
        restore_cursor();
    else if (is_capturing())
        force_cursor_visible();
}

bool cursor_control_enabled()
{
    return g_cursor_control.load(std::memory_order_acquire);
}

bool should_ignore_cursor_pos()
{
    return is_capturing();
}

bool should_release_clip()
{
    return is_capturing();
}

bool should_force_arrow_cursor(HCURSOR requested)
{
    return is_capturing() && requested == nullptr;
}

void set_toggle_key(const int vk)
{
    g_toggle_key.store(vk, std::memory_order_release);
}

void set_destroy_handler(const destroy_fn handler)
{
    g_destroy_handler = handler;
}

int toggle_key()
{
    return g_toggle_key.load(std::memory_order_acquire);
}

void set_toggle_handler(void (*handler)())
{
    g_toggle_handler = handler;
}

void set_unload_key(const int vk)
{
    g_unload_key.store(vk, std::memory_order_release);
}

int unload_key()
{
    return g_unload_key.load(std::memory_order_acquire);
}

void set_unload_handler(void (*handler)())
{
    g_unload_handler = handler;
}

void force_cursor_visible()
{
    std::lock_guard lock(g_cursor_mutex);
    if (g_cursor_lift != 0)
        return;

    // 游戏的 ShowCursor(FALSE) 会把显示计数压到负数 这里抬回非负并记下抬了几次
    int lifted = 0;
    while (g_show_cursor(TRUE) < 0 && lifted < 64)
        ++lifted;
    g_cursor_lift = lifted;
}

void restore_cursor()
{
    std::lock_guard lock(g_cursor_mutex);
    for (int i = 0; i < g_cursor_lift; ++i)
        g_show_cursor(FALSE);
    g_cursor_lift = 0;
}

void repair_cursor_visibility()
{
    std::lock_guard lock(g_cursor_mutex);
    if (g_cursor_lift == 0)
        return;

    int lifted = 0;
    while (g_show_cursor(TRUE) < 0 && lifted < 64)
        ++lifted;
    g_cursor_lift += lifted;
}

void set_show_cursor_impl(const show_cursor_fn implementation)
{
    std::lock_guard lock(g_cursor_mutex);
    g_show_cursor = implementation != nullptr ? implementation : &ShowCursor;
}
}
