#include "dx11_renderer.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char *g_vertex_shader_source =
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };"
    "cbuffer Constants : register(b0) { float2 viewport; float2 padding; };"
    "VSOut main(VSIn input) {"
    "  VSOut output;"
    "  float2 ndc = float2((input.pos.x / viewport.x) * 2.0f - 1.0f, 1.0f - (input.pos.y / viewport.y) * 2.0f);"
    "  output.pos = float4(ndc, 0.0f, 1.0f);"
    "  output.uv = input.uv;"
    "  output.color = input.color;"
    "  return output;"
    "}";

static const char *g_pixel_shader_source =
    "Texture2D atlas_texture : register(t0);"
    "SamplerState atlas_sampler : register(s0);"
    "struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };"
    "float4 main(PSIn input) : SV_Target {"
    "  if (input.uv.x < 0.0f) return input.color;"
    "  float4 texel = atlas_texture.Sample(atlas_sampler, input.uv);"
    "  return texel * input.color;"
    "}";

typedef struct RendererConstants {
    f32 viewport[2];
    f32 padding[2];
} RendererConstants;

static void
safe_release(IUnknown **object)
{
    if (*object) {
        IUnknown_Release(*object);
        *object = NULL;
    }
}

static bool
compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **blob)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, entry, target, flags, 0, blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA((const char *)ID3D10Blob_GetBufferPointer(errors));
            ID3D10Blob_Release(errors);
        }
        return false;
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    return true;
}

static bool
create_render_target(Dx11Renderer *renderer)
{
    ID3D11Texture2D *backbuffer = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(renderer->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    if (FAILED(hr)) {
        return false;
    }
    hr = ID3D11Device_CreateRenderTargetView(renderer->device, (ID3D11Resource *)backbuffer, NULL, &renderer->rtv);
    ID3D11Texture2D_Release(backbuffer);
    return SUCCEEDED(hr);
}

static bool
create_texture_rgba(ID3D11Device *device, int width, int height, const void *pixels, ID3D11Texture2D **texture, ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial = {0};
    initial.pSysMem = pixels;
    initial.SysMemPitch = width * 4;

    HRESULT hr = ID3D11Device_CreateTexture2D(device, &desc, &initial, texture);
    if (FAILED(hr)) {
        return false;
    }
    hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)*texture, NULL, srv);
    return SUCCEEDED(hr);
}

static void
push_vertex(Dx11Renderer *renderer, RendererVertex v)
{
    if (renderer->vertex_count >= renderer->vertex_capacity) {
        u32 new_capacity = renderer->vertex_capacity ? renderer->vertex_capacity * 2 : 4096;
        renderer->vertices = (RendererVertex *)heap_realloc(renderer->vertices, sizeof(RendererVertex) * new_capacity);
        renderer->vertex_capacity = new_capacity;
    }
    renderer->vertices[renderer->vertex_count++] = v;
}

static void
push_quad(Dx11Renderer *renderer, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, RenderColor color)
{
    RendererVertex a = {x0, y0, u0, v0, color.r, color.g, color.b, color.a};
    RendererVertex b = {x1, y0, u1, v0, color.r, color.g, color.b, color.a};
    RendererVertex c = {x1, y1, u1, v1, color.r, color.g, color.b, color.a};
    RendererVertex d = {x0, y1, u0, v1, color.r, color.g, color.b, color.a};

    push_vertex(renderer, a);
    push_vertex(renderer, b);
    push_vertex(renderer, c);
    push_vertex(renderer, a);
    push_vertex(renderer, c);
    push_vertex(renderer, d);
}

