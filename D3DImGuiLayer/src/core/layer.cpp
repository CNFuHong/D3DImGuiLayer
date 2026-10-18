#include "core/layer.h"

#include "core/log.h"
#include "hook/dxgi_hook.h"
#include "hook/gl_hook.h"
#include "platform/font.h"
#include "platform/input.h"

#include <cstdio>
#include <cstring>
#include <iterator>

#include <d3d_imgui_layer/d3dimgui_user.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace
{
std::atomic<bool> g_process_exiting{false};

void toggle_thunk()
{
    d3d_imgui_layer::layer::instance().toggle();
}

void window_destroyed_thunk()
{
    d3d_imgui_layer::layer::instance().on_window_destroyed();
}

void unload_thunk()
{
    d3d_imgui_layer::layer::instance().unload();
}

int toggle_key_thunk()
{
    return d3d_imgui_layer::input::toggle_key();
}

int unload_key_thunk()
{
    return d3d_imgui_layer::input::unload_key();
}

unsigned int primary_screen_area()
{
    static unsigned int cached = 0;
    if (cached != 0)
        return cached;

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width > 0 && height > 0)
        cached = static_cast<unsigned int>(width) * static_cast<unsigned int>(height);
    return cached;
}
}

namespace d3d_imgui_layer
{
void set_process_exiting()
{
    g_process_exiting.store(true, std::memory_order_release);
}

bool process_exiting()
{
    return g_process_exiting.load(std::memory_order_acquire);
}

unsigned int swap_target::score() const
{
    if (!valid)
        return 0;

    unsigned int value = 0;
    if (top_level)
        value += 100;
    if (visible)
        value += 50;

    const unsigned int reference = primary_screen_area();
    if (reference != 0 &&
        area >= static_cast<unsigned int>(static_cast<float>(reference) * d3dimgui_primary_area_ratio))
        value += 20;

    // 面积项封顶 40 分 参与比较但不压过顶层可见
    const unsigned int area_bonus = area / 100000u;
    value += area_bonus > 40u ? 40u : area_bonus;
    return value;
}

layer& layer::instance()
{
    static layer self;
    return self;
}

// 故意不做任何拆解 静态析构跑在进程退出阶段 那时 dxgi 和驱动可能已经先走了一步
// 要完整清理只有宿主主动调 d3dimgui_shutdown 这一条路
layer::~layer() = default;

void layer::set_error(const int error)
{
    error_.store(error, std::memory_order_release);
}

void layer::set_state(const unsigned int state)
{
    state_.store(state, std::memory_order_release);
}

bool layer::running() const
{
    return running_.load(std::memory_order_acquire);
}

bool layer::stopping() const
{
    return stopping_.load(std::memory_order_acquire);
}

int layer::last_error() const
{
    return error_.load(std::memory_order_acquire);
}

bool layer::startup(const d3dimgui_desc* desc)
{
    if (running_.load(std::memory_order_acquire))
        return true;

    const bool shaped = desc != nullptr && desc->struct_size >= sizeof(d3dimgui_desc);

    unsigned int flags = flags_.load(std::memory_order_relaxed);
    if (shaped && desc->flags != 0)
        flags = desc->flags;
    flags_.store(flags, std::memory_order_release);

    if (shaped)
    {
        if (desc->font_pixel > 1.0f)
        {
            font_pixel_ = desc->font_pixel;
            font::set_default_pixel(desc->font_pixel);
        }
        if (desc->font_data != nullptr && desc->font_size != 0)
            font::push(desc->font_data, desc->font_size, desc->font_pixel);
        if (desc->toggle_key != 0)
            input::set_toggle_key(desc->toggle_key);
    }

    input::set_toggle_handler(&toggle_thunk);
    input::set_destroy_handler(&window_destroyed_thunk);
    input::set_unload_key(d3dimgui_unload_key);
    input::set_unload_handler(&unload_thunk);
    if (shaped && desc->unload_key != 0)
        input::set_unload_key(desc->unload_key);

    // 注入完默认是关着的 框架自己不画东西 一上来就吞输入宿主只会觉得没人操作
    // 按呼出键才进菜单态 那时候才需要吞输入和抬光标
    active_.store(false, std::memory_order_release);
    input::set_capturing(false);
    input::set_cursor_control((flags & d3dimgui_flag_manage_cursor) != 0);

    stopping_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    if (d3dimgui_start_with_console)
        open_console(true);
    if (d3dimgui_log_to_file)
        open_log_file(true);
    if (d3dimgui_crash_observer)
        install_crash_observer();

    const bool with_hooks = (flags & d3dimgui_flag_install_hooks) != 0;
    // 状态要先落到 waiting 再开钩子 钩子一开 present 线程立刻就能绑好并把状态推到 ready
    // 这时候再写一次状态就把刚绑好的后端当成没绑 白拆一遍
    set_state(with_hooks ? d3dimgui_state_waiting : d3dimgui_state_idle);
    if (with_hooks)
    {
        hook::options opt;
        opt.swapchain = true;
        opt.present1 = (flags & d3dimgui_flag_hook_present1) != 0;
        opt.command_queue = d3dimgui_hook_command_queue != 0;
        opt.cursor = (flags & d3dimgui_flag_manage_cursor) != 0;

        if (!hook::install(opt))
        {
            log_error("hook install failed");
            running_.store(false, std::memory_order_release);
            input::set_capturing(false);
            set_state(d3dimgui_state_idle);
            set_error(d3dimgui_error_hook_install);
            return false;
        }
    }
    else
    {
        log_info("hooks off, manual mode only");
    }

    // gl 的钩子跟 dxgi 那条互不影响 装不上时 hook 层自己会等 opengl32 出现
    if (d3dimgui_hook_wgl_swap && !hook::gl::installed() && !hook::gl::install())
        log_info("opengl32 not loaded yet, waiting for it");

    if (shaped)
    {
        std::lock_guard lock(table_mutex);
        if (desc->hwnd != nullptr)
            manual_window_hints_ = static_cast<HWND>(desc->hwnd);
        if (desc->swapchain != nullptr)
        {
            swap_target* target = register_target(static_cast<IDXGISwapChain*>(desc->swapchain));
            if (target != nullptr)
            {
                if (desc->device != nullptr)
                    target->device = desc->device;
                manual_primary_ = target->swapchain;
                primary_ = manual_primary_;
            }
        }
    }

    log_info("startup done, hooks=%d mode=%s", with_hooks ? 1 : 0,
        shaped && desc->swapchain != nullptr ? "manual" : "auto");
    return true;
}

bool layer::shutdown()
{
    stopping_.store(true, std::memory_order_release);

    // 先摘钩 保证之后不会再有帧进来
    hook::gl::shutdown();
    hook::shutdown();

    if (running_.exchange(false, std::memory_order_acq_rel))
        log_info("shutting down");

    // 限时拿锁 渲染线程可能正卡在一个不会再回来的 present 上
    // 拿不到就跳过深拆 挂住宿主比漏几块内存严重得多
    bool clean = true;
    std::unique_lock frame_lock(frame_mutex, std::defer_lock);
    if (frame_lock.try_lock_for(std::chrono::milliseconds(400)))
        unbind_backend();
    else
    {
        clean = false;
        log_warn("a frame is wedged, skipping deep teardown so the host can exit");
    }

    {
        std::lock_guard lock(hooks_mutex);
        frame_callback_ = nullptr;
        frame_callback_user_ = nullptr;
    }

    {
        std::lock_guard lock(table_mutex);
        for (swap_target& target : targets_)
            target = swap_target{};
        target_count_ = 0;
        dead_count_ = 0;
        primary_ = nullptr;
        manual_primary_ = nullptr;
        manual_window_hints_ = nullptr;
    }

    input::set_capturing(false);
    input::set_toggle_handler(nullptr);
    input::set_unload_handler(nullptr);
    input::set_destroy_handler(nullptr);
    stopping_.store(false, std::memory_order_release);
    set_state(d3dimgui_state_idle);
    set_error(d3dimgui_error_none);
    log_info("shutdown done");
    return clean;
}

bool layer::is_dead(IDXGISwapChain* swapchain) const
{
    std::lock_guard lock(dead_mutex);
    for (unsigned int i = 0; i < dead_count_; ++i)
    {
        if (dead_targets_[i] == swapchain)
            return true;
    }
    return false;
}

void layer::mark_dead(IDXGISwapChain* swapchain)
{
    if (swapchain == nullptr)
        return;

    std::lock_guard lock(dead_mutex);
    for (unsigned int i = 0; i < dead_count_; ++i)
    {
        if (dead_targets_[i] == swapchain)
            return;
    }

    if (dead_count_ < dead_targets_.size())
    {
        dead_targets_[dead_count_++] = swapchain;
        return;
    }

    // 记满了就丢掉最老的一个 留最新的
    for (unsigned int i = 1; i < dead_targets_.size(); ++i)
        dead_targets_[i - 1] = dead_targets_[i];
    dead_targets_[dead_targets_.size() - 1] = swapchain;
}

void layer::unmark_dead(IDXGISwapChain* swapchain)
{
    std::lock_guard lock(dead_mutex);
    for (unsigned int i = 0; i < dead_count_; ++i)
    {
        if (dead_targets_[i] != swapchain)
            continue;
        for (unsigned int j = i + 1; j < dead_count_; ++j)
            dead_targets_[j - 1] = dead_targets_[j];
        --dead_count_;
        return;
    }
}

swap_target* layer::find_target(IDXGISwapChain* swapchain)
{
    for (swap_target& target : targets_)
    {
        if (target.valid && target.swapchain == swapchain)
            return &target;
    }
    return nullptr;
}

bool layer::probe(swap_target& target, IDXGISwapChain* swapchain)
{
    target = swap_target{};
    target.swapchain = swapchain;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&d3d11))))
    {
        target.api = d3dimgui_graphics_d3d11;
        target.device = d3d11.Get();
        d3d11.As(&target.device_ref);
    }
    else if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&d3d12))))
    {
        target.api = d3dimgui_graphics_d3d12;
        target.device = d3d12.Get();
        d3d12.As(&target.device_ref);
    }
    else
    {
        // 不是 d3d11 也不是 d3d12 的链 这版直接放过
        target.api = d3dimgui_graphics_unknown;
        return false;
    }

    HWND output = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))))
    {
        DXGI_SWAP_CHAIN_DESC1 desc1{};
        if (SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        {
            width = desc1.Width;
            height = desc1.Height;
        }
        swapchain1->GetHwnd(&output);
    }

    if (output == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swapchain->GetDesc(&desc)))
        {
            output = desc.OutputWindow;
            if (width == 0)
                width = desc.BufferDesc.Width;
            if (height == 0)
                height = desc.BufferDesc.Height;
        }
    }

    if (output == nullptr)
        output = manual_window_hints_;
    if (output == nullptr)
        return false;

    const HWND root = GetAncestor(output, GA_ROOT);
    target.window = root != nullptr ? root : output;

    if (width == 0 || height == 0)
    {
        RECT rect{};
        if (GetClientRect(target.window, &rect))
        {
            width = static_cast<unsigned int>(rect.right - rect.left);
            height = static_cast<unsigned int>(rect.bottom - rect.top);
        }
    }

    target.area = width * height;
    target.top_level = GetAncestor(target.window, GA_ROOT) == target.window;
    target.visible = IsWindowVisible(target.window) != FALSE;
    return true;
}

