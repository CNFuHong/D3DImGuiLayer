#include "platform/font.h"

#include "core/log.h"

#include <d3d_imgui_layer/d3dimgui_settings.h>

#include <mutex>
#include <vector>

namespace
{
// 字体表可能被宿主线程改 渲染线程同时在重建图集 没这把锁就是并发读写 vector
std::mutex g_source_mutex;
std::vector<d3d_imgui_layer::font::source> g_sources;
float g_default_pixel = d3dimgui_default_font_pixel;
}

namespace d3d_imgui_layer::font
{
void reset()
{
    std::lock_guard lock(g_source_mutex);
    g_sources.clear();
}

bool push(const void* data, unsigned int size, float pixel)
{
    if (data == nullptr || size == 0)
        return false;

    source item;
    item.data = data;
    item.size = size;
    item.pixel = pixel;
    return push(item);
}

bool push(const source& item)
{
    if (item.data == nullptr || item.size == 0)
        return false;

    std::lock_guard lock(g_source_mutex);
    for (const source& existing : g_sources)
    {
        if (existing.data == item.data && existing.size == item.size)
            return true;
    }

    g_sources.push_back(item);
    return true;
}

void set_default_pixel(const float pixel)
{
    if (pixel <= 1.0f)
        return;

    std::lock_guard lock(g_source_mutex);
    g_default_pixel = pixel;
}

float default_pixel()
{
    std::lock_guard lock(g_source_mutex);
    return g_default_pixel;
}

bool rebuild()
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr)
    {
        log_error("font rebuild without imgui context");
        return false;
    }

    // 锁里只抄一份 之后往 imgui 里加字体不带着锁 免得叶锁变成跨模块的持有点
    std::vector<source> snapshot;
    float pixel_default = 0.0f;
    {
        std::lock_guard lock(g_source_mutex);
        snapshot = g_sources;
        pixel_default = g_default_pixel;
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    // clearfonts 会连已烘焙字形和纹理一起清掉 之后的 add 让 imgui 自己重建图集
    atlas->ClearFonts();

    if (snapshot.empty())
    {
        if (atlas->AddFontDefault() == nullptr)
        {
            log_error("builtin font unavailable");
            return false;
        }
        return true;
    }

    for (const source& item : snapshot)
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = 2;
        config.OversampleV = 1;

        const float pixel = item.pixel > 1.0f ? item.pixel : pixel_default;
        if (atlas->AddFontFromMemoryTTF(const_cast<void*>(item.data), static_cast<int>(item.size),
                pixel, &config, nullptr) == nullptr)
        {
            log_error("add font failed, size=%u bytes", item.size);
            return false;
        }
    }

    log_info("fonts rebuilt, sources=%zu pixel=%.1f", snapshot.size(), pixel_default);
    return true;
}
}
