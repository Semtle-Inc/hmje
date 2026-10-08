/*
 * HMJE hosted GPU command ABI -> Android OpenGL ES 3 renderer.
 *
 * Policy and display-list interpretation stay in hmje-ui.  This file is a thin
 * executor for the normalized GPU_COMMAND_* stream.  A transaction renders to
 * a work texture and only copies into the persistent target at END, so ABORT
 * never exposes a partially rendered frame.
 */

#define HMJE_GLES_GPU_BEGIN 1
#define HMJE_GLES_GPU_CLIP 2
#define HMJE_GLES_GPU_UPLOAD_GLYPH 3
#define HMJE_GLES_GPU_DRAW 4
#define HMJE_GLES_GPU_IMAGE_BLIT 5
#define HMJE_GLES_GPU_END_PRESENT 6
#define HMJE_GLES_GPU_ABORT 7
#define HMJE_GLES_GPU_SCROLL_SHIFT 8
#define HMJE_GLES_GPU_COMPOSITE_TARGET 9
#define HMJE_GLES_GPU_BACKDROP_BLUR 10
#define HMJE_GLES_GPU_GRADIENT_FILL 11
#define HMJE_GLES_GPU_IMAGE_SCALE_BLIT 12
#define HMJE_GLES_GPU_LAYER_BEGIN 13
#define HMJE_GLES_GPU_LAYER_END 14
#define HMJE_GLES_GPU_PLANAR_SHIFT 15

#define HMJE_GLES_LAYER_MAX 8

static GLuint g_gpu_shape_program;
static GLuint g_gpu_texture_program;
static GLuint g_gpu_gradient_program;

