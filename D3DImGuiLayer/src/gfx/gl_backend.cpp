#include "gfx/gl_backend.h"

#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

namespace
{
using d3d_imgui_layer::log_error;
using d3d_imgui_layer::log_info;
using d3d_imgui_layer::log_warn;

// opengl32 不从链接期要 这六个入口自己取
// 静态导入等于注进每一个进程都替宿主把渲染模块拖进来 而延迟加载那条不许写在 pragma 里(lnk4229)
// 所以依赖彻底挪回代码 工程配置里不用出现 opengl32 三个字
struct wgl_api
{
    HGLRC(WINAPI* create_context)(HDC) = nullptr;
    BOOL(WINAPI* delete_context)(HGLRC) = nullptr;
    HGLRC(WINAPI* get_current_context)() = nullptr;
    HDC(WINAPI* get_current_dc)() = nullptr;
    BOOL(WINAPI* share_lists)(HGLRC, HGLRC) = nullptr;
    BOOL(WINAPI* make_current)(HDC, HGLRC) = nullptr;
};

wgl_api g_wgl;
bool g_wgl_ready = false;
bool g_wgl_reported = false;

const wgl_api* resolve_wgl()
{
    if (g_wgl_ready)
        return &g_wgl;

    // 失败不记死 手工模式下宿主可能晚一点才把 opengl32 带进来 那时还得再试一次
    // 但话只说一遍 这里每帧都会被碰到
    HMODULE module = GetModuleHandleW(L"opengl32.dll");
    if (module == nullptr)
    {
        if (!g_wgl_reported)
        {
            g_wgl_reported = true;
            log_warn("opengl32 is not loaded here, gl backend stays unused");
        }
        return nullptr;
    }

    g_wgl.create_context =
        reinterpret_cast<HGLRC(WINAPI*)(HDC)>(GetProcAddress(module, "wglCreateContext"));
    g_wgl.delete_context =
        reinterpret_cast<BOOL(WINAPI*)(HGLRC)>(GetProcAddress(module, "wglDeleteContext"));
    g_wgl.get_current_context =
        reinterpret_cast<HGLRC(WINAPI*)()>(GetProcAddress(module, "wglGetCurrentContext"));
    g_wgl.get_current_dc =
        reinterpret_cast<HDC(WINAPI*)()>(GetProcAddress(module, "wglGetCurrentDC"));
    g_wgl.share_lists =
        reinterpret_cast<BOOL(WINAPI*)(HGLRC, HGLRC)>(GetProcAddress(module, "wglShareLists"));
    g_wgl.make_current =
        reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(GetProcAddress(module, "wglMakeCurrent"));
    if (g_wgl.create_context == nullptr || g_wgl.delete_context == nullptr ||
        g_wgl.get_current_context == nullptr || g_wgl.get_current_dc == nullptr ||
        g_wgl.share_lists == nullptr || g_wgl.make_current == nullptr)
    {
        if (!g_wgl_reported)
        {
            g_wgl_reported = true;
            log_error("opengl32 is missing the wgl entries we need");
        }
        return nullptr;
    }

    g_wgl_ready = true;
    log_info("wgl entries resolved from opengl32=%p", module);
    return &g_wgl;
}

// 用 gl 之前必须先把上下文换成自己的 出作用域一定原样还回去
// 还不回去宿主下一帧就画在我们的上下文上 那是黑屏级的事故
class scoped_context
{
public:
    scoped_context(const wgl_api* api, HDC device_context, HGLRC context)
        : api_(api)
        , previous_context_(api_ != nullptr ? api_->get_current_context() : nullptr)
        , previous_device_context_(api_ != nullptr ? api_->get_current_dc() : nullptr)
    {
        valid_ = api_ != nullptr && device_context != nullptr && context != nullptr &&
            api_->make_current(device_context, context) == TRUE;
    }

