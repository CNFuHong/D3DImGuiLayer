#include "hook/dxgi_hook.h"

#include "core/layer.h"
#include "core/log.h"
#include "hook/vtable.h"
#include "platform/input.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winnt.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>

namespace
{
using d3d_imgui_layer::log_error;
using d3d_imgui_layer::log_info;
using d3d_imgui_layer::log_warn;

constexpr std::size_t max_slots = 2;


using present_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using present1_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using resize_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using execute_fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using set_cursor_fn = HCURSOR(WINAPI*)(HCURSOR);
using show_cursor_fn = int(WINAPI*)(BOOL);
using clip_cursor_fn = BOOL(WINAPI*)(const RECT*);
using set_cursor_pos_fn = BOOL(WINAPI*)(int, int);

present_fn originals_present[max_slots] = {};
present1_fn originals_present1[max_slots] = {};
resize_fn originals_resize[max_slots] = {};
execute_fn original_execute = nullptr;
set_cursor_fn original_set_cursor = nullptr;
show_cursor_fn original_show_cursor = nullptr;
clip_cursor_fn original_clip_cursor = nullptr;
set_cursor_pos_fn original_set_cursor_pos = nullptr;

void* targets_present[max_slots] = {};
void* targets_present1[max_slots] = {};
void* targets_resize[max_slots] = {};
void* target_execute = nullptr;
void* target_set_cursor = nullptr;
void* target_show_cursor = nullptr;
void* target_clip_cursor = nullptr;
void* target_set_cursor_pos = nullptr;

std::size_t count_present = 0;
std::size_t count_present1 = 0;
std::size_t count_resize = 0;

std::mutex queue_mutex;
constexpr std::size_t max_queues = 4;
Microsoft::WRL::ComPtr<ID3D12CommandQueue> captured_queues[max_queues];
std::size_t queue_count = 0;
bool queue_is_manual = false;
// executecommandlists 是热路径 同一套队列反复过来 先用原子量挡掉绝大多数
std::atomic<ID3D12CommandQueue*> last_observed_queue{nullptr};
std::atomic<bool> queue_locked{false};
std::atomic<unsigned long long> device_errors{0};
bool minhook_ready = false;

template <typename T, std::size_t N>
void add_unique(T (&table)[N], std::size_t& count, T target)
{
    if (target == nullptr || count >= N)
        return;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (table[i] == target)
            return;
    }
    table[count++] = target;
}

// 把模块按映像文件读进来 和内存里同一 rva 的字节比一次
// 这是判断函数体被人 inline 改过唯一可靠的办法 嗅首字节会把正常函数头误判成跳板
// 磁盘上没有重定位信息 只有头部若干字节可比 够了 我们要的就是"有没有被改过"
bool looks_patched_in_memory(void* target)
{
    if (target == nullptr)
        return false;

    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(target), &owner) == FALSE ||
        owner == nullptr)
        return false;

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(owner, path, MAX_PATH) == 0)
        return false;

    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const HANDLE view = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (view == nullptr)
        return false;

    const auto* base = static_cast<const unsigned char*>(
        MapViewOfFile(view, FILE_MAP_READ, 0, 0, 0));
    bool patched = false;
    if (base != nullptr)
    {
        const auto* nt = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (nt->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* nt_header = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                base + nt->e_lfanew);
            if (nt_header->Signature == IMAGE_NT_SIGNATURE)
            {
                const auto* sections = IMAGE_FIRST_SECTION(nt_header);
                const size_t rva = static_cast<const unsigned char*>(target) -
                    reinterpret_cast<const unsigned char*>(owner);
                for (WORD i = 0; i < nt_header->FileHeader.NumberOfSections; ++i)
                {
                    const IMAGE_SECTION_HEADER& section = sections[i];
                    if (rva < section.VirtualAddress ||
                        rva >= section.VirtualAddress + section.Misc.VirtualSize)
                        continue;
                    if (section.SizeOfRawData == 0)
                        break;
                    const size_t offset = section.PointerToRawData + (rva - section.VirtualAddress);
                    const size_t available = offset < section.PointerToRawData + section.SizeOfRawData
                        ? section.PointerToRawData + section.SizeOfRawData - offset : 0;
                    const size_t count = available < 16 ? available : 16;
                    if (count >= 5 && memcmp(base + offset, target, count) != 0)
                        patched = true;
                    break;
                }
            }
        }
        UnmapViewOfFile(base);
    }
    CloseHandle(view);
    return patched;
}

