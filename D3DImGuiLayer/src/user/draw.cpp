// 这里是二次开发的主要落点 框架每帧调这几个函数
// 想加东西就往 frame 里写 不想用这个文件也可以改 d3dimgui_set_frame_callback
// 默认带一张程序生成的测试图 用来确认纹理真的上传成功 不要就把 d3dimgui_show_demo 改 0
#include <d3d_imgui_layer/d3dimgui_user.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace
{
ImTextureData* g_demo_texture = nullptr;

constexpr int demo_width = 128;
constexpr int demo_height = 128;

// 棋盘加对角渐变再套一圈白边 糊了 没上传 画错缓冲区 一眼就能分辨
void ensure_demo_texture()
{
    if (g_demo_texture != nullptr)
        return;

    ImTextureData* texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, demo_width, demo_height);
    texture->UseColors = true;

    auto* pixels = static_cast<unsigned int*>(texture->GetPixels());
    for (int y = 0; y < demo_height; ++y)
    {
        for (int x = 0; x < demo_width; ++x)
        {
            const bool edge = x < 2 || y < 2 || x >= demo_width - 2 || y >= demo_height - 2;
            if (edge)
            {
                pixels[y * demo_width + x] = IM_COL32(255, 255, 255, 255);
                continue;
            }

            const bool checker = (((x >> 4) + (y >> 4)) & 1) != 0;
            const unsigned char red = checker ? 230 : static_cast<unsigned char>(30 + x);
            const unsigned char green = checker ? 80 : static_cast<unsigned char>(30 + y);
            const unsigned char blue = static_cast<unsigned char>(210 - x);
            pixels[y * demo_width + x] = IM_COL32(red, green, blue, 255);
        }
    }

    texture->SetStatus(ImTextureStatus_WantCreate);
    ImGui::RegisterUserTexture(texture);
    g_demo_texture = texture;
}
}

namespace d3d_imgui_layer::user
{
void initialize(const struct d3dimgui_frame_info* info)
{
    (void)info;
    // 一次性准备 想换字体就 d3dimgui_load_font 字体数据自己在进程里保活
    // d3dimgui_load_font(my_font_ttf, my_font_ttf_size, 20.0f);
#if d3dimgui_show_demo
    ensure_demo_texture();
#endif
}

void frame(const struct d3dimgui_frame_info* info)
{
    (void)info;
    // 这里已经是 imgui newframe 之后 直接写 imgui 调用就行
#if d3dimgui_show_demo
    ensure_demo_texture();

    ImGui::SetNextWindowPos(ImVec2(18.0f, 240.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("d3dimgui demo") == false)
    {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("texture test");
    if (g_demo_texture != nullptr)
    {
        ImGui::Image(g_demo_texture->GetTexRef(), ImVec2(demo_width, demo_height));
        ImGui::SameLine();

        // 顺便把矢量绘制也验一遍 这块能看见说明 draw list 也通了
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        list->AddRectFilled(at, ImVec2(at.x + 120.0f, at.y + 128.0f), IM_COL32(24, 24, 32, 255));
        list->AddCircleFilled(ImVec2(at.x + 60.0f, at.y + 54.0f), 26.0f, IM_COL32(250, 120, 60, 230));
        list->AddCircle(ImVec2(at.x + 60.0f, at.y + 54.0f), 40.0f, IM_COL32(120, 220, 255, 230), 0, 2.0f);
        list->AddLine(at, ImVec2(at.x + 120.0f, at.y + 128.0f), IM_COL32(255, 255, 255, 200), 1.5f);
        list->AddText(ImVec2(at.x + 8.0f, at.y + 104.0f), IM_COL32(230, 230, 230, 255), "vector ok");
    }
    else
    {
        ImGui::TextUnformatted("texture create failed");
    }

    ImGui::End();
#endif
}

void shutdown()
{
    // 后端销毁之前 释放自己申请的东西
    if (g_demo_texture == nullptr)
        return;

    // 上下文重建之后旧纹理指针就是野的 一定要在拆后端之前放掉
    ImGui::UnregisterUserTexture(g_demo_texture);
    IM_DELETE(g_demo_texture);
    g_demo_texture = nullptr;
}
}
