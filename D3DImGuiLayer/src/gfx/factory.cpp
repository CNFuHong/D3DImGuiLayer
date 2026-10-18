#include "gfx/backend.h"

#include "gfx/d3d11_backend.h"
#include "gfx/d3d12_backend.h"
#include "gfx/gl_backend.h"

namespace d3d_imgui_layer::gfx
{
std::unique_ptr<backend> create_backend(const unsigned int api)
{
    switch (api)
    {
    case d3dimgui_graphics_d3d11:
        return std::make_unique<d3d11_backend>();
    case d3dimgui_graphics_d3d12:
        return std::make_unique<d3d12_backend>();
    case d3dimgui_graphics_opengl:
        return std::make_unique<gl_backend>();
    default:
        return nullptr;
    }
}
}