swap_target* layer::register_target(IDXGISwapChain* swapchain)
{
    // 设备销毁时 dxgi 内部会自己调一次 resizebuffers 并且必然失败
    // 那条链之后还会被 present 一两次 认成死的就不再往上绑资源
    if (is_dead(swapchain))
        return nullptr;

    swap_target* slot = nullptr;
    for (swap_target& candidate : targets_)
    {
        if (!candidate.valid)
        {
            slot = &candidate;
            break;
        }
    }

    if (slot == nullptr)
    {
        // 表满 淘汰最久没在 present 里出现的那条 游戏反复重建 swapchain 靠这里回收
        swap_target* stalest = nullptr;
        for (swap_target& candidate : targets_)
        {
            if (stalest == nullptr || candidate.last_seen < stalest->last_seen)
                stalest = &candidate;
        }
        slot = stalest;

        if (slot->swapchain == primary_)
            primary_ = nullptr;
        if (slot->swapchain == manual_primary_)
            manual_primary_ = nullptr;
        if (slot->valid)
        {
            if (target_count_ > 0)
                --target_count_;
            log_warn("target table full, evicted swapchain %p", slot->swapchain);
        }
        *slot = swap_target{};
    }

    if (!probe(*slot, swapchain))
    {
        slot->valid = false;
        *slot = swap_target{};
        return nullptr;
    }

    slot->valid = true;
    slot->ordinal = ordinal_next_++;
    slot->last_seen = frame_counter_.load(std::memory_order_relaxed);
    ++target_count_;

    log_info("registered swapchain %p api=%u area=%u window=%p score=%u",
        swapchain, slot->api, slot->area, slot->window, slot->score());

    evaluate_primary();
    return slot;
}

