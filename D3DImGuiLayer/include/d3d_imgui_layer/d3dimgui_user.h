// 用户在 src/user/draw.cpp 里实现这四个函数 除 menu 外都由框架在固定时机调
// 它们是源码级挂载点 不走 dll 导出
#pragma once
#ifndef d3dimgui_user_h_
#define d3dimgui_user_h_

#include <d3d_imgui_layer/d3dimgui.h>

namespace d3d_imgui_layer
{
namespace user
{
// 后端建好之后调一次 适合放一次性的东西 比如加载字体 注册菜单页
void initialize(const struct d3dimgui_frame_info* info);

// 每帧 NewFrame 之后 Render 之前调 想直接画就用 imgui
void frame(const struct d3dimgui_frame_info* info);

// 后端销毁之前调 用来释放用户自己申请的资源
void shutdown();
}
}

#endif
