#pragma once
#ifndef d3dimgui_layer_h_
#define d3dimgui_layer_h_

#include "gfx/backend.h"

#include <d3d_imgui_layer/d3dimgui.h>
#include <d3d_imgui_layer/d3dimgui_settings.h>

#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ImGuiContext;

namespace d3d_imgui_layer
{
// 进程正在退出 由 dllmain 置位 置了之后所有路径静默
// 退出阶段 dxgi 和驱动可能已经先拆完了 这时候再去 release 宿主的对象必炸
void set_process_exiting();
bool process_exiting();

// 一条被登记过的 swapchain 只在 present 期间用它的裸指针 不加引用
// 游戏重建 swapchain 时地址可能复用 所以额外记 device 做二次校验
// device 必须被 device_ref 撑住 裸指针悬了之后地址复用会把新设备认成老设备
struct swap_target
{
    IDXGISwapChain* swapchain = nullptr;
    void* device = nullptr;
    Microsoft::WRL::ComPtr<IUnknown> device_ref;
    HWND window = nullptr;
    unsigned int api = d3dimgui_graphics_unknown;
    unsigned int area = 0;
    unsigned int ordinal = 0;
    bool top_level = false;
    bool visible = false;
    bool valid = false;
    unsigned long long last_seen = 0;

    unsigned int score() const;
};

// 二次开发登记的菜单页 只存名字和回调 别的状态一概不碰
struct menu_tab_entry
{
    std::string name;
    d3dimgui_menu_fn draw = nullptr;
    void* user_data = nullptr;
};

// 锁顺序永远是 table_mutex -> frame_mutex -> hooks_mutex 反向禁止
// 窗口线程只碰原子量和 input 模块自己的事件锁 不进这三把锁
// 渲染那一段真正需要的三样东西 dxgi 和 opengl 都填得出来
// 不直接把 swap_target 传下去是因为 gl 那边没有交换链 不该让它去解引用一个交换链指针
struct frame_source
{
    unsigned int api = d3dimgui_graphics_unknown;
    void* surface = nullptr;
    void* device = nullptr;
};

class layer
{
public:
    static layer& instance();

    bool startup(const d3dimgui_desc* desc);
    // 返回 false 表示有帧卡住没拆干净 这时不能卸载自己
    bool shutdown();
    bool running() const;
    bool stopping() const;

    // 唯一渲染入口 hook 和手动模式都走这里
    bool draw(void* raw_swapchain);
    // opengl 那条 surface 是 hdc 只认一条 gl 表面 换 hdc 就等于换目标
    bool draw_gl(void* device_context);
    void pre_resize(void* raw_swapchain);
    void post_resize(void* raw_swapchain, HRESULT result);
    void on_present_result(void* raw_swapchain, HRESULT result);

    void set_command_queue(void* command_queue);
    // 这一帧是从哪个提交钩子进来的 1 present 2 present1 3 wglSwapBuffers
    void note_route(unsigned int route);

    // 摘钩 拆后端 然后把自己从宿主进程 unmapped
    void unload();
    void set_self_module(HMODULE module);

    // 宿主窗口销毁 抢在它放掉设备之前把后端拆了
    // 只要我们还握着设备引用 游戏那份 release 就不是最后一次
    // 之后 d3d11 自己收尾会撞上没释放完的交换链 抛 invalid_call 把宿主带走
    void on_window_destroyed();
    void set_primary(void* raw_swapchain);

    void set_active(bool active);
    bool active() const;
    void toggle();

    void set_flags(unsigned int flags);
    unsigned int flags() const;

    bool set_frame_callback(d3dimgui_frame_fn callback, void* user_data);
    bool add_menu_tab(const char* name, d3dimgui_menu_fn callback, void* user_data);
    void clear_menu_tabs();

    bool load_font(const void* data, unsigned int size, float pixel);
    void reset_fonts();

    void fill_status(d3dimgui_status* out) const;
    void* imgui_context() const;
    int last_error() const;

private:
    layer() = default;
    ~layer();
    layer(const layer&) = delete;
    layer& operator=(const layer&) = delete;

    swap_target* find_target(IDXGISwapChain* swapchain);
    swap_target* register_target(IDXGISwapChain* swapchain);
    void evaluate_primary();
    bool probe(swap_target& target, IDXGISwapChain* swapchain);
    bool draw_body(void* raw_swapchain);

