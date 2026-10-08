/* Shared macOS/iOS GPU blur: the CPU compositor's numerical contract. */
/* 배경블러(GPU_COMMAND_BACKDROP_BLUR) 스크래치. RGBA32Float 두 장을 grow-only로
 * 재사용한다(다운샘플 결과/분리형 블러 핑퐁) — 중간 단계를 8-bit로 양자화하지
 * 않아야 CPU 기준(core.hmje, 실수64 파이프라인)과의 차이가 최종 양자화 1회로
 * 수렴한다. */
static id<MTLTexture> g_gpu_blur_scratch_a = nil;
static id<MTLTexture> g_gpu_blur_scratch_b = nil;
static id<MTLRenderPipelineState> g_gpu_blur_down_pipeline = nil;
static id<MTLRenderPipelineState> g_gpu_blur_conv_pipeline = nil;
static id<MTLRenderPipelineState> g_gpu_blur_composite_pipeline = nil;

#define HMJE_GPU_BACKDROP_BLUR 10

/* GPU_COMMAND_BACKDROP_BLUR: glassmorphism. 현재 활성 target 텍스처의 영역을
 * 읽어 core.hmje 캔버스배경블러그리기의 CPU 기준 사양을 그대로 재현한다:
 *   다운샘플(s=1/2/4, 블록 평균) → 박스 필터(길이 2r'+1)
 *   가로 3패스 + 세로 3패스 (각 패스 경계 클램프) → 픽셀 중심 이중선형 업샘플 → 틴트 over 블러 →
 *   rounded-box SDF 하드 엣지 커버리지를 src 알파로 고정기능 블렌드 합성.
 * 셰이더/파이프라인은 기존 hmje_gpu_msl과 독립적으로 지연 생성한다(기존
 * 경로 무접촉). 페이로드(56B): [x, y, w, h, cornerRadius, blurRadius, tint]. */
static const char *hmje_gpu_blur_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct BU{float2 clipOrigin;float2 clipSize;float2 maskOrigin;float2 maskSize;float2 viewport;float2 grid;float2 misc;float2 pad;float4 params;float4 tint;};\n"
"struct O{float4 position [[position]];};\n"
"float bsd(float2 l,float2 s,float r){float2 h=s*.5;float2 q=abs(l-h)-h+r;return length(max(q,float2(0)))+min(max(q.x,q.y),0.)-r;}\n"
"uint bcv(float2 l,float2 s,float r){return uint(clamp(.5-bsd(l,s,r),0.,1.)*255.);}\n"
"vertex O bqvs(uint i [[vertex_id]],constant BU&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=q*u.grid;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
"vertex O bcvs(uint i [[vertex_id]],constant BU&u [[buffer(0)]]){float2 q=float2(float(i&1u),float((i>>1u)&1u));float2 p=u.clipOrigin+q*u.clipSize;O o;o.position=float4(p.x/u.viewport.x*2.-1.,1.-p.y/u.viewport.y*2.,0.,1.);return o;}\n"
/* 다운샘플: 잘린 영역의 s×s 블록 평균(경계 블록은 실제 덮인 표본 수로 나눔). */
"fragment float4 bpd(O i [[stage_in]],constant BU&u [[buffer(0)]],texture2d<float,access::read> src [[texture(0)]]){\n"
"int2 g=int2(i.position.xy);int s=int(u.misc.x+.5);int cw=int(u.clipSize.x+.5);int ch=int(u.clipSize.y+.5);\n"
"float3 acc=float3(0);int cnt=0;\n"
"for(int py=0;py<s;++py){for(int px=0;px<s;++px){int lx=g.x*s+px;int ly=g.y*s+py;\n"
"if(lx<cw&&ly<ch){uint2 tc=uint2(int2(u.clipOrigin)+int2(lx,ly));acc+=src.read(tc).rgb;cnt+=1;}}}\n"
"if(cnt==0)return float4(0,0,0,1);return float4(acc/float(cnt),1);}\n"
/* 분리형 컨볼루션: params.y=0 가로 / 1 세로, buffer(1)=정규화 커널. */
"fragment float4 bpb(O i [[stage_in]],constant BU&u [[buffer(0)]],texture2d<float,access::read> src [[texture(0)]],constant float*kw [[buffer(1)]]){\n"
"int2 g=int2(i.position.xy);int n=int(u.params.z+.5);int c=int(u.misc.y+.5);bool horiz=u.params.y<.5;\n"
"int limit=horiz?(int(u.grid.x+.5)-1):(int(u.grid.y+.5)-1);\n"
"float3 acc=float3(0);\n"
"for(int k=0;k<n;++k){int pos=(horiz?g.x:g.y)+k-c;pos=clamp(pos,0,limit);\n"
"uint2 tc=horiz?uint2(pos,g.y):uint2(g.x,pos);acc+=src.read(tc).rgb*kw[k];}\n"
"return float4(acc,1);}\n"
/* 합성: 픽셀 중심 이중선형 업샘플 + 틴트 over 블러, rounded-box 커버리지를 알파로. */
"fragment float4 bpc(O i [[stage_in]],constant BU&u [[buffer(0)]],texture2d<float,access::read> blurred [[texture(0)]]){\n"
"float2 lm=i.position.xy-u.maskOrigin;\n"
"float2 lc=i.position.xy-u.clipOrigin;\n"
"float s=u.misc.x;int gw=int(u.grid.x+.5);int gh=int(u.grid.y+.5);\n"
"float ux=lc.x/s-0.5;float uy=lc.y/s-0.5;\n"
"float fu=floor(ux);float fv=floor(uy);float fx=ux-fu;float fy=uy-fv;\n"
"int x0=clamp(int(fu),0,gw-1);int x1=clamp(int(fu)+1,0,gw-1);\n"
"int y0=clamp(int(fv),0,gh-1);int y1=clamp(int(fv)+1,0,gh-1);\n"
"float3 b=blurred.read(uint2(x0,y0)).rgb*(1.-fx)*(1.-fy)+blurred.read(uint2(x1,y0)).rgb*fx*(1.-fy)+blurred.read(uint2(x0,y1)).rgb*(1.-fx)*fy+blurred.read(uint2(x1,y1)).rgb*fx*fy;\n"
"float3 outc=u.tint.rgb*u.tint.a+b*(1.-u.tint.a);\n"
"uint covb=bcv(lm,u.maskSize,u.params.x);\n"
"return float4(outc,float(covb)/255.);}\n";

