/*
 * posix_ios_metal_renderer.inc
 *
 * Native half of HmjeGraphics' flat GPU command ABI for iOS.  The HMJE side
 * owns display-list decoding and falls back by replaying the complete frame on
 * the CPU whenever any command returns ENOSYS.  Consequently this backend must
 * keep BEGIN..END atomic: no target command buffer is committed before END,
 * and ABORT only drops the uncommitted transaction.
 *
 * Keep this file included by posix_ios_window.m rather than compiling it as a
 * separate translation unit.  It deliberately reuses that file's UIKit-owned
 * g_device/g_queue/g_metal_layer and ensure_window(), while keeping renderer
 * state away from input recovery and bundle-font code.
 */

#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>

#define HMJE_IOS_GPU_BEGIN 1
#define HMJE_IOS_GPU_CLIP 2
#define HMJE_IOS_GPU_UPLOAD_GLYPH 3
#define HMJE_IOS_GPU_DRAW 4
#define HMJE_IOS_GPU_IMAGE_BLIT 5
#define HMJE_IOS_GPU_END_PRESENT 6
#define HMJE_IOS_GPU_ABORT 7
#define HMJE_IOS_GPU_SCROLL_SHIFT 8
#define HMJE_IOS_GPU_COMPOSITE_TARGET 9
#define HMJE_IOS_GPU_GRADIENT_FILL 11
#define HMJE_IOS_GPU_IMAGE_SCALE_BLIT 12
#define HMJE_IOS_GPU_LAYER_BEGIN 13
#define HMJE_IOS_GPU_LAYER_END 14
#define HMJE_IOS_GPU_PLANAR_SHIFT 15

#define HMJE_IOS_GPU_LAYER_MAX 8
#define HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT HMJE_IOS_METAL_MAX_FRAMES_IN_FLIGHT

typedef struct {
    float origin[2];
    float size[2];
    float color[4];
    float viewport[2];
    float padding[2];
    float params[4];
    float shadow[4];
} HmjeIosGpuUniforms;

static id<MTLTexture> g_ios_gpu_screen_target = nil;
static id<MTLTexture> g_ios_gpu_background_target = nil;
static id<MTLTexture>
    g_ios_gpu_scroll_scratch[HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT] = {nil};
static id<MTLTexture> g_ios_gpu_glyph = nil;
static id<MTLRenderPipelineState> g_ios_gpu_shape_pipeline = nil;
static id<MTLRenderPipelineState> g_ios_gpu_glyph_pipeline = nil;
static id<MTLRenderPipelineState> g_ios_gpu_image_pipeline = nil;
static id<MTLRenderPipelineState> g_ios_gpu_layer_pipeline = nil;
static id<MTLRenderPipelineState> g_ios_gpu_gradient_pipeline = nil;

/* Layer and scroll textures are mutable GPU scratch.  A single pool is unsafe
 * once command buffers overlap: later frames can encode writes to a resource
 * still sampled by an earlier frame.  Give every in-flight transaction its own
 * exact-size depth pool. */
static id<MTLTexture>
    g_ios_gpu_layer_texture[HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT]
                           [HMJE_IOS_GPU_LAYER_MAX] = {{nil}};
static id<MTLTexture> g_ios_gpu_layer_saved_target[HMJE_IOS_GPU_LAYER_MAX] = {nil};
static MTLScissorRect g_ios_gpu_layer_saved_clip[HMJE_IOS_GPU_LAYER_MAX];
static u64 g_ios_gpu_layer_x[HMJE_IOS_GPU_LAYER_MAX];
static u64 g_ios_gpu_layer_y[HMJE_IOS_GPU_LAYER_MAX];
static u64 g_ios_gpu_layer_width[HMJE_IOS_GPU_LAYER_MAX];
static u64 g_ios_gpu_layer_height[HMJE_IOS_GPU_LAYER_MAX];
static u64 g_ios_gpu_layer_alpha[HMJE_IOS_GPU_LAYER_MAX];

static id<MTLCommandBuffer> g_ios_gpu_commands = nil;
static id<MTLRenderCommandEncoder> g_ios_gpu_render_encoder = nil;
static MTLScissorRect g_ios_gpu_clip = {0, 0, 1, 1};
static int g_ios_gpu_current_target = 0;
static int g_ios_gpu_layer_depth = 0;
/* Fully clipped layers stay inside the Metal transaction but do not allocate
 * textures. Commands are consumed until the matching layer end. */
static int g_ios_gpu_skipped_layer_depth = 0;
static BOOL g_ios_gpu_screen_needs_clear = YES;
static BOOL g_ios_gpu_background_needs_clear = YES;
static BOOL g_ios_gpu_needs_clear_before_transaction = YES;
static BOOL g_ios_gpu_transaction_has_permit = NO;
static int g_ios_gpu_transaction_slot = -1;
static atomic_uint g_ios_gpu_free_frame_slots =
    ATOMIC_VAR_INIT((1u << HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) - 1u);
/* Explicit resource ownership makes upload lifetime independent of Metal's
 * retainedReferences default and of glyph-cache collision replacement. */
static NSMutableArray *g_ios_gpu_transaction_resources = nil;
/* Completion handlers run off the HMJE app thread.  Keep only a target bitmask
 * atomic there; the ordinary renderer state remains single-thread-owned. */
static atomic_uint g_ios_gpu_async_failed_targets = ATOMIC_VAR_INIT(0);
static atomic_bool g_ios_gpu_debug_reported = ATOMIC_VAR_INIT(false);

#include "metal_glyph_cache.inc"

static id<MTLTexture> hmje_ios_gpu_active_target(void) {
    if (g_ios_gpu_layer_depth > 0) {
        if (g_ios_gpu_transaction_slot < 0 ||
            g_ios_gpu_transaction_slot >= HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) return nil;
        return g_ios_gpu_layer_texture[g_ios_gpu_transaction_slot]
                                      [g_ios_gpu_layer_depth - 1];
    }
    return g_ios_gpu_current_target == 1
        ? g_ios_gpu_background_target : g_ios_gpu_screen_target;
}

static BOOL *hmje_ios_gpu_root_needs_clear_flag(void) {
    return g_ios_gpu_current_target == 1
        ? &g_ios_gpu_background_needs_clear : &g_ios_gpu_screen_needs_clear;
}

static void hmje_ios_gpu_mark_active_initialized(void) {
    if (g_ios_gpu_layer_depth == 0) {
        *hmje_ios_gpu_root_needs_clear_flag() = NO;
    }
}

