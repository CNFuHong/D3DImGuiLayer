#pragma once
#ifndef d3dimgui_gl_backend_h_
#define d3dimgui_gl_backend_h_

#include "gfx/backend.h"

#include <Windows.h>

namespace d3d_imgui_layer::gfx
{
// opengl 后端 surface 是一个 hdc 由 wglSwapBuffers 钩子交进来
// 上下文是我们自己建的一个 跟游戏的共享 状态天然隔开 这点比 d3d11 省事
// 代价是每次用之前要把当前上下文换成自己的 用完必须原样还回去
// 还不回去就是宿主下一帧画在错的上下文上 表现跟黑屏一样
class gl_backend final : public backend
{
public:
    ~gl_backend() override;

    unsigned int api() const override;
    const char* name() const override;
    void* native_device() const override;

    bool initialize(void* surface, void* auxiliary) override;
    void new_frame() override;
    void render(ImDrawData* draw_data) override;

    void pre_reset() override;
    bool post_reset() override;

    unsigned int buffer_index() const override;
    unsigned int buffer_count() const override;

    void release() override;

private:
    HDC device_context_ = nullptr;
    HGLRC context_ = nullptr;
    bool imgui_bound_ = false;
};
}

#endif