static GLuint g_gpu_vao;
static GLuint g_gpu_target_texture[2];
static GLuint g_gpu_target_fbo[2];
static GLuint g_gpu_work_texture;
static GLuint g_gpu_work_fbo;
#include "gpu_glyph_cache.h"
static GLuint g_gpu_glyph_texture;
static GLuint g_gpu_glyph_transient;
static GLuint g_gpu_glyph_textures[HMJE_GLYPH_CACHE_SLOTS];
static hmje_glyph_cache g_android_glyph_cache;
static void hmje_android_glyph_release(unsigned slot) {
    if (g_gpu_glyph_textures[slot]) glDeleteTextures(1, &g_gpu_glyph_textures[slot]);
    g_gpu_glyph_textures[slot] = 0;
}
static GLuint g_gpu_upload_texture;
static GLuint g_gpu_shift_texture;
static GLuint g_gpu_shift_fbo;
static GLuint g_gpu_layer_texture[HMJE_GLES_LAYER_MAX];
static GLuint g_gpu_layer_fbo[HMJE_GLES_LAYER_MAX];
static GLuint g_gpu_layer_saved_fbo[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_x[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_y[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_w[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_h[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_alpha[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_saved_width[HMJE_GLES_LAYER_MAX];
static u64 g_gpu_layer_saved_height[HMJE_GLES_LAYER_MAX];
static int g_gpu_layer_depth;
static int g_gpu_active;
static int g_gpu_target;
static u64 g_gpu_width;
static u64 g_gpu_height;
static u64 g_gpu_root_width;
static u64 g_gpu_root_height;
static u64 g_gpu_transaction_count;
static int g_gpu_logged_first_draw;

static GLuint hmje_gles_link_program(const char *vertex_source, const char *fragment_source) {
    GLuint vertex = compile_gl_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_gl_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) return 0;
    GLuint result = glCreateProgram();
    glAttachShader(result, vertex);
    glAttachShader(result, fragment);
    glLinkProgram(result);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(result, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024]; GLsizei count = 0;
        glGetProgramInfoLog(result, sizeof(log), &count, log);
        LOGI("direct GLES link failed: %.*s", (int)count, log);
        glDeleteProgram(result);
        return 0;
    }
    return result;
}

static int hmje_gles_prepare_programs(void) {
    if (g_gpu_shape_program) return 1;
    static const char *quad_vertex =
        "#version 300 es\nprecision highp float;"
        "uniform vec4 uRect;uniform vec2 uViewport;out vec2 vLocal;out vec2 vUV;"
        "void main(){int i=gl_VertexID;float qx=(i==1||i==3)?1.:0.;float qy=(i>=2)?1.:0.;"
        "vec2 p=uRect.xy+vec2(qx,qy)*uRect.zw;vLocal=vec2(qx,qy)*uRect.zw;vUV=vec2(qx,qy);"
        "gl_Position=vec4(p.x/uViewport.x*2.-1.,1.-p.y/uViewport.y*2.,0.,1.);}";
    static const char *shape_fragment =
        "#version 300 es\nprecision highp float;precision highp int;"
        "uniform vec4 uRect;uniform vec4 uColor;uniform vec4 uParams;uniform vec4 uShadow;"
        "uniform sampler2D uGlyph;in vec2 vLocal;in vec2 vUV;out vec4 outColor;"
        "float rr(vec2 p,vec2 s,float r){vec2 q=abs(p-s*.5)-s*.5+r;return length(max(q,vec2(0)))+min(max(q.x,q.y),0.)-r;}"
        "float capsule(vec2 p,vec2 a,vec2 b,float r){vec2 pa=p-a,ba=b-a;float h=clamp(dot(pa,ba)/max(dot(ba,ba),.0001),0.,1.);return length(pa-ba*h)-r;}"
        "void main(){int k=int(uParams.x+.5);float cov=1.;"
        "if(k==4){float a=texture(uGlyph,vUV).r;outColor=vec4(uColor.rgb,uColor.a*a);return;}"
        "if(k==3){vec2 p=uRect.xy+vLocal;float d=capsule(p,uShadow.xy,uShadow.zw,uParams.w*.5);cov=clamp(.5-d,0.,1.);}"
        "else if(k==5){vec2 p=uRect.xy+vLocal;float b=uParams.z;vec2 so=uRect.xy+vec2(b),ss=uRect.zw-vec2(2.*b);float d=rr(p-so,ss,uParams.y);cov=b<=0.?clamp(.5-d,0.,1.):1.-smoothstep(-b,b,d);float inner=clamp(.5-rr(p-uShadow.xy,uShadow.zw,uParams.w),0.,1.);cov*=1.-inner;}"
        "else {float outer=clamp(.5-rr(vLocal,uRect.zw,uParams.y),0.,1.);cov=outer;if(k==2){vec2 is=uRect.zw-vec2(2.*uParams.w);float inner=clamp(.5-rr(vLocal-vec2(uParams.w),is,uParams.z),0.,1.);cov=max(0.,outer-inner);}}"
        "outColor=vec4(uColor.rgb,uColor.a*cov);}";
    static const char *texture_fragment =
        "#version 300 es\nprecision highp float;uniform sampler2D uTexture;uniform vec4 uUV;uniform float uAlpha;uniform float uRadius;uniform vec4 uRect;uniform int uSwizzle;uniform int uFlipY;uniform int uForceOpaque;uniform int uSourceAlpha;"
        "in vec2 vLocal;in vec2 vUV;out vec4 outColor;float rr(vec2 p,vec2 s,float r){vec2 q=abs(p-s*.5)-s*.5+r;return length(max(q,vec2(0)))+min(max(q.x,q.y),0.)-r;}void main(){vec2 q=vec2(vUV.x,uFlipY!=0?1.-vUV.y:vUV.y);vec2 p=mix(uUV.xy,uUV.zw,q);vec4 c=texture(uTexture,p);"
        /* Existing screen/layer/XRGB operations ignore stored alpha. Only the
         * explicit 112-byte IMAGE_SCALE request consumes straight source alpha.
         * RGBA coverage is quantized before source-over like CPU/Metal;
         * existing XRGB/layer coverage retains its previous rounding. */
        "if(uSwizzle!=0)c=c.bgra;float cov=clamp(.5-rr(vLocal,uRect.zw,uRadius),0.,1.);float a=(uForceOpaque!=0?1.:uAlpha)*cov;if(uSourceAlpha!=0&&uForceOpaque==0)a=floor(floor(cov*255.)*round(c.a*255.)/255.)/255.;outColor=vec4(c.rgb,a);}";
    static const char *gradient_fragment =
        "#version 300 es\nprecision highp float;uniform vec4 uRect;uniform float uRadius;uniform int uDirection;uniform int uCount;uniform vec4 uColors[8];"
        "in vec2 vLocal;in vec2 vUV;out vec4 outColor;float rr(vec2 p,vec2 s,float r){vec2 q=abs(p-s*.5)-s*.5+r;return length(max(q,vec2(0)))+min(max(q.x,q.y),0.)-r;}"
        "void main(){vec2 p=floor(vLocal);float position=uDirection==1?p.y:(uDirection==3?p.x+p.y:p.x);float denominator=uDirection==1?uRect.w-1.:(uDirection==3?uRect.z+uRect.w-2.:uRect.z-1.);"
        "float z=(denominator>0.?clamp(position/denominator,0.,1.):0.)*float(uCount-1);int i=clamp(int(floor(z)),0,uCount-2);"
        "vec4 c=round(mix(uColors[i],uColors[i+1],z-float(i))*255.);float a=floor(clamp(.5-rr(vLocal,uRect.zw,uRadius),0.,1.)*255.);outColor=vec4(c.rgb/255.,floor(c.a*a/255.)/255.);}";
    g_gpu_shape_program = hmje_gles_link_program(quad_vertex, shape_fragment);
    g_gpu_texture_program = hmje_gles_link_program(quad_vertex, texture_fragment);
    g_gpu_gradient_program = hmje_gles_link_program(quad_vertex, gradient_fragment);
    if (!g_gpu_shape_program || !g_gpu_texture_program || !g_gpu_gradient_program) return 0;
    glGenVertexArrays(1, &g_gpu_vao);
    glGenTextures(1, &g_gpu_upload_texture);
    glGenTextures(1, &g_gpu_shift_texture);
    glGenFramebuffers(1, &g_gpu_shift_fbo);
    return 1;
}

static void hmje_gles_texture_storage(GLuint texture, u64 width, u64 height) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static int hmje_gles_ensure_targets(u64 width, u64 height) {
    if (g_gpu_root_width == width && g_gpu_root_height == height && g_gpu_target_texture[0]) return 1;
    if (!g_gpu_target_texture[0]) {
        glGenTextures(2, g_gpu_target_texture);
        glGenFramebuffers(2, g_gpu_target_fbo);
        glGenTextures(1, &g_gpu_work_texture);
        glGenFramebuffers(1, &g_gpu_work_fbo);
    }
    for (int i = 0; i < 2; ++i) {
        hmje_gles_texture_storage(g_gpu_target_texture[i], width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_target_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gpu_target_texture[i], 0);
    }
    hmje_gles_texture_storage(g_gpu_work_texture, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_work_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gpu_work_texture, 0);
    g_gpu_root_width = width; g_gpu_root_height = height;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void hmje_gles_common_state(void) {
    glBindVertexArray(g_gpu_vao);
    glViewport(0, 0, (GLsizei)g_gpu_width, (GLsizei)g_gpu_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)g_gpu_width, (GLsizei)g_gpu_height);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDisable(GL_DITHER);
}

static void hmje_gles_uniform_rect(GLuint program, float x, float y, float w, float h) {
    glUniform4f(glGetUniformLocation(program, "uRect"), x, y, w, h);
    glUniform2f(glGetUniformLocation(program, "uViewport"), (float)g_gpu_width, (float)g_gpu_height);
}

static void hmje_gles_color(GLuint program, u64 packed, const char *name) {
    float a = (float)((packed >> 24) & 255u) / 255.f;
    float r = (float)((packed >> 16) & 255u) / 255.f;
    float g = (float)((packed >> 8) & 255u) / 255.f;
    float b = (float)(packed & 255u) / 255.f;
    glUniform4f(glGetUniformLocation(program, name), r, g, b, a);
}

static i64 hmje_android_gpu_abort(void) {
    g_gpu_active = 0;
    g_gpu_layer_depth = 0;
    return 1;
}

static i64 hmje_android_gpu_begin(const u64 *r, u64 length) {
    if (length != 24 || !r || r[0] == 0 || r[1] == 0 || r[2] > 1 || g_gpu_active) return HMJE_ANDROID_ENOSYS;
    if (!init_gles_presenter() || !eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context) ||
        !hmje_gles_prepare_programs() || !hmje_gles_ensure_targets(r[0], r[1])) return HMJE_ANDROID_ENOSYS;
    g_gpu_target = (int)r[2]; g_gpu_width = r[0]; g_gpu_height = r[1]; g_gpu_layer_depth = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gpu_target_fbo[g_gpu_target]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gpu_work_fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)g_gpu_width, (GLint)g_gpu_height, 0, 0, (GLint)g_gpu_width, (GLint)g_gpu_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_work_fbo);
    hmje_gles_common_state();
    g_gpu_active = 1;
    ++g_gpu_transaction_count;
    if (g_gpu_transaction_count <= 4) LOGI("direct GLES BEGIN target=%llu size=%llux%llu", (unsigned long long)r[2], (unsigned long long)r[0], (unsigned long long)r[1]);
    if (glGetError() != GL_NO_ERROR) {
        hmje_android_gpu_abort();
        return HMJE_ANDROID_ENOSYS;
    }
    return 1;
}

static i64 hmje_android_gpu_clip(const u64 *r, u64 length) {
    if (length != 32 || !r || !g_gpu_active || r[2] < r[0] || r[3] < r[1]) return HMJE_ANDROID_ENOSYS;
    u64 right = r[2] > g_gpu_width ? g_gpu_width : r[2];
    u64 bottom = r[3] > g_gpu_height ? g_gpu_height : r[3];
    u64 left = r[0] > right ? right : r[0];
    u64 top = r[1] > bottom ? bottom : r[1];
    glScissor((GLint)left, (GLint)(g_gpu_height - bottom), (GLsizei)(right - left), (GLsizei)(bottom - top));
    return 1;
}

static i64 hmje_android_gpu_upload_glyph(const u64 *r, u64 length) {
    if (length != 32 || !r || !g_gpu_active || !r[0] || !r[1] || !r[2] || r[3] < r[1] ||
        r[1] > INT_MAX || r[2] > INT_MAX || r[3] > INT_MAX || r[3] > SIZE_MAX / r[2])
        return HMJE_ANDROID_ENOSYS;
    int slot = hmje_glyph_cache_get(&g_android_glyph_cache,
        (const unsigned char *)(uintptr_t)r[0], r[1], r[2], r[3], hmje_android_glyph_release);
    if (slot >= 0 && g_gpu_glyph_textures[slot]) {
        g_gpu_glyph_texture = g_gpu_glyph_textures[slot];
        return 1;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(slot >= 0 ? r[1] : r[3]));
    const void *bytes = slot >= 0 ? g_android_glyph_cache.entries[slot].bytes : (const void *)(uintptr_t)r[0];
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)r[1], (GLsizei)r[2], 0, GL_RED, GL_UNSIGNED_BYTE, bytes);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (!texture || glGetError() != GL_NO_ERROR) {
        if (texture) glDeleteTextures(1, &texture);
        if (slot >= 0) hmje_glyph_cache_drop(&g_android_glyph_cache, (unsigned)slot, hmje_android_glyph_release);
        return HMJE_ANDROID_ENOSYS;
    }
    if (slot >= 0) g_gpu_glyph_textures[slot] = texture;
    else {
        if (g_gpu_glyph_transient) glDeleteTextures(1, &g_gpu_glyph_transient);
        g_gpu_glyph_transient = texture;
    }
    g_gpu_glyph_texture = texture;
    return 1;
}