    bool create_context(HWND window);
    void destroy_context();
    bool bind_backend(swap_target& target);
    bool bind_backend_gl(void* device_context, HWND window);
    void unbind_backend();
    bool render_frame(const frame_source& source);
    __declspec(noinline) bool try_render_frame(const frame_source& source);
    __declspec(noinline) bool run_frame_body(d3dimgui_frame_info* info,
        d3dimgui_frame_fn callback, void* callback_user);
    void note_render_failure();
    void draw_tabs();
    void draw_info();
    void describe_environment(const swap_target& target);
    void refresh_geometry();
    void update_dpi();
    unsigned int count_targets() const;

    void set_error(int error);
    void set_state(unsigned int state);

    mutable std::mutex table_mutex;
    // 窗口线程拆后端时要限时拿这把锁 所以用 timed 版
    mutable std::timed_mutex frame_mutex;
    // 菜单页和回调表单独一把叶锁 帧内也会取它 别和 table 混用
    std::mutex hooks_mutex;

    std::array<swap_target, d3dimgui_max_targets> targets_{};
    unsigned int target_count_ = 0;
    unsigned int ordinal_next_ = 1;
    std::atomic<unsigned long long> frame_counter_{0};

    IDXGISwapChain* primary_ = nullptr;
    IDXGISwapChain* manual_primary_ = nullptr;
    // 绑在哪个提交表面上 d3d 是一条交换链 gl 是一个 hdc 这里只比身份从不解引用
    void* backend_owner_ = nullptr;
    HWND manual_window_hints_ = nullptr;
    // 设备销毁时 dxgi 会自己内部调一次 resizebuffers 并且必然失败
    // 这种 swapchain 不能再绑回去 否则会在垂死的设备上重建资源然后崩
    std::array<IDXGISwapChain*, d3dimgui_max_targets> dead_targets_{};
    unsigned int dead_count_ = 0;
    // 死亡名单单独一把叶锁 post_resize 里也要写 那里已经握着 frame_mutex
    mutable std::mutex dead_mutex;
    bool is_dead(IDXGISwapChain* swapchain) const;
    void mark_dead(IDXGISwapChain* swapchain);
    void unmark_dead(IDXGISwapChain* swapchain);

    std::unique_ptr<gfx::backend> backend_;
    ImGuiContext* context_ = nullptr;
    // 连续重绑计数 只由握着 frame_mutex 的线程读写
    unsigned int rebind_streak_ = 0;
    // 信息窗被用户自己关掉就不再画
    bool info_closed_ = false;
    // 改分辨率之后要重算面积 这里只置标志 真正重探在握着 table_mutex 的地方做
    std::atomic<bool> geometry_dirty_{false};
    HWND attached_window_ = nullptr;
    unsigned int attached_api_ = d3dimgui_graphics_unknown;
    void* attached_device_ = nullptr;
    bool win32_bound_ = false;
    std::vector<menu_tab_entry> menu_tabs_;
    d3dimgui_frame_fn frame_callback_ = nullptr;
    void* frame_callback_user_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> active_{false};
    std::atomic<unsigned int> state_{d3dimgui_state_idle};
    std::atomic<int> error_{d3dimgui_error_none};
    std::atomic<unsigned int> flags_{
        (d3dimgui_install_hooks ? d3dimgui_flag_install_hooks : 0) |
        (d3dimgui_hook_present1 ? d3dimgui_flag_hook_present1 : 0) |
        (d3dimgui_suppress_input ? d3dimgui_flag_suppress_input : 0) |
        (d3dimgui_hook_cursor ? d3dimgui_flag_manage_cursor : 0)};

    float dpi_scale_ = 1.0f;
    float font_pixel_ = d3dimgui_default_font_pixel;
    bool user_prepared_ = false;
    std::atomic<bool> font_dirty_{true};
    std::atomic<int> render_failures_{0};
    std::atomic<bool> fused_{false};
    std::atomic<bool> teardown_pending_{false};
    std::atomic<bool> unloading_{false};
    bool gl_conflict_logged_ = false;
    std::atomic<unsigned int> route_{0};
    char adapter_[96] = {};
    HMODULE self_module_ = nullptr;
};
}

#endif
