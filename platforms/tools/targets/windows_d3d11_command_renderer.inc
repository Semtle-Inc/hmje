/* Normalized GPU_COMMAND executor. Display-list interpretation remains in HMJE.
 * Commands draw into a work surface; END commits, ABORT discards. */
#include <d3dcompiler.h>
#include <math.h>
#include "gpu_glyph_cache.h"
#include "windows_d3d11_renderer.inc"
#pragma comment(lib, "d3dcompiler.lib")

typedef struct {
    ID3D11Texture2D *texture;
    ID3D11RenderTargetView *view;
    ID3D11ShaderResourceView *resource;
    UINT width, height;
    DXGI_FORMAT format;
} HmjeD3DSurface;
typedef struct {
    float origin[2], size[2], color[4], viewport[2], padding[2];
    float params[4], shadow[4], sourceRect[4], sourceInfo[4], blurClip[4], mask[4], grid[4];
    float colors[8][4];
} HmjeD3DUniforms;
#define HMJE_D3D_LAYERS 8
static ID3D11VertexShader *g_d3d_vertex;
static ID3D11PixelShader *g_d3d_shape, *g_d3d_image, *g_d3d_gradient;
static ID3D11PixelShader *g_d3d_downsample, *g_d3d_convolve, *g_d3d_blur_composite;
static ID3D11Buffer *g_d3d_uniforms, *g_d3d_kernel;
static ID3D11RasterizerState *g_d3d_rasterizer;
static ID3D11BlendState *g_d3d_blend;
/* 대상 텍스처 두 장(화면·배경)은 창마다 따로다(windows_window.c hmje_windows_d3d_targets).
 * 작업·스크래치·블러 텍스처는 한 프레임 안에서만 쓰므로 창들이 나눠 쓴다. */
static HmjeD3DSurface g_d3d_work, g_d3d_scratch, g_d3d_blur[2];
#define g_d3d_target ((HmjeD3DSurface *)hmje_windows_d3d_targets())
typedef char hmje_d3d_window_targets_fit[(sizeof(HmjeD3DSurface) * 2 <= 128) ? 1 : -1];
static HmjeD3DSurface g_d3d_layers[HMJE_D3D_LAYERS], *g_d3d_saved_target[HMJE_D3D_LAYERS];
static D3D11_RECT g_d3d_saved_clip[HMJE_D3D_LAYERS], g_d3d_clip;
static i64 g_d3d_layer_x[HMJE_D3D_LAYERS], g_d3d_layer_y[HMJE_D3D_LAYERS];
static u64 g_d3d_layer_alpha[HMJE_D3D_LAYERS];
static HmjeD3DSurface *g_d3d_active_surface;
static int g_d3d_active, g_d3d_target_index, g_d3d_depth, g_d3d_prepared;
static int g_d3d_blur_supported = -1;
static hmje_glyph_cache g_d3d_glyph_cache;
static ID3D11Texture2D *g_d3d_glyph_texture[HMJE_GLYPH_CACHE_SLOTS];
static ID3D11ShaderResourceView *g_d3d_glyph_view[HMJE_GLYPH_CACHE_SLOTS], *g_d3d_current_glyph;
static ID3D11Texture2D *g_d3d_uncached_glyph;
static ID3D11ShaderResourceView *g_d3d_uncached_view;
static u64 g_d3d_transactions;