bool
dx11_renderer_init(Dx11Renderer *renderer, HWND hwnd, u32 width, u32 height)
{
    ZeroMemory(renderer, sizeof(*renderer));
    renderer->hwnd = hwnd;
    renderer->width = width;
    renderer->height = height;

    DXGI_SWAP_CHAIN_DESC swap_desc;
    ZeroMemory(&swap_desc, sizeof(swap_desc));
    swap_desc.BufferCount = 2;
    swap_desc.BufferDesc.Width = width;
    swap_desc.BufferDesc.Height = height;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.OutputWindow = hwnd;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, &feature_level, 1, D3D11_SDK_VERSION, &swap_desc, &renderer->swap_chain, &renderer->device, NULL, &renderer->context);
    if (FAILED(hr)) {
        return false;
    }
    if (!create_render_target(renderer)) {
        return false;
    }

    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    if (!compile_shader(g_vertex_shader_source, "main", "vs_4_0", &vs_blob) ||
        !compile_shader(g_pixel_shader_source, "main", "ps_4_0", &ps_blob)) {
        return false;
    }

    hr = ID3D11Device_CreateVertexShader(renderer->device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &renderer->vertex_shader);
    if (FAILED(hr)) {
        return false;
    }
    hr = ID3D11Device_CreatePixelShader(renderer->device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &renderer->pixel_shader);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(RendererVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(RendererVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(RendererVertex, r), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = ID3D11Device_CreateInputLayout(renderer->device, layout, array_count(layout), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &renderer->input_layout);
    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_blob);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_BUFFER_DESC vb_desc;
    ZeroMemory(&vb_desc, sizeof(vb_desc));
    vb_desc.ByteWidth = sizeof(RendererVertex) * 65536;
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(renderer->device, &vb_desc, NULL, &renderer->vertex_buffer);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(blend_desc));
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(renderer->device, &blend_desc, &renderer->blend_state);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc;
    ZeroMemory(&sampler_desc, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = ID3D11Device_CreateSamplerState(renderer->device, &sampler_desc, &renderer->sampler);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_RASTERIZER_DESC rs_desc;
    ZeroMemory(&rs_desc, sizeof(rs_desc));
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.ScissorEnable = TRUE;
    rs_desc.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(renderer->device, &rs_desc, &renderer->rasterizer);
    if (FAILED(hr)) {
        return false;
    }

    u8 white_pixel[4] = {255, 255, 255, 255};
    if (!create_texture_rgba(renderer->device, 1, 1, white_pixel, &renderer->white_texture, &renderer->white_srv)) {
        return false;
    }

    renderer->vertices = (RendererVertex *)heap_alloc_zero(sizeof(RendererVertex) * 4096);
    renderer->vertex_capacity = 4096;
    return true;
}

void
dx11_renderer_shutdown(Dx11Renderer *renderer)
{
    if (!renderer) {
        return;
    }
    safe_release((IUnknown **)&renderer->white_srv);
    safe_release((IUnknown **)&renderer->white_texture);
    safe_release((IUnknown **)&renderer->atlas_srv_b);
    safe_release((IUnknown **)&renderer->atlas_texture_b);
    safe_release((IUnknown **)&renderer->atlas_srv);
    safe_release((IUnknown **)&renderer->atlas_texture);
    safe_release((IUnknown **)&renderer->vertex_buffer);
    safe_release((IUnknown **)&renderer->input_layout);
    safe_release((IUnknown **)&renderer->pixel_shader);
    safe_release((IUnknown **)&renderer->vertex_shader);
    safe_release((IUnknown **)&renderer->rasterizer);
    safe_release((IUnknown **)&renderer->sampler);
    safe_release((IUnknown **)&renderer->blend_state);
    safe_release((IUnknown **)&renderer->rtv);
    safe_release((IUnknown **)&renderer->swap_chain);
    safe_release((IUnknown **)&renderer->context);
    safe_release((IUnknown **)&renderer->device);
    heap_free(renderer->vertices);
    ZeroMemory(renderer, sizeof(*renderer));
}

