// d3dimgui.h 的导出实现 只做参数转发 逻辑都在 layer 里
#include <d3d_imgui_layer/d3dimgui.h>

#include "core/layer.h"
#include "core/log.h"
#include "platform/input.h"

#include <imgui.h>

extern "C"
{
int d3dimgui_startup(const d3dimgui_desc* desc)
{
    return d3d_imgui_layer::layer::instance().startup(desc) ? 1 : 0;
}

void d3dimgui_shutdown(void)
{
    d3d_imgui_layer::layer::instance().shutdown();
}

int d3dimgui_is_running(void)
{
    return d3d_imgui_layer::layer::instance().running() ? 1 : 0;
}

int d3dimgui_draw(void* swapchain)
{
    return d3d_imgui_layer::layer::instance().draw(swapchain) ? 1 : 0;
}

int d3dimgui_draw_gl(void* device_context)
{
    return d3d_imgui_layer::layer::instance().draw_gl(device_context) ? 1 : 0;
}

void d3dimgui_pre_resize(void* swapchain)
{
    d3d_imgui_layer::layer::instance().pre_resize(swapchain);
}

void d3dimgui_post_resize(void* swapchain, int result)
{
    d3d_imgui_layer::layer::instance().post_resize(swapchain, static_cast<HRESULT>(result));
}

void d3dimgui_set_command_queue(void* command_queue)
{
    d3d_imgui_layer::layer::instance().set_command_queue(command_queue);
}

void d3dimgui_set_primary_target(void* swapchain)
{
    d3d_imgui_layer::layer::instance().set_primary(swapchain);
}

void d3dimgui_set_active(int active)
{
    d3d_imgui_layer::layer::instance().set_active(active != 0);
}

int d3dimgui_is_active(void)
{
    return d3d_imgui_layer::layer::instance().active() ? 1 : 0;
}

void d3dimgui_toggle(void)
{
    d3d_imgui_layer::layer::instance().toggle();
}

void d3dimgui_set_toggle_key(int vk)
{
    d3d_imgui_layer::input::set_toggle_key(vk);
}

void d3dimgui_set_flags(unsigned int flags)
{
    d3d_imgui_layer::layer::instance().set_flags(flags);
}

unsigned int d3dimgui_get_flags(void)
{
    return d3d_imgui_layer::layer::instance().flags();
}

int d3dimgui_set_frame_callback(d3dimgui_frame_fn callback, void* user_data)
{
    return d3d_imgui_layer::layer::instance().set_frame_callback(callback, user_data) ? 1 : 0;
}

int d3dimgui_add_menu_tab(const char* name, d3dimgui_menu_fn callback, void* user_data)
{
    return d3d_imgui_layer::layer::instance().add_menu_tab(name, callback, user_data) ? 1 : 0;
}

void d3dimgui_clear_menu_tabs(void)
{
    d3d_imgui_layer::layer::instance().clear_menu_tabs();
}

int d3dimgui_load_font(const void* data, unsigned int size, float pixel)
{
    return d3d_imgui_layer::layer::instance().load_font(data, size, pixel) ? 1 : 0;
}

void d3dimgui_reset_fonts(void)
{
    d3d_imgui_layer::layer::instance().reset_fonts();
}

void d3dimgui_get_status(d3dimgui_status* out_status)
{
    d3d_imgui_layer::layer::instance().fill_status(out_status);
}

void* d3dimgui_get_imgui_context(void)
{
    return d3d_imgui_layer::layer::instance().imgui_context();
}

const char* d3dimgui_version(void)
{
    return "d3dimgui layer 1.0 imgui " IMGUI_VERSION;
}

void d3dimgui_set_unload_key(int vk)
{
    d3d_imgui_layer::input::set_unload_key(vk);
}

void d3dimgui_unload(void)
{
    d3d_imgui_layer::layer::instance().unload();
}

void d3dimgui_set_logger(d3dimgui_log_fn callback, void* user_data)
{
    d3d_imgui_layer::set_logger(callback, user_data);
}

void d3dimgui_open_console(int open)
{
    d3d_imgui_layer::open_console(open != 0);
}
}