static i64 hmje_android_gpu_draw(const u64 *r, u64 length) {
    if (length != 104 || !r || !g_gpu_active || r[0] > 5) return HMJE_ANDROID_ENOSYS;
    float x = (float)(i64)r[1], y = (float)(i64)r[2], w = (float)r[3], h = (float)r[4];
    if (!g_gpu_logged_first_draw) {
        LOGI("direct GLES first DRAW kind=%llu rect=%lld,%lld %llux%llu color=0x%08llx",
             (unsigned long long)r[0], (long long)(i64)r[1], (long long)(i64)r[2],
             (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[8]);
        g_gpu_logged_first_draw = 1;
    }
    float shadow_x = (float)(i64)r[9], shadow_y = (float)(i64)r[10], shadow_w = (float)r[11], shadow_h = (float)r[12];
    if (r[0] == 3) {
        float end_x=(float)(i64)r[3], end_y=(float)(i64)r[4], half=(float)r[7]*.5f+1.f;
        float left=fminf(x,end_x)-half, top=fminf(y,end_y)-half;
        w=fabsf(end_x-x)+2.f*half; h=fabsf(end_y-y)+2.f*half;
        shadow_x=x; shadow_y=y; shadow_w=end_x; shadow_h=end_y; x=left; y=top;
    }
    glUseProgram(g_gpu_shape_program);
    hmje_gles_uniform_rect(g_gpu_shape_program, x, y, w, h);
    hmje_gles_color(g_gpu_shape_program, r[8], "uColor");
    u64 outer=r[5],inner=r[6];
    if(r[0]==1||r[0]==2) {
        if(outer>r[3]/2)outer=r[3]/2;if(outer>r[4]/2)outer=r[4]/2;
        if(r[0]==2) {
            u64 iw=r[7]<=r[3]/2?r[3]-2*r[7]:0,ih=r[7]<=r[4]/2?r[4]-2*r[7]:0;
            if(inner>iw/2)inner=iw/2;if(inner>ih/2)inner=ih/2;
        }
    }
    glUniform4f(glGetUniformLocation(g_gpu_shape_program, "uParams"), (float)r[0], (float)outer, (float)inner, (float)r[7]);
    glUniform4f(glGetUniformLocation(g_gpu_shape_program, "uShadow"), shadow_x, shadow_y, shadow_w, shadow_h);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_gpu_glyph_texture);
    glUniform1i(glGetUniformLocation(g_gpu_shape_program, "uGlyph"), 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR ? 1 : HMJE_ANDROID_ENOSYS;
}

static i64 hmje_android_gpu_texture_draw_mode(GLuint texture, int swizzle, int flip_y, float sx, float sy, float sw, float sh,
                                         float source_width, float source_height, float dx, float dy, float dw, float dh,
                                         float alpha, float radius, int source_alpha) {
    glUseProgram(g_gpu_texture_program);
    hmje_gles_uniform_rect(g_gpu_texture_program, dx, dy, dw, dh);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program, "uTexture"), 0);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program, "uSwizzle"), swizzle);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program, "uFlipY"), flip_y);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program, "uForceOpaque"), 0);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program, "uSourceAlpha"), source_alpha);
    glUniform1f(glGetUniformLocation(g_gpu_texture_program, "uAlpha"), alpha);
    glUniform1f(glGetUniformLocation(g_gpu_texture_program, "uRadius"), radius);
    glUniform4f(glGetUniformLocation(g_gpu_texture_program, "uUV"), sx/source_width, sy/source_height, (sx+sw)/source_width, (sy+sh)/source_height);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR ? 1 : HMJE_ANDROID_ENOSYS;
}

