#pragma once
#ifndef d3dimgui_input_h_
#define d3dimgui_input_h_

#include <Windows.h>

namespace d3d_imgui_layer::input
{
// 子类化目标窗口 内部记住原窗口过程 hwnd 变了会先摘旧的再挂新的
bool attach(HWND window);
void detach();
HWND attached_window();

// 每帧调一次 在 imgui newframe 之前
void new_frame();

// 菜单是否正在吃输入 光标 hook 和消息过滤都看这个
void set_capturing(bool capturing);
bool is_capturing();

void set_cursor_control(bool enabled);
bool cursor_control_enabled();

// 子类化时 msg 已经交给 imgui 处理完 这里决定要不要发给游戏
bool should_swallow_message(UINT message);

// 给 hook 层用的三个判断
bool should_ignore_cursor_pos();
bool should_release_clip();
bool should_force_arrow_cursor(HCURSOR requested);

// 宿主窗口被销毁 这是游戏准备收摊的信号 渲染层要赶在它放掉设备之前拆干净
using destroy_fn = void(*)();
void set_destroy_handler(destroy_fn handler);

// 呼出键和卸载键都在子类化里就地处理 命中后走 handler 不进游戏
void set_toggle_key(int vk);
int toggle_key();
void set_toggle_handler(void (*handler)());

void set_unload_key(int vk);
int unload_key();
void set_unload_handler(void (*handler)());

// 光标引用计账 菜单打开期间把系统光标抬到可见 关掉时按记账次数还原
void force_cursor_visible();
void restore_cursor();
void repair_cursor_visibility();

using show_cursor_fn = int(WINAPI*)(BOOL);
// hook 层把未被自己改过的 showcursor 交进来 否则会绕回 detour 死循环
void set_show_cursor_impl(show_cursor_fn implementation);
}

#endif
