#include "gfx/d3d11_backend.h"

#include "core/log.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <iterator>

namespace
{
using Microsoft::WRL::ComPtr;

// imgui 的 dx11 后端会把即时上下文的管线状态整体换成自己那一套
// 引擎侧普遍带状态缓存 不会每帧重新绑定 我们不改回去它的下一帧就画在错的管线上
// 表现是画面变黑或全空 但没有异常 所以抓不到崩溃 只能靠还原状态根治
struct pipeline_state
{
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11Buffer> vertex_buffer;
    ComPtr<ID3D11Buffer> index_buffer;
    unsigned int vertex_stride = 0;
    unsigned int vertex_offset = 0;
    unsigned int index_offset = 0;
    DXGI_FORMAT index_format = DXGI_FORMAT_R32_UINT;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11GeometryShader> geometry_shader;
    ComPtr<ID3D11HullShader> hull_shader;
    ComPtr<ID3D11DomainShader> domain_shader;
    ComPtr<ID3D11ComputeShader> compute_shader;
    ComPtr<ID3D11Buffer> vertex_constants;
    ComPtr<ID3D11ShaderResourceView> pixel_resource;
    ComPtr<ID3D11SamplerState> pixel_sampler;

    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilView> depth_view;
    ComPtr<ID3D11RenderTargetView> color_views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];

    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    unsigned int viewport_count = 0;
    unsigned int scissor_count = 0;
    unsigned int color_count = 0;
    unsigned int stencil_ref = 0;
    unsigned int sample_mask = 0xffffffff;
    float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // 字节码参数一律给空 setshader 只需要对象本身 带旧字节码反而会被驱动当覆盖用
    void capture(ID3D11DeviceContext* context);
    void apply(ID3D11DeviceContext* context) const;
};

void pipeline_state::capture(ID3D11DeviceContext* context)
{
    context->IAGetInputLayout(&input_layout);
    context->IAGetPrimitiveTopology(&topology);
    context->IAGetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride, &vertex_offset);
    context->IAGetIndexBuffer(&index_buffer, &index_format, &index_offset);

    context->VSGetShader(&vertex_shader, nullptr, nullptr);
    context->PSGetShader(&pixel_shader, nullptr, nullptr);
    context->GSGetShader(&geometry_shader, nullptr, nullptr);
    context->HSGetShader(&hull_shader, nullptr, nullptr);
    context->DSGetShader(&domain_shader, nullptr, nullptr);
    context->CSGetShader(&compute_shader, nullptr, nullptr);
    context->VSGetConstantBuffers(0, 1, &vertex_constants);
    context->PSGetShaderResources(0, 1, &pixel_resource);
    context->PSGetSamplers(0, 1, &pixel_sampler);

    context->RSGetState(&rasterizer);
    context->OMGetDepthStencilState(&depth_stencil, &stencil_ref);
    context->OMGetBlendState(&blend, blend_factor, &sample_mask);

    viewport_count = static_cast<unsigned int>(std::size(viewports));
    context->RSGetViewports(&viewport_count, viewports);
    scissor_count = static_cast<unsigned int>(std::size(scissors));
    context->RSGetScissorRects(&scissor_count, scissors);

    // 取满 8 个槽 没绑的驱动会写回空指针 前缀连续的非空个数就是引擎真正绑的数量
    static constexpr unsigned int target_slots = static_cast<unsigned int>(
        static_cast<unsigned int>(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT));
    ID3D11RenderTargetView* raw_views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    context->OMGetRenderTargets(target_slots, raw_views, &depth_view);
    for (unsigned int i = 0; i < target_slots; ++i)
        color_views[i].Attach(raw_views[i]);
    while (color_count < target_slots && color_views[color_count] != nullptr)
        ++color_count;
}

void pipeline_state::apply(ID3D11DeviceContext* context) const
{
    ID3D11Buffer* vertex = vertex_buffer.Get();
    context->IASetVertexBuffers(0, 1, &vertex, &vertex_stride, &vertex_offset);
    ID3D11Buffer* index = index_buffer.Get();
    context->IASetIndexBuffer(index, index_format, index_offset);
    context->IASetInputLayout(input_layout.Get());
    context->IASetPrimitiveTopology(topology);

    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    ID3D11Buffer* constants = vertex_constants.Get();
    context->VSSetConstantBuffers(0, 1, &constants);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* resource = pixel_resource.Get();
    context->PSSetShaderResources(0, 1, &resource);
    ID3D11SamplerState* sampler = pixel_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->GSSetShader(geometry_shader.Get(), nullptr, 0);
    context->HSSetShader(hull_shader.Get(), nullptr, 0);
    context->DSSetShader(domain_shader.Get(), nullptr, 0);
    context->CSSetShader(compute_shader.Get(), nullptr, 0);

    if (viewport_count > 0)
        context->RSSetViewports(viewport_count, viewports);
    if (scissor_count > 0)
        context->RSSetScissorRects(scissor_count, scissors);
    context->RSSetState(rasterizer.Get());

    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    for (unsigned int i = 0; i < color_count; ++i)
        targets[i] = color_views[i].Get();
    context->OMSetRenderTargets(color_count, targets, depth_view.Get());
    context->OMSetDepthStencilState(depth_stencil.Get(), stencil_ref);
    context->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
}
}