/* Existing canvas/layer/XRGB callers intentionally ignore the stored alpha byte. */
static i64 hmje_android_gpu_texture_draw(GLuint texture, int swizzle, int flip_y, float sx, float sy, float sw, float sh,
                                         float source_width, float source_height, float dx, float dy, float dw, float dh,
                                         float alpha, float radius) {
    return hmje_android_gpu_texture_draw_mode(texture,swizzle,flip_y,sx,sy,sw,sh,source_width,source_height,dx,dy,dw,dh,alpha,radius,0);
}

static i64 hmje_android_gpu_image(const u64 *r, u64 length, int scaled) {
    if (!r || !g_gpu_active || (scaled ? (length != 104 && length != 112) : length != 80)) return HMJE_ANDROID_ENOSYS;
    int source_alpha = scaled && length == 112;
    if ((source_alpha && r[13] != 1) || !r[0] || !r[1] || !r[2] || r[1] > INT32_MAX || r[2] > INT32_MAX ||
        r[1] > UINT64_MAX/4 || r[3] < r[1]*4 || r[3]%4 || r[3]/4 > INT32_MAX || r[3] > UINT64_MAX/r[2]) return HMJE_ANDROID_ENOSYS;
    if (r[4] >= r[1] || r[5] >= r[2]) return 1;
    u64 copy_width=r[6], copy_height=r[7];
    if (copy_width > r[1]-r[4]) copy_width=r[1]-r[4];
    if (copy_height > r[2]-r[5]) copy_height=r[2]-r[5];
    if (!copy_width || !copy_height || (scaled && (!r[10] || !r[11]))) return 1;
    glBindTexture(GL_TEXTURE_2D, g_gpu_upload_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(r[3] / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)r[1], (GLsizei)r[2], 0, GL_RGBA, GL_UNSIGNED_BYTE, (const void *)(uintptr_t)r[0]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    u64 radius=scaled?r[12]:0;
    if (scaled && radius > r[10]/2) radius=r[10]/2;
    if (scaled && radius > r[11]/2) radius=r[11]/2;
    float dw = (float)(scaled ? r[10] : copy_width), dh = (float)(scaled ? r[11] : copy_height);
    return hmje_android_gpu_texture_draw_mode(g_gpu_upload_texture, 1, 0, (float)r[4], (float)r[5], (float)copy_width, (float)copy_height,
                                         (float)r[1], (float)r[2], (float)(i64)r[8], (float)(i64)r[9], dw, dh, 1.f, (float)radius, source_alpha);
}

static i64 hmje_android_gpu_composite(const u64 *r, u64 length) {
    if (length != 56 || !r || !g_gpu_active || r[0] > 1 || (int)r[0] == g_gpu_target) return HMJE_ANDROID_ENOSYS;
    /* Damage is rounded in logical pixels before the device scale is applied.
     * Its last row can therefore exceed a physical surface (e.g. 716*3 > 2146).
     * Match the CPU/Metal blit contract: crop the source and let viewport plus
     * scissor clip the destination, preserving the original 1:1 mapping. */
    if (r[1] >= g_gpu_root_width || r[2] >= g_gpu_root_height) return 1;
    u64 width = r[3], height = r[4];
    if (width > g_gpu_root_width - r[1]) width = g_gpu_root_width - r[1];
    if (height > g_gpu_root_height - r[2]) height = g_gpu_root_height - r[2];
    if (!width || !height) return 1;
    /* Persistent targets are GL render textures (bottom-left storage), while
     * the HMJE ABI names source rectangles from the top-left.  Whole-target
     * composites hid this distinction; partial scroll damage sampled the
     * vertically mirrored band and resurrected unrelated rows. */
    float source_y = (float)(g_gpu_root_height - r[2] - height);
    return hmje_android_gpu_texture_draw(g_gpu_target_texture[r[0]], 0, 1, (float)r[1], source_y, (float)width, (float)height,
                                         (float)g_gpu_root_width, (float)g_gpu_root_height, (float)(i64)r[5], (float)(i64)r[6], (float)width, (float)height, 1.f, 0.f);
}

static i64 hmje_android_gpu_gradient(const u64 *r, u64 length) {
    if (length != 120 || !r || !g_gpu_active || r[6] < 2 || r[6] > 8) return HMJE_ANDROID_ENOSYS;
    glUseProgram(g_gpu_gradient_program);
    hmje_gles_uniform_rect(g_gpu_gradient_program, (float)(i64)r[0], (float)(i64)r[1], (float)r[2], (float)r[3]);
    u64 radius=r[4];if(radius>r[2]/2)radius=r[2]/2;if(radius>r[3]/2)radius=r[3]/2;
    glUniform1f(glGetUniformLocation(g_gpu_gradient_program, "uRadius"), (float)radius);
    glUniform1i(glGetUniformLocation(g_gpu_gradient_program, "uDirection"), (GLint)r[5]);
    glUniform1i(glGetUniformLocation(g_gpu_gradient_program, "uCount"), (GLint)r[6]);
    for (int i = 0; i < 8; ++i) { char name[24]; snprintf(name, sizeof(name), "uColors[%d]", i); hmje_gles_color(g_gpu_gradient_program, r[7+i], name); }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return glGetError() == GL_NO_ERROR ? 1 : HMJE_ANDROID_ENOSYS;
}

#include <limits.h>
#include "android_gles_blur.inc"

static i64 hmje_android_gpu_shift(const u64 *r, u64 length, int planar) {
    if (length != (planar ? 64u : 48u) || !r || !g_gpu_active || !r[2] || !r[3]) return HMJE_ANDROID_ENOSYS;
    i64 signed_x = (i64)r[0], signed_y = (i64)r[1];
    u64 width = r[2], height = r[3];
    u64 dx = planar ? r[4] : 0, dy = planar ? r[5] : r[4];
    int right = planar ? (r[6] != 0) : 0, down = planar ? (r[7] != 0) : (r[5] != 0);

    /* Match the CPU and Metal contract exactly: clip first, then move only the
     * overlapping pixels.  Moving the whole rectangle and relying on clipping
     * duplicates stale rows in the newly exposed scroll band. */
    if (signed_x < 0) {
        u64 amount = (u64)(-(signed_x + 1)) + 1;
        if (amount >= width) return 1;
        width -= amount;
        signed_x = 0;
    }
    if (signed_y < 0) {
        u64 amount = (u64)(-(signed_y + 1)) + 1;
        if (amount >= height) return 1;
        height -= amount;
        signed_y = 0;
    }
    u64 x = (u64)signed_x, y = (u64)signed_y;
    if (x >= g_gpu_width || y >= g_gpu_height || width == 0 || height == 0) return 1;
    if (width > g_gpu_width - x) width = g_gpu_width - x;
    if (height > g_gpu_height - y) height = g_gpu_height - y;
    if ((!planar && dy == 0) || (planar && dx == 0 && dy == 0) || dx >= width || dy >= height) return 1;

    u64 copy_width = width - dx, copy_height = height - dy;
    u64 source_x = right ? x : x + dx;
    u64 destination_x = right ? x + dx : x;
    u64 source_y = down ? y : y + dy;
    u64 destination_y = down ? y + dy : y;

    GLint active_fbo = 0;
    GLint saved_scissor[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &active_fbo);
    glGetIntegerv(GL_SCISSOR_BOX, saved_scissor);
    hmje_gles_texture_storage(g_gpu_shift_texture, copy_width, copy_height);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_shift_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gpu_shift_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)active_fbo);
        return HMJE_ANDROID_ENOSYS;
    }

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)active_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gpu_shift_fbo);
    glBlitFramebuffer((GLint)source_x, (GLint)(g_gpu_height - source_y - copy_height),
                      (GLint)(source_x + copy_width), (GLint)(g_gpu_height - source_y),
                      0, 0, (GLint)copy_width, (GLint)copy_height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gpu_shift_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)active_fbo);
    glBlitFramebuffer(0, 0, (GLint)copy_width, (GLint)copy_height,
                      (GLint)destination_x, (GLint)(g_gpu_height - destination_y - copy_height),
                      (GLint)(destination_x + copy_width), (GLint)(g_gpu_height - destination_y),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)active_fbo);
    hmje_gles_common_state();
    glScissor(saved_scissor[0], saved_scissor[1], saved_scissor[2], saved_scissor[3]);
    return glGetError() == GL_NO_ERROR ? 1 : HMJE_ANDROID_ENOSYS;
}