#define HMJE_D3D_RELEASE(Type, value) do { if (value) Type##_Release(value); (value)=NULL; } while(0)
static void hmje_d3d_unbind(void) {
    ID3D11ShaderResourceView *none=NULL;
    ID3D11DeviceContext_PSSetShaderResources(g_context,0,1,&none);
    ID3D11DeviceContext_OMSetRenderTargets(g_context,0,NULL,NULL);
}
static void hmje_d3d_surface_release(HmjeD3DSurface *s) {
    HMJE_D3D_RELEASE(ID3D11ShaderResourceView,s->resource);
    HMJE_D3D_RELEASE(ID3D11RenderTargetView,s->view);
    HMJE_D3D_RELEASE(ID3D11Texture2D,s->texture);
    memset(s,0,sizeof(*s));
}
static int hmje_d3d_surface_ensure(HmjeD3DSurface *s,u64 width,u64 height,DXGI_FORMAT format,int preserve) {
    if(!width||!height||width>16384||height>16384) return 0;
    if(s->texture&&s->width==width&&s->height==height&&s->format==format) return 1;
    HmjeD3DSurface next={0}; next.width=(UINT)width;next.height=(UINT)height;next.format=format;
    D3D11_TEXTURE2D_DESC d={0}; d.Width=next.width;d.Height=next.height;d.MipLevels=1;d.ArraySize=1;
    d.Format=format;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr=ID3D11Device_CreateTexture2D(g_device,&d,NULL,&next.texture);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateRenderTargetView(g_device,(ID3D11Resource*)next.texture,NULL,&next.view);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateShaderResourceView(g_device,(ID3D11Resource*)next.texture,NULL,&next.resource);
    if(FAILED(hr)){hmje_d3d_surface_release(&next);return 0;}
    float black[4]={0}; ID3D11DeviceContext_ClearRenderTargetView(g_context,next.view,black);
    if(preserve&&s->texture&&s->format==format) {
        D3D11_BOX box={0,0,0,min(s->width,next.width),min(s->height,next.height),1};
        hmje_d3d_unbind();
        ID3D11DeviceContext_CopySubresourceRegion(g_context,(ID3D11Resource*)next.texture,0,0,0,0,(ID3D11Resource*)s->texture,0,&box);
    }
    hmje_d3d_surface_release(s);*s=next;return 1;
}
static ID3DBlob *hmje_d3d_compile(const char *entry,const char *profile) {
    ID3DBlob *code=NULL,*error=NULL;
    HRESULT hr=D3DCompile(hmje_d3d_shader_source,sizeof(hmje_d3d_shader_source)-1,"hmje-windows.hlsl",NULL,NULL,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(hr)) fprintf(stderr,"windows-gpu: shader %s failed: %s\n",entry,error?(char*)ID3D10Blob_GetBufferPointer(error):"D3DCompile");
    HMJE_D3D_RELEASE(ID3D10Blob,error);
    if(FAILED(hr))HMJE_D3D_RELEASE(ID3D10Blob,code);
    return code;
}
static int hmje_d3d_pixel_shader(const char *entry,ID3D11PixelShader **shader) {
    ID3DBlob *code=hmje_d3d_compile(entry,"ps_4_0");if(!code)return 0;
    HRESULT hr=ID3D11Device_CreatePixelShader(g_device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,shader);
    ID3D10Blob_Release(code);return SUCCEEDED(hr);
}
static int hmje_d3d_prepare(void) {
    if(g_d3d_prepared) return g_d3d_prepared>0;
    g_d3d_prepared=-1;
    if(!g_device||!g_context||!g_swapchain)return 0;
    ID3DBlob *code=hmje_d3d_compile("vs_main","vs_4_0");if(!code)return 0;
    HRESULT hr=ID3D11Device_CreateVertexShader(g_device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&g_d3d_vertex);
    ID3D10Blob_Release(code);
    if(FAILED(hr)||!hmje_d3d_pixel_shader("ps_shape",&g_d3d_shape)||!hmje_d3d_pixel_shader("ps_image",&g_d3d_image)||
       !hmje_d3d_pixel_shader("ps_gradient",&g_d3d_gradient)||!hmje_d3d_pixel_shader("ps_downsample",&g_d3d_downsample)||
       !hmje_d3d_pixel_shader("ps_convolve",&g_d3d_convolve)||!hmje_d3d_pixel_shader("ps_blur_composite",&g_d3d_blur_composite))return 0;
    D3D11_BUFFER_DESC buffer={0};buffer.ByteWidth=sizeof(HmjeD3DUniforms);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(ID3D11Device_CreateBuffer(g_device,&buffer,NULL,&g_d3d_uniforms)))return 0;
    buffer.ByteWidth=4096;if(FAILED(ID3D11Device_CreateBuffer(g_device,&buffer,NULL,&g_d3d_kernel)))return 0;
    D3D11_RASTERIZER_DESC raster={0};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
    if(FAILED(ID3D11Device_CreateRasterizerState(g_device,&raster,&g_d3d_rasterizer)))return 0;
    D3D11_BLEND_DESC blend={0};blend.RenderTarget[0].BlendEnable=TRUE;
    blend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ZERO;blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(FAILED(ID3D11Device_CreateBlendState(g_device,&blend,&g_d3d_blend)))return 0;
    g_d3d_prepared=1;return 1;
}
static int hmje_d3d_blur_available(void) {
    if(g_d3d_blur_supported>=0)return g_d3d_blur_supported;
    UINT support=0;
    UINT required=D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_RENDER_TARGET|D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    g_d3d_blur_supported=SUCCEEDED(ID3D11Device_CheckFormatSupport(g_device,DXGI_FORMAT_R32G32B32A32_FLOAT,&support))&&(support&required)==required;
    return g_d3d_blur_supported;
}
static void hmje_d3d_color(float c[4],u64 packed) {
    c[0]=(float)((packed>>16)&255)/255;c[1]=(float)((packed>>8)&255)/255;c[2]=(float)(packed&255)/255;c[3]=(float)((packed>>24)&255)/255;
}
static void hmje_d3d_draw_quad(HmjeD3DSurface *target,ID3D11PixelShader *shader,HmjeD3DUniforms *u,ID3D11ShaderResourceView *source,int blend,D3D11_RECT clip) {
    hmje_d3d_unbind();
    D3D11_VIEWPORT viewport={0,0,(float)target->width,(float)target->height,0,1};
    u->viewport[0]=(float)target->width;u->viewport[1]=(float)target->height;
    ID3D11DeviceContext_UpdateSubresource(g_context,(ID3D11Resource*)g_d3d_uniforms,0,NULL,u,0,0);
    ID3D11DeviceContext_OMSetRenderTargets(g_context,1,&target->view,NULL);
    ID3D11DeviceContext_OMSetBlendState(g_context,blend?g_d3d_blend:NULL,NULL,0xffffffffu);
    ID3D11DeviceContext_RSSetState(g_context,g_d3d_rasterizer);
    ID3D11DeviceContext_RSSetViewports(g_context,1,&viewport);
    ID3D11DeviceContext_RSSetScissorRects(g_context,1,&clip);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_context,g_d3d_vertex,NULL,0);
    ID3D11DeviceContext_PSSetShader(g_context,shader,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_context,0,1,&g_d3d_uniforms);
    ID3D11DeviceContext_PSSetConstantBuffers(g_context,0,1,&g_d3d_uniforms);
    ID3D11DeviceContext_PSSetConstantBuffers(g_context,1,1,&g_d3d_kernel);
    ID3D11DeviceContext_PSSetShaderResources(g_context,0,1,&source);
    ID3D11DeviceContext_Draw(g_context,4,0);
}
static D3D11_RECT hmje_d3d_full(HmjeD3DSurface *s) { D3D11_RECT r={0,0,(LONG)s->width,(LONG)s->height};return r; }
static void hmje_d3d_glyph_release(unsigned slot) {
    HMJE_D3D_RELEASE(ID3D11ShaderResourceView,g_d3d_glyph_view[slot]);
    HMJE_D3D_RELEASE(ID3D11Texture2D,g_d3d_glyph_texture[slot]);
}
static int hmje_d3d_upload(const unsigned char *bytes,u64 width,u64 height,u64 pitch,DXGI_FORMAT format,ID3D11Texture2D **texture,ID3D11ShaderResourceView **view) {
    if(!bytes||!width||!height||!pitch||width>16384||height>16384||pitch>UINT_MAX||height>SIZE_MAX/pitch)return 0;
    D3D11_TEXTURE2D_DESC d={0};d.Width=(UINT)width;d.Height=(UINT)height;d.MipLevels=1;d.ArraySize=1;d.Format=format;
    d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data={bytes,(UINT)pitch,0};
    HRESULT hr=ID3D11Device_CreateTexture2D(g_device,&d,&data,texture);
    if(SUCCEEDED(hr))hr=ID3D11Device_CreateShaderResourceView(g_device,(ID3D11Resource*)*texture,NULL,view);
    if(FAILED(hr)){HMJE_D3D_RELEASE(ID3D11Texture2D,*texture);return 0;}return 1;
}
static i64 hmje_d3d_glyph(const u64 *r) {
    if(!r[0]||!r[1]||!r[2]||r[3]<r[1])return -38;
    const unsigned char *bytes=(const unsigned char*)(uintptr_t)r[0];
    int slot=hmje_glyph_cache_get(&g_d3d_glyph_cache,bytes,r[1],r[2],r[3],hmje_d3d_glyph_release);
    HMJE_D3D_RELEASE(ID3D11ShaderResourceView,g_d3d_uncached_view);
    HMJE_D3D_RELEASE(ID3D11Texture2D,g_d3d_uncached_glyph);
    if(slot<0) {
        if(!hmje_d3d_upload(bytes,r[1],r[2],r[3],DXGI_FORMAT_R8_UNORM,&g_d3d_uncached_glyph,&g_d3d_uncached_view))return -38;
        g_d3d_current_glyph=g_d3d_uncached_view;
    } else {
        if(!g_d3d_glyph_view[slot]&&!hmje_d3d_upload(g_d3d_glyph_cache.entries[slot].bytes,r[1],r[2],r[1],DXGI_FORMAT_R8_UNORM,&g_d3d_glyph_texture[slot],&g_d3d_glyph_view[slot])){
            hmje_glyph_cache_drop(&g_d3d_glyph_cache,(unsigned)slot,hmje_d3d_glyph_release);return -38;
        }
        g_d3d_current_glyph=g_d3d_glyph_view[slot];
    }
    return 1;
}
static i64 hmje_d3d_draw(const u64 *r) {
    if(r[0]>5||(r[0]==4&&!g_d3d_current_glyph))return -38;
    HmjeD3DUniforms u={0};u.origin[0]=(float)(i64)r[1];u.origin[1]=(float)(i64)r[2];u.size[0]=(float)r[3];u.size[1]=(float)r[4];
    for(int i=0;i<4;++i)u.params[i]=(float)(i==0?r[0]:r[4+i]);
    if(r[0]==1||r[0]==2) {
        u.params[1]=(float)min(r[5],min(r[3]/2,r[4]/2));
        if(r[0]==2) {
            u64 iw=r[7]<=r[3]/2?r[3]-2*r[7]:0,ih=r[7]<=r[4]/2?r[4]-2*r[7]:0;
            u.params[2]=(float)min(r[6],min(iw/2,ih/2));
        }
    }
    u.shadow[0]=(float)(i64)r[9];u.shadow[1]=(float)(i64)r[10];u.shadow[2]=(float)r[11];u.shadow[3]=(float)r[12];
    if(r[0]==3){
        float ex=(float)(i64)r[3],ey=(float)(i64)r[4],half=(float)r[7]*.5f+1;
        u.shadow[0]=u.origin[0];u.shadow[1]=u.origin[1];u.shadow[2]=ex;u.shadow[3]=ey;
        u.size[0]=fabsf(ex-u.origin[0])+2*half;u.size[1]=fabsf(ey-u.origin[1])+2*half;
        u.origin[0]=fminf(ex,u.origin[0])-half;u.origin[1]=fminf(ey,u.origin[1])-half;
    }
    hmje_d3d_color(u.color,r[8]);
    hmje_d3d_draw_quad(g_d3d_active_surface,g_d3d_shape,&u,r[0]==4?g_d3d_current_glyph:NULL,1,g_d3d_clip);
    return 1;
}
static i64 hmje_d3d_texture_draw_mode(ID3D11ShaderResourceView *source,u64 source_width,u64 source_height,u64 sx,u64 sy,u64 sw,u64 sh,i64 x,i64 y,u64 width,u64 height,u64 alpha,u64 radius,int source_alpha) {
    if(!width||!height||!sw||!sh)return 1;
    HmjeD3DUniforms u={0};u.origin[0]=(float)x;u.origin[1]=(float)y;u.size[0]=(float)width;u.size[1]=(float)height;
    u.sourceRect[0]=(float)sx;u.sourceRect[1]=(float)sy;u.sourceRect[2]=(float)sw;u.sourceRect[3]=(float)sh;
    u.sourceInfo[0]=(float)source_width;u.sourceInfo[1]=(float)source_height;
    u.color[3]=(float)min(alpha,255)/255;u.params[1]=(float)min(radius,min(width/2,height/2));u.params[2]=(float)source_alpha;
    hmje_d3d_draw_quad(g_d3d_active_surface,g_d3d_image,&u,source,1,g_d3d_clip);return 1;
}
static i64 hmje_d3d_texture_draw(ID3D11ShaderResourceView *source,u64 source_width,u64 source_height,u64 sx,u64 sy,u64 sw,u64 sh,i64 x,i64 y,u64 width,u64 height,u64 alpha,u64 radius) {
    return hmje_d3d_texture_draw_mode(source,source_width,source_height,sx,sy,sw,sh,x,y,width,height,alpha,radius,0);
}
static i64 hmje_d3d_image(const u64 *r,int scaled,int source_alpha) {
    if(!r[0]||!r[1]||!r[2]||r[1]>SIZE_MAX/4||r[3]<r[1]*4)return -38;
    if(r[4]>=r[1]||r[5]>=r[2])return 1;
    u64 width=min(r[6],r[1]-r[4]),height=min(r[7],r[2]-r[5]);
    if(!width||!height)return 1;
    ID3D11Texture2D *texture=NULL;ID3D11ShaderResourceView *view=NULL;
    if(!hmje_d3d_upload((const unsigned char*)(uintptr_t)r[0],r[1],r[2],r[3],DXGI_FORMAT_B8G8R8A8_UNORM,&texture,&view))return -38;
    i64 result=hmje_d3d_texture_draw_mode(view,r[1],r[2],r[4],r[5],width,height,(i64)r[8],(i64)r[9],scaled?r[10]:width,scaled?r[11]:height,255,scaled?r[12]:0,source_alpha);
    ID3D11ShaderResourceView_Release(view);ID3D11Texture2D_Release(texture);return result;
}
static i64 hmje_d3d_shift(const u64 *r,int planar) {
    i64 x=(i64)r[0],y=(i64)r[1];u64 w=r[2],h=r[3];
    u64 dx=planar?r[4]:0,dy=planar?r[5]:r[4];int right=planar?r[6]!=0:0,down=planar?r[7]!=0:r[5]!=0;
    if(x<0){u64 d=(u64)(-(x+1))+1;if(d>=w)return 1;w-=d;x=0;}
    if(y<0){u64 d=(u64)(-(y+1))+1;if(d>=h)return 1;h-=d;y=0;}
    HmjeD3DSurface *s=g_d3d_active_surface;
    if((u64)x>=s->width||(u64)y>=s->height)return 1;
    w=min(w,s->width-(u64)x);h=min(h,s->height-(u64)y);
    if(!w||!h||dx>=w||dy>=h||(!dx&&!dy))return 1;
    UINT cw=(UINT)(w-dx),ch=(UINT)(h-dy);
    if(!hmje_d3d_surface_ensure(&g_d3d_scratch,cw,ch,s->format,0))return -38;
    D3D11_BOX box={(UINT)x+(right?0:(UINT)dx),(UINT)y+(down?0:(UINT)dy),0,0,0,1};box.right=box.left+cw;box.bottom=box.top+ch;
    hmje_d3d_unbind();
    ID3D11DeviceContext_CopySubresourceRegion(g_context,(ID3D11Resource*)g_d3d_scratch.texture,0,0,0,0,(ID3D11Resource*)s->texture,0,&box);
    box.left=0;box.top=0;box.right=cw;box.bottom=ch;
    ID3D11DeviceContext_CopySubresourceRegion(g_context,(ID3D11Resource*)s->texture,0,(UINT)x+(right?(UINT)dx:0),(UINT)y+(down?(UINT)dy:0),0,(ID3D11Resource*)g_d3d_scratch.texture,0,&box);
    return 1;
}
static i64 hmje_d3d_gradient(const u64 *r) {
    if(r[6]<2||r[6]>8)return -38;
    HmjeD3DUniforms u={0};u.origin[0]=(float)(i64)r[0];u.origin[1]=(float)(i64)r[1];u.size[0]=(float)r[2];u.size[1]=(float)r[3];
    u.params[1]=(float)min(r[4],min(r[2]/2,r[3]/2));u.params[2]=(float)r[5];u.params[3]=(float)r[6];
    for(int i=0;i<8;++i){hmje_d3d_color(u.colors[i],r[7+i]);for(int j=0;j<4;++j)u.colors[i][j]*=255;}
    hmje_d3d_draw_quad(g_d3d_active_surface,g_d3d_gradient,&u,NULL,1,g_d3d_clip);return 1;
}
static i64 hmje_d3d_blur(const u64 *r) {
    if(!hmje_d3d_blur_available())return -38;
    HmjeD3DSurface *target=g_d3d_active_surface;
    if(!r[2]||!r[3])return 1;
    if(r[2]>INT_MAX||r[3]>INT_MAX||r[5]>680)return -38;
    i64 x=(i64)r[0],y=(i64)r[1];
    if(x>INT_MAX||x<INT_MIN||y>INT_MAX||y<INT_MIN)return -38;
    i64 left=max(x,max(0,g_d3d_clip.left)),top=max(y,max(0,g_d3d_clip.top));
    i64 right=min(x+(i64)r[2],min((i64)target->width,g_d3d_clip.right));
    i64 bottom=min(y+(i64)r[3],min((i64)target->height,g_d3d_clip.bottom));
    if(right<=left||bottom<=top)return 1;
    u64 scale=r[5]<=8?1:(r[5]<=16?2:4),radius=r[5]/scale,n=radius*2+1;
    if(n>1024)return -38;
    u64 width=(u64)(right-left),height=(u64)(bottom-top),gw=(width+scale-1)/scale,gh=(height+scale-1)/scale;
    for(int i=0;i<2;++i)if(!hmje_d3d_surface_ensure(&g_d3d_blur[i],max(gw,g_d3d_blur[i].width),max(gh,g_d3d_blur[i].height),DXGI_FORMAT_R32G32B32A32_FLOAT,0))return -38;
    float weights[1024]={0};
    for(u64 k=0;k<n;++k)weights[k]=1.f/(float)n;
    ID3D11DeviceContext_UpdateSubresource(g_context,(ID3D11Resource*)g_d3d_kernel,0,NULL,weights,0,0);
    HmjeD3DUniforms u={0};u.size[0]=(float)gw;u.size[1]=(float)gh;
    u.blurClip[0]=(float)left;u.blurClip[1]=(float)top;u.blurClip[2]=(float)width;u.blurClip[3]=(float)height;
    u.grid[0]=(float)gw;u.grid[1]=(float)gh;u.grid[2]=(float)scale;u.grid[3]=(float)radius;
    u.mask[0]=(float)x;u.mask[1]=(float)y;u.mask[2]=(float)r[2];u.mask[3]=(float)r[3];
    u.params[0]=(float)min(r[4],min(r[2]/2,r[3]/2));u.params[2]=(float)n;hmje_d3d_color(u.color,r[6]);
    hmje_d3d_draw_quad(&g_d3d_blur[0],g_d3d_downsample,&u,target->resource,0,hmje_d3d_full(&g_d3d_blur[0]));
    /* CPU clamps each of three horizontal and three vertical box passes.
     * Collapsing the kernels changes pixels near every clipped boundary. */
    if(r[5])for(int pass=0;pass<6;++pass){int source=pass&1,dest=1-source;u.params[1]=(float)(pass/3);hmje_d3d_draw_quad(&g_d3d_blur[dest],g_d3d_convolve,&u,g_d3d_blur[source].resource,0,hmje_d3d_full(&g_d3d_blur[dest]));}
    u.origin[0]=(float)left;u.origin[1]=(float)top;u.size[0]=(float)width;u.size[1]=(float)height;
    hmje_d3d_draw_quad(target,g_d3d_blur_composite,&u,g_d3d_blur[0].resource,1,g_d3d_clip);return 1;
}
static i64 hmje_d3d_layer_begin(const u64 *r) {
    if(g_d3d_depth>=HMJE_D3D_LAYERS||!r[2]||!r[3])return -38;
    int slot=g_d3d_depth;HmjeD3DSurface *source=g_d3d_active_surface,*layer=&g_d3d_layers[slot];
    if(!hmje_d3d_surface_ensure(layer,r[2],r[3],source->format,0))return -38;
    hmje_d3d_unbind();float black[4]={0};ID3D11DeviceContext_ClearRenderTargetView(g_context,layer->view,black);
    i64 x=(i64)r[0],y=(i64)r[1];
    if(x<INT_MIN||x>INT_MAX||y<INT_MIN||y>INT_MAX)return -38;
    i64 left=max(x,0),top=max(y,0),right=min(x+(i64)r[2],source->width),bottom=min(y+(i64)r[3],source->height);
    if(right>left&&bottom>top){D3D11_BOX box={(UINT)left,(UINT)top,0,(UINT)right,(UINT)bottom,1};
        ID3D11DeviceContext_CopySubresourceRegion(g_context,(ID3D11Resource*)layer->texture,0,(UINT)(left-x),(UINT)(top-y),0,(ID3D11Resource*)source->texture,0,&box);}
    g_d3d_saved_target[slot]=source;g_d3d_saved_clip[slot]=g_d3d_clip;g_d3d_layer_x[slot]=x;g_d3d_layer_y[slot]=y;g_d3d_layer_alpha[slot]=r[4];
    g_d3d_active_surface=layer;g_d3d_clip=hmje_d3d_full(layer);++g_d3d_depth;return 1;
}
static i64 hmje_d3d_layer_end(void) {
    if(!g_d3d_depth)return -38;
    int slot=--g_d3d_depth;HmjeD3DSurface *layer=&g_d3d_layers[slot];
    g_d3d_active_surface=g_d3d_saved_target[slot];g_d3d_clip=g_d3d_saved_clip[slot];
    return hmje_d3d_texture_draw(layer->resource,layer->width,layer->height,0,0,layer->width,layer->height,g_d3d_layer_x[slot],g_d3d_layer_y[slot],layer->width,layer->height,g_d3d_layer_alpha[slot],0);
}
static void hmje_d3d_resize_invalidate(void) { g_d3d_active=0;g_d3d_depth=0;g_d3d_active_surface=NULL; }
static i64 hmje_d3d_begin(const u64 *r,u64 length) {
    const char *enabled=getenv("HMJE_WINDOWS_GPU");if(enabled&&strcmp(enabled,"0")==0)return -38;
    if(g_d3d_active||!r||!r[0]||!r[1]||(length==24&&r[2]>1))return -38;
    ensure_window();if(!hmje_d3d_prepare())return -38;
    for(int i=0;i<2;++i)if(!hmje_d3d_surface_ensure(&g_d3d_target[i],r[0],r[1],DXGI_FORMAT_B8G8R8A8_UNORM,1))return -38;
    if(!hmje_d3d_surface_ensure(&g_d3d_work,r[0],r[1],DXGI_FORMAT_B8G8R8A8_UNORM,0))return -38;
    g_d3d_target_index=length==24?(int)r[2]:0;
    hmje_d3d_unbind();ID3D11DeviceContext_CopyResource(g_context,(ID3D11Resource*)g_d3d_work.texture,(ID3D11Resource*)g_d3d_target[g_d3d_target_index].texture);
    g_d3d_active=1;g_d3d_depth=0;g_d3d_active_surface=&g_d3d_work;g_d3d_clip=hmje_d3d_full(&g_d3d_work);return 1;
}
static i64 hmje_d3d_end(void) {
    if(g_d3d_depth)return -38;
    hmje_d3d_unbind();ID3D11DeviceContext_CopyResource(g_context,(ID3D11Resource*)g_d3d_target[g_d3d_target_index].texture,(ID3D11Resource*)g_d3d_work.texture);
    i64 result=1;
    if(g_d3d_target_index==0){
        if(!g_staging_tex||!g_backbuffer_tex){hmje_d3d_resize_invalidate();return -38;}
        D3D11_BOX box={0,0,0,min((UINT)g_width,g_d3d_work.width),min((UINT)g_height,g_d3d_work.height),1};
        ID3D11DeviceContext_CopySubresourceRegion(g_context,(ID3D11Resource*)g_staging_tex,0,0,0,0,(ID3D11Resource*)g_d3d_work.texture,0,&box);
        g_repaint_pending=FALSE;result=present_staging_frame();if(result>0)g_has_presented_frame=TRUE;
    }
    ++g_d3d_transactions;
    if(g_d3d_transactions==1)fprintf(stderr,"windows-gpu: direct D3D11 transaction committed\n");
    hmje_d3d_resize_invalidate();
    if(g_d3d_target_index==0&&result>0)show_window(g_win);
    pump_messages();return result;
}
static i64 hmje_d3d_dispatch(i64 kind,i64 address,i64 length) {
    u64 *r=(u64*)(uintptr_t)(u64)address;
    if(kind==0){
        if(!r||length!=32)return -38;
        ensure_window();
        const char *enabled=getenv("HMJE_WINDOWS_GPU");
        int ready=(!enabled||strcmp(enabled,"0")!=0)&&hmje_d3d_prepare();
        int blur=ready&&hmje_d3d_blur_available();
        r[0]=2;r[1]=3;r[2]=ready?(UINT64_C(0xfffe)&(blur?~UINT64_C(0):~(UINT64_C(1)<<10))):0;
        r[3]=ready&&blur?0:1;return 32;
    }
    if(kind==1&&(length==16||length==24))return hmje_d3d_begin(r,(u64)length);
    if(kind==7&&length==0){hmje_d3d_resize_invalidate();if(g_context)hmje_d3d_unbind();return 1;}
    if(!g_d3d_active)return -38;
    if(kind==6&&length==0)return hmje_d3d_end();
    if(kind==14&&length==0)return hmje_d3d_layer_end();
    if(!r)return -38;
    switch(kind){
    case 2: if(length!=32)return -38; {
        u64 right=min(r[2],g_d3d_active_surface->width),bottom=min(r[3],g_d3d_active_surface->height);
        g_d3d_clip=(D3D11_RECT){(LONG)min(r[0],right),(LONG)min(r[1],bottom),(LONG)right,(LONG)bottom};return 1;}
    case 3:return length==32?hmje_d3d_glyph(r):-38;
    case 4:return length==104?hmje_d3d_draw(r):-38;
    case 5:return length==80?hmje_d3d_image(r,0,0):-38;
    case 8:return length==48?hmje_d3d_shift(r,0):-38;
    case 9: if(length!=56||r[0]>1||(int)r[0]==g_d3d_target_index)return -38; {
        HmjeD3DSurface *s=&g_d3d_target[r[0]];if(r[1]>=s->width||r[2]>=s->height)return 1;
        u64 width=min(r[3],s->width-r[1]),height=min(r[4],s->height-r[2]);
        return hmje_d3d_texture_draw(s->resource,s->width,s->height,r[1],r[2],width,height,(i64)r[5],(i64)r[6],width,height,255,0);}
    case 10:return length==56?hmje_d3d_blur(r):-38;
    case 11:return length==120?hmje_d3d_gradient(r):-38;
    case 12:return length==104?hmje_d3d_image(r,1,0):(length==112&&r[13]==1?hmje_d3d_image(r,1,1):-38);
    case 13:return length==40?hmje_d3d_layer_begin(r):-38;
    case 15:return length==64?hmje_d3d_shift(r,1):-38;
    default:return -38;
    }
}
i64 hmje_windows_gpu_command(i64 kind,i64 request_address,i64 request_length) {
    i64 result=hmje_d3d_dispatch(kind,request_address,request_length);
    if(result<0)fprintf(stderr,"windows-gpu: rejected command kind=%lld length=%lld active=%d layers=%d\n",kind,request_length,g_d3d_active,g_d3d_depth);
    return result;
}