void
dx11_renderer_resize(Dx11Renderer *renderer, u32 width, u32 height)
{
    if (!renderer->swap_chain || width == 0 || height == 0) {
        return;
    }
    renderer->width = width;
    renderer->height = height;
    safe_release((IUnknown **)&renderer->rtv);
    IDXGISwapChain_ResizeBuffers(renderer->swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    create_render_target(renderer);
}

void
dx11_renderer_set_scissor_u32(Dx11Renderer *renderer, u32 left, u32 top, u32 right, u32 bottom)
{
    if (!renderer) {
        return;
    }
    if (left >= renderer->width) {
        left = renderer->width ? renderer->width - 1 : 0;
    }
    if (top >= renderer->height) {
        top = renderer->height ? renderer->height - 1 : 0;
    }
    if (right > renderer->width) {
        right = renderer->width;
    }
    if (bottom > renderer->height) {
        bottom = renderer->height;
    }
    if (right <= left || bottom <= top) {
        left = 0;
        top = 0;
        right = 0;
        bottom = 0;
    }
    renderer->scissor_left = left;
    renderer->scissor_top = top;
    renderer->scissor_right = right;
    renderer->scissor_bottom = bottom;
}

void
dx11_renderer_begin(Dx11Renderer *renderer, RenderColor clear_color)
{
    renderer->vertex_count = 0;
    renderer->pending_text_srv = NULL;
    FLOAT color[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
    ID3D11DeviceContext_OMSetRenderTargets(renderer->context, 1, &renderer->rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(renderer->context, renderer->rtv, color);

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = (FLOAT)renderer->width;
    viewport.Height = (FLOAT)renderer->height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(renderer->context, 1, &viewport);
    dx11_renderer_set_scissor_u32(renderer, 0, 0, renderer->width, renderer->height);
}

void
dx11_renderer_draw_rect(Dx11Renderer *renderer, f32 x, f32 y, f32 w, f32 h, RenderColor color)
{
    push_quad(renderer, x, y, x + w, y + h, -1.0f, -1.0f, -1.0f, -1.0f, color);
}

void
dx11_renderer_draw_text(Dx11Renderer *renderer, const ShapedText *text, RenderColor color)
{
    for (u32 i = 0; i < text->count; ++i) {
        const TextQuad *quad = &text->quads[i];
        push_quad(renderer, quad->x0, quad->y0, quad->x1, quad->y1, quad->u0, quad->v0, quad->u1, quad->v1, color);
    }
}

void
dx11_renderer_upload_atlas(Dx11Renderer *renderer, const FontRaster *raster, u32 atlas_index)
{
    if (!raster->atlas_pixels) {
        return;
    }
    if (atlas_index == 0) {
        if (!renderer->atlas_texture) {
            create_texture_rgba(renderer->device, raster->atlas_width, raster->atlas_height, raster->atlas_pixels, &renderer->atlas_texture, &renderer->atlas_srv);
            return;
        }
        ID3D11DeviceContext_UpdateSubresource(renderer->context, (ID3D11Resource *)renderer->atlas_texture, 0, NULL, raster->atlas_pixels, raster->atlas_width * 4, 0);
        return;
    }
    if (!renderer->atlas_texture_b) {
        create_texture_rgba(renderer->device, raster->atlas_width, raster->atlas_height, raster->atlas_pixels, &renderer->atlas_texture_b, &renderer->atlas_srv_b);
        return;
    }
    ID3D11DeviceContext_UpdateSubresource(renderer->context, (ID3D11Resource *)renderer->atlas_texture_b, 0, NULL, raster->atlas_pixels, raster->atlas_width * 4, 0);
}

void
dx11_renderer_flush(Dx11Renderer *renderer)
{
    if (!renderer || !renderer->vertex_count) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    if (FAILED(ID3D11DeviceContext_Map(renderer->context, (ID3D11Resource *)renderer->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        renderer->vertex_count = 0;
        return;
    }
    memcpy(mapped.pData, renderer->vertices, sizeof(RendererVertex) * renderer->vertex_count);
    ID3D11DeviceContext_Unmap(renderer->context, (ID3D11Resource *)renderer->vertex_buffer, 0);

    UINT stride = sizeof(RendererVertex);
    UINT offset = 0;
    ID3D11DeviceContext_IASetInputLayout(renderer->context, renderer->input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(renderer->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(renderer->context, 0, 1, &renderer->vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(renderer->context, renderer->vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(renderer->context, renderer->pixel_shader, NULL, 0);
    ID3D11DeviceContext_RSSetState(renderer->context, renderer->rasterizer);
    ID3D11DeviceContext_OMSetBlendState(renderer->context, renderer->blend_state, NULL, 0xffffffffu);
    ID3D11DeviceContext_PSSetSamplers(renderer->context, 0, 1, &renderer->sampler);

    D3D11_RECT sr = {(LONG)renderer->scissor_left, (LONG)renderer->scissor_top, (LONG)renderer->scissor_right,
                     (LONG)renderer->scissor_bottom};
    ID3D11DeviceContext_RSSetScissorRects(renderer->context, 1, &sr);

    RendererConstants constants = {{(f32)renderer->width, (f32)renderer->height}, {0, 0}};
    ID3D11Buffer *constant_buffer = NULL;
    D3D11_BUFFER_DESC cb_desc;
    ZeroMemory(&cb_desc, sizeof(cb_desc));
    cb_desc.ByteWidth = sizeof(constants);
    cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cb_data = {0};
    cb_data.pSysMem = &constants;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(renderer->device, &cb_desc, &cb_data, &constant_buffer))) {
        ID3D11DeviceContext_VSSetConstantBuffers(renderer->context, 0, 1, &constant_buffer);
        ID3D11Buffer_Release(constant_buffer);
    }

    ID3D11ShaderResourceView *srv = renderer->pending_text_srv ? renderer->pending_text_srv : renderer->white_srv;
    ID3D11DeviceContext_PSSetShaderResources(renderer->context, 0, 1, &srv);
    ID3D11DeviceContext_Draw(renderer->context, renderer->vertex_count, 0);
    renderer->vertex_count = 0;
    renderer->pending_text_srv = NULL;
}

void
dx11_renderer_end(Dx11Renderer *renderer)
{
    dx11_renderer_flush(renderer);
    IDXGISwapChain_Present(renderer->swap_chain, 1, 0);
}