namespace d3d_imgui_layer::gfx
{
d3d11_backend::~d3d11_backend()
{
    release();
}

unsigned int d3d11_backend::api() const
{
    return d3dimgui_graphics_d3d11;
}

const char* d3d11_backend::name() const
{
    return "direct3d 11";
}

void* d3d11_backend::native_device() const
{
    return device_.Get();
}

bool d3d11_backend::read_desc()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (swapchain_ == nullptr || FAILED(swapchain_->GetDesc(&desc)))
        return false;

    buffer_count_ = desc.BufferCount > 0 ? desc.BufferCount : 1;
    // 只有 flip 模型才轮换热区 老的 discard/blt 链永远画在 0 上
    flip_model_ = desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
        desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // 换热区要的那个 swapchain3 在这里认一次 resizebuffers 之后 swap effect 可能变 所以跟着 desc 一起重认
    // 只借地址不攥引用 QI 拿到的那一份立刻还掉 理由跟 swapchain_ 一样 多一个引用宿主就重建不了链
    chain3_ = nullptr;
    if (flip_model_)
    {
        IDXGISwapChain3* probed = nullptr;
        if (SUCCEEDED(swapchain_->QueryInterface(IID_PPV_ARGS(&probed))) && probed != nullptr)
        {
            chain3_ = probed;
            probed->Release();
        }
    }
    return true;
}

unsigned int d3d11_backend::current_index() const
{
    // 每帧一次 QI 没有意义 swapchain3 的地址在整条链活着期间不会变
    if (chain3_ == nullptr)
        return 0;
    const UINT index = chain3_->GetCurrentBackBufferIndex();
    return index < buffer_count_ ? index : 0;
}

bool d3d11_backend::initialize(void* surface, void*)
{
    auto* swapchain = static_cast<IDXGISwapChain*>(surface);
    if (swapchain == nullptr)
        return false;

    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device_))))
    {
        log_error("d3d11 backend get device failed");
        return false;
    }

    device_->GetImmediateContext(&context_);
    if (context_ == nullptr)
    {
        release();
        return false;
    }

    swapchain_ = swapchain;

    if (!read_desc())
    {
        release();
        return false;
    }

    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get()))
    {
        release();
        return false;
    }

    imgui_bound_ = true;
    log_info("d3d11 backend ready, buffers=%u rotation=%s",
        buffer_count_, flip_model_ ? "flip" : "fixed 0");
    return true;
}

void d3d11_backend::new_frame()
{
    buffer_index_ = current_index();
    ImGui_ImplDX11_NewFrame();
}

void d3d11_backend::render(ImDrawData* draw_data)
{
    if (context_ == nullptr || swapchain_ == nullptr || draw_data == nullptr)
        return;

    // rtv 每帧现做现放 不留到帧外 免得我们成了 backbuffer 的最后一个持有者
    // 游戏调 resizebuffers 之前必须放掉所有对热区的引用 我们多攥一个它就是 invalid_call
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain_->GetBuffer(buffer_index_, IID_PPV_ARGS(&backbuffer))))
        return;

    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &target)))
        return;

    pipeline_state state;
    state.capture(context_.Get());

    ID3D11RenderTargetView* raw_target = target.Get();
    context_->OMSetRenderTargets(1, &raw_target, nullptr);
    ImGui_ImplDX11_RenderDrawData(draw_data);

    state.apply(context_.Get());
}

void d3d11_backend::pre_reset()
{
    // 不在帧外缓存任何指向 backbuffer 的东西 这里没东西要放
}

bool d3d11_backend::post_reset()
{
    if (swapchain_ == nullptr)
        return false;

    return read_desc();
}

unsigned int d3d11_backend::buffer_index() const
{
    return buffer_index_;
}

unsigned int d3d11_backend::buffer_count() const
{
    return buffer_count_;
}

void d3d11_backend::release()
{
    if (imgui_bound_)
    {
        ImGui_ImplDX11_Shutdown();
        imgui_bound_ = false;
    }

    context_.Reset();
    device_.Reset();
    swapchain_ = nullptr;
    chain3_ = nullptr;
    buffer_index_ = 0;
    buffer_count_ = 1;
    flip_model_ = false;
}
}