static i64 hmje_android_gpu_layer_begin(const u64 *r, u64 length) {
    if (length != 40 || !r || !g_gpu_active || g_gpu_layer_depth >= HMJE_GLES_LAYER_MAX || !r[2] || !r[3]) return HMJE_ANDROID_ENOSYS;
    int slot = g_gpu_layer_depth++;
    if (!g_gpu_layer_texture[slot]) { glGenTextures(1, &g_gpu_layer_texture[slot]); glGenFramebuffers(1, &g_gpu_layer_fbo[slot]); }
    hmje_gles_texture_storage(g_gpu_layer_texture[slot], r[2], r[3]);
    GLint current = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_layer_fbo[slot]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gpu_layer_texture[slot], 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)current); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gpu_layer_fbo[slot]); glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer((GLint)r[0], (GLint)(g_gpu_height-r[1]-r[3]), (GLint)(r[0]+r[2]), (GLint)(g_gpu_height-r[1]), 0, 0, (GLint)r[2], (GLint)r[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_gpu_layer_saved_fbo[slot]=(GLuint)current; g_gpu_layer_x[slot]=r[0]; g_gpu_layer_y[slot]=r[1]; g_gpu_layer_w[slot]=r[2]; g_gpu_layer_h[slot]=r[3]; g_gpu_layer_alpha[slot]=r[4];
    g_gpu_layer_saved_width[slot]=g_gpu_width; g_gpu_layer_saved_height[slot]=g_gpu_height; g_gpu_width=r[2]; g_gpu_height=r[3];
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_layer_fbo[slot]); hmje_gles_common_state();
    return 1;
}