    ~scoped_context()
    {
        if (!valid_)
            return;
        if (previous_context_ != nullptr)
            api_->make_current(previous_device_context_, previous_context_);
        else
            api_->make_current(nullptr, nullptr);
    }

    bool valid() const { return valid_; }

private:
    const wgl_api* api_ = nullptr;
    HDC previous_device_context_ = nullptr;
    HGLRC previous_context_ = nullptr;
    bool valid_ = false;
};
}

namespace d3d_imgui_layer::gfx
{
gl_backend::~gl_backend()
{
    release();
}

unsigned int gl_backend::api() const
{
    return d3dimgui_graphics_opengl;
}

const char* gl_backend::name() const
{
    return "opengl";
}

void* gl_backend::native_device() const
{
    return context_;
}

bool gl_backend::initialize(void* surface, void*)
{
    auto* device_context = static_cast<HDC>(surface);
    if (device_context == nullptr)
        return false;

    const wgl_api* wgl = resolve_wgl();
    if (wgl == nullptr)
        return false;

    // 只能在游戏自己的上下文正 current 的时候建和共享 这里是它的 swap 调用里 正好
    const HGLRC game_context = wgl->get_current_context();
    if (game_context == nullptr || wgl->get_current_dc() != device_context)
    {
        log_warn("gl backend needs the game context current, dc=%p current=%p", device_context,
            game_context);
        return false;
    }

    const HGLRC own = wgl->create_context(device_context);
    if (own == nullptr)
    {
        log_error("wglCreateContext failed, gle=%lu", GetLastError());
        return false;
    }

    // 源是游戏的 目标是刚建出来的空上下文 这条顺序才满足 wglShareLists 的要求
    // 新上下文一旦有 display list 或者纹理就不能再共享了
    if (wgl->share_lists(game_context, own) == FALSE)
    {
        log_error("wglShareLists failed, gle=%lu", GetLastError());
        wgl->delete_context(own);
        return false;
    }

    device_context_ = device_context;
    context_ = own;

    scoped_context bind(wgl, device_context_, context_);
    if (!bind.valid())
    {
        log_error("cannot make our gl context current");
        release();
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init(d3dimgui_glsl_version))
    {
        log_error("imgui opengl backend init failed, check d3dimgui_glsl_version");
        release();
        return false;
    }

    imgui_bound_ = true;
    log_info("gl backend ready, context=%p dc=%p", context_, device_context_);
    return true;
}

void gl_backend::new_frame()
{
    scoped_context bind(resolve_wgl(), device_context_, context_);
    if (bind.valid())
        ImGui_ImplOpenGL3_NewFrame();
}

void gl_backend::render(ImDrawData* draw_data)
{
    if (context_ == nullptr || draw_data == nullptr)
        return;

    scoped_context bind(resolve_wgl(), device_context_, context_);
    if (!bind.valid())
    {
        log_warn("cannot bind our gl context, frame dropped");
        return;
    }
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

void gl_backend::pre_reset()
{
    // gl 没有热区这个东西 换尺寸对我们是透明的
}

bool gl_backend::post_reset()
{
    return context_ != nullptr;
}

unsigned int gl_backend::buffer_index() const
{
    return 0;
}

unsigned int gl_backend::buffer_count() const
{
    return 1;
}

void gl_backend::release()
{
    const wgl_api* wgl = resolve_wgl();
    if (wgl == nullptr)
        return;

    if (imgui_bound_)
    {
        scoped_context bind(wgl, device_context_, context_);
        ImGui_ImplOpenGL3_Shutdown();
        imgui_bound_ = false;
    }

    if (context_ != nullptr)
    {
        if (wgl->get_current_context() == context_)
            wgl->make_current(nullptr, nullptr);
        // 删失败说明还有线程把它 current 着 只能留着 强删会把别人的上下文搞坏
        if (wgl->delete_context(context_) == FALSE)
            log_warn("wglDeleteContext deferred, gle=%lu", GetLastError());
        context_ = nullptr;
    }
    device_context_ = nullptr;
}
}
