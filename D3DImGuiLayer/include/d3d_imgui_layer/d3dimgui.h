// d3dimgui.h - Direct3D 通用 ImGui 绘制层 唯一对外头
//
// 用法一 注入即用
//   把这个 dll 注入任意 d3d11 d3d12 或 opengl 程序 框架自己找挂钩点 自己建 imgui 上下文
//   除了 d3dimgui_settings.h 里那个信息窗 框架不画任何自己的东西
//   界面靠 d3dimgui_add_menu_tab 登记菜单页 不改源码也可以 d3dimgui_set_frame_callback 挂每帧回调
//   改源码就写 src/user/draw.cpp
//
// 用法二 手动接管
//   定义 d3dimgui_install_hooks 为 0 自己钩 present 在调用真 present 之前调 d3dimgui_draw
//   device 和 swapchain 通过 d3dimgui_desc 传进来 或者让框架从 swapchain 探
#pragma once
#ifndef d3dimgui_h_
#define d3dimgui_h_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "d3dimgui_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// 图形接口 目前 11 12 和 opengl 有实现 9 和 10 是预留位 vulkan 还没做
enum d3dimgui_graphics_api
{
    d3dimgui_graphics_unknown = 0,
    d3dimgui_graphics_d3d9    = 1,
    d3dimgui_graphics_d3d10   = 2,
    d3dimgui_graphics_d3d11   = 3,
    d3dimgui_graphics_d3d12   = 4,
    d3dimgui_graphics_opengl  = 5,
    d3dimgui_graphics_vulkan  = 6
};

// 框架状态机
enum d3dimgui_state
{
    d3dimgui_state_idle     = 0, // 没 startup 或 hook 还没装
    d3dimgui_state_waiting  = 1, // hook 已装 还没认到能建后端的 swapchain
    d3dimgui_state_ready    = 2, // 正常出图
    d3dimgui_state_lost     = 3, // 设备丢失或 resize 中 等下一次 present 重建
    d3dimgui_state_failed   = 4  // 建后端失败 详情看 last_error
};

enum d3dimgui_error
{
    d3dimgui_error_none               = 0,
    d3dimgui_error_hook_install       = 1,
    d3dimgui_error_vtable_resolve     = 2,
    d3dimgui_error_device_probe       = 3,
    d3dimgui_error_backend_init       = 4,
    d3dimgui_error_queue_not_found    = 5, // dx12 还没蹲到 direct 队列
    d3dimgui_error_imgui_context      = 6,
    d3dimgui_error_font               = 7,
    d3dimgui_error_device_removed     = 8
};

// 运行期开关 初值来自 d3dimgui_settings.h
enum d3dimgui_flag
{
    d3dimgui_flag_install_hooks     = 1u << 0,
    d3dimgui_flag_hook_present1     = 1u << 1,
    d3dimgui_flag_suppress_input    = 1u << 3,
    d3dimgui_flag_manage_cursor     = 1u << 4
};

struct d3dimgui_desc
{
    unsigned int struct_size;    // 填 sizeof d3dimgui_desc 框架据此判兼容
    unsigned int flags;          // d3dimgui_flag 组合 0 表示全用编译期默认
    void* hwnd;                  // 可空 空则由 swapchain 的 OutputWindow 决定
    void* device;                // 可空 手动模式建议填
    void* swapchain;             // 可空 填了直接当主目标 不再走打分
    float  font_pixel;           // 0 用 d3dimgui_default_font_pixel
    const void* font_data;       // ttf 内存块 框架不接管所有权
    unsigned int font_size;
    int  toggle_key;             // 0 用 d3dimgui_toggle_key
    int  unload_key;             // 0 用 d3dimgui_unload_key 负数表示不绑
    unsigned int surface_api;     // 手动模式填 swapchain 还是 hdc 0 按 d3d 交换链处理
};

// 每帧喂给回调的信息 只在回调期间有效 别存指针
struct d3dimgui_frame_info
{
    unsigned int api;            // d3dimgui_graphics_*
    void* hwnd;
    void* swapchain;
    void* device;
    int   width;
    int   height;
    unsigned int buffer_count;
    unsigned int buffer_index;
    float delta_time;
    float framerate;
    unsigned int frame_index;
    void* user_data;
};