bool create_and_enable(void* target, void* detour, void** original)
{
    if (target == nullptr || detour == nullptr || original == nullptr)
        return false;

    // 只报告不拒绝 真机上和别的覆盖层叠钩子是常态 拒绝就是没有界面
    if (looks_patched_in_memory(target))
        log_warn("target %p differs from its on-disk image, another overlay is already inline-hooked here",
            target);

    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK)
    {
        log_error("create hook on %p failed: %s", target, MH_StatusToString(created));
        return false;
    }

    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK)
    {
        log_error("enable hook on %p failed: %s", target, MH_StatusToString(enabled));
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }
    return true;
}

void observe_queue(ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
        return;
    if (last_observed_queue.load(std::memory_order_relaxed) == queue)
        return;

    std::lock_guard lock(queue_mutex);
    // 用户手动指定过队列就不再被观察结果覆盖
    if (queue_is_manual)
        return;

    for (std::size_t i = 0; i < queue_count; ++i)
    {
        if (captured_queues[i].Get() != queue)
            continue;
        last_observed_queue.store(queue, std::memory_order_relaxed);
        return;
    }

    // 表满就丢掉最老的一条 多适配器进程里最新出现的才是当前在用的
    if (queue_count == max_queues)
    {
        for (std::size_t i = 1; i < max_queues; ++i)
            captured_queues[i - 1] = captured_queues[i];
        --queue_count;
    }
    captured_queues[queue_count++] = queue;
    last_observed_queue.store(queue, std::memory_order_relaxed);
    log_info("captured direct command queue %p as slot %zu", queue, queue_count - 1);
}

