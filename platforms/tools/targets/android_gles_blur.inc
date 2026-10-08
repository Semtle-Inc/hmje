/* CPU/Metal backdrop contract: clipped block average -> three horizontal and three vertical box
 * passes -> bilinear upsample -> tint and rounded mask. No blur-radius truncation. */
static GLuint g_gpu_blur_down_program, g_gpu_blur_conv_program, g_gpu_blur_composite_program;
static GLuint g_gpu_blur_texture[2], g_gpu_blur_fbo[2], g_gpu_blur_kernel_texture;
static u64 g_gpu_blur_grid_width, g_gpu_blur_grid_height;
static int g_gpu_blur_supported = -1;
static const char *hmje_blur_vertex =
    "#version 300 es\nprecision highp float;uniform vec4 uRect;uniform vec2 uViewport;out vec2 vLocal;"
    "void main(){int i=gl_VertexID;vec2 q=vec2(float(i&1),float((i>>1)&1));vLocal=q*uRect.zw;vec2 p=uRect.xy+vLocal;gl_Position=vec4(p.x/uViewport.x*2.-1.,1.-p.y/uViewport.y*2.,0.,1.);}";
static int hmje_gles_blur_prepare(void) {
    if(g_gpu_blur_down_program&&g_gpu_blur_conv_program&&g_gpu_blur_composite_program)return 1;
    static const char *down =
        "#version 300 es\nprecision highp float;precision highp int;uniform sampler2D uTexture;uniform vec4 uClip;uniform int uSourceHeight;uniform int uScale;in vec2 vLocal;out vec4 outColor;"
        "void main(){ivec2 g=ivec2(floor(vLocal));vec3 acc=vec3(0);int count=0;for(int y=0;y<uScale;y++)for(int x=0;x<uScale;x++){ivec2 p=g*uScale+ivec2(x,y);if(p.x<int(uClip.z)&&p.y<int(uClip.w)){p+=ivec2(uClip.xy);acc+=texelFetch(uTexture,ivec2(p.x,uSourceHeight-1-p.y),0).rgb;count++;}}outColor=vec4(acc/float(max(count,1)),1.);}";
    static const char *conv =
        "#version 300 es\nprecision highp float;precision highp int;uniform sampler2D uTexture;uniform ivec2 uGrid;uniform int uAxis;uniform int uCenter;uniform int uCount;uniform sampler2D uKernel;in vec2 vLocal;out vec4 outColor;"
        "void main(){ivec2 g=ivec2(floor(vLocal));int limit=(uAxis==0?uGrid.x:uGrid.y)-1;vec3 acc=vec3(0);for(int k=0;k<uCount;k++){int p=clamp((uAxis==0?g.x:g.y)+k-uCenter,0,limit);ivec2 c=uAxis==0?ivec2(p,g.y):ivec2(g.x,p);acc+=texelFetch(uTexture,ivec2(c.x,uGrid.y-1-c.y),0).rgb*texelFetch(uKernel,ivec2(k,0),0).r;}outColor=vec4(acc,1.);}";
    static const char *composite =
        "#version 300 es\nprecision highp float;precision highp int;uniform sampler2D uTexture;uniform vec4 uClip;uniform vec4 uMask;uniform ivec2 uGrid;uniform float uScale;uniform float uCorner;uniform vec4 uTint;in vec2 vLocal;out vec4 outColor;"
        "vec3 fetch(ivec2 p){p=clamp(p,ivec2(0),uGrid-1);return texelFetch(uTexture,ivec2(p.x,uGrid.y-1-p.y),0).rgb;}"
        "float rr(vec2 p,vec2 s,float r){vec2 q=abs(p-s*.5)-s*.5+r;return length(max(q,vec2(0)))+min(max(q.x,q.y),0.)-r;}"
        "void main(){vec2 p=vLocal/uScale-.5;ivec2 c=ivec2(floor(p));vec2 f=fract(p);vec3 b=mix(mix(fetch(c),fetch(c+ivec2(1,0)),f.x),mix(fetch(c+ivec2(0,1)),fetch(c+1),f.x),f.y);b=mix(b,uTint.rgb,uTint.a);float cov=floor(clamp(.5-rr(uClip.xy+vLocal-uMask.xy,uMask.zw,uCorner),0.,1.)*255.)/255.;outColor=vec4(b,cov);}";
    g_gpu_blur_down_program=hmje_gles_link_program(hmje_blur_vertex,down);
    g_gpu_blur_conv_program=hmje_gles_link_program(hmje_blur_vertex,conv);
    g_gpu_blur_composite_program=hmje_gles_link_program(hmje_blur_vertex,composite);
    return g_gpu_blur_down_program&&g_gpu_blur_conv_program&&g_gpu_blur_composite_program;
}
static int hmje_gles_blur_scratch(u64 width,u64 height) {
    if(width<=g_gpu_blur_grid_width&&height<=g_gpu_blur_grid_height&&g_gpu_blur_texture[0])return 1;
    if(width<g_gpu_blur_grid_width)width=g_gpu_blur_grid_width;
    if(height<g_gpu_blur_grid_height)height=g_gpu_blur_grid_height;
    if(!g_gpu_blur_texture[0]){glGenTextures(2,g_gpu_blur_texture);glGenFramebuffers(2,g_gpu_blur_fbo);}
    for(int i=0;i<2;++i){
        glBindTexture(GL_TEXTURE_2D,g_gpu_blur_texture[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,(GLsizei)width,(GLsizei)height,0,GL_RGBA,GL_FLOAT,NULL);
        glBindFramebuffer(GL_FRAMEBUFFER,g_gpu_blur_fbo[i]);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g_gpu_blur_texture[i],0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return 0;
    }
    g_gpu_blur_grid_width=width;g_gpu_blur_grid_height=height;return glGetError()==GL_NO_ERROR;
}
/* Query the actual color attachment rather than assuming an optional ES3
 * extension. Cached for the current renderer/context lifetime. */
static int hmje_gles_blur_available(void) {
    if(g_gpu_blur_supported>=0)return g_gpu_blur_supported;
    GLint draw=0,read=0;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
    g_gpu_blur_supported=hmje_gles_blur_prepare()&&hmje_gles_blur_scratch(1,1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)read);
    while(glGetError()!=GL_NO_ERROR){} /* A rejected optional attachment is handled by QUERY policy. */
    return g_gpu_blur_supported;
}
static void hmje_gles_blur_rect(GLuint program,float x,float y,float width,float height,float vw,float vh) {
    glUseProgram(program);glBindVertexArray(g_gpu_vao);
    glUniform4f(glGetUniformLocation(program,"uRect"),x,y,width,height);glUniform2f(glGetUniformLocation(program,"uViewport"),vw,vh);
    glUniform1i(glGetUniformLocation(program,"uTexture"),0);
}
static i64 hmje_android_gpu_blur(const u64 *r,u64 length) {
    if(length!=56||!r||!g_gpu_active)return HMJE_ANDROID_ENOSYS;
    if(!hmje_gles_blur_available())return HMJE_ANDROID_ENOSYS;
    if(!r[2]||!r[3])return 1;
    if(r[2]>INT_MAX||r[3]>INT_MAX||r[5]>680)return HMJE_ANDROID_ENOSYS;
    i64 x=(i64)r[0],y=(i64)r[1];if(x<INT_MIN||x>INT_MAX||y<INT_MIN||y>INT_MAX)return HMJE_ANDROID_ENOSYS;
    GLint active_fbo=0,clip[4];glGetIntegerv(GL_FRAMEBUFFER_BINDING,&active_fbo);glGetIntegerv(GL_SCISSOR_BOX,clip);
    i64 clip_top=(i64)g_gpu_height-clip[1]-clip[3];
    i64 left=x>clip[0]?x:clip[0],top=y>clip_top?y:clip_top;
    i64 right=x+(i64)r[2],bottom=y+(i64)r[3];
    if(left<0)left=0;if(top<0)top=0;
    if(right>clip[0]+clip[2])right=clip[0]+clip[2];if(right>(i64)g_gpu_width)right=(i64)g_gpu_width;
    if(bottom>clip_top+clip[3])bottom=clip_top+clip[3];if(bottom>(i64)g_gpu_height)bottom=(i64)g_gpu_height;
    if(right<=left||bottom<=top)return 1;
    u64 width=(u64)(right-left),height=(u64)(bottom-top),scale=r[5]<=8?1:(r[5]<=16?2:4),radius=r[5]/scale;
    u64 count=radius*2+1,gw=(width+scale-1)/scale,gh=(height+scale-1)/scale;if(count>1024)return HMJE_ANDROID_ENOSYS;
    if(!hmje_gles_blur_prepare())return HMJE_ANDROID_ENOSYS;
    GLint source=0;glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&source);
    i64 result=HMJE_ANDROID_ENOSYS;
    if(!hmje_gles_blur_scratch(gw,gh))goto restore;
    float weights[1024]={0};for(u64 k=0;k<count;++k)weights[k]=1.f/(float)count;
    if(!g_gpu_blur_kernel_texture)glGenTextures(1,&g_gpu_blur_kernel_texture);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,g_gpu_blur_kernel_texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,(GLsizei)count,1,0,GL_RED,GL_FLOAT,weights);
    glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER,g_gpu_blur_fbo[0]);glViewport(0,0,(GLsizei)gw,(GLsizei)gh);
    hmje_gles_blur_rect(g_gpu_blur_down_program,0,0,(float)gw,(float)gh,(float)gw,(float)gh);
    glBindTexture(GL_TEXTURE_2D,(GLuint)source);
    glUniform4f(glGetUniformLocation(g_gpu_blur_down_program,"uClip"),(float)left,(float)top,(float)width,(float)height);
    glUniform1i(glGetUniformLocation(g_gpu_blur_down_program,"uSourceHeight"),(GLint)g_gpu_height);glUniform1i(glGetUniformLocation(g_gpu_blur_down_program,"uScale"),(GLint)scale);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    if(r[5])for(int pass=0;pass<6;++pass){int source_slot=pass&1,destination=1-source_slot;
        glBindFramebuffer(GL_FRAMEBUFFER,g_gpu_blur_fbo[destination]);
        hmje_gles_blur_rect(g_gpu_blur_conv_program,0,0,(float)gw,(float)gh,(float)gw,(float)gh);
        glBindTexture(GL_TEXTURE_2D,g_gpu_blur_texture[source_slot]);
        glUniform2i(glGetUniformLocation(g_gpu_blur_conv_program,"uGrid"),(GLint)gw,(GLint)gh);glUniform1i(glGetUniformLocation(g_gpu_blur_conv_program,"uAxis"),pass/3);
        glUniform1i(glGetUniformLocation(g_gpu_blur_conv_program,"uCenter"),(GLint)radius);glUniform1i(glGetUniformLocation(g_gpu_blur_conv_program,"uCount"),(GLint)count);
        glUniform1i(glGetUniformLocation(g_gpu_blur_conv_program,"uKernel"),1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)active_fbo);hmje_gles_common_state();glScissor(clip[0],clip[1],clip[2],clip[3]);
    hmje_gles_blur_rect(g_gpu_blur_composite_program,(float)left,(float)top,(float)width,(float)height,(float)g_gpu_width,(float)g_gpu_height);
    glBindTexture(GL_TEXTURE_2D,g_gpu_blur_texture[0]);
    glUniform4f(glGetUniformLocation(g_gpu_blur_composite_program,"uClip"),(float)left,(float)top,(float)width,(float)height);
    glUniform4f(glGetUniformLocation(g_gpu_blur_composite_program,"uMask"),(float)x,(float)y,(float)r[2],(float)r[3]);
    glUniform2i(glGetUniformLocation(g_gpu_blur_composite_program,"uGrid"),(GLint)gw,(GLint)gh);glUniform1f(glGetUniformLocation(g_gpu_blur_composite_program,"uScale"),(float)scale);
    u64 corner=r[4];if(corner>r[2]/2)corner=r[2]/2;if(corner>r[3]/2)corner=r[3]/2;
    glUniform1f(glGetUniformLocation(g_gpu_blur_composite_program,"uCorner"),(float)corner);hmje_gles_color(g_gpu_blur_composite_program,r[6],"uTint");
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);result=glGetError()==GL_NO_ERROR?1:HMJE_ANDROID_ENOSYS;
restore:
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)active_fbo);hmje_gles_common_state();glScissor(clip[0],clip[1],clip[2],clip[3]);return result;
}