typedef struct {
    float clipOrigin[2];
    float clipSize[2];
    float maskOrigin[2];
    float maskSize[2];
    float viewport[2];
    float grid[2];
    float misc[2];
    float pad[2];
    float params[4];
    float tint[4];
} HmjeGpuBlurUniforms;

static BOOL hmje_gpu_prepare_blur_pipelines(void) {
    if (g_gpu_blur_down_pipeline != nil && g_gpu_blur_conv_pipeline != nil &&
        g_gpu_blur_composite_pipeline != nil) return YES;
    NSError *error = nil;
    id<MTLLibrary> library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:hmje_gpu_blur_msl]
                                                    options:nil error:&error];
    if (library == nil) {
        fprintf(stderr, "posix-host-gpu: blur shader compile failed: %s\n",
                error.localizedDescription.UTF8String);
        return NO;
    }
    /* RGBA32Float 스크래치 패스: 블렌드 없음(부동소수 attachment 블렌드는 기기별
     * 지원이 갈리고, 어차피 전체 덮어쓰기라 불필요). */
    MTLRenderPipelineDescriptor *description = [MTLRenderPipelineDescriptor new];
    description.vertexFunction = [library newFunctionWithName:@"bqvs"];
    description.fragmentFunction = [library newFunctionWithName:@"bpd"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    description.colorAttachments[0].blendingEnabled = NO;
    g_gpu_blur_down_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_gpu_blur_down_pipeline == nil) return NO;
    description.fragmentFunction = [library newFunctionWithName:@"bpb"];
    g_gpu_blur_conv_pipeline = [g_device newRenderPipelineStateWithDescriptor:description error:&error];
    if (g_gpu_blur_conv_pipeline == nil) return NO;
    /* 합성 패스는 기존 도형 파이프라인과 같은 고정기능 블렌드(BGRA target). */
    MTLRenderPipelineDescriptor *composite = [MTLRenderPipelineDescriptor new];
    composite.vertexFunction = [library newFunctionWithName:@"bcvs"];
    composite.fragmentFunction = [library newFunctionWithName:@"bpc"];
    composite.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    composite.colorAttachments[0].blendingEnabled = YES;
    composite.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    composite.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    composite.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    composite.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    composite.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    composite.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
    g_gpu_blur_composite_pipeline = [g_device newRenderPipelineStateWithDescriptor:composite error:&error];
    return g_gpu_blur_composite_pipeline != nil;
}