void layer::evaluate_primary()
{
    if (manual_primary_ != nullptr)
    {
        primary_ = manual_primary_;
        return;
    }

    swap_target* best = nullptr;
    for (swap_target& target : targets_)
    {
        if (!target.valid)
            continue;
        if (best == nullptr || target.score() > best->score())
            best = &target;
    }

    if (best == nullptr)
        return;

    if (best->swapchain != primary_)
    {
        primary_ = best->swapchain;
        log_info("primary switched to swapchain %p", primary_);
    }
}

bool layer::create_context(HWND window)
{
    if (context_ != nullptr)
        return true;

    context_ = ImGui::CreateContext();
    if (context_ == nullptr)
    {
        set_error(d3dimgui_error_imgui_context);
        return false;
    }
    ImGui::SetCurrentContext(context_);

    ImGuiIO& io = ImGui::GetIO();
    // 配置全在代码里 不往磁盘写 imgui.ini
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = false;
    ImGui::GetStyle().FontScaleDpi = 1.0f;

    if (window == nullptr)
    {
        log_warn("no window to bind, imgui will have no input");
        return true;
    }

    if (!ImGui_ImplWin32_Init(window))
    {
        log_error("win32 backend init failed");
        ImGui::DestroyContext(context_);
        context_ = nullptr;
        set_error(d3dimgui_error_imgui_context);
        return false;
    }
    win32_bound_ = true;
    return true;
}

void layer::destroy_context()
{
    if (context_ == nullptr)
        return;

    ImGui::SetCurrentContext(context_);
    if (win32_bound_)
    {
        ImGui_ImplWin32_Shutdown();
        win32_bound_ = false;
    }
    ImGui::DestroyContext(context_);
    context_ = nullptr;
}

