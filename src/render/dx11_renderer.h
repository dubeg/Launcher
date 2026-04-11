#ifndef LAUNCHER_DX11_RENDERER_H
#define LAUNCHER_DX11_RENDERER_H

#include "../core/base.h"
#include "../text/kb_text.h"

typedef struct RenderColor {
    f32 r;
    f32 g;
    f32 b;
    f32 a;
} RenderColor;

typedef struct RendererVertex {
    f32 x;
    f32 y;
    f32 u;
    f32 v;
    f32 r;
    f32 g;
    f32 b;
    f32 a;
} RendererVertex;

typedef struct Dx11Texture {
    struct ID3D11Texture2D *texture;
    struct ID3D11ShaderResourceView *srv;
    u32 width;
    u32 height;
} Dx11Texture;

typedef enum TextRenderMode {
    TextRenderMode_Legacy = 0,
    TextRenderMode_RawAlpha = 1,
    TextRenderMode_FullGammaAlpha = 2,
    TextRenderMode_LinearCompositeBg = 3,
    TextRenderMode_Count = 4,
} TextRenderMode;

typedef struct Dx11Renderer {
    HWND hwnd;
    u32 width;
    u32 height;
    struct IDXGISwapChain *swap_chain;
    struct ID3D11Device *device;
    struct ID3D11DeviceContext *context;
    struct ID3D11RenderTargetView *rtv;
    struct ID3D11BlendState *blend_state;
    struct ID3D11SamplerState *sampler;
    struct ID3D11SamplerState *sampler_font;
    struct ID3D11RasterizerState *rasterizer;
    struct ID3D11VertexShader *vertex_shader;
    struct ID3D11PixelShader *pixel_shader;
    struct ID3D11InputLayout *input_layout;
    struct ID3D11Buffer *vertex_buffer;
    struct ID3D11Texture2D *atlas_texture;
    struct ID3D11ShaderResourceView *atlas_srv;
    struct ID3D11Texture2D *atlas_texture_b;
    struct ID3D11ShaderResourceView *atlas_srv_b;
    struct ID3D11Texture2D *atlas_texture_c;
    struct ID3D11ShaderResourceView *atlas_srv_c;
    struct ID3D11Texture2D *atlas_texture_d;
    struct ID3D11ShaderResourceView *atlas_srv_d;
    struct ID3D11ShaderResourceView *pending_text_srv;
    struct ID3D11Texture2D *white_texture;
    struct ID3D11ShaderResourceView *white_srv;
    RendererVertex *vertices;
    u32 vertex_count;
    u32 vertex_capacity;
    u32 scissor_left;
    u32 scissor_top;
    u32 scissor_right;
    u32 scissor_bottom;
    bool text_snap_pixels;
    f32 text_alpha_gamma;
    f32 text_gamma_blend;
    u32 text_render_mode;
    RenderColor frame_clear_color;
} Dx11Renderer;

bool dx11_renderer_init(Dx11Renderer *renderer, HWND hwnd, u32 width, u32 height);
void dx11_renderer_shutdown(Dx11Renderer *renderer);
void dx11_renderer_resize(Dx11Renderer *renderer, u32 width, u32 height);
void dx11_renderer_begin(Dx11Renderer *renderer, RenderColor clear_color);
void dx11_renderer_set_scissor_u32(Dx11Renderer *renderer, u32 left, u32 top, u32 right, u32 bottom);
void dx11_renderer_flush(Dx11Renderer *renderer);
void dx11_renderer_draw_rect(Dx11Renderer *renderer, f32 x, f32 y, f32 w, f32 h, RenderColor color);
void dx11_renderer_draw_text(Dx11Renderer *renderer, const ShapedText *text, RenderColor color);
void dx11_renderer_set_text_pixel_snap(Dx11Renderer *renderer, bool enabled);
bool dx11_renderer_toggle_text_pixel_snap(Dx11Renderer *renderer);
void dx11_renderer_set_text_alpha_gamma(Dx11Renderer *renderer, f32 gamma);
void dx11_renderer_set_text_gamma_blend(Dx11Renderer *renderer, f32 blend);
void dx11_renderer_set_text_render_mode(Dx11Renderer *renderer, u32 mode);
u32 dx11_renderer_cycle_text_render_mode(Dx11Renderer *renderer);
void dx11_renderer_draw_image(Dx11Renderer *renderer, f32 x, f32 y, f32 w, f32 h, RenderColor color);
void dx11_renderer_upload_atlas(Dx11Renderer *renderer, const FontRaster *raster, u32 atlas_index);
bool dx11_renderer_create_texture_rgba(Dx11Renderer *renderer, s32 width, s32 height, const void *pixels, Dx11Texture *out_texture);
void dx11_renderer_destroy_texture(Dx11Texture *texture);
void dx11_renderer_end(Dx11Renderer *renderer);

#endif