static i64 hmje_android_gpu_layer_end(u64 length) {
    if (length != 0 || !g_gpu_active || g_gpu_layer_depth <= 0) return HMJE_ANDROID_ENOSYS;
    int slot = --g_gpu_layer_depth; g_gpu_width=g_gpu_layer_saved_width[slot]; g_gpu_height=g_gpu_layer_saved_height[slot];
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_layer_saved_fbo[slot]); hmje_gles_common_state();
    return hmje_android_gpu_texture_draw(g_gpu_layer_texture[slot], 0, 1, 0, 0, (float)g_gpu_layer_w[slot], (float)g_gpu_layer_h[slot],
                                         (float)g_gpu_layer_w[slot], (float)g_gpu_layer_h[slot], (float)g_gpu_layer_x[slot], (float)g_gpu_layer_y[slot],
                                         (float)g_gpu_layer_w[slot], (float)g_gpu_layer_h[slot], (float)(g_gpu_layer_alpha[slot] > 255 ? 255 : g_gpu_layer_alpha[slot])/255.f, 0.f);
}

/* A native window resize need not reach the EGL drawable until swap.  Drawing
 * only the new logical viewport into the old, taller buffer leaves old pixels
 * at its top, then SurfaceFlinger scales that mixed frame into the new window.
 * Always fill the actual drawable and, if swap changes it, redraw the retained
 * complete frame before going idle.  No UI commands or glyphs are replayed. */
