#pragma once
#ifndef d3dimgui_log_h_
#define d3dimgui_log_h_

#include <d3d_imgui_layer/d3dimgui.h>

#include <cstdarg>

namespace d3d_imgui_layer
{
// 默认落到 outputdebugstring 装了自定义 sink 之后完全交给用户
void set_logger(d3dimgui_log_fn callback, void* user_data);

// 注入场景下开一个独立控制台 关了就回收
void open_console(bool open);
bool console_open();

// 控制台跟着进程一起没 崩溃正好发生在进程要没的时候 诊断日志要落到文件
void open_log_file(bool open);

// 只记现场不改行为 装成第一个 vectored handler
void install_crash_observer();

void log_write(unsigned int level, const char* format, va_list args);

void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);
}

#endif
