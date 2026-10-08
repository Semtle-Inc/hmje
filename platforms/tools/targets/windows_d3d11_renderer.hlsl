// Normalized HMJE GPU ABI. Coordinates and images are top-left canonical XRGB.
cbuffer Uniforms : register(b0) {
    float2 origin, size;
    float4 color;
    float2 viewport, padding;
    float4 params, shadow, sourceRect, sourceInfo, blurClip, mask, grid;
    float4 colors[8];
};
cbuffer Kernel : register(b1) { float4 weights[256]; };
Texture2D<float4> source : register(t0);
struct VertexOut { float4 position : SV_POSITION; };
VertexOut vs_main(uint id : SV_VertexID) {
    float2 pixel=origin+float2(id&1,(id>>1)&1)*size;
    VertexOut o; o.position=float4(pixel.x/viewport.x*2-1,1-pixel.y/viewport.y*2,0,1); return o;
}
float sdf(float2 p,float2 extent,float radius) {
    float2 halfSize=extent*.5, q=abs(p-halfSize)-halfSize+radius;
    return length(max(q,0))+min(max(q.x,q.y),0)-radius;
}
uint coverage(float2 p,float2 extent,float radius) { return (uint)(saturate(.5-sdf(p,extent,radius))*255); }
float alpha(uint cov,float a) { return (float)((cov*(uint)round(a*255))/255u)/255; }
float4 ps_shape(VertexOut i) : SV_TARGET {
    int kind=(int)params.x; float2 local=i.position.xy-origin;
    if(kind==0) return color;
    if(kind==4) {
        uint cov=(uint)round(source.Load(int3((int2)floor(local/max(params.y,1.)),0)).r*255);
        return float4(color.rgb,alpha(cov,color.a));
    }
    if(kind==3) {
        float2 p=i.position.xy-shadow.xy, b=shadow.zw-shadow.xy;
        float t=saturate(dot(p,b)/max(dot(b,b),.0001));
        float cov=saturate(.5-(length(p-b*t)-params.w*.5));
        return float4(color.rgb,alpha((uint)(cov*255),color.a));
    }
    if(kind==5) {
        float blur=params.z;
        float d=sdf(i.position.xy-origin-blur,size-2*blur,params.y);
        float t=saturate((d+blur)/max(2*blur,.00001));
        float factor=blur<=0?saturate(.5-d):1-t*t*(3-2*t);
        factor*=1-saturate(.5-sdf(i.position.xy-shadow.xy,shadow.zw,params.w));
        return float4(color.rgb,alpha((uint)(factor*255),color.a));
    }
    uint outer=coverage(local,size,params.y), cov=outer;
    if(kind==2) { uint inner=coverage(local-params.w,size-2*params.w,params.z); cov=outer>inner?outer-inner:0; }
    return float4(color.rgb,alpha(cov,color.a));
}
float4 ps_image(VertexOut i) : SV_TARGET {
    float2 local=i.position.xy-origin;
    int2 cell=(int2)floor(sourceRect.xy+local/size*sourceRect.zw);
    cell=clamp(cell,int2(0,0),(int2)sourceInfo.xy-1);
    float4 sampled=source.Load(int3(cell,0));
    // The 112-byte image request explicitly uses straight source alpha.
    // XRGB framebuffers, layers and legacy 104-byte images ignore this byte.
    return float4(sampled.rgb,alpha(coverage(local,size,params.y),params.z>0.5?sampled.a:color.a));
}
float4 ps_gradient(VertexOut i) : SV_TARGET {
    float2 local=i.position.xy-origin,p=floor(local);
    int dir=(int)params.z,n=(int)params.w;
    float position=dir==1?p.y:(dir==3?p.x+p.y:p.x);
    float denom=dir==1?size.y-1:(dir==3?size.x+size.y-2:size.x-1);
    float t=denom>0?saturate(position/denom):0;
    if(dir==4||dir==5) {
        float2 radius=max((size-1)*.5,float2(.5,.5));
        if(dir==4) radius=float2(min(radius.x,radius.y),min(radius.x,radius.y));
        t=saturate(length((p-(size-1)*.5)/radius));
    }
    float s=t*(n-1);
    int segment=clamp((int)floor(s),0,n-2);
    float4 c=round(lerp(colors[segment],colors[segment+1],s-segment));
    return float4(c.rgb/255,alpha(coverage(local,size,params.y),c.a/255));
}
float4 ps_downsample(VertexOut i) : SV_TARGET {
    int2 g=(int2)i.position.xy; int scale=(int)grid.z;
    float3 acc=0; int count=0;
    for(int y=0;y<scale;++y)for(int x=0;x<scale;++x) {
        int2 local=g*scale+int2(x,y);
        if(local.x<(int)blurClip.z&&local.y<(int)blurClip.w) {
            acc+=source.Load(int3((int2)blurClip.xy+local,0)).rgb; ++count;
        }
    }
    return float4(acc/max(count,1),1);
}
float4 ps_convolve(VertexOut i) : SV_TARGET {
    int2 g=(int2)i.position.xy; bool vertical=params.y>.5;
    int limit=(int)(vertical?grid.y:grid.x)-1;
    float3 acc=0;
    for(int k=0;k<(int)params.z;++k) {
        int pos=clamp((vertical?g.y:g.x)+k-(int)grid.w,0,limit);
        int2 cell=vertical?int2(g.x,pos):int2(pos,g.y);
        acc+=source.Load(int3(cell,0)).rgb*weights[k/4][k%4];
    }
    return float4(acc,1);
}
float4 ps_blur_composite(VertexOut i) : SV_TARGET {
    float2 p=(i.position.xy-blurClip.xy)/grid.z-.5;
    int2 cell=(int2)floor(p), upper=(int2)grid.xy-1;
    float2 f=frac(p);
    float3 a=source.Load(int3(clamp(cell,int2(0,0),upper),0)).rgb;
    float3 b=source.Load(int3(clamp(cell+int2(1,0),int2(0,0),upper),0)).rgb;
    float3 c=source.Load(int3(clamp(cell+int2(0,1),int2(0,0),upper),0)).rgb;
    float3 d=source.Load(int3(clamp(cell+1,int2(0,0),upper),0)).rgb;
    float3 result=lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
    result=lerp(result,color.rgb,color.a);
    return float4(result,(float)coverage(i.position.xy-mask.xy,mask.zw,params.x)/255);
}