static BOOL hmje_gpu_blur_scratch_ensure(u64 gridWidth, u64 gridHeight) {
    u64 wantWidth = gridWidth, wantHeight = gridHeight;
    if (g_gpu_blur_scratch_a != nil) {
        wantWidth = MAX(wantWidth, (u64)g_gpu_blur_scratch_a.width);
        wantHeight = MAX(wantHeight, (u64)g_gpu_blur_scratch_a.height);
    }
    if (g_gpu_blur_scratch_a == nil || (u64)g_gpu_blur_scratch_a.width < gridWidth ||
        (u64)g_gpu_blur_scratch_a.height < gridHeight) {
        MTLTextureDescriptor *description = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                          width:(NSUInteger)wantWidth
                                         height:(NSUInteger)wantHeight
                                      mipmapped:NO];
        description.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModePrivate;
        g_gpu_blur_scratch_a = [g_device newTextureWithDescriptor:description];
        g_gpu_blur_scratch_b = [g_device newTextureWithDescriptor:description];
    }
    return g_gpu_blur_scratch_a != nil && g_gpu_blur_scratch_b != nil;
}

static i64 hmje_gpu_backdrop_blur(const u64 *r, u64 length) {
    id<MTLTexture> target = hmje_gpu_active_target();
    if (length != 56 || r == NULL || g_gpu_command_buffer == nil || target == nil)
        return HMJE_POSIX_ENOSYS_LOCAL;
    if (!hmje_gpu_prepare_blur_pipelines()) return HMJE_POSIX_ENOSYS_LOCAL;
    i64 regionX = (i64)r[0], regionY = (i64)r[1];
    u64 regionWidth = r[2], regionHeight = r[3];
    u64 cornerRadius = r[4], blurRadius = r[5];
    uint32_t tint = (uint32_t)r[6];
    if (regionWidth == 0 || regionHeight == 0) return 1;
    /* Match the representable region domain of the other native backends.
     * Check before converting extents or adding signed endpoints. */
    if (regionX < INT32_MIN || regionX > INT32_MAX ||
        regionY < INT32_MIN || regionY > INT32_MAX ||
        regionWidth > INT32_MAX || regionHeight > INT32_MAX)
        return HMJE_POSIX_ENOSYS_LOCAL;
    /* 모서리 반경 클램프(CPU와 동일 규칙). */
    u64 radius = cornerRadius;
    if (radius > regionWidth / 2) radius = regionWidth / 2;
    if (radius > regionHeight / 2) radius = regionHeight / 2;
    /* 영역 ∩ target ∩ 현재 scissor(클립) — CPU의 잘린영역과 동일. */
    i64 left = regionX, top = regionY;
    i64 right = regionX + (i64)regionWidth, bottom = regionY + (i64)regionHeight;
    if (left < (i64)g_gpu_clip.x) left = (i64)g_gpu_clip.x;
    if (top < (i64)g_gpu_clip.y) top = (i64)g_gpu_clip.y;
    if (right > (i64)(g_gpu_clip.x + g_gpu_clip.width)) right = (i64)(g_gpu_clip.x + g_gpu_clip.width);
    if (bottom > (i64)(g_gpu_clip.y + g_gpu_clip.height)) bottom = (i64)(g_gpu_clip.y + g_gpu_clip.height);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (i64)target.width) right = (i64)target.width;
    if (bottom > (i64)target.height) bottom = (i64)target.height;
    if (right <= left || bottom <= top) return 1;
    u64 clipWidth = (u64)(right - left), clipHeight = (u64)(bottom - top);

    /* 다운샘플 배율/박스반경 — core.hmje 배경블러다운배율과 동일. */
    u64 scale = blurRadius <= 8 ? 1 : (blurRadius <= 16 ? 2 : 4);
    u64 boxRadius = blurRadius / scale;
    /* Bound before multiplication and before allocating scratch. */
    if (boxRadius > 511) return HMJE_POSIX_ENOSYS_LOCAL;
    u64 kernelLength = boxRadius * 2 + 1;
    u64 gridWidth = (clipWidth + scale - 1) / scale;
    u64 gridHeight = (clipHeight + scale - 1) / scale;
    if (!hmje_gpu_blur_scratch_ensure(gridWidth, gridHeight)) return HMJE_POSIX_ENOSYS_LOCAL;

    /* Current CPU contract is six box passes, clamping intermediate edges.
     * Collapsing these into a single triple-box kernel changes edge pixels. */
    float kernelWeights[1024];
    for (u64 k = 0; k < kernelLength; ++k)
        kernelWeights[k] = (float)(1.0 / (double)kernelLength);

    HmjeGpuBlurUniforms u = {{0}};
    u.clipOrigin[0] = (float)left; u.clipOrigin[1] = (float)top;
    u.clipSize[0] = (float)clipWidth; u.clipSize[1] = (float)clipHeight;
    u.maskOrigin[0] = (float)regionX; u.maskOrigin[1] = (float)regionY;
    u.maskSize[0] = (float)regionWidth; u.maskSize[1] = (float)regionHeight;
    u.grid[0] = (float)gridWidth; u.grid[1] = (float)gridHeight;
    u.misc[0] = (float)scale; u.misc[1] = (float)boxRadius;
    u.params[0] = (float)radius; u.params[2] = (float)kernelLength;
    u.tint[0] = (float)((tint >> 16) & 255) / 255.f;
    u.tint[1] = (float)((tint >> 8) & 255) / 255.f;
    u.tint[2] = (float)(tint & 255) / 255.f;
    u.tint[3] = (float)((tint >> 24) & 255) / 255.f;

    hmje_gpu_end_render_encoder();

    /* 패스 1: 다운샘플(target → scratch A). */
    HmjeGpuBlurUniforms du = u;
    du.viewport[0] = (float)g_gpu_blur_scratch_a.width;
    du.viewport[1] = (float)g_gpu_blur_scratch_a.height;
    MTLRenderPassDescriptor *downPass = [MTLRenderPassDescriptor renderPassDescriptor];
    downPass.colorAttachments[0].texture = g_gpu_blur_scratch_a;
    downPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    downPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> down = [g_gpu_command_buffer renderCommandEncoderWithDescriptor:downPass];
    [down setRenderPipelineState:g_gpu_blur_down_pipeline];
    [down setVertexBytes:&du length:sizeof(du) atIndex:0];
    [down setFragmentBytes:&du length:sizeof(du) atIndex:0];
    [down setFragmentTexture:target atIndex:0];
    [down drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [down endEncoding];

    /* 패스 2~7: 가로 3회, 세로 3회 박스 필터(A↔B). 블러 0이면 다운샘플 결과(s=1이면
     * 원본 복사와 동일)를 그대로 합성한다. */
    if (blurRadius > 0) {
        for (int passIndex = 0; passIndex < 6; ++passIndex) {
            int direction = passIndex < 3 ? 0 : 1;
            id<MTLTexture> sourceTexture = (passIndex & 1) == 0 ? g_gpu_blur_scratch_a : g_gpu_blur_scratch_b;
            id<MTLTexture> destinationTexture = (passIndex & 1) == 0 ? g_gpu_blur_scratch_b : g_gpu_blur_scratch_a;
            HmjeGpuBlurUniforms cu = u;
            cu.viewport[0] = (float)destinationTexture.width;
            cu.viewport[1] = (float)destinationTexture.height;
            cu.params[1] = (float)direction;
            MTLRenderPassDescriptor *convPass = [MTLRenderPassDescriptor renderPassDescriptor];
            convPass.colorAttachments[0].texture = destinationTexture;
            convPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            convPass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> conv = [g_gpu_command_buffer renderCommandEncoderWithDescriptor:convPass];
            [conv setRenderPipelineState:g_gpu_blur_conv_pipeline];
            [conv setVertexBytes:&cu length:sizeof(cu) atIndex:0];
            [conv setFragmentBytes:&cu length:sizeof(cu) atIndex:0];
            [conv setFragmentBytes:kernelWeights length:sizeof(float) * kernelLength atIndex:1];
            [conv setFragmentTexture:sourceTexture atIndex:0];
            [conv drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [conv endEncoding];
        }
    }

    /* 패스 8: 합성(scratch A → target, 기존 scissor/블렌드 존중). */
    HmjeGpuBlurUniforms su = u;
    su.viewport[0] = (float)target.width;
    su.viewport[1] = (float)target.height;
    MTLRenderPassDescriptor *compositePass = [MTLRenderPassDescriptor renderPassDescriptor];
    compositePass.colorAttachments[0].texture = target;
    compositePass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    compositePass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> compositeEncoder =
        [g_gpu_command_buffer renderCommandEncoderWithDescriptor:compositePass];
    if (g_gpu_clip.width > 0 && g_gpu_clip.height > 0)
        [compositeEncoder setScissorRect:g_gpu_clip];
    [compositeEncoder setRenderPipelineState:g_gpu_blur_composite_pipeline];
    [compositeEncoder setVertexBytes:&su length:sizeof(su) atIndex:0];
    [compositeEncoder setFragmentBytes:&su length:sizeof(su) atIndex:0];
    [compositeEncoder setFragmentTexture:g_gpu_blur_scratch_a atIndex:0];
    [compositeEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [compositeEncoder endEncoding];
    HMJE_METAL_BLUR_MARK_INITIALIZED();
    return 1;
}