static int hmje_gles_present_frame(int (*draw)(GLsizei, GLsizei), u64 width, u64 height) {
    unsigned resize_retries = 0;
retry_present:
    if (!init_gles_presenter() ||
        !eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) return 0;
    EGLint before_width = 0, before_height = 0, after_width = 0, after_height = 0;
    if (!eglQuerySurface(g_egl_display, g_egl_surface, EGL_WIDTH, &before_width) ||
        !eglQuerySurface(g_egl_display, g_egl_surface, EGL_HEIGHT, &before_height) ||
        before_width <= 0 || before_height <= 0) goto failed;
    if (!draw((GLsizei)before_width, (GLsizei)before_height)) return 0;
    if (!eglSwapBuffers(g_egl_display, g_egl_surface)) goto failed;
    if (!eglQuerySurface(g_egl_display, g_egl_surface, EGL_WIDTH, &after_width) ||
        !eglQuerySurface(g_egl_display, g_egl_surface, EGL_HEIGHT, &after_height)) goto failed;
    if (before_width == after_width && before_height == after_height &&
        (u64)after_width == width && (u64)after_height == height) return 1;
    LOGI("EGL drawable resize: frame=%llux%llu before=%dx%d after=%dx%d retry=%u",
         width, height, before_width, before_height, after_width, after_height, resize_retries);
    /* Continuous resize must not monopolize the render thread.  The published
     * geometry is consumed on the next input poll; even a same-size event must
     * wake the UI if the drawable has not converged in these three swaps. */
    if (++resize_retries < 3) goto retry_present;
    hmje_android_request_surface_redraw();
    return 1;
failed: {
    EGLint error = eglGetError();
    if ((error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW) && surface_replacement_pending()) goto retry_present;
    LOGI("GLES frame failed egl=0x%x", error);
    return 0;
}
}