// 钩子函数体里我们自己的代码出问题不能把宿主带走
// 这几个壳函数单独拎出来 是因为 __try 所在函数不能有带析构的对象
__declspec(noinline) void guard_draw(void* swapchain)
{
    __try
    {
        d3d_imgui_layer::layer::instance().draw(swapchain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("present detour raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

__declspec(noinline) void guard_pre_resize(void* swapchain)
{
    __try
    {
        d3d_imgui_layer::layer::instance().pre_resize(swapchain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("resize preflight raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

__declspec(noinline) void guard_post_resize(void* swapchain, HRESULT result)
{
    __try
    {
        d3d_imgui_layer::layer::instance().post_resize(swapchain, result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("resize postflight raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

__declspec(noinline) void guard_present_result(void* swapchain, HRESULT result)
{
    __try
    {
        d3d_imgui_layer::layer::instance().on_present_result(swapchain, result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("present result handling raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

void note_present_result(IDXGISwapChain* swapchain, const HRESULT result)
{
    if (SUCCEEDED(result))
        return;

    const bool device_level = result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_UNSUPPORTED ||
        result == DXGI_ERROR_ACCESS_LOST ||
        result == DXGI_ERROR_INVALID_CALL;

    if (!device_level)
        return;

    // 设备已经没了之后宿可能还会照常 present 几百次 每次都打一行日志就淹了
    const unsigned long long hit = device_errors.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hit == 1 || hit % 300 == 0)
        log_warn("present returned device level error 0x%08lx (hit %llu)",
            static_cast<unsigned long>(result), hit);

    guard_present_result(swapchain, result);
}

std::atomic<unsigned long long> hits_present{0};
std::atomic<unsigned long long> hits_present1{0};

// 一个地址只能装一个 detour 但 dx11 和 dx12 的 dummy swapchain 可能给出同一个地址
// 所以每个槽位一个独立壳函数 各自记住自己的 original
template <std::size_t Index>
struct present_slot
{
    static HRESULT STDMETHODCALLTYPE hook(IDXGISwapChain* swapchain, UINT interval, UINT flags)
    {
        const unsigned long long n = hits_present.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
            log_info("present detour hit first frame on %p", swapchain);
        d3d_imgui_layer::layer::instance().note_route(1);

        d3d_imgui_layer::layer& app = d3d_imgui_layer::layer::instance();
        if (!app.stopping())
            guard_draw(swapchain);

        const present_fn original = originals_present[Index];
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;

        const HRESULT result = original(swapchain, interval, flags);
        note_present_result(swapchain, result);
        return result;
    }
};

template <std::size_t Index>
struct resize_slot
{
    static HRESULT STDMETHODCALLTYPE hook(IDXGISwapChain* swapchain, UINT buffers, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags)
    {
        guard_pre_resize(swapchain);

        const resize_fn original = originals_resize[Index];
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;

        const HRESULT result = original(swapchain, buffers, width, height, format, flags);
        guard_post_resize(swapchain, result);
        return result;
    }
};

template <std::size_t Index>
struct present1_slot
{
    static HRESULT STDMETHODCALLTYPE hook(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
        const DXGI_PRESENT_PARAMETERS* parameters)
    {
        d3d_imgui_layer::layer& app = d3d_imgui_layer::layer::instance();
        IDXGISwapChain* base = static_cast<IDXGISwapChain*>(swapchain);
        if (hits_present1.fetch_add(1, std::memory_order_relaxed) == 0)
            log_info("present1 detour hit first frame on %p", base);
        d3d_imgui_layer::layer::instance().note_route(2);
        if (!app.stopping())
            guard_draw(base);

        const present1_fn original = originals_present1[Index];
        if (original == nullptr)
            return DXGI_ERROR_INVALID_CALL;

        // parameters 是 _In_ 不是 _In_opt_ 空指针得自己造一个
        DXGI_PRESENT_PARAMETERS fallback_params{};
        const DXGI_PRESENT_PARAMETERS* used = parameters != nullptr ? parameters : &fallback_params;

        const HRESULT result = original(swapchain, interval, flags, used);
        note_present_result(base, result);
        return result;
    }
};

__declspec(noinline) void guard_observe(ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
        return;

    __try
    {
        if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            observe_queue(queue);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("queue observation raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
    }
}

void STDMETHODCALLTYPE hook_execute(ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists)
{
    // getdesc 是虚调用 每条队列每帧都要过这里 认全了就不再蹲
    if (!queue_locked.load(std::memory_order_relaxed))
        guard_observe(queue);

    if (original_execute != nullptr)
        original_execute(queue, count, lists);
}

HCURSOR WINAPI hook_set_cursor(HCURSOR cursor)
{
    if (original_set_cursor == nullptr)
        return cursor;
    if (d3d_imgui_layer::input::should_force_arrow_cursor(cursor))
        cursor = LoadCursorW(nullptr, IDC_ARROW);
    return original_set_cursor(cursor);
}

int WINAPI hook_show_cursor(BOOL show)
{
    if (original_show_cursor == nullptr)
        return ShowCursor(show);

    const int result = original_show_cursor(show);
    // 游戏在我们接管期间藏光标 立刻按记账补回来 比拦截转发更容易还原
    if (result < 0 && show == FALSE && d3d_imgui_layer::input::is_capturing() &&
        d3d_imgui_layer::input::cursor_control_enabled())
    {
        d3d_imgui_layer::input::repair_cursor_visibility();
    }
    return result;
}

BOOL WINAPI hook_clip_cursor(const RECT* rectangle)
{
    if (original_clip_cursor == nullptr)
        return ClipCursor(rectangle);
    if (rectangle != nullptr && d3d_imgui_layer::input::should_release_clip())
        return original_clip_cursor(nullptr);
    return original_clip_cursor(rectangle);
}

BOOL WINAPI hook_set_cursor_pos(int x, int y)
{
    if (original_set_cursor_pos == nullptr)
        return SetCursorPos(x, y);
    if (d3d_imgui_layer::input::should_ignore_cursor_pos())
        return TRUE;
    return original_set_cursor_pos(x, y);
}

bool install_cursor_hooks()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
        user32 = LoadLibraryW(L"user32.dll");
    if (user32 == nullptr)
        return false;

    target_set_cursor = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursor"));
    target_show_cursor = reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor"));
    target_clip_cursor = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    target_set_cursor_pos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));

    bool ok = create_and_enable(target_set_cursor,
        reinterpret_cast<void*>(&hook_set_cursor), reinterpret_cast<void**>(&original_set_cursor));
    ok = create_and_enable(target_show_cursor,
        reinterpret_cast<void*>(&hook_show_cursor), reinterpret_cast<void**>(&original_show_cursor)) && ok;
    ok = create_and_enable(target_clip_cursor,
        reinterpret_cast<void*>(&hook_clip_cursor), reinterpret_cast<void**>(&original_clip_cursor)) && ok;
    ok = create_and_enable(target_set_cursor_pos,
        reinterpret_cast<void*>(&hook_set_cursor_pos), reinterpret_cast<void**>(&original_set_cursor_pos)) && ok;

    // 把没被改过的 showcursor 交给 input 模块 它自己抬光标时不能绕回这里
    d3d_imgui_layer::input::set_show_cursor_impl(
        original_show_cursor != nullptr ? original_show_cursor : &ShowCursor);
    return ok;
}
}

namespace d3d_imgui_layer::hook
{
// minhook 只该有一份状态 谁先要用谁把它拉起来
// 关掉 dxgi 钩子只跑 opengl 那条时 原来根本没人调 MH_Initialize 钩子永远装不上
// 拆仍然只由 shutdown 做 这里只管初始化
bool ensure_minhook()
{
    if (minhook_ready)
        return true;

    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    {
        log_error("minhook init failed: %s", MH_StatusToString(initialized));
        return false;
    }
    minhook_ready = true;
    return true;
}

namespace
{
// 装钩子全过程包一层 seh 探测设备或者 minhook 出问题都只是装不上 不能带走宿主
bool install_body(const options& opt)
{
    swapchain_slots d3d11{};
    swapchain_slots d3d12{};
    void* execute_target = nullptr;

    const bool has_d3d11 = opt.swapchain && resolve_d3d11(d3d11);
    const bool has_d3d12 = opt.swapchain && resolve_d3d12(d3d12, execute_target);

    if (!has_d3d11 && !has_d3d12)
    {
        log_error("no dummy device could be created, cannot resolve swapchain vtable");
        return false;
    }

    add_unique(targets_present, count_present, d3d11.present);
    add_unique(targets_present, count_present, d3d12.present);
    add_unique(targets_resize, count_resize, d3d11.resize_buffers);
    add_unique(targets_resize, count_resize, d3d12.resize_buffers);
    add_unique(targets_present1, count_present1, d3d11.present1);
    add_unique(targets_present1, count_present1, d3d12.present1);

    if (count_present == 0 || count_resize == 0)
    {
        log_error("present or resizebuffers slot missing");
        return false;
    }

    if (has_d3d11 && has_d3d12 && d3d11.present != d3d12.present)
    {
        // 两条链给出不同地址说明进程里存在两套 dxgi 实现 两套都要钩
        log_warn("d3d11 and d3d12 dummy swapchains disagree on present slot");
    }

    if (!ensure_minhook())
        return false;

    if (count_present > max_slots || count_resize > max_slots)
    {
        log_error("too many distinct swapchain targets");
        shutdown();
        return false;
    }

    // 队列钩子要在 present 钩子之前开 dx12 建后端要先蹲到 direct 队列
    // 反过来就是头几十帧全都绑不成
    if (opt.command_queue && execute_target != nullptr)
    {
        if (!create_and_enable(execute_target, reinterpret_cast<void*>(&hook_execute),
                reinterpret_cast<void**>(&original_execute)))
        {
            log_warn("command queue hook unavailable, d3d12 needs d3dimgui_set_command_queue");
            original_execute = nullptr;
            target_execute = nullptr;
        }
        else
        {
            target_execute = execute_target;
        }
    }
    else
    {
        log_warn("d3d12 dummy queue unavailable, d3d12 path needs a manual queue");
    }

    for (std::size_t i = 0; i < count_present; ++i)
    {
        void* detour = i == 0 ? reinterpret_cast<void*>(&present_slot<0>::hook)
                              : reinterpret_cast<void*>(&present_slot<1>::hook);
        if (!create_and_enable(targets_present[i], detour,
                reinterpret_cast<void**>(&originals_present[i])))
        {
            log_error("present slot %zu refused, nothing to hook on", i);
            shutdown();
            return false;
        }
    }

    for (std::size_t i = 0; i < count_resize; ++i)
    {
        void* detour = i == 0 ? reinterpret_cast<void*>(&resize_slot<0>::hook)
                              : reinterpret_cast<void*>(&resize_slot<1>::hook);
        if (!create_and_enable(targets_resize[i], detour,
                reinterpret_cast<void**>(&originals_resize[i])))
        {
            log_error("resizebuffers slot %zu refused", i);
            shutdown();
            return false;
        }
    }

    if (opt.present1)
    {
        for (std::size_t i = 0; i < count_present1; ++i)
        {
            void* detour = i == 0 ? reinterpret_cast<void*>(&present1_slot<0>::hook)
                                  : reinterpret_cast<void*>(&present1_slot<1>::hook);
            if (!create_and_enable(targets_present1[i], detour,
                    reinterpret_cast<void**>(&originals_present1[i])))
            {
                // present1 装不上不致命 大部分引擎只走 present
                log_warn("present1 hook unavailable, hdr titles may stay blank");
                targets_present1[i] = nullptr;
                originals_present1[i] = nullptr;
            }
        }
    }

    if (opt.cursor && !install_cursor_hooks())
        log_warn("some cursor hooks failed, the game may steal the mouse back");

    log_info("hooks installed, present=%p[%zu] resize=%p present1=%p queue=%s cursor=%d",
        targets_present[0], count_present, targets_resize[0], targets_present1[0],
        target_execute != nullptr ? "observed" : "none",
        target_set_cursor != nullptr ? 1 : 0);
    return true;
}
}

bool install(const options& opt)
{
    bool ok = false;
    __try
    {
        ok = install_body(opt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("hook install raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
        ok = false;
    }

    // 半套钩子比没钩子危险得多 装不成就是没装
    if (!ok)
        shutdown();
    return ok;
}

void shutdown()
{
    // 先把 showcursor 的实现换回原函数再拆钩子 反过来就是给别的线程留一个已释放的跳板
    d3d_imgui_layer::input::set_show_cursor_impl(nullptr);

    if (!minhook_ready)
        return;

    for (std::size_t i = 0; i < max_slots; ++i)
    {
        if (targets_present[i] != nullptr)
        {
            MH_DisableHook(targets_present[i]);
            MH_RemoveHook(targets_present[i]);
        }
        if (targets_resize[i] != nullptr)
        {
            MH_DisableHook(targets_resize[i]);
            MH_RemoveHook(targets_resize[i]);
        }
        if (targets_present1[i] != nullptr)
        {
            MH_DisableHook(targets_present1[i]);
            MH_RemoveHook(targets_present1[i]);
        }
    }

    if (target_execute != nullptr)
    {
        MH_DisableHook(target_execute);
        MH_RemoveHook(target_execute);
    }
    if (target_set_cursor != nullptr)
    {
        MH_DisableHook(target_set_cursor);
        MH_RemoveHook(target_set_cursor);
    }
    if (target_show_cursor != nullptr)
    {
        MH_DisableHook(target_show_cursor);
        MH_RemoveHook(target_show_cursor);
    }
    if (target_clip_cursor != nullptr)
    {
        MH_DisableHook(target_clip_cursor);
        MH_RemoveHook(target_clip_cursor);
    }
    if (target_set_cursor_pos != nullptr)
    {
        MH_DisableHook(target_set_cursor_pos);
        MH_RemoveHook(target_set_cursor_pos);
    }

    MH_Uninitialize();
    minhook_ready = false;

    for (std::size_t i = 0; i < max_slots; ++i)
    {
        targets_present[i] = nullptr;
        targets_resize[i] = nullptr;
        targets_present1[i] = nullptr;
        originals_present[i] = nullptr;
        originals_resize[i] = nullptr;
        originals_present1[i] = nullptr;
    }
    count_present = 0;
    count_resize = 0;
    count_present1 = 0;

    target_execute = nullptr;
    original_execute = nullptr;
    target_set_cursor = nullptr;
    target_show_cursor = nullptr;
    target_clip_cursor = nullptr;
    target_set_cursor_pos = nullptr;
    original_set_cursor = nullptr;
    original_show_cursor = nullptr;
    original_clip_cursor = nullptr;
    original_set_cursor_pos = nullptr;

    forget_command_queues();
}

bool get_command_queue(Microsoft::WRL::ComPtr<ID3D12CommandQueue>& out)
{
    std::lock_guard lock(queue_mutex);
    out = queue_count > 0 ? captured_queues[queue_count - 1] : nullptr;
    return out != nullptr;
}

// 挑一条确实属于这条 swapchain 所在设备的队列
// 拿错设备的队列提交上去就是跨设备误用 运行时直接把整个设备判死
bool match_command_queue(void* device, Microsoft::WRL::ComPtr<ID3D12CommandQueue>& out)
{
    out = nullptr;
    std::lock_guard lock(queue_mutex);

    for (std::size_t i = 0; i < queue_count; ++i)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (FAILED(captured_queues[i]->GetDevice(IID_PPV_ARGS(&owner))) || owner == nullptr)
            continue;
        if (device == nullptr || owner.Get() == device)
        {
            out = captured_queues[i];
            return true;
        }
    }

    if (queue_count == 0)
        return false;

    log_warn("no direct queue belongs to the swapchain device, using the last observed one");
    out = captured_queues[queue_count - 1];
    return true;
}

void lock_command_queue()
{
    queue_locked.store(true, std::memory_order_release);
}

void forget_command_queues()
{
    std::lock_guard lock(queue_mutex);
    for (Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue : captured_queues)
        queue.Reset();
    queue_count = 0;
    queue_is_manual = false;
    last_observed_queue.store(nullptr, std::memory_order_relaxed);
    queue_locked.store(false, std::memory_order_release);
    device_errors.store(0, std::memory_order_relaxed);
}

void set_command_queue(ID3D12CommandQueue* queue)
{
    std::lock_guard lock(queue_mutex);
    for (Microsoft::WRL::ComPtr<ID3D12CommandQueue>& item : captured_queues)
        item.Reset();
    queue_count = 0;

    if (queue == nullptr)
    {
        queue_is_manual = false;
        last_observed_queue.store(nullptr, std::memory_order_relaxed);
        queue_locked.store(false, std::memory_order_release);
        return;
    }

    captured_queues[queue_count++] = queue;
    queue_is_manual = true;
    last_observed_queue.store(queue, std::memory_order_relaxed);
}
}
