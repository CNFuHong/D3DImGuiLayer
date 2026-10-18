#pragma once
#ifndef d3dimgui_font_h_
#define d3dimgui_font_h_

#include <imgui.h>

#include <vector>

namespace d3d_imgui_layer::font
{
// 字体请求表 ttf 数据由调用方保活 imgui 1.92 在整个图集生命周期里都要读它
struct source
{
    const void* data = nullptr;
    unsigned int size = 0;
    float pixel = 0.0f;
};

void reset();
bool push(const void* data, unsigned int size, float pixel);
bool push(const struct source& item);

void set_default_pixel(float pixel);
float default_pixel();

// 清空现有字形再按请求表重建 表为空时退回 imgui 内置字体
bool rebuild();
}

#endif