static const char *hmje_ios_gpu_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct U{float2 origin;float2 size;float4 color;float2 viewport;float2 padding;float4 params;float4 shadow;};\n"
"struct O{float4 position [[position]];};\n"
"float sd(float2 l,float2 s,float r){float2 h=s*.5;float2 q=abs(l-h)-h+r;return length(max(q,float2(0)))+min(max(q.x,q.y),0.)-r;}\n"
"uint cv(float2 l,float2 s,float r){return uint(clamp(.5-sd(l,s,r),0.,1.)*255.);}\n"
"float af(uint c,float a){uint x=uint(round(a*255.));return float((c*x)/255u)/255.;}\n"
"vertex O vs(uint i [[vertex_id]],constant U&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
"fragment float4 ps(O i [[stage_in]],constant U&u [[buffer(0)]]){int k=int(u.params.x+.5);if(k==0)return u.color;float2 l=i.position.xy-u.origin;if(k==3){float2 a=u.shadow.xy,b=u.shadow.zw,v=b-a;float vv=dot(v,v);float t=vv>1e-6?clamp(dot(i.position.xy-a,v)/vv,0.,1.):0.;float d=length(i.position.xy-(a+t*v));float f=clamp(u.params.y+.5-d,0.,1.);return float4(u.color.rgb,af(uint(f*255.),u.color.a));}if(k==5){float b=u.params.z;float2 so=u.origin+float2(b);float2 ss=u.size-float2(2.*b);float d=sd(i.position.xy-so,ss,u.params.y);float f;if(b<=0.)f=clamp(.5-d,0.,1.);else{float t=clamp((d+b)/(2.*b),0.,1.);f=1.-t*t*(3.-2.*t);}float inner=clamp(.5-sd(i.position.xy-u.shadow.xy,u.shadow.zw,u.params.w),0.,1.);f*=1.-inner;return float4(u.color.rgb,af(uint(f*255.),u.color.a));}uint o=cv(l,u.size,u.params.y);uint c=o;if(k==2){float2 s=u.size-float2(u.params.w*2.);uint n=cv(l-float2(u.params.w),s,u.params.z);c=o>n?o-n:0u;}return float4(u.color.rgb,af(c,u.color.a));}\n"
"fragment float4 pg(O i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float,access::read> g [[texture(0)]]){int2 c=int2(floor((i.position.xy-u.origin)/max(u.params.y,1.)));uint v=uint(round(g.read(uint2(c)).r*255.));return float4(u.color.rgb,af(v,u.color.a));}\n"
"struct OI{float4 position [[position]];float2 uv;};\n"
"vertex OI vi(uint i [[vertex_id]],constant U&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;OI o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);o.uv=q;return o;}\n"
"fragment float4 pi(OI i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float> src [[texture(0)]]){constexpr sampler samp(coord::normalized,address::clamp_to_edge,filter::nearest);float4 c=src.sample(samp,i.uv);uint cov=cv(i.position.xy-u.origin,u.size,u.params.y);float a=u.params.z>0.5?af(cov,c.a):float(cov)/255.;return float4(c.rgb,a);}\n"
"fragment float4 pl(OI i [[stage_in]],constant U&u [[buffer(0)]],texture2d<float> src [[texture(0)]]){constexpr sampler samp(coord::normalized,address::clamp_to_edge,filter::nearest);float4 c=src.sample(samp,i.uv);return float4(c.rgb,u.color.a);}\n";

/* GPU_COMMAND_GRADIENT_FILL: HmjeGraphics의 CPU 기준과 같은 균등 정지점
 * 보간과 rounded-box coverage를 한 Metal pass에서 계산한다. */
static const char *hmje_ios_gpu_gradient_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct GU{float2 origin;float2 size;float2 viewport;float2 misc;float4 params;float4 colors[8];};\n"
"struct O{float4 position [[position]];};\n"
"float sd(float2 l,float2 s,float r){float2 h=s*.5;float2 q=abs(l-h)-h+r;return length(max(q,float2(0)))+min(max(q.x,q.y),0.)-r;}\n"
"vertex O vs(uint i [[vertex_id]],constant GU&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.origin+q*u.size;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
"fragment float4 fs(O i [[stage_in]],constant GU&u [[buffer(0)]]){float2 l=i.position.xy-u.origin;float x=floor(l.x),y=floor(l.y);int d=int(u.misc.x+.5),n=int(u.misc.y+.5);float p=d==1?y:(d==3?x+y:x);float den=d==1?(u.size.y-1.):(d==3?((u.size.x-1.)+(u.size.y-1.)):(u.size.x-1.));float t=den>0.?clamp(p/den,0.,1.):0.;float s=t*float(n-1);int k=clamp(int(floor(s)),0,n-2);float4 c=round(u.colors[k]+(u.colors[k+1]-u.colors[k])*(s-float(k)));uint cov=uint(saturate(.5-sd(l,u.size,u.params.x))*255.);uint a=(cov*uint(c.a))/255u;return float4(c.rgb/255.,float(a)/255.);}\n";

typedef struct {
    float origin[2];
    float size[2];
    float viewport[2];
    float misc[2];
    float params[4];
    float colors[8][4];
} HmjeIosGpuGradientUniforms;

static void hmje_ios_gpu_set_blending(MTLRenderPipelineColorAttachmentDescriptor *attachment,
                                      BOOL enabled) {
    attachment.blendingEnabled = enabled;
    if (!enabled) return;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    /* HMJE's screen targets are canonical XRGB, so their stored alpha stays 0. */
    attachment.sourceAlphaBlendFactor = MTLBlendFactorZero;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorZero;
}

static BOOL hmje_ios_gpu_prepare_pipelines(void) {
    if (g_ios_gpu_shape_pipeline != nil && g_ios_gpu_glyph_pipeline != nil &&
        g_ios_gpu_image_pipeline != nil && g_ios_gpu_layer_pipeline != nil) {
        return YES;
    }

    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:hmje_ios_gpu_msl];
    id<MTLLibrary> library = [g_device newLibraryWithSource:source options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "posix-ios-gpu: shader compile failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return NO;
    }

    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    description.vertexFunction = [library newFunctionWithName:@"vs"];
    description.fragmentFunction = [library newFunctionWithName:@"ps"];
    hmje_ios_gpu_set_blending(description.colorAttachments[0], YES);
    g_ios_gpu_shape_pipeline = [g_device newRenderPipelineStateWithDescriptor:description
                                                                         error:&error];
    if (g_ios_gpu_shape_pipeline == nil) return NO;

    description.fragmentFunction = [library newFunctionWithName:@"pg"];
    g_ios_gpu_glyph_pipeline = [g_device newRenderPipelineStateWithDescriptor:description
                                                                         error:&error];
    if (g_ios_gpu_glyph_pipeline == nil) return NO;

    /* The image shader supplies coverage alpha itself: canonical source pixels
     * are XRGB, while rounded images need their corner coverage composed with
     * the existing target.  Radius zero produces alpha 1, so the ordinary
     * opaque copy remains equivalent while rounded corners blend correctly. */
    description.vertexFunction = [library newFunctionWithName:@"vi"];
    description.fragmentFunction = [library newFunctionWithName:@"pi"];
    hmje_ios_gpu_set_blending(description.colorAttachments[0], YES);
    g_ios_gpu_image_pipeline = [g_device newRenderPipelineStateWithDescriptor:description
                                                                         error:&error];
    if (g_ios_gpu_image_pipeline == nil) return NO;

    description.fragmentFunction = [library newFunctionWithName:@"pl"];
    hmje_ios_gpu_set_blending(description.colorAttachments[0], YES);
    g_ios_gpu_layer_pipeline = [g_device newRenderPipelineStateWithDescriptor:description
                                                                         error:&error];
    return g_ios_gpu_layer_pipeline != nil;
}