void layer::describe_environment(const swap_target& target)
{
    // 一次绑定报一遍 真机上出问题时这十几行就是病历
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    char process[128] = {};
    if (exe[0] != L'\0')
    {
        const wchar_t* tail = wcsrchr(exe, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, tail != nullptr ? tail + 1 : exe, -1, process,
            static_cast<int>(sizeof(process)), nullptr, nullptr);
    }

    DXGI_ADAPTER_DESC adapter_desc{};
    char adapter_name[96] = "unknown";
    unsigned int vendor_id = 0;
    unsigned int device_id = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;

    // d3d11 设备能直接问到 dxgi 设备 d3d12 只给 luid 两条路分开走
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (target.device != nullptr &&
        SUCCEEDED(static_cast<IUnknown*>(target.device)->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)))
    {
        if (SUCCEEDED(adapter->GetDesc(&adapter_desc)))
        {
            WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name,
                sizeof(adapter_name), nullptr, nullptr);
            vendor_id = adapter_desc.VendorId;
            device_id = adapter_desc.DeviceId;
        }
    }

    log_info("env process=%s api=%u", process[0] != '\0' ? process : "?", target.api);
    if (adapter == nullptr && target.api == d3dimgui_graphics_d3d12 && target.device != nullptr)
    {
        // d3d12 设备不认 dxgi 接口 只能拿 luid 回工厂里对着枚举
        const LUID luid = static_cast<ID3D12Device*>(target.device)->GetAdapterLuid();
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (target.swapchain != nullptr &&
            SUCCEEDED(target.swapchain->GetParent(IID_PPV_ARGS(&factory))))
        {
            for (UINT index = 0;; ++index)
            {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
                if (FAILED(factory->EnumAdapters1(index, &candidate)))
                    break;
                if (FAILED(candidate->GetDesc(&adapter_desc)))
                    continue;
                if (adapter_desc.AdapterLuid.HighPart != luid.HighPart ||
                    adapter_desc.AdapterLuid.LowPart != luid.LowPart)
                    continue;
                adapter = candidate;
                break;
            }
        }
    }

    if (adapter != nullptr)
    {
        WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name,
            sizeof(adapter_name), nullptr, nullptr);
        snprintf(adapter_, sizeof(adapter_), "%s%s", adapter_name,
            vendor_id == 0x1414 ? " (software)" : "");
        vendor_id = adapter_desc.VendorId;
        device_id = adapter_desc.DeviceId;
        log_info("env adapter=%s vendor=0x%04x device=0x%04x%s", adapter_name, vendor_id, device_id,
            vendor_id == 0x1414 ? " (microsoft software)" : "");
    }

    DXGI_SWAP_CHAIN_DESC legacy{};
    if (target.swapchain != nullptr && SUCCEEDED(target.swapchain->GetDesc(&legacy)))
    {
        RECT client{};
        if (target.window != nullptr)
            GetClientRect(target.window, &client);
        BOOL fullscreen = FALSE;
        target.swapchain->GetFullscreenState(&fullscreen, nullptr);

        log_info("env swapchain buffers=%u format=%u effect=%u sample=%u%d flags=%u",
            legacy.BufferCount, static_cast<unsigned>(legacy.BufferDesc.Format),
            static_cast<unsigned>(legacy.SwapEffect), legacy.SampleDesc.Count, legacy.SampleDesc.Quality,
            legacy.Flags);
        log_info("env geometry buffer=%ux%u client=%ldx%ld windowed=%d fullscreen_state=%d visible=%d",
            legacy.BufferDesc.Width, legacy.BufferDesc.Height,
            static_cast<long>(client.right - client.left), static_cast<long>(client.bottom - client.top),
            legacy.Windowed ? 1 : 0, fullscreen ? 1 : 0,
            target.window != nullptr && IsWindowVisible(target.window) ? 1 : 0);
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain1;
    if (target.swapchain != nullptr && SUCCEEDED(target.swapchain->QueryInterface(IID_PPV_ARGS(&chain1))))
    {
        DXGI_SWAP_CHAIN_DESC1 modern{};
        if (SUCCEEDED(chain1->GetDesc1(&modern)))
            log_info("env flip alpha=%u scaling=%u rotation=%u stride=%u swap_flags=%u",
                static_cast<unsigned>(modern.AlphaMode), static_cast<unsigned>(modern.Scaling),
                static_cast<unsigned>(modern.SwapEffect), modern.BufferUsage, modern.Flags);
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain2> chain2;
    if (target.swapchain != nullptr && SUCCEEDED(target.swapchain->QueryInterface(IID_PPV_ARGS(&chain2))))
    {
        UINT latency = 0;
        chain2->GetMaximumFrameLatency(&latency);
        log_info("env frame latency=%u waitable=%d", latency,
            chain2->GetFrameLatencyWaitableObject() != nullptr ? 1 : 0);
    }
}

bool layer::bind_backend(swap_target& target)
{
    // 唯一建资源的地方 死了的链一律不给绑 挡住设备销毁期的复活
    if (is_dead(target.swapchain))
        return false;
    if (target.window != nullptr && IsWindow(target.window) == FALSE)
        return false;

    if (target.api == d3dimgui_graphics_unknown || target.swapchain == nullptr)
    {
        set_error(d3dimgui_error_device_probe);
        set_state(d3dimgui_state_failed);
        return false;
    }

    std::unique_ptr<gfx::backend> backend = gfx::create_backend(target.api);
    if (backend == nullptr)
    {
        log_error("api %u has no backend in this build", target.api);
        set_error(d3dimgui_error_backend_init);
        set_state(d3dimgui_state_failed);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (target.api == d3dimgui_graphics_d3d12)
    {
        // 按设备挑队列 进程里可能有好几条 direct 队列 拿错设备的提交就是把设备送走
        if (!hook::match_command_queue(target.device, queue) || queue == nullptr)
        {
            // 还没蹲到 direct 队列 保持 waiting 下一帧重试
            set_error(d3dimgui_error_queue_not_found);
            set_state(d3dimgui_state_waiting);
            return false;
        }
    }

    if (!create_context(target.window))
    {
        set_state(d3dimgui_state_failed);
        return false;
    }

    if (!backend->initialize(target.swapchain, queue.Get()))
    {
        log_error("backend initialize failed, api=%u", target.api);
        destroy_context();
        set_error(d3dimgui_error_backend_init);
        set_state(d3dimgui_state_failed);
        return false;
    }

    backend_ = std::move(backend);
    backend_owner_ = target.swapchain;
    attached_api_ = target.api;
    attached_device_ = target.device;

    if (target.window != nullptr)
    {
        if (input::attach(target.window))
            attached_window_ = target.window;
        else
            log_warn("window subclass failed, menu will not receive input");
    }

    set_error(d3dimgui_error_none);
    set_state(d3dimgui_state_ready);
    // 队列认准了就停掉 executecommandlists 蹲点 那条钩子在宿主每帧要过几百次
    if (queue != nullptr)
        hook::lock_command_queue();
    log_info("bound %s on swapchain %p", backend_->name(), target.swapchain);
    describe_environment(target);
    return true;
}

void layer::unbind_backend()
{
    if (user_prepared_)
    {
        user::shutdown();
        user_prepared_ = false;
    }

    // 后端析构里要放 imgui 的资源 必须在销毁上下文之前
    backend_.reset();
    backend_owner_ = nullptr;
    attached_api_ = d3dimgui_graphics_unknown;
    attached_device_ = nullptr;

    destroy_context();
    input::detach();
    attached_window_ = nullptr;
    dpi_scale_ = 1.0f;
    font_dirty_.store(true, std::memory_order_release);
}

void layer::update_dpi()
{
#if d3dimgui_apply_dpi_scale
    if (attached_window_ == nullptr)
        return;

    UINT dpi = GetDpiForWindow(attached_window_);
    if (dpi == 0)
        dpi = 96;

    const float scale = static_cast<float>(dpi) / 96.0f;
    if (scale == dpi_scale_)
        return;

    dpi_scale_ = scale;
    // 注入进已经在跑的进程时不能改它的 dpi awareness 只缩放字形
    ImGui::GetStyle().FontScaleDpi = scale;
#endif
}

bool layer::run_frame_body(d3dimgui_frame_info* info, d3dimgui_frame_fn callback, void* callback_user)
{
    __try
    {
        if (!user_prepared_)
        {
            user_prepared_ = true;
            user::initialize(info);
        }

        user::frame(info);
        if (callback != nullptr)
            callback(info, callback_user);

        draw_info();
        if (active_.load(std::memory_order_relaxed))
            draw_tabs();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("frame body raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
        user_prepared_ = false;
        return false;
    }
    return true;
}

bool layer::render_frame(const frame_source& source)
{
    if (font_dirty_.exchange(false, std::memory_order_acq_rel))
        font::rebuild();

    ImGuiIO& io = ImGui::GetIO();

    backend_->new_frame();
    input::new_frame();
    update_dpi();
    ImGui::NewFrame();

    d3dimgui_frame_info info{};
    info.api = source.api;
    info.hwnd = attached_window_;
    info.swapchain = source.surface;
    info.device = source.device;
    info.width = static_cast<int>(io.DisplaySize.x);
    info.height = static_cast<int>(io.DisplaySize.y);
    info.buffer_count = backend_->buffer_count();
    info.buffer_index = backend_->buffer_index();
    info.delta_time = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;
    info.framerate = io.Framerate;
    info.frame_index = static_cast<unsigned int>(
        frame_counter_.load(std::memory_order_relaxed) & 0xffffffffu);

    d3dimgui_frame_fn callback = nullptr;
    void* callback_user = nullptr;
    {
        std::lock_guard lock(hooks_mutex);
        callback = frame_callback_;
        callback_user = frame_callback_user_;
    }

    if (!run_frame_body(&info, callback, callback_user))
        return false;

    ImGui::Render();
    backend_->render(ImGui::GetDrawData());
    return true;
}

// 整条渲染路径包一层 seh 后端或 imgui 里出异常也只丢这一帧
// 连续几次就熔断 宁可没界面也别把宿主进程带走
__declspec(noinline) bool layer::try_render_frame(const frame_source& source)
{
    bool ok = false;
    __try
    {
        ok = render_frame(source);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("render path raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
        ok = false;
    }
    return ok;
}

void layer::note_render_failure()
{
    unbind_backend();

    const int count = render_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count >= d3dimgui_render_failure_fuse)
    {
        fused_.store(true, std::memory_order_release);
        set_state(d3dimgui_state_failed);
        set_error(d3dimgui_error_backend_init);
        log_error("render broke %d times, layer disabled to keep the host alive", count);
        return;
    }

    set_state(d3dimgui_state_lost);
    log_warn("frame dropped, rebinding next present (%d/%d)", count, d3dimgui_render_failure_fuse);
}

bool layer::draw_body(void* raw_swapchain)
{
    if (!running_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire) ||
        process_exiting())
        return false;
    if (fused_.load(std::memory_order_acquire))
        return false;

    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(raw_swapchain);
    if (swapchain == nullptr)
        return false;

    if (teardown_pending_.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(frame_mutex);
        unbind_backend();
        set_state(d3dimgui_state_lost);
        return false;
    }

    swap_target snapshot{};
    {
        std::lock_guard lock(table_mutex);
        frame_counter_.fetch_add(1, std::memory_order_relaxed);

        swap_target* target = find_target(swapchain);
        if (target == nullptr)
            target = register_target(swapchain);
        if (target == nullptr)
            return false;

        target->last_seen = frame_counter_.load(std::memory_order_relaxed);

        if (geometry_dirty_.exchange(false, std::memory_order_acq_rel))
            refresh_geometry();

        // 现在的 primary 长期不在 present 里出现 就是这条链已经不作画了
        // 游戏重建交换链或者关掉开场窗口之后 得让它让位 不然一直往死链上绑资源
        swap_target* current = primary_ != nullptr ? find_target(primary_) : nullptr;
        if (current != nullptr && current != target)
        {
            const unsigned long long now = frame_counter_.load(std::memory_order_relaxed);
            if (now > current->last_seen + d3dimgui_stale_primary_frames)
            {
                log_warn("swapchain %p idle %llu frames, dropped as primary", current->swapchain,
                    now - current->last_seen);
                IDXGISwapChain* retired = current->swapchain;
                *current = swap_target{};
                if (target_count_ > 0)
                    --target_count_;
                if (primary_ == retired)
                    primary_ = nullptr;
                if (manual_primary_ == retired)
                    manual_primary_ = nullptr;
                evaluate_primary();
            }
        }

        if (manual_primary_ != nullptr && swapchain != manual_primary_)
            return false;
        if (manual_primary_ == nullptr && primary_ != swapchain)
        {
            evaluate_primary();
            if (primary_ != swapchain)
                return false;
        }

        snapshot = *target;
    }

    std::lock_guard lock(frame_mutex);

    const bool bound_here = backend_owner_ == snapshot.swapchain && snapshot.device != nullptr &&
        attached_device_ == snapshot.device;
    if (!bound_here || state_.load(std::memory_order_acquire) != d3dimgui_state_ready)
    {
        // 只有真的拆过东西才算一次重绑 dx12 头几帧蹲不到队列是等而不是绑错
        // 那种情况下面向 waiting 重试 不进熔断计数
        const bool real_rebind = backend_owner_ != nullptr || context_ != nullptr;
        if (real_rebind)
        {
            log_warn("rebind, state=%u owner=%p want=%p attached=%p want_device=%p streak=%u",
                state_.load(std::memory_order_relaxed), backend_owner_, snapshot.swapchain,
                attached_device_, snapshot.device, rebind_streak_ + 1);
            if (++rebind_streak_ > d3dimgui_rebind_fuse)
            {
                fused_.store(true, std::memory_order_release);
                set_state(d3dimgui_state_failed);
                set_error(d3dimgui_error_backend_init);
                log_error("rebinding %u times in a row without drawing, layer disabled",
                    rebind_streak_);
                return false;
            }
        }

        unbind_backend();
        return bind_backend(snapshot);
    }

    const frame_source source{ snapshot.api, snapshot.swapchain, snapshot.device };
    if (!try_render_frame(source))
    {
        note_render_failure();
        return false;
    }

    rebind_streak_ = 0;

    render_failures_.store(0, std::memory_order_relaxed);
    return true;
}

bool layer::draw(void* raw_swapchain)
{
    // 认目标和建资源整段都在调 d3d 设备移除或者驱动发火时它们会抛 c++ 异常
    // try_render_frame 只盖住了渲染那一段 这里不盖住异常就直接跑出宿主的 present 里
    bool ok = false;
    __try
    {
        ok = draw_body(raw_swapchain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        log_error("draw path raised 0x%08lx", static_cast<unsigned long>(GetExceptionCode()));
        ok = false;
    }
    return ok;
}

bool layer::bind_backend_gl(void* device_context, HWND window)
{
    std::unique_ptr<gfx::backend> backend = gfx::create_backend(d3dimgui_graphics_opengl);
    if (backend == nullptr)
    {
        log_error("this build has no opengl backend");
        set_error(d3dimgui_error_backend_init);
        set_state(d3dimgui_state_failed);
        return false;
    }

    if (!create_context(window))
    {
        set_state(d3dimgui_state_failed);
        return false;
    }

    // initialize 里要建共享上下文 只能在游戏自己的上下文正 current 的时候做
    // 我们是从它的 swapbuffers 里进来的 正好
    if (!backend->initialize(device_context, nullptr))
    {
        destroy_context();
        set_error(d3dimgui_error_backend_init);
        set_state(d3dimgui_state_waiting);
        return false;
    }

    backend_ = std::move(backend);
    backend_owner_ = device_context;
    attached_api_ = d3dimgui_graphics_opengl;
    attached_device_ = backend_->native_device();

    if (input::attach(window))
        attached_window_ = window;
    else
        log_warn("gl window subclass failed, menu will not receive input");

    set_error(d3dimgui_error_none);
    set_state(d3dimgui_state_ready);
    log_info("bound opengl on surface %p window %p", device_context, window);
    return true;
}

bool layer::draw_gl(void* device_context)
{
    if (!running_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire) ||
        process_exiting() || fused_.load(std::memory_order_acquire))
        return false;
    if (device_context == nullptr)
        return false;

    frame_counter_.fetch_add(1, std::memory_order_relaxed);
    route_.store(3, std::memory_order_relaxed);

    // 内嵌浏览器那类也会 swap 认不出顶层窗口的表面一律不画
    HWND window = hook::gl::surface_window(static_cast<HDC>(device_context));
    if (window == nullptr)
        return false;

    std::lock_guard lock(frame_mutex);

    if (backend_owner_ != device_context)
    {
        if (backend_ != nullptr)
        {
            // dxgi 那条先绑上了就不抢 一次画两个后端必出事
            if (!gl_conflict_logged_)
            {
                gl_conflict_logged_ = true;
                log_warn("a presentation target is already bound, ignoring the gl surface");
            }
            return false;
        }
        if (!bind_backend_gl(device_context, window))
            return false;
    }
    else if (attached_window_ != window ||
             state_.load(std::memory_order_acquire) != d3dimgui_state_ready)
    {
        // 同一个 hdc 换了窗口 或者上一次没绑成 整块重来
        unbind_backend();
        if (!bind_backend_gl(device_context, window))
            return false;
    }

    const frame_source source{ d3dimgui_graphics_opengl, device_context, attached_device_ };
    if (!try_render_frame(source))
    {
        note_render_failure();
        return false;
    }

    render_failures_.store(0, std::memory_order_relaxed);
    return true;
}

void layer::pre_resize(void* raw_swapchain)
{
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(raw_swapchain);
    if (swapchain == nullptr)
        return;

    std::lock_guard lock(frame_mutex);
    if (backend_owner_ != swapchain || backend_ == nullptr)
        return;

    backend_->pre_reset();
}

void layer::post_resize(void* raw_swapchain, const HRESULT result)
{
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(raw_swapchain);
    if (swapchain == nullptr)
        return;

    // 之后有一次 resize 成功了 说明先前那次只是偶发 把这条链放回来
    if (SUCCEEDED(result))
        unmark_dead(swapchain);

    std::lock_guard lock(frame_mutex);
    if (backend_owner_ != swapchain || backend_ == nullptr)
        return;

    log_info("resizebuffers on %p result=0x%08lx", swapchain, static_cast<unsigned long>(result));

    if (FAILED(result))
    {
        // resize 都失败了说明这条链已经不可用 再绑就是在垂死的设备上建资源
        log_warn("resizebuffers failed 0x%08lx on %p, treating it as dead",
            static_cast<unsigned long>(result), swapchain);
        mark_dead(swapchain);
        unbind_backend();
        set_state(d3dimgui_state_lost);
        return;
    }

    // 交换链自己变了尺寸 登记项里的面积和可见性要跟着重算
    geometry_dirty_.store(true, std::memory_order_release);

    if (!backend_->post_reset())
    {
        log_warn("post reset failed, rebuilding backend");
        unbind_backend();
        set_state(d3dimgui_state_lost);
    }
}

void layer::on_present_result(void* raw_swapchain, const HRESULT result)
{
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(raw_swapchain);
    if (swapchain == nullptr)
        return;

    {
        std::lock_guard lock(table_mutex);
        swap_target* target = find_target(swapchain);
        if (target == nullptr)
            return;

        // 整条登记项一起清 只把 valid 置假的话 device_ref 还攥着垂死的设备
        // 宿主要重建交换链时就被我们这个引用挡住
        if (target_count_ > 0)
            --target_count_;
        if (primary_ == swapchain)
            primary_ = nullptr;
        if (manual_primary_ == swapchain)
            manual_primary_ = nullptr;
        *target = swap_target{};
        evaluate_primary();
    }

    // 设备级失败之后这条地址还可能被新 swapchain 复用 必须彻底重绑
    mark_dead(swapchain);
    // 设备没了 抓住旧队列不放就是僵尸对象 让钩子重新蹲
    hook::forget_command_queues();

    std::lock_guard lock(frame_mutex);
    if (backend_owner_ == swapchain)
    {
        unbind_backend();
        log_warn("device level error 0x%08lx, backend dropped", static_cast<unsigned long>(result));
    }
    set_state(d3dimgui_state_lost);
    set_error(d3dimgui_error_device_removed);
}

void layer::on_window_destroyed()
{
    if (!running_.load(std::memory_order_acquire) || stopping_.load(std::memory_order_acquire))
        return;

    // 先把这条链钉死 再拆资源
    // 窗口开始销毁之后游戏还会再 present 几帧 不钉死的话下一帧就把后端建回来了
    // 那是在一台正在放掉设备的机器上重新申请资源 宿主收尾必然撞上我们
    HWND window = attached_window_;
    {
        std::lock_guard lock(table_mutex);
        for (swap_target& target : targets_)
        {
            if (!target.valid)
                continue;
            // 认不出是哪扇窗就不敢钉链 免得把还活着的那条一起废了
            if (window == nullptr || target.window != window)
                continue;

            IDXGISwapChain* chain = target.swapchain;
            target = swap_target{};
            if (target_count_ > 0)
                --target_count_;
            if (primary_ == chain)
                primary_ = nullptr;
            if (manual_primary_ == chain)
                manual_primary_ = nullptr;
            mark_dead(chain);
        }
    }

    // 这是窗口线程调过来的 可以拿 frame_mutex 但要限时
    // 渲染线程里 imgui 的 setcapture 之类可能正往这个窗口发消息
    // 两边互等就是死锁 拿不到就推到下一帧
    std::unique_lock lock(frame_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(150)))
    {
        teardown_pending_.store(true, std::memory_order_release);
        log_warn("window destroyed while a frame was in flight, teardown deferred");
        return;
    }

    log_info("host window destroyed, releasing backend before the device goes away");
    unbind_backend();
    set_state(d3dimgui_state_lost);
}

void layer::note_route(const unsigned int route)
{
    route_.store(route, std::memory_order_relaxed);
}

void layer::set_command_queue(void* command_queue)
{
    hook::set_command_queue(static_cast<ID3D12CommandQueue*>(command_queue));
}

void layer::set_primary(void* raw_swapchain)
{
    std::lock_guard lock(table_mutex);
    manual_primary_ = static_cast<IDXGISwapChain*>(raw_swapchain);
    evaluate_primary();
    if (manual_primary_ != nullptr && backend_owner_ != manual_primary_)
        set_state(d3dimgui_state_lost);
}

void layer::set_active(const bool active)
{
    active_.store(active, std::memory_order_release);
    const unsigned int flags = flags_.load(std::memory_order_relaxed);
    input::set_capturing(active && (flags & d3dimgui_flag_suppress_input) != 0);
}

bool layer::active() const
{
    return active_.load(std::memory_order_acquire);
}

void layer::toggle()
{
    set_active(!active_.load(std::memory_order_acquire));
}

void layer::set_flags(const unsigned int flags)
{
    flags_.store(flags, std::memory_order_release);
    input::set_cursor_control((flags & d3dimgui_flag_manage_cursor) != 0);
    input::set_capturing(active_.load(std::memory_order_relaxed) &&
        (flags & d3dimgui_flag_suppress_input) != 0);
}

unsigned int layer::flags() const
{
    return flags_.load(std::memory_order_acquire);
}

bool layer::set_frame_callback(d3dimgui_frame_fn callback, void* user_data)
{
    std::lock_guard lock(hooks_mutex);
    frame_callback_ = callback;
    frame_callback_user_ = user_data;
    return true;
}

bool layer::add_menu_tab(const char* name, d3dimgui_menu_fn callback, void* user_data)
{
    if (name == nullptr || callback == nullptr)
        return false;

    std::lock_guard lock(hooks_mutex);
    menu_tab_entry entry;
    entry.name = name;
    entry.draw = callback;
    entry.user_data = user_data;
    menu_tabs_.push_back(std::move(entry));
    return true;
}

void layer::clear_menu_tabs()
{
    std::lock_guard lock(hooks_mutex);
    menu_tabs_.clear();
}

bool layer::load_font(const void* data, unsigned int size, float pixel)
{
    if (!font::push(data, size, pixel))
        return false;

    // 不在这里直接重建图集 调用方很可能正站在帧里 拿 frame_mutex 会自锁
    // 标脏就行 render_frame 下一帧开头处理
    font_dirty_.store(true, std::memory_order_release);
    return true;
}

void layer::reset_fonts()
{
    font::reset();
    font_dirty_.store(true, std::memory_order_release);
}

unsigned int layer::count_targets() const
{
    unsigned int count = 0;
    for (const swap_target& target : targets_)
    {
        if (target.valid)
            ++count;
    }
    return count;
}

void layer::fill_status(d3dimgui_status* out) const
{
    if (out == nullptr || out->struct_size < sizeof(d3dimgui_status))
        return;

    *out = d3dimgui_status{};
    out->struct_size = sizeof(d3dimgui_status);
    out->state = state_.load(std::memory_order_relaxed);
    out->last_error = error_.load(std::memory_order_relaxed);
    out->active = active_.load(std::memory_order_relaxed) ? 1 : 0;
    out->frame_index = static_cast<unsigned int>(
        frame_counter_.load(std::memory_order_relaxed) & 0xffffffffu);
    out->imgui_version = IMGUI_VERSION;
    out->dpi_scale = dpi_scale_;

    if (table_mutex.try_lock())
    {
        out->target_count = count_targets();
        out->swapchain = primary_;
        for (const swap_target& target : targets_)
        {
            if (target.valid && target.swapchain == primary_)
            {
                out->api = target.api;
                out->hwnd = target.window;
                out->device = target.device;
                break;
            }
        }
        table_mutex.unlock();
    }

    if (out->hwnd == nullptr)
        out->hwnd = attached_window_;
    if (out->device == nullptr)
        out->device = attached_device_;
    if (out->api == d3dimgui_graphics_unknown)
        out->api = attached_api_;

    // 已经在帧里的时候 try_lock 会失败 这些字段留给调用方下次再读
    if (frame_mutex.try_lock())
    {
        if (backend_ != nullptr)
        {
            out->buffer_count = backend_->buffer_count();
            out->buffer_index = backend_->buffer_index();
            out->backend_name = backend_->name();
        }
        if (context_ != nullptr)
        {
            const ImGuiIO& io = ImGui::GetIO();
            out->delta_time = io.DeltaTime;
            out->framerate = io.Framerate;
            out->width = static_cast<int>(io.DisplaySize.x);
            out->height = static_cast<int>(io.DisplaySize.y);
        }
        frame_mutex.unlock();
    }

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (hook::get_command_queue(queue))
        out->command_queue = queue.Get();
}

void* layer::imgui_context() const
{
    return context_;
}

// 唯一的框架窗口 没有菜单页就一个 imgui 调用都不发
// 页表先拷一份再画 回调里可能反过来调 add_menu_tab 或 clear_menu_tabs 带着锁进去就是自锁
void layer::refresh_geometry()
{
    // 面积和可见性只在登记时探过一次 游戏改分辨率或者窗口切全屏之后打分用的还是老数据
    // 只在握着 table_mutex 的帧里调 别处进来会撞锁顺序
    for (swap_target& target : targets_)
    {
        if (!target.valid)
            continue;

        const unsigned int ordinal = target.ordinal;
        const unsigned long long last_seen = target.last_seen;
        IDXGISwapChain* chain = target.swapchain;

        if (probe(target, chain))
        {
            target.ordinal = ordinal;
            target.last_seen = last_seen;
            continue;
        }

        target = swap_target{};
        if (target_count_ > 0)
            --target_count_;
        if (primary_ == chain)
            primary_ = nullptr;
        if (manual_primary_ == chain)
            manual_primary_ = nullptr;
    }

    evaluate_primary();
}

void layer::draw_info()
{
    if (!d3dimgui_show_info || info_closed_)
        return;

    d3dimgui_status status{};
    status.struct_size = sizeof(status);
    fill_status(&status);
    // 面板就在帧里画 这时候 frame_mutex 已经被我们握着
    // fill_status 里那两把 try_lock 必然拿不到 这里把关键项补全
    status.backend_name = backend_ != nullptr ? backend_->name() : nullptr;
    status.buffer_count = backend_ != nullptr ? backend_->buffer_count() : 0;
    status.buffer_index = backend_ != nullptr ? backend_->buffer_index() : 0;
    status.api = attached_api_;
    status.hwnd = attached_window_;
    status.device = attached_device_;
    status.active = active_.load(std::memory_order_relaxed) ? 1 : 0;
    status.dpi_scale = dpi_scale_;
    if (context_ != nullptr)
    {
        const ImGuiIO& io = ImGui::GetIO();
        status.width = static_cast<int>(io.DisplaySize.x);
        status.height = static_cast<int>(io.DisplaySize.y);
        status.framerate = io.Framerate;
    }

    // 之前这里写死 5 项 opengl 落到越界分支上就成一个问号了 数组多大就认多大
    static const char* const api_names[] = { "unknown", "d3d9", "d3d10", "d3d11", "d3d12",
        "opengl", "vulkan" };
    static const char* const state_names[] = { "idle", "waiting", "ready", "lost", "failed" };
    const char* api = status.api < static_cast<unsigned int>(std::size(api_names))
        ? api_names[status.api] : "?";
    const char* state = status.state < static_cast<unsigned int>(std::size(state_names))
        ? state_names[status.state] : "?";

    const bool is_gl = status.api == d3dimgui_graphics_opengl;
    const unsigned int route = route_.load(std::memory_order_relaxed);
    const char* route_name = route == 2 ? "IDXGISwapChain1::Present1"
        : route == 3 ? "wglSwapBuffers" : route == 1 ? "IDXGISwapChain::Present" : "-";
    // gl 不进那张交换链表 绑定成功就算一个表面 不然这里永远是 0 看着像没目标
    unsigned int surfaces = status.target_count;
    if (surfaces == 0 && backend_owner_ != nullptr)
        surfaces = 1;

    // 内置字体没有中文字形 这里的标签一律 ascii 要中文自己往框架里塞 ttf
    char line[160];
    bool open = true;
    if (ImGui::Begin("d3dimgui info", &open) == false)
    {
        ImGui::End();
        info_closed_ = !open;
        return;
    }

    // 后端名和 api 名重复就不打 gl 两者都叫 opengl 再抄一遍看着像坏了
    char tail[48] = {};
    if (status.backend_name != nullptr && std::strcmp(status.backend_name, api) != 0)
        snprintf(tail, sizeof(tail), "  %s", status.backend_name);
    snprintf(line, sizeof(line), "api        %s%s", api, tail);
    ImGui::TextUnformatted(line);
    snprintf(line, sizeof(line), "state      %s  error %d", state, status.last_error);
    ImGui::TextUnformatted(line);
    snprintf(line, sizeof(line), "size       %dx%d  dpi %.2f", status.width, status.height,
        static_cast<double>(status.dpi_scale));
    ImGui::TextUnformatted(line);
    // gl 没有后缓冲轮转 打 0/1 是纯噪音
    if (!is_gl)
    {
        snprintf(line, sizeof(line), "buffer     %u/%u", status.buffer_index, status.buffer_count);
        ImGui::TextUnformatted(line);
    }
    snprintf(line, sizeof(line), "frame      %u  %.1f fps", status.frame_index,
        static_cast<double>(status.framerate));
    ImGui::TextUnformatted(line);
    // 标签跟着 api 走 gl 那个指针是 hdc 不是交换链 写错名字比不写更容易误导
    snprintf(line, sizeof(line), "%s  %p  %s %p",
        is_gl ? "hdc        " : "swapchain  ", backend_owner_,
        is_gl ? "context" : "device", status.device);
    ImGui::TextUnformatted(line);
    snprintf(line, sizeof(line), "window     %p  surfaces %u", status.hwnd, surfaces);
    ImGui::TextUnformatted(line);
    snprintf(line, sizeof(line), "route      %s", route_name);
    ImGui::TextUnformatted(line);
    if (adapter_[0] != '\0')
    {
        snprintf(line, sizeof(line), "adapter    %s", adapter_);
        ImGui::TextUnformatted(line);
    }
    if (status.api == d3dimgui_graphics_d3d12)
    {
        snprintf(line, sizeof(line), "queue      %p", status.command_queue);
        ImGui::TextUnformatted(line);
    }
    snprintf(line, sizeof(line), "imgui      %s  menu %s",
        status.imgui_version != nullptr ? status.imgui_version : "-",
        status.active != 0 ? "open" : "closed");
    ImGui::TextUnformatted(line);
    ImGui::TextUnformatted("insert toggle menu   end unload");

    ImGui::End();
    info_closed_ = !open;
}

void layer::draw_tabs()
{
    std::vector<menu_tab_entry> tabs;
    {
        std::lock_guard lock(hooks_mutex);
        tabs = menu_tabs_;
    }
    if (tabs.empty())
        return;

    if (!ImGui::Begin("d3dimgui"))
    {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("d3dimgui_tab_bar"))
    {
        for (size_t i = 0; i < tabs.size(); ++i)
        {
            const menu_tab_entry& tab = tabs[i];
            if (tab.draw == nullptr)
                continue;

            // 同名页会被 imgui 当成同一个 id 序号藏在 ## 后面不显示出来
            const std::string label = tab.name + "##d3dimgui_tab_" + std::to_string(i);
            if (ImGui::BeginTabItem(label.c_str()))
            {
                tab.draw(tab.user_data);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void layer::set_self_module(const HMODULE module)
{
    self_module_ = module;
}

namespace
{
// 卸载不能发生在调用栈里 我们自己的代码还在 dll 上跑着
// 起一个线程 等当前这帧走完 拆干净再 FreeLibraryAndExitThread
DWORD WINAPI unload_worker(LPVOID)
{
    Sleep(150);
    if (!layer::instance().shutdown())
    {
        log_error("unload aborted, teardown did not finish");
        return 1;
    }
    Sleep(50);
#if d3dimgui_self_unload
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&unload_worker), &self) != FALSE && self != nullptr)
    {
        log_info("unloading self at %p", self);
        FreeLibraryAndExitThread(self, 0);
    }
#else
    // 摘完钩子拆完资源就停在这 镜像留在原地
    // 这一刻还可能有线程站在我们那些 detour 的中间 unmap 就是把它的返回地址抽走
    log_info("teardown finished, image kept mapped because d3dimgui_self_unload is 0");
#endif
    ExitThread(1);
    return 1;
}
}

void layer::unload()
{
    if (unloading_.exchange(true, std::memory_order_acq_rel))
        return;

    log_info("unload requested");
    HANDLE thread = CreateThread(nullptr, 0, unload_worker, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
    {
        log_error("cannot start unload thread, doing an in-place shutdown instead");
        unloading_.store(false, std::memory_order_release);
        shutdown();
    }
}
}
