#include "../../tools/targets/windows_window.c"
const unsigned char hmje_window_app_identity[] = "net.semtle.hmje.window-rendering-probe";
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#v); return 1; } } while(0)
static WNDPROC previous_proc;
static unsigned title_changes, style_changes;
static LRESULT CALLBACK counted_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETTEXT) ++title_changes;
    if (m == WM_STYLECHANGED) ++style_changes;
    return CallWindowProcW(previous_proc,h,m,w,l);
}
static int gradient(int direction, UINT width, UINT height, UINT x, UINT y) {
    u64 begin[2]={width,height}, request[15]={0,0,width,height,0,direction,2,0xffffffff,0xff000000};
    if (hmje_d3d_begin(begin,sizeof(begin)) != 1 || hmje_d3d_gradient(request) != 1) return -1;
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(g_d3d_work.texture,&d);
    d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ; d.MiscFlags=0;
    ID3D11Texture2D *read=NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_device,&d,NULL,&read))) return -1;
    hmje_d3d_unbind(); ID3D11DeviceContext_CopyResource(g_context,(ID3D11Resource*)read,(ID3D11Resource*)g_d3d_work.texture);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ID3D11DeviceContext_Map(g_context,(ID3D11Resource*)read,0,D3D11_MAP_READ,0,&map))) return -1;
    int pixel=((unsigned char *)map.pData)[y*map.RowPitch+x*4];
    ID3D11DeviceContext_Unmap(g_context,(ID3D11Resource*)read,0); ID3D11Texture2D_Release(read);
    hmje_d3d_resize_invalidate(); return pixel;
}
int main(void) {
    g_win=create_window(1,900,600,FALSE,0,0,L"HMJE window probe",TRUE); CHECK(g_win && g_hwnd);
    RECT initial, fitted; CHECK(GetClientRect(g_hwnd,&initial));
    g_win->had_saved_frame=FALSE; u64 flexible[4]={2,2,0,0};
    CHECK(hmje_windows_ui_window(16,(i64)(uintptr_t)flexible,0)==0);
    CHECK(GetClientRect(g_hwnd,&fitted)); CHECK(initial.right==fitted.right && initial.bottom==fitted.bottom);
    previous_proc=(WNDPROC)SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,(LONG_PTR)counted_proc);
    const char *name="HMJE window probe"; u64 title[2]={(u64)(uintptr_t)name,strlen(name)};
    for(int i=0;i<100;++i) {
        CHECK(hmje_windows_ui_window_config((i64)(uintptr_t)name,strlen(name),0)==0);
        CHECK(hmje_windows_ui_window(13,g_win->id,(i64)(uintptr_t)title)==0);
    }
    CHECK(title_changes==0 && style_changes==0);
    const char *changed="HMJE changed title";
    CHECK(hmje_windows_ui_window_config((i64)(uintptr_t)changed,strlen(changed),1)==0);
    CHECK(title_changes==1 && style_changes==1);
    hmje_restore_window_frame(g_hwnd,-50000,-50000,50000,50000);
    RECT bounded; MONITORINFO monitor={sizeof(monitor)}; CHECK(GetWindowRect(g_hwnd,&bounded));
    CHECK(GetMonitorInfoW(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTONEAREST),&monitor));
    CHECK(bounded.left>=monitor.rcWork.left && bounded.top>=monitor.rcWork.top);
    CHECK(bounded.right<=monitor.rcWork.right && bounded.bottom<=monitor.rcWork.bottom);
    hmje_restore_window_frame(g_hwnd,INT_MIN,INT_MIN,INT_MAX,INT_MAX);
    CHECK(GetWindowRect(g_hwnd,&bounded));
    CHECK(bounded.left>=monitor.rcWork.left && bounded.right<=monitor.rcWork.right);
    CHECK(bounded.top>=monitor.rcWork.top && bounded.bottom<=monitor.rcWork.bottom);
    CHECK(gradient(4,65,33,32,16)==255);
    CHECK(gradient(4,65,33,48,16)==0);
    CHECK(abs(gradient(5,65,33,48,16)-128)<=1);
    CHECK(abs(gradient(5,65,33,32,8)-128)<=1);
    CHECK(gradient(4,65,33,0,0)==0);
    CHECK(gradient(5,65,33,0,0)==0);
    CHECK(gradient(4,1,1,0,0)==255);
    CHECK(gradient(1,65,33,32,0)==255);
    CHECK(abs(gradient(2,65,33,32,16)-128)<=1);
    SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,(LONG_PTR)previous_proc); DestroyWindow(g_hwnd);
    puts("PASS Windows caption idempotence, flexible initial size, restored frame bounds and D3D radial/elliptical pixels");
    return 0;
}