static BOOL hmje_ios_gpu_prepare_gradient_pipeline(void) {
    if (g_ios_gpu_gradient_pipeline != nil) return YES;
    NSError *error = nil;
    id<MTLLibrary> library = [g_device
        newLibraryWithSource:[NSString stringWithUTF8String:hmje_ios_gpu_gradient_msl]
                     options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "posix-ios-gpu: gradient shader compile failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return NO;
    }
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [library newFunctionWithName:@"vs"];
    description.fragmentFunction = [library newFunctionWithName:@"fs"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    hmje_ios_gpu_set_blending(description.colorAttachments[0], YES);
    g_ios_gpu_gradient_pipeline =
        [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_ios_gpu_gradient_pipeline == nil) {
        fprintf(stderr, "posix-ios-gpu: gradient pipeline failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
    }
    return g_ios_gpu_gradient_pipeline != nil;
}

static void hmje_ios_gpu_end_render_encoder(void) {
    if (g_ios_gpu_render_encoder != nil) {
        [g_ios_gpu_render_encoder endEncoding];
        g_ios_gpu_render_encoder = nil;
    }
}

static i64 hmje_ios_gpu_gradient_fill(const u64 *request, u64 length) {
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (length != 120 || request == NULL || g_ios_gpu_commands == nil || target == nil ||
        !hmje_ios_gpu_prepare_gradient_pipeline()) return HMJE_POSIX_ENOSYS_LOCAL;

    i64 x = (i64)request[0], y = (i64)request[1];
    u64 width = request[2], height = request[3], radius = request[4];
    u64 direction = request[5], count = request[6];
    if (width == 0 || height == 0 || count < 2) return 1;
    if (count > 8) count = 8;
    if (radius > width / 2) radius = width / 2;
    if (radius > height / 2) radius = height / 2;

    HmjeIosGpuGradientUniforms uniforms = {{0}};
    uniforms.origin[0] = (float)x; uniforms.origin[1] = (float)y;
    uniforms.size[0] = (float)width; uniforms.size[1] = (float)height;
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    uniforms.misc[0] = (float)direction; uniforms.misc[1] = (float)count;
    uniforms.params[0] = (float)radius;
    for (u64 i = 0; i < 8; ++i) {
        uint32_t argb = (uint32_t)request[7 + (i < count ? i : count - 1)];
        uniforms.colors[i][0] = (float)((argb >> 16) & 255);
        uniforms.colors[i][1] = (float)((argb >> 8) & 255);
        uniforms.colors[i][2] = (float)(argb & 255);
        uniforms.colors[i][3] = (float)((argb >> 24) & 255);
    }

    hmje_ios_gpu_end_render_encoder();
    BOOL clear = g_ios_gpu_layer_depth == 0 && *hmje_ios_gpu_root_needs_clear_flag();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [g_ios_gpu_commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_ios_gpu_clip.width > 0 && g_ios_gpu_clip.height > 0)
        [encoder setScissorRect:g_ios_gpu_clip];
    [encoder setRenderPipelineState:g_ios_gpu_gradient_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    if (clear) *hmje_ios_gpu_root_needs_clear_flag() = NO;
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static int hmje_ios_gpu_take_frame_slot(void) {
    unsigned available = atomic_load_explicit(&g_ios_gpu_free_frame_slots,
                                              memory_order_acquire);
    while (available != 0) {
        unsigned bit = available & (0u - available);
        unsigned desired = available & ~bit;
        if (atomic_compare_exchange_weak_explicit(&g_ios_gpu_free_frame_slots,
                                                  &available, desired,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return __builtin_ctz(bit);
        }
    }
    return -1;
}

static void hmje_ios_gpu_release_frame_slot(int slot) {
    if (slot < 0 || slot >= HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) return;
    atomic_fetch_or_explicit(&g_ios_gpu_free_frame_slots, 1u << slot,
                             memory_order_release);
}

static void hmje_ios_gpu_hold_resource(id resource) {
    if (resource != nil && g_ios_gpu_transaction_resources != nil) {
        [g_ios_gpu_transaction_resources addObject:resource];
    }
}

static id<MTLRenderCommandEncoder> hmje_ios_gpu_encoder(void) {
    if (g_ios_gpu_render_encoder != nil) return g_ios_gpu_render_encoder;
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil || g_ios_gpu_commands == nil) return nil;

    BOOL clear = g_ios_gpu_layer_depth == 0 && *hmje_ios_gpu_root_needs_clear_flag();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_ios_gpu_render_encoder = [g_ios_gpu_commands renderCommandEncoderWithDescriptor:pass];
    if (g_ios_gpu_render_encoder == nil) return nil;
    if (clear) *hmje_ios_gpu_root_needs_clear_flag() = NO;
    if (g_ios_gpu_clip.width > 0 && g_ios_gpu_clip.height > 0) {
        [g_ios_gpu_render_encoder setScissorRect:g_ios_gpu_clip];
    }
    return g_ios_gpu_render_encoder;
}

/* A layer may be the first operation on a newly allocated root target.  Clear
 * before copying it; otherwise the layer snapshot reads undefined pixels. */
static BOOL hmje_ios_gpu_ensure_root_initialized(void) {
    if (g_ios_gpu_layer_depth > 0 || !*hmje_ios_gpu_root_needs_clear_flag()) return YES;
    hmje_ios_gpu_end_render_encoder();
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil || g_ios_gpu_commands == nil) return NO;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [g_ios_gpu_commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) return NO;
    [encoder endEncoding];
    *hmje_ios_gpu_root_needs_clear_flag() = NO;
    return YES;
}

static i64 hmje_ios_gpu_begin(const u64 *request, u64 length) {
    if ((length != 16 && length != 24) || request == NULL || g_ios_gpu_commands != nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    const char *enabled = getenv("HMJE_POSIX_GPU");
    if (enabled != NULL && strcmp(enabled, "0") == 0) return HMJE_POSIX_ENOSYS_LOCAL;

    ensure_window();
    u64 width = request[0], height = request[1];
    int target_index = length == 24 && request[2] == 1 ? 1 : 0;
    if (g_device == nil || g_queue == nil || g_metal_layer == nil || width == 0 || height == 0 ||
        width > (u64)NSUIntegerMax || height > (u64)NSUIntegerMax ||
        !hmje_ios_gpu_prepare_pipelines()) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }

    g_ios_gpu_current_target = target_index;
    __strong id<MTLTexture> *slot = target_index == 1
        ? &g_ios_gpu_background_target : &g_ios_gpu_screen_target;
    BOOL *needs_clear = target_index == 1
        ? &g_ios_gpu_background_needs_clear : &g_ios_gpu_screen_needs_clear;

    /* GPU execution errors are asynchronous once END stops waiting.  Consume
     * only this target's failure and reject one BEGIN: the HMJE renderer then
     * clears its own target-synchronized flag and presents the always-current
     * CPU canvas.  The following GPU attempt is necessarily a full resync;
     * merely clearing the native texture here would corrupt a partial frame. */
    unsigned failed_target_bit = target_index == 1 ? 2u : 1u;
    unsigned prior_failures = atomic_fetch_and_explicit(&g_ios_gpu_async_failed_targets,
                                                        ~failed_target_bit,
                                                        memory_order_acq_rel);
    if ((prior_failures & failed_target_bit) != 0) {
        *needs_clear = YES;
        return HMJE_POSIX_ENOSYS_LOCAL;
    }

    if (*slot == nil || (u64)(*slot).width != width || (u64)(*slot).height != height) {
        id<MTLTexture> previous = *slot;
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:NO];
        description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        id<MTLTexture> replacement = [g_device newTextureWithDescriptor:description];
        if (replacement == nil) return HMJE_POSIX_ENOSYS_LOCAL;
        *slot = replacement;
        *needs_clear = YES;

        if (previous != nil) {
            id<MTLCommandBuffer> copy_commands = [g_queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [copy_commands blitCommandEncoder];
            NSUInteger copy_width = MIN(previous.width, replacement.width);
            NSUInteger copy_height = MIN(previous.height, replacement.height);
            if (copy_commands != nil && blit != nil && copy_width > 0 && copy_height > 0) {
                [blit copyFromTexture:previous sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                            toTexture:replacement destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                [copy_commands commit];
                /* Same queue ordering makes this visible to the new transaction. */
                *needs_clear = NO;
            }
        }
    }

    /* Acquire before encoding and before END asks CAMetalLayer for a drawable;
     * this is the usual triple-buffer bound, rather than letting nextDrawable
     * become an accidental fourth-frame throttle. */
    if (!hmje_ios_metal_take_in_flight_permit()) return HMJE_POSIX_ENOSYS_LOCAL;
    g_ios_gpu_transaction_has_permit = YES;
    g_ios_gpu_transaction_slot = hmje_ios_gpu_take_frame_slot();
    if (g_ios_gpu_transaction_slot < 0) {
        hmje_ios_metal_release_in_flight_permit();
        g_ios_gpu_transaction_has_permit = NO;
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    g_ios_gpu_commands = [g_queue commandBuffer];
    if (g_ios_gpu_commands == nil) {
        hmje_ios_gpu_release_frame_slot(g_ios_gpu_transaction_slot);
        g_ios_gpu_transaction_slot = -1;
        hmje_ios_metal_release_in_flight_permit();
        g_ios_gpu_transaction_has_permit = NO;
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    g_ios_gpu_transaction_resources = [NSMutableArray array];
    if (g_ios_gpu_transaction_resources == nil) {
        g_ios_gpu_commands = nil;
        hmje_ios_gpu_release_frame_slot(g_ios_gpu_transaction_slot);
        g_ios_gpu_transaction_slot = -1;
        hmje_ios_metal_release_in_flight_permit();
        g_ios_gpu_transaction_has_permit = NO;
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    hmje_ios_gpu_hold_resource(*slot);
    g_ios_gpu_render_encoder = nil;
    g_ios_gpu_glyph = nil;
    g_ios_gpu_layer_depth = 0;
    g_ios_gpu_skipped_layer_depth = 0;
    g_ios_gpu_clip = (MTLScissorRect){0, 0, (NSUInteger)width, (NSUInteger)height};
    g_ios_gpu_needs_clear_before_transaction = *needs_clear;
    return 1;
}

static i64 hmje_ios_gpu_set_clip(const u64 *request, u64 length) {
    if (length != 32 || request == NULL || g_ios_gpu_commands == nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 left = request[0], top = request[1], right = request[2], bottom = request[3];
    u64 width = (u64)target.width, height = (u64)target.height;
    if (left > width) left = width;
    if (top > height) top = height;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    g_ios_gpu_clip = (MTLScissorRect){(NSUInteger)left, (NSUInteger)top,
        (NSUInteger)(right - left), (NSUInteger)(bottom - top)};
    if (g_ios_gpu_render_encoder != nil && g_ios_gpu_clip.width > 0 &&
        g_ios_gpu_clip.height > 0) {
        [g_ios_gpu_render_encoder setScissorRect:g_ios_gpu_clip];
    }
    return 1;
}

static i64 hmje_ios_gpu_upload_glyph(const u64 *request, u64 length) {
    if (length != 32 || request == NULL || g_ios_gpu_commands == nil)
        return HMJE_POSIX_ENOSYS_LOCAL;
    g_ios_gpu_glyph = hmje_metal_glyph_texture(g_device,
        (const unsigned char *)(uintptr_t)request[0], request[1], request[2], request[3]);
    if (g_ios_gpu_glyph == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    hmje_ios_gpu_hold_resource(g_ios_gpu_glyph);
    return 1;
}

static i64 hmje_ios_gpu_draw(const u64 *request, u64 length) {
    if (length != 104 || request == NULL || g_ios_gpu_commands == nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    int kind = (int)request[0];
    if (kind != 0 && kind != 1 && kind != 2 && kind != 3 && kind != 4 && kind != 5) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (g_ios_gpu_clip.width == 0 || g_ios_gpu_clip.height == 0) return 1;
    if (kind != 3 && (request[3] == 0 || request[4] == 0)) return 1;
    if (kind == 3 && request[7] == 0) return 1;
    if (kind == 4 && g_ios_gpu_glyph == nil) return HMJE_POSIX_ENOSYS_LOCAL;

    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    HmjeIosGpuUniforms uniforms = {0};
    if (kind == 3) {
        /* HMJE has already flattened the cubic.  Draw one conservative quad
         * around this round-cap segment; the fragment shader evaluates exact
         * capsule distance, including the zero-length (circle) case. */
        double ax = (double)(i64)request[1], ay = (double)(i64)request[2];
        double bx = (double)(i64)request[3], by = (double)(i64)request[4];
        if (request[7] > UINT64_C(16777216)) return HMJE_POSIX_ENOSYS_LOCAL;
        double radius = (double)request[7] * 0.5;
        double left = floor(fmin(ax, bx) - radius - 1.0);
        double top = floor(fmin(ay, by) - radius - 1.0);
        double right = ceil(fmax(ax, bx) + radius + 1.0);
        double bottom = ceil(fmax(ay, by) + radius + 1.0);
        double clip_left = (double)g_ios_gpu_clip.x;
        double clip_top = (double)g_ios_gpu_clip.y;
        double clip_right = clip_left + (double)g_ios_gpu_clip.width;
        double clip_bottom = clip_top + (double)g_ios_gpu_clip.height;
        left = fmax(left, clip_left);
        top = fmax(top, clip_top);
        right = fmin(right, clip_right);
        bottom = fmin(bottom, clip_bottom);
        if (right <= left || bottom <= top) return 1;
        uniforms.origin[0] = (float)left;
        uniforms.origin[1] = (float)top;
        uniforms.size[0] = (float)(right - left);
        uniforms.size[1] = (float)(bottom - top);
        uniforms.params[1] = (float)radius;
        uniforms.shadow[0] = (float)ax;
        uniforms.shadow[1] = (float)ay;
        uniforms.shadow[2] = (float)bx;
        uniforms.shadow[3] = (float)by;
    } else {
        uniforms.origin[0] = (float)(i64)request[1];
        uniforms.origin[1] = (float)(i64)request[2];
        uniforms.size[0] = (float)request[3];
        uniforms.size[1] = (float)request[4];
        uniforms.params[1] = (float)request[5];
        uniforms.shadow[0] = (float)(i64)request[9];
        uniforms.shadow[1] = (float)(i64)request[10];
        uniforms.shadow[2] = (float)request[11];
        uniforms.shadow[3] = (float)request[12];
    }
    uniforms.color[0] = (float)((request[8] >> 16) & 255) / 255.0f;
    uniforms.color[1] = (float)((request[8] >> 8) & 255) / 255.0f;
    uniforms.color[2] = (float)(request[8] & 255) / 255.0f;
    uniforms.color[3] = (float)((request[8] >> 24) & 255) / 255.0f;
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    uniforms.params[0] = (float)kind;
    uniforms.params[2] = (float)request[6];
    uniforms.params[3] = (float)request[7];
    if (kind == 1 || kind == 2) {
        u64 outer = MIN(request[5], MIN(request[3] / 2, request[4] / 2));
        uniforms.params[1] = (float)outer;
        if (kind == 2) {
            u64 innerWidth = request[7] <= request[3] / 2 ? request[3] - 2 * request[7] : 0;
            u64 innerHeight = request[7] <= request[4] / 2 ? request[4] - 2 * request[7] : 0;
            uniforms.params[2] = (float)MIN(request[6], MIN(innerWidth / 2, innerHeight / 2));
        }
    }

    id<MTLRenderCommandEncoder> encoder = hmje_ios_gpu_encoder();
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder setRenderPipelineState:kind == 4
        ? g_ios_gpu_glyph_pipeline : g_ios_gpu_shape_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    if (kind == 4) [encoder setFragmentTexture:g_ios_gpu_glyph atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    return 1;
}

static u64 hmje_ios_gpu_negative_magnitude(i64 value) {
    return (u64)(-(value + 1)) + 1;
}

/* Crop a 1:1 copy against source bounds, signed destination bounds, and the
 * current clip while advancing the source origin by the same amount. */
static BOOL hmje_ios_gpu_crop_copy(u64 source_width, u64 source_height,
                                   u64 target_width, u64 target_height,
                                   MTLScissorRect clip,
                                   u64 *source_x, u64 *source_y,
                                   u64 *copy_width, u64 *copy_height,
                                   i64 *destination_x, i64 *destination_y) {
    if (*source_x >= source_width || *source_y >= source_height ||
        *copy_width == 0 || *copy_height == 0 || clip.width == 0 || clip.height == 0) {
        return NO;
    }
    if (*copy_width > source_width - *source_x) *copy_width = source_width - *source_x;
    if (*copy_height > source_height - *source_y) *copy_height = source_height - *source_y;

    if (*destination_x < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(*destination_x);
        if (amount >= *copy_width) return NO;
        *source_x += amount;
        *copy_width -= amount;
        *destination_x = 0;
    }
    if (*destination_y < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(*destination_y);
        if (amount >= *copy_height) return NO;
        *source_y += amount;
        *copy_height -= amount;
        *destination_y = 0;
    }

    u64 dx = (u64)*destination_x, dy = (u64)*destination_y;
    if (dx >= target_width || dy >= target_height) return NO;
    if (*copy_width > target_width - dx) *copy_width = target_width - dx;
    if (*copy_height > target_height - dy) *copy_height = target_height - dy;

    u64 clip_left = (u64)clip.x, clip_top = (u64)clip.y;
    u64 clip_right = clip_left + (u64)clip.width;
    u64 clip_bottom = clip_top + (u64)clip.height;
    if (dx < clip_left) {
        u64 amount = clip_left - dx;
        if (amount >= *copy_width) return NO;
        *source_x += amount;
        *copy_width -= amount;
        dx = clip_left;
        *destination_x = (i64)dx;
    }
    if (dy < clip_top) {
        u64 amount = clip_top - dy;
        if (amount >= *copy_height) return NO;
        *source_y += amount;
        *copy_height -= amount;
        dy = clip_top;
        *destination_y = (i64)dy;
    }
    if (dx >= clip_right || dy >= clip_bottom) return NO;
    if (*copy_width > clip_right - dx) *copy_width = clip_right - dx;
    if (*copy_height > clip_bottom - dy) *copy_height = clip_bottom - dy;
    return *copy_width > 0 && *copy_height > 0;
}

static i64 hmje_ios_gpu_image_blit(const u64 *request, u64 length) {
    if (length != 80 || request == NULL || g_ios_gpu_commands == nil || request[0] == 0) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 source_width = request[1], source_height = request[2], source_pitch = request[3];
    if (source_height != 0 && source_pitch > UINT64_MAX / source_height) return HMJE_POSIX_ENOSYS_LOCAL;
    if (source_width > UINT64_MAX / 4 || source_pitch < source_width * 4 ||
        source_pitch > (u64)NSUIntegerMax) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 source_x = request[4], source_y = request[5];
    u64 copy_width = request[6], copy_height = request[7];
    i64 destination_x = (i64)request[8], destination_y = (i64)request[9];
    if (!hmje_ios_gpu_crop_copy(source_width, source_height, (u64)target.width,
                                (u64)target.height, g_ios_gpu_clip, &source_x, &source_y,
                                &copy_width, &copy_height, &destination_x, &destination_y)) {
        return 1;
    }
    if (copy_width > (u64)NSUIntegerMax || copy_height > (u64)NSUIntegerMax) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }

    /* A temporary shared texture accepts arbitrary source pitch and avoids the
     * buffer-to-texture row-alignment restrictions that differ by Metal GPU. */
    MTLTextureDescriptor *description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                 width:(NSUInteger)copy_width
                                height:(NSUInteger)copy_height
                             mipmapped:NO];
    description.usage = MTLTextureUsageShaderRead;
    description.storageMode = MTLStorageModeShared;
    id<MTLTexture> upload = [g_device newTextureWithDescriptor:description];
    if (upload == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    hmje_ios_gpu_hold_resource(upload);
    const unsigned char *source = (const unsigned char *)(uintptr_t)request[0]
        + source_y * source_pitch + source_x * 4;
    [upload replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)copy_width,
                                          (NSUInteger)copy_height)
             mipmapLevel:0 withBytes:source bytesPerRow:(NSUInteger)source_pitch];

    hmje_ios_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_ios_gpu_commands blitCommandEncoder];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder copyFromTexture:upload sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake((NSUInteger)copy_width, (NSUInteger)copy_height, 1)
                   toTexture:target destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake((NSUInteger)destination_x,
                                           (NSUInteger)destination_y, 0)];
    [encoder endEncoding];
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static i64 hmje_ios_gpu_image_scale_blit(const u64 *request, u64 length) {
    if ((length != 104 && length != 112) || request == NULL || (length == 112 && request[13] != 1) || g_ios_gpu_commands == nil || request[0] == 0) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (g_ios_gpu_clip.width == 0 || g_ios_gpu_clip.height == 0) return 1;
    u64 source_width = request[1], source_height = request[2], source_pitch = request[3];
    u64 source_x = request[4], source_y = request[5];
    u64 copy_width = request[6], copy_height = request[7];
    i64 destination_x = (i64)request[8], destination_y = (i64)request[9];
    u64 destination_width = request[10], destination_height = request[11];
    if (source_width > UINT64_MAX / 4 || source_pitch < source_width * 4 ||
        source_pitch > (u64)NSUIntegerMax || source_x >= source_width ||
        source_y >= source_height || destination_width == 0 || destination_height == 0) {
        return source_x >= source_width || source_y >= source_height ||
            destination_width == 0 || destination_height == 0
            ? 1 : HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (copy_width > source_width - source_x) copy_width = source_width - source_x;
    if (copy_height > source_height - source_y) copy_height = source_height - source_y;
    if (copy_width == 0 || copy_height == 0) return 1;
    if (copy_width > (u64)NSUIntegerMax || copy_height > (u64)NSUIntegerMax) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }

    /* A fresh texture per command is intentional.  Replacing a reused texture
     * before the queued draw executes would make earlier image draws sample the
     * last upload in the transaction. */
    MTLTextureDescriptor *description = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                 width:(NSUInteger)copy_width
                                height:(NSUInteger)copy_height
                             mipmapped:NO];
    description.usage = MTLTextureUsageShaderRead;
    description.storageMode = MTLStorageModeShared;
    id<MTLTexture> source_texture = [g_device newTextureWithDescriptor:description];
    if (source_texture == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    hmje_ios_gpu_hold_resource(source_texture);
    const unsigned char *source = (const unsigned char *)(uintptr_t)request[0]
        + source_y * source_pitch + source_x * 4;
    [source_texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)copy_width,
                                                  (NSUInteger)copy_height)
                       mipmapLevel:0 withBytes:source bytesPerRow:(NSUInteger)source_pitch];

    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    HmjeIosGpuUniforms uniforms = {0};
    uniforms.origin[0] = (float)destination_x;
    uniforms.origin[1] = (float)destination_y;
    uniforms.size[0] = (float)destination_width;
    uniforms.size[1] = (float)destination_height;
    u64 image_radius = request[12];
    if (image_radius > destination_width/2) image_radius=destination_width/2;
    if (image_radius > destination_height/2) image_radius=destination_height/2;
    uniforms.params[1] = (float)image_radius;
    uniforms.params[2] = length == 112 ? 1.f : 0.f; /* explicit straight RGBA */
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    id<MTLRenderCommandEncoder> encoder = hmje_ios_gpu_encoder();
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder setRenderPipelineState:g_ios_gpu_image_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:source_texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    return 1;
}

static i64 hmje_ios_gpu_composite_target(const u64 *request, u64 length) {
    if (length != 56 || request == NULL || g_ios_gpu_commands == nil || request[0] > 1) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    unsigned source_failed_bit = request[0] == 1 ? 2u : 1u;
    /* A source target can fail after END already returned success to HMJE.
     * Do not consume its bit here: BEGIN for that source owns acknowledgement
     * and native clear/resync.  Rejecting the composite makes the common
     * renderer invalidate the source synchronization flag.  HmjeUI then
     * rebuilds any lazy CPU background shadow before software fallback; a
     * later screen transaction may safely upload that recovered shadow. */
    if ((atomic_load_explicit(&g_ios_gpu_async_failed_targets,
                              memory_order_acquire) & source_failed_bit) != 0) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> source = request[0] == 1
        ? g_ios_gpu_background_target : g_ios_gpu_screen_target;
    id<MTLTexture> destination = hmje_ios_gpu_active_target();
    if (source == nil || destination == nil || source == destination) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    hmje_ios_gpu_hold_resource(source);
    u64 source_x = request[1], source_y = request[2];
    u64 copy_width = request[3], copy_height = request[4];
    i64 destination_x = (i64)request[5], destination_y = (i64)request[6];
    if (!hmje_ios_gpu_crop_copy((u64)source.width, (u64)source.height,
                                (u64)destination.width, (u64)destination.height,
                                g_ios_gpu_clip, &source_x, &source_y, &copy_width,
                                &copy_height, &destination_x, &destination_y)) {
        return 1;
    }
    hmje_ios_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_ios_gpu_commands blitCommandEncoder];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake((NSUInteger)source_x, (NSUInteger)source_y, 0)
                  sourceSize:MTLSizeMake((NSUInteger)copy_width, (NSUInteger)copy_height, 1)
                   toTexture:destination destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake((NSUInteger)destination_x,
                                           (NSUInteger)destination_y, 0)];
    [encoder endEncoding];
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static BOOL hmje_ios_gpu_ensure_scroll_scratch(u64 width, u64 height) {
    if (g_ios_gpu_transaction_slot < 0 ||
        g_ios_gpu_transaction_slot >= HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) return NO;
    __strong id<MTLTexture> *scratch =
        &g_ios_gpu_scroll_scratch[g_ios_gpu_transaction_slot];
    u64 allocated_width = width, allocated_height = height;
    if (*scratch != nil) {
        allocated_width = MAX(allocated_width, (u64)(*scratch).width);
        allocated_height = MAX(allocated_height, (u64)(*scratch).height);
    }
    if (*scratch == nil || (u64)(*scratch).width < width ||
        (u64)(*scratch).height < height) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)allocated_width
                                    height:(NSUInteger)allocated_height
                                 mipmapped:NO];
        description.usage = MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        *scratch = [g_device newTextureWithDescriptor:description];
    }
    hmje_ios_gpu_hold_resource(*scratch);
    return *scratch != nil;
}

static i64 hmje_ios_gpu_scroll_shift(const u64 *request, u64 length) {
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (length != 48 || request == NULL || g_ios_gpu_commands == nil || target == nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    i64 signed_x = (i64)request[0], signed_y = (i64)request[1];
    u64 width = request[2], height = request[3], shift = request[4];
    BOOL down = request[5] != 0;
    if (signed_x < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(signed_x);
        if (amount >= width) return 1;
        width -= amount;
        signed_x = 0;
    }
    if (signed_y < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(signed_y);
        if (amount >= height) return 1;
        height -= amount;
        signed_y = 0;
    }
    u64 x = (u64)signed_x, y = (u64)signed_y;
    u64 target_width = (u64)target.width, target_height = (u64)target.height;
    if (x >= target_width || y >= target_height || width == 0 || height == 0) return 1;
    if (width > target_width - x) width = target_width - x;
    if (height > target_height - y) height = target_height - y;
    if (shift == 0 || shift >= height) return 1;
    u64 copy_height = height - shift;
    u64 source_y = down ? y : y + shift;
    u64 destination_y = down ? y + shift : y;
    if (!hmje_ios_gpu_ensure_scroll_scratch(width, copy_height)) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> scratch = g_ios_gpu_scroll_scratch[g_ios_gpu_transaction_slot];
    hmje_ios_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_ios_gpu_commands blitCommandEncoder];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder copyFromTexture:target sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)source_y, 0)
                  sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)copy_height, 1)
                   toTexture:scratch destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder copyFromTexture:scratch sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)copy_height, 1)
                   toTexture:target destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)destination_y, 0)];
    [encoder endEncoding];
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static i64 hmje_ios_gpu_planar_shift(const u64 *request, u64 length) {
    id<MTLTexture> target = hmje_ios_gpu_active_target();
    if (length != 64 || request == NULL || g_ios_gpu_commands == nil || target == nil) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    i64 signed_x = (i64)request[0], signed_y = (i64)request[1];
    u64 width = request[2], height = request[3], dx = request[4], dy = request[5];
    BOOL right = request[6] != 0, down = request[7] != 0;
    if (signed_x < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(signed_x);
        if (amount >= width) return 1;
        width -= amount;
        signed_x = 0;
    }
    if (signed_y < 0) {
        u64 amount = hmje_ios_gpu_negative_magnitude(signed_y);
        if (amount >= height) return 1;
        height -= amount;
        signed_y = 0;
    }
    u64 x = (u64)signed_x, y = (u64)signed_y;
    u64 target_width = (u64)target.width, target_height = (u64)target.height;
    if (x >= target_width || y >= target_height || width == 0 || height == 0) return 1;
    if (width > target_width - x) width = target_width - x;
    if (height > target_height - y) height = target_height - y;
    if ((dx == 0 && dy == 0) || dx >= width || dy >= height) return 1;
    u64 copy_width = width - dx, copy_height = height - dy;
    u64 source_x = right ? x : x + dx;
    u64 destination_x = right ? x + dx : x;
    u64 source_y = down ? y : y + dy;
    u64 destination_y = down ? y + dy : y;
    if (!hmje_ios_gpu_ensure_scroll_scratch(copy_width, copy_height)) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> scratch = g_ios_gpu_scroll_scratch[g_ios_gpu_transaction_slot];
    hmje_ios_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_ios_gpu_commands blitCommandEncoder];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder copyFromTexture:target sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake((NSUInteger)source_x, (NSUInteger)source_y, 0)
                  sourceSize:MTLSizeMake((NSUInteger)copy_width, (NSUInteger)copy_height, 1)
                   toTexture:scratch destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder copyFromTexture:scratch sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake((NSUInteger)copy_width, (NSUInteger)copy_height, 1)
                   toTexture:target destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake((NSUInteger)destination_x,
                                           (NSUInteger)destination_y, 0)];
    [encoder endEncoding];
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static i64 hmje_ios_gpu_layer_begin(const u64 *request, u64 length) {
    if (length != 40 || request == NULL || g_ios_gpu_commands == nil ||
        g_ios_gpu_layer_depth >= HMJE_IOS_GPU_LAYER_MAX ||
        g_ios_gpu_transaction_slot < 0 ||
        g_ios_gpu_transaction_slot >= HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    id<MTLTexture> source = hmje_ios_gpu_active_target();
    if (source == nil || !hmje_ios_gpu_ensure_root_initialized()) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    u64 x = request[0], y = request[1], width = request[2], height = request[3];
    u64 alpha = request[4] > 255 ? 255 : request[4];
    /* The software display-list interpreter treats a layer whose intersection
     * with the target is empty as an omitted layer. Keep the transaction on
     * Metal and consume its balancing end marker as a no-op. */
    if (x >= (u64)source.width || y >= (u64)source.height) {
        g_ios_gpu_skipped_layer_depth = 1;
        return 1;
    }
    if (width > (u64)source.width - x) width = (u64)source.width - x;
    if (height > (u64)source.height - y) height = (u64)source.height - y;
    if (width == 0 || height == 0) {
        g_ios_gpu_skipped_layer_depth = 1;
        return 1;
    }

    int slot = g_ios_gpu_layer_depth;
    id<MTLTexture> layer =
        g_ios_gpu_layer_texture[g_ios_gpu_transaction_slot][slot];
    /* UVs cover the whole texture.  Exact-size reuse prevents a prior larger
     * layer from being rescaled (and leaking stale pixels) into a smaller one. */
    if (layer == nil || (u64)layer.width != width || (u64)layer.height != height) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:NO];
        description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        layer = [g_device newTextureWithDescriptor:description];
        if (layer == nil) return HMJE_POSIX_ENOSYS_LOCAL;
        g_ios_gpu_layer_texture[g_ios_gpu_transaction_slot][slot] = layer;
    }
    hmje_ios_gpu_hold_resource(layer);

    hmje_ios_gpu_end_render_encoder();
    id<MTLBlitCommandEncoder> encoder = [g_ios_gpu_commands blitCommandEncoder];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                  sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
                   toTexture:layer destinationSlice:0 destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];

    g_ios_gpu_layer_saved_target[slot] = source;
    g_ios_gpu_layer_saved_clip[slot] = g_ios_gpu_clip;
    g_ios_gpu_layer_x[slot] = x;
    g_ios_gpu_layer_y[slot] = y;
    g_ios_gpu_layer_width[slot] = width;
    g_ios_gpu_layer_height[slot] = height;
    g_ios_gpu_layer_alpha[slot] = alpha;
    g_ios_gpu_layer_depth = slot + 1;
    g_ios_gpu_clip = (MTLScissorRect){0, 0, (NSUInteger)width, (NSUInteger)height};
    return 1;
}

