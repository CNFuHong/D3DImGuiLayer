#pragma once
#ifndef d3dimgui_gl_hook_h_
#define d3dimgui_gl_hook_h_

#include <Windows.h>

namespace d3d_imgui_layer::hook::gl
{
// opengl32 已经在进程里就当场挂上 还没有就自己派一条线程等它出现
// 不去 LoadLibrary 那是替宿主决定要不要用 gl
bool install();
void shutdown();
bool installed();

// 一个进程里可以有好几条 gl 表面 内嵌浏览器的也会 swap
// 拿 hdc 反查它真正的顶层窗口 认不出来的表面一律不画
HWND surface_window(HDC device_context);
}

#endif