static int hmje_gles_draw_retained_frame(GLsizei drawable_width, GLsizei drawable_height) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0,0,drawable_width,drawable_height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glUseProgram(g_gpu_texture_program); g_gpu_width=g_gpu_root_width; g_gpu_height=g_gpu_root_height;
    /* The quad's coverage is measured in drawable pixels, too: scaling the
     * old logical rectangle would otherwise leave translucent outer rows. */
    glUniform4f(glGetUniformLocation(g_gpu_texture_program,"uRect"),0,0,(float)drawable_width,(float)drawable_height);
    glUniform2f(glGetUniformLocation(g_gpu_texture_program,"uViewport"),(float)drawable_width,(float)drawable_height);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g_gpu_target_texture[0]);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program,"uTexture"),0); glUniform1i(glGetUniformLocation(g_gpu_texture_program,"uSwizzle"),0);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program,"uFlipY"),1);
    glUniform1i(glGetUniformLocation(g_gpu_texture_program,"uForceOpaque"),1);
    glUniform1f(glGetUniformLocation(g_gpu_texture_program,"uRadius"),0.f);
    glUniform1f(glGetUniformLocation(g_gpu_texture_program,"uAlpha"),1.f); glUniform4f(glGetUniformLocation(g_gpu_texture_program,"uUV"),0,0,1,1);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    return glGetError()==GL_NO_ERROR;
}

static i64 hmje_android_gpu_end(u64 length) {
    if (length != 0 || !g_gpu_active || g_gpu_layer_depth != 0) return HMJE_ANDROID_ENOSYS;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gpu_work_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gpu_target_fbo[g_gpu_target]);
    glBlitFramebuffer(0,0,(GLint)g_gpu_root_width,(GLint)g_gpu_root_height,0,0,(GLint)g_gpu_root_width,(GLint)g_gpu_root_height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    if (g_gpu_target == 0 && !hmje_gles_present_frame(hmje_gles_draw_retained_frame, g_gpu_root_width, g_gpu_root_height)) {
        g_gpu_active=0; return HMJE_ANDROID_ENOSYS;
    }
    if (g_gpu_transaction_count <= 4) LOGI("direct GLES END target=%d gl=0x%x", g_gpu_target, glGetError());
    g_gpu_active=0;
    return 1;
}

static i64 hmje_android_gpu_dispatch(i64 kind, i64 request_address, i64 request_length) {
    const u64 *r=(const u64 *)(uintptr_t)(u64)request_address;
    u64 length=request_length<0?0:(u64)request_length;
    switch(kind) {
    case 0:
        if (length != 32 || !r) return HMJE_ANDROID_ENOSYS;
        ((u64 *)r)[0]=2; ((u64 *)r)[1]=2;
        {
            int ready=init_gles_presenter()&&hmje_gles_prepare_programs();
            int blur=ready&&hmje_gles_blur_available();
            ((u64 *)r)[2]=ready?(UINT64_C(0xfffe)&(blur?~UINT64_C(0):~(UINT64_C(1)<<10))):0;
            ((u64 *)r)[3]=ready&&blur?0:1;
            return 32;
        }
    case HMJE_GLES_GPU_BEGIN:return hmje_android_gpu_begin(r,length);
    case HMJE_GLES_GPU_CLIP:return hmje_android_gpu_clip(r,length);
    case HMJE_GLES_GPU_UPLOAD_GLYPH:return hmje_android_gpu_upload_glyph(r,length);
    case HMJE_GLES_GPU_DRAW:return hmje_android_gpu_draw(r,length);
    case HMJE_GLES_GPU_IMAGE_BLIT:return hmje_android_gpu_image(r,length,0);
    case HMJE_GLES_GPU_IMAGE_SCALE_BLIT:return hmje_android_gpu_image(r,length,1);
    case HMJE_GLES_GPU_COMPOSITE_TARGET:return hmje_android_gpu_composite(r,length);
    case HMJE_GLES_GPU_BACKDROP_BLUR:return hmje_android_gpu_blur(r,length);
    case HMJE_GLES_GPU_GRADIENT_FILL:return hmje_android_gpu_gradient(r,length);
    case HMJE_GLES_GPU_SCROLL_SHIFT:return hmje_android_gpu_shift(r,length,0);
    case HMJE_GLES_GPU_PLANAR_SHIFT:return hmje_android_gpu_shift(r,length,1);
    case HMJE_GLES_GPU_LAYER_BEGIN:return hmje_android_gpu_layer_begin(r,length);
    case HMJE_GLES_GPU_LAYER_END:return hmje_android_gpu_layer_end(length);
    case HMJE_GLES_GPU_END_PRESENT:return hmje_android_gpu_end(length);
    case HMJE_GLES_GPU_ABORT:return length==0?hmje_android_gpu_abort():HMJE_ANDROID_ENOSYS;
    default:return HMJE_ANDROID_ENOSYS;
    }
}

/* Report the rejected normalized operation so a hosted display failure is actionable. */
i64 hmje_android_gpu_command(i64 kind, i64 request_address, i64 request_length) {
    i64 result = hmje_android_gpu_dispatch(kind, request_address, request_length);
    if (result < 0) {
        LOGI("direct GLES command failed kind=%lld length=%lld active=%d target=%d layers=%d",
             (long long)kind, (long long)request_length, g_gpu_active, g_gpu_target, g_gpu_layer_depth);
    }
    return result;
}