static i64 hmje_ios_gpu_layer_end(u64 length) {
    if (length != 0 || g_ios_gpu_commands == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_ios_gpu_layer_depth <= 0) return 1;
    if (g_ios_gpu_transaction_slot < 0 ||
        g_ios_gpu_transaction_slot >= HMJE_IOS_GPU_MAX_FRAMES_IN_FLIGHT) {
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    int slot = --g_ios_gpu_layer_depth;
    id<MTLTexture> layer =
        g_ios_gpu_layer_texture[g_ios_gpu_transaction_slot][slot];
    id<MTLTexture> target = g_ios_gpu_layer_saved_target[slot];
    g_ios_gpu_clip = g_ios_gpu_layer_saved_clip[slot];
    g_ios_gpu_layer_saved_target[slot] = nil;
    if (layer == nil || target == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    hmje_ios_gpu_end_render_encoder();
    if (g_ios_gpu_clip.width == 0 || g_ios_gpu_clip.height == 0) return 1;

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        [g_ios_gpu_commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    [encoder setScissorRect:g_ios_gpu_clip];
    HmjeIosGpuUniforms uniforms = {0};
    uniforms.origin[0] = (float)g_ios_gpu_layer_x[slot];
    uniforms.origin[1] = (float)g_ios_gpu_layer_y[slot];
    uniforms.size[0] = (float)g_ios_gpu_layer_width[slot];
    uniforms.size[1] = (float)g_ios_gpu_layer_height[slot];
    uniforms.viewport[0] = (float)target.width;
    uniforms.viewport[1] = (float)target.height;
    uniforms.color[3] = (float)g_ios_gpu_layer_alpha[slot] / 255.0f;
    [encoder setRenderPipelineState:g_ios_gpu_layer_pipeline];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:layer atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    hmje_ios_gpu_mark_active_initialized();
    return 1;
}

static void hmje_ios_gpu_drop_transaction(void) {
    hmje_ios_gpu_end_render_encoder();
    g_ios_gpu_commands = nil;
    for (int index = 0; index < HMJE_IOS_GPU_LAYER_MAX; ++index) {
        g_ios_gpu_layer_saved_target[index] = nil;
    }
    g_ios_gpu_layer_depth = 0;
    g_ios_gpu_skipped_layer_depth = 0;
    g_ios_gpu_transaction_resources = nil;
    if (g_ios_gpu_transaction_has_permit) {
        hmje_ios_gpu_release_frame_slot(g_ios_gpu_transaction_slot);
        g_ios_gpu_transaction_slot = -1;
        hmje_ios_metal_release_in_flight_permit();
        g_ios_gpu_transaction_has_permit = NO;
    }
}

static i64 hmje_ios_gpu_abort(void) {
    if (g_ios_gpu_commands != nil) {
        *hmje_ios_gpu_root_needs_clear_flag() = g_ios_gpu_needs_clear_before_transaction;
    }
    hmje_ios_gpu_drop_transaction();
    return 1;
}

static i64 hmje_ios_gpu_finish_committed(BOOL presented) {
    if (!g_ios_gpu_transaction_has_permit) {
        hmje_ios_gpu_abort();
        return HMJE_POSIX_ENOSYS_LOCAL;
    }

    /* A single Metal command queue preserves all dependencies between
     * persistent targets and successive frames.  The permit transfers from the
     * active transaction to this completion handler at commit. */
    id<MTLCommandBuffer> commands = g_ios_gpu_commands;
    NSArray *resources = [g_ios_gpu_transaction_resources copy];
    int frame_slot = g_ios_gpu_transaction_slot;
    unsigned failed_target_bit = g_ios_gpu_current_target == 1 ? 2u : 1u;
    BOOL screen_transaction = g_ios_gpu_current_target == 0;
    BOOL report_debug = presented && getenv("HMJE_POSIX_DEBUG_GPU") != NULL;
    g_ios_gpu_commands = nil;
    g_ios_gpu_transaction_resources = nil;
    g_ios_gpu_transaction_slot = -1;
    g_ios_gpu_transaction_has_permit = NO;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        @autoreleasepool {
            /* Capturing the immutable array retains every texture until the GPU
             * has consumed the command buffer. */
            (void)resources;
            if (completed.status != MTLCommandBufferStatusCompleted) {
                atomic_fetch_or_explicit(&g_ios_gpu_async_failed_targets, failed_target_bit,
                                         memory_order_release);
                fprintf(stderr, "posix-ios-gpu: command buffer failed: %s\n",
                        completed.error.localizedDescription.UTF8String ?: "unknown error");
            } else if (report_debug &&
                       !atomic_exchange_explicit(&g_ios_gpu_debug_reported, true,
                                                 memory_order_acq_rel)) {
                fprintf(stderr,
                        "posix-ios-gpu: direct command rendering active (%llux%llu, scale=%llu)\n",
                        g_width, g_height, g_scale);
            }
            hmje_ios_gpu_release_frame_slot(frame_slot);
            hmje_ios_metal_release_in_flight_permit();
        }
    }];
    if (screen_transaction) {
        /* This persistent screen transaction is ordered after every earlier
         * software upload on the shared queue.  Supersede its pending epoch
         * even when nextDrawable was temporarily nil: otherwise that older CPU
         * snapshot could flush on foreground and roll back the newer target.
         * Background-only transactions must not invalidate screen pending. */
        hmje_ios_present_supersede_software();
    }
    [commands commit];
    return 1;
}

static i64 hmje_ios_gpu_end_present(u64 length) {
    if (length != 0 || g_ios_gpu_commands == nil) return HMJE_POSIX_ENOSYS_LOCAL;
    if (g_ios_gpu_layer_depth != 0 || g_ios_gpu_skipped_layer_depth != 0) {
        hmje_ios_gpu_abort();
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    if (!hmje_ios_gpu_ensure_root_initialized()) {
        hmje_ios_gpu_abort();
        return HMJE_POSIX_ENOSYS_LOCAL;
    }
    hmje_ios_gpu_end_render_encoder();
    if (g_ios_gpu_current_target == 1) return hmje_ios_gpu_finish_committed(NO);

    id<CAMetalDrawable> drawable = [g_metal_layer nextDrawable];
    if (drawable == nil) {
        /* The app may be temporarily backgrounded.  Commit the persistent
         * target anyway so its synchronization state remains truthful. */
        return hmje_ios_gpu_finish_committed(NO);
    }
    id<MTLTexture> target = g_ios_gpu_screen_target;
    NSUInteger copy_width = MIN(target.width, drawable.texture.width);
    NSUInteger copy_height = MIN(target.height, drawable.texture.height);
    if (copy_width > 0 && copy_height > 0) {
        id<MTLBlitCommandEncoder> blit = [g_ios_gpu_commands blitCommandEncoder];
        if (blit == nil) {
            hmje_ios_gpu_abort();
            return HMJE_POSIX_ENOSYS_LOCAL;
        }
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(copy_width, copy_height, 1)
                    toTexture:drawable.texture destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    }
    [g_ios_gpu_commands presentDrawable:drawable];
    return hmje_ios_gpu_finish_committed(YES);
}

/* The same downsample/convolution/composite implementation as macOS. */
#define g_gpu_command_buffer g_ios_gpu_commands
#define g_gpu_clip g_ios_gpu_clip
#define hmje_gpu_active_target hmje_ios_gpu_active_target
#define hmje_gpu_end_render_encoder hmje_ios_gpu_end_render_encoder
#define HMJE_METAL_BLUR_MARK_INITIALIZED() hmje_ios_gpu_mark_active_initialized()
#include "metal_backdrop_blur.inc"
#undef HMJE_METAL_BLUR_MARK_INITIALIZED
#undef hmje_gpu_end_render_encoder
#undef hmje_gpu_active_target
#undef g_gpu_clip
#undef g_gpu_command_buffer

i64 hmje_posix_gpu_command(i64 kind, i64 request_address, i64 request_length) {
    @autoreleasepool {
        const u64 *request = (const u64 *)(uintptr_t)(u64)request_address;
        u64 length = request_length < 0 ? 0 : (u64)request_length;
        if (kind == 0) {
            if (length != 32 || request == NULL) return HMJE_POSIX_ENOSYS_LOCAL;
            u64 *output = (u64 *)(uintptr_t)(u64)request_address;
            output[0]=2; output[1]=1; output[2]=UINT64_C(0x000000000000fffe); output[3]=0;
            return 32;
        }
        if (g_ios_gpu_skipped_layer_depth > 0) {
            if (kind == HMJE_IOS_GPU_LAYER_BEGIN) {
                ++g_ios_gpu_skipped_layer_depth;
                return 1;
            }
            if (kind == HMJE_IOS_GPU_LAYER_END) {
                --g_ios_gpu_skipped_layer_depth;
                return 1;
            }
            if (kind != HMJE_IOS_GPU_ABORT && kind != HMJE_IOS_GPU_END_PRESENT) {
                return 1;
            }
        }
        switch (kind) {
            case HMJE_IOS_GPU_BEGIN:
                return hmje_ios_gpu_begin(request, length);
            case HMJE_IOS_GPU_CLIP:
                return hmje_ios_gpu_set_clip(request, length);
            case HMJE_IOS_GPU_UPLOAD_GLYPH:
                return hmje_ios_gpu_upload_glyph(request, length);
            case HMJE_IOS_GPU_DRAW:
                return hmje_ios_gpu_draw(request, length);
            case HMJE_IOS_GPU_IMAGE_BLIT:
                return hmje_ios_gpu_image_blit(request, length);
            case HMJE_IOS_GPU_IMAGE_SCALE_BLIT:
                return hmje_ios_gpu_image_scale_blit(request, length);
            case HMJE_IOS_GPU_COMPOSITE_TARGET:
                return hmje_ios_gpu_composite_target(request, length);
            case HMJE_GPU_BACKDROP_BLUR:
                if (!hmje_ios_gpu_ensure_root_initialized()) return HMJE_POSIX_ENOSYS_LOCAL;
                return hmje_gpu_backdrop_blur(request, length);
            case HMJE_IOS_GPU_GRADIENT_FILL:
                return hmje_ios_gpu_gradient_fill(request, length);
            case HMJE_IOS_GPU_SCROLL_SHIFT:
                return hmje_ios_gpu_scroll_shift(request, length);
            case HMJE_IOS_GPU_PLANAR_SHIFT:
                return hmje_ios_gpu_planar_shift(request, length);
            case HMJE_IOS_GPU_LAYER_BEGIN:
                return hmje_ios_gpu_layer_begin(request, length);
            case HMJE_IOS_GPU_LAYER_END:
                return hmje_ios_gpu_layer_end(length);
            case HMJE_IOS_GPU_END_PRESENT:
                return hmje_ios_gpu_end_present(length);
            case HMJE_IOS_GPU_ABORT:
                return length == 0 ? hmje_ios_gpu_abort() : HMJE_POSIX_ENOSYS_LOCAL;
            default:
                /* Unsupported effects deliberately reject the whole frame;
                 * HmjeGraphics then replays it through the CPU framebuffer. */
                return HMJE_POSIX_ENOSYS_LOCAL;
        }
    }
}