// 状态快照 给宿主查询和日志用
struct d3dimgui_status
{
    unsigned int struct_size;
    unsigned int api;
    unsigned int state;
    int   last_error;
    int   width;
    int   height;
    unsigned int buffer_count;
    unsigned int buffer_index;
    float framerate;
    float delta_time;
    unsigned int frame_index;
    unsigned int target_count;   // 登记了几条 swapchain
    void* hwnd;
    void* swapchain;
    void* device;
    void* command_queue;         // dx12 抓到的 direct 队列 没抓到为 null
    int   active;
    float dpi_scale;
    const char* backend_name;    // 静态串 长期有效
    const char* imgui_version;   // 静态串 长期有效
};

typedef void (*d3dimgui_frame_fn)(const struct d3dimgui_frame_info* info, void* user_data);
typedef void (*d3dimgui_menu_fn)(void* user_data);
typedef void (*d3dimgui_log_fn)(unsigned int level, const char* message, void* user_data);

enum
{
    d3dimgui_log_info  = 0,
    d3dimgui_log_warn  = 1,
    d3dimgui_log_error = 2
};

// ---- 生命周期 ----
// desc 传 null 表示全用编译期默认 注入模式就是这么用
d3dimgui_export int  d3dimgui_startup(const struct d3dimgui_desc* desc);
d3dimgui_export void d3dimgui_shutdown(void);
d3dimgui_export int  d3dimgui_is_running(void);

// ---- 渲染入口 ----
// 手动模式在真 present 之前调一次 自动模式的 hook 内部走的也是这里
d3dimgui_export int  d3dimgui_draw(void* swapchain);
// opengl 手动模式在真 wglSwapBuffers 之前调 传那个 hdc 自动模式由框架的 wgl 钩子调
d3dimgui_export int  d3dimgui_draw_gl(void* device_context);
// 手动模式要自己在 ResizeBuffers 前后各调一次
d3dimgui_export void d3dimgui_pre_resize(void* swapchain);
d3dimgui_export void d3dimgui_post_resize(void* swapchain, int result);
// dx12 手动模式没有队列蹲点时用它把队列交进来
d3dimgui_export void d3dimgui_set_command_queue(void* command_queue);

// ---- 主目标 ----
// 默认按顶层窗口和面积打分选 手动钉一个就跳过打分
d3dimgui_export void d3dimgui_set_primary_target(void* swapchain);

// ---- 呼出与输入 ----
d3dimgui_export void d3dimgui_set_active(int active);
d3dimgui_export int  d3dimgui_is_active(void);
d3dimgui_export void d3dimgui_toggle(void);
d3dimgui_export void d3dimgui_set_toggle_key(int vk);
d3dimgui_export void d3dimgui_set_flags(unsigned int flags);
d3dimgui_export unsigned int d3dimgui_get_flags(void);

// ---- 卸载 ----
d3dimgui_export void d3dimgui_set_unload_key(int vk);
// 拆钩子 拆后端 然后把自己从宿主进程里卸掉
// 拆不干净时会放弃卸载只停渲染 免得把宿主挂住
d3dimgui_export void d3dimgui_unload(void);

// ---- 二次开发挂载点 ----
// 回调固定在 present 线程执行 里面不许长阻塞
d3dimgui_export int  d3dimgui_set_frame_callback(d3dimgui_frame_fn callback, void* user_data);
// 至少登记一页框架才建那个标题为 d3dimgui 的窗 一页都没有就什么都不画
d3dimgui_export int  d3dimgui_add_menu_tab(const char* name, d3dimgui_menu_fn callback, void* user_data);
d3dimgui_export void d3dimgui_clear_menu_tabs(void);

// ---- 字体 ----
// imgui 1.92 是按需烘焙字形 只要 ttf 里有中文就直接能显示 不用再传字符区间
// 任何时候调都有效 框架会清空图集重建 多次调用是叠加不是替换
d3dimgui_export int  d3dimgui_load_font(const void* data, unsigned int size, float pixel);
d3dimgui_export void d3dimgui_reset_fonts(void);

// ---- 查询与诊断 ----
d3dimgui_export void d3dimgui_get_status(struct d3dimgui_status* out_status);
d3dimgui_export void* d3dimgui_get_imgui_context(void);   // ImGuiContext*
d3dimgui_export const char* d3dimgui_version(void);
d3dimgui_export void d3dimgui_set_logger(d3dimgui_log_fn callback, void* user_data);
// 注入进没有调试器的进程时开一个独立控制台看日志
d3dimgui_export void d3dimgui_open_console(int open);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
