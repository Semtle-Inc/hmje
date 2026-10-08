/* C is restricted to SDK COM/WinRT vtables, DATA_BLOB and Win32 thread/handle
 * lifetimes. Public API, payload framing and migration verification are HMJE. */
#define COBJMACROS
#include "vault_protected_windows.h"
#include <roapi.h>
#include <winstring.h>
#include <windows.security.credentials.ui.h>
#include <userconsentverifierinterop.h>
#include <asyncinfo.h>
#include <stdlib.h>
#include <string.h>

typedef __x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics ConsentStatics;
typedef __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CUI__CUserConsentVerifierAvailability AvailabilityOperation;
typedef __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CUI__CUserConsentVerificationResult VerificationOperation;
typedef enum __x_ABI_CWindows_CSecurity_CCredentials_CUI_CUserConsentVerifierAvailability Availability;
typedef enum __x_ABI_CWindows_CSecurity_CCredentials_CUI_CUserConsentVerificationResult Verification;
/* SDK-declared C IIDs lack definitions in WindowsApp.lib. These UUIDs are
 * the SDK's IUserConsentVerifierStatics/Interop and parameterized operation. */
static const IID vault_consent_statics_iid={0xaf4f3f91,0x564c,0x4ddc,{0xb8,0xb5,0x97,0x34,0x47,0x62,0x7c,0x65}};
#ifndef HMJE_VAULT_WINDOWS_PRESENCE_TEST
static const IID vault_consent_interop_iid={0x39e050c3,0x4e74,0x441a,{0x8d,0xc0,0xb8,0x11,0x04,0xdf,0x94,0x9c}};
static const IID vault_consent_result_iid={0xfd596ffd,0x2318,0x558f,{0x9d,0xbe,0xd2,0x1d,0xf4,0x37,0x64,0xa5}};
#endif
enum { VP_CANCEL=8, VP_DENIED=9, VP_PENDING=10, VP_CLOSED=11 };

typedef struct vault_presence_request {
    struct vault_presence_request *next;
    uint64_t handle, operation;
    LONG references;
    SRWLOCK lock;
    HANDLE cancel_event;
    int64_t status;
    int cancelled, closed, migration_ready, migration_step;
    unsigned char ns[VAULT_NAME_MAX], key[VAULT_NAME_MAX];
    int64_t nl, kl, input_length;
    unsigned char *input, *result;
    uint64_t result_length;
    WCHAR *prompt;
    HWND owner;
} vault_presence_request;
static SRWLOCK registry_lock = SRWLOCK_INIT;
static vault_presence_request *requests;
static uint64_t next_handle;
static void wipe_free(unsigned char **p, uint64_t length) {
    if (*p) { SecureZeroMemory(*p,(SIZE_T)length); free(*p); *p=NULL; }
}
static void release_request(vault_presence_request *r) {
    if (InterlockedDecrement(&r->references)) return;
    wipe_free(&r->input,(uint64_t)r->input_length);
    wipe_free(&r->result,r->result_length);
    free(r->prompt); CloseHandle(r->cancel_event); free(r);
}
static vault_presence_request *lookup(uint64_t handle) {
    AcquireSRWLockShared(&registry_lock);
    vault_presence_request *r=requests;
    while (r && r->handle!=handle) r=r->next;
    if (r) InterlockedIncrement(&r->references);
    ReleaseSRWLockShared(&registry_lock);
    return r;
}
static int64_t hresult_status(HRESULT hr) {
    if (hr==E_ABORT || hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return VP_CANCEL;
    if (hr==E_ACCESSDENIED) return VP_DENIED;
    if (hr==HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION)) return VAULT_LOCKED;
    return VAULT_INTERNAL;
}
static int64_t availability_status(unsigned reason) {
    if (reason==0) return VAULT_OK;
    if (reason==1 || reason==2) return VAULT_UNSUPPORTED;
    if (reason==3) return VP_DENIED;
    if (reason==4) return VAULT_LOCKED;
    return VAULT_INTERNAL;
}
static int64_t verification_status(unsigned result) {
    if (result==0) return VAULT_OK;
    if (result==1 || result==2) return VAULT_UNSUPPORTED;
    if (result==3) return VP_DENIED;
    if (result==4 || result==5) return VAULT_LOCKED;
    if (result==6) return VP_CANCEL;
    return VAULT_INTERNAL;
}
static HRESULT factory(REFIID iid, void **out) {
    const WCHAR name[]=L"Windows.Security.Credentials.UI.UserConsentVerifier";
    HSTRING class_name=NULL;
    HRESULT hr=WindowsCreateString(name,(UINT32)(sizeof(name)/sizeof(WCHAR)-1),&class_name);
    if (SUCCEEDED(hr)) hr=RoGetActivationFactory(class_name,iid,out);
    WindowsDeleteString(class_name); return hr;
}
/* Polling IAsyncInfo avoids invoking HMJE/ARC on OS callback threads. Cancel is
 * issued from the same WinRT apartment as creation. Completion after cancel is
 * never published, even when the provider ignores IAsyncInfo::Cancel. */
/* GetResults must precede Close; use a separate wait that leaves the operation
 * open, then release/Close it after consuming the enum. */
static HRESULT wait_result(IUnknown *operation,HANDLE cancel,DWORD timeout) {
    IAsyncInfo *info=NULL;
    HRESULT hr=operation->lpVtbl->QueryInterface(operation,&IID_IAsyncInfo,(void **)&info);
    if (FAILED(hr)) return hr;
    ULONGLONG start=GetTickCount64();
    for (;;) {
        if ((cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) || (timeout!=INFINITE && GetTickCount64()-start>=timeout)) {
            info->lpVtbl->Cancel(info); hr=E_ABORT; break;
        }
        AsyncStatus status;
        hr=info->lpVtbl->get_Status(info,&status);
        if (FAILED(hr) || status==Completed) break;
        if (status==Canceled) { hr=E_ABORT; break; }
        if (status==Error) { info->lpVtbl->get_ErrorCode(info,&hr); break; }
        if (cancel) WaitForSingleObject(cancel,5); else Sleep(5);
    }
    if (FAILED(hr)) info->lpVtbl->Close(info);
    info->lpVtbl->Release(info); return hr;
}
static void close_operation(IUnknown *operation) {
    IAsyncInfo *info=NULL;
    if (SUCCEEDED(operation->lpVtbl->QueryInterface(operation,&IID_IAsyncInfo,(void **)&info))) {
        info->lpVtbl->Close(info); info->lpVtbl->Release(info);
    }
    operation->lpVtbl->Release(operation);
}
static int64_t check_availability(uint64_t *reason,HANDLE cancel) {
    *reason=7;
    ConsentStatics *statics=NULL;
    HRESULT hr=factory(&vault_consent_statics_iid,(void **)&statics);
    if (FAILED(hr)) return hresult_status(hr);
    AvailabilityOperation *operation=NULL;
    hr=statics->lpVtbl->CheckAvailabilityAsync(statics,&operation);
    statics->lpVtbl->Release(statics);
    if (FAILED(hr)) return hresult_status(hr);
    hr=wait_result((IUnknown *)operation,cancel,30000);
    Availability value=(Availability)0;
    if (SUCCEEDED(hr)) hr=operation->lpVtbl->GetResults(operation,&value);
    close_operation((IUnknown *)operation);
    if (FAILED(hr)) return hresult_status(hr);
    *reason=(uint64_t)value; return availability_status((unsigned)value);
}
typedef struct { uint64_t reason; int64_t status; } availability_check;
static DWORD WINAPI availability_worker(void *arg) {
    availability_check *check=arg;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    check->status=FAILED(hr) ? hresult_status(hr) : check_availability(&check->reason,NULL);
    if (SUCCEEDED(hr)) RoUninitialize();
    return 0;
}
int64_t hmje_vault_protected_availability(uint64_t *reason) {
    if (!reason) return VAULT_INVALID;
    availability_check check={7,VAULT_INTERNAL};
    HANDLE thread=CreateThread(NULL,0,availability_worker,&check,0,NULL);
    if (!thread) { *reason=7; return VAULT_INTERNAL; }
    WaitForSingleObject(thread,INFINITE); CloseHandle(thread);
    *reason=check.reason; return check.status;
}
static BOOL CALLBACK find_window(HWND window,LPARAM argument) {
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window,GW_OWNER)) {
        *(HWND *)argument=window; return FALSE;
    }
    return TRUE;
}
static HWND owner_window(void) {
    HWND owner=GetForegroundWindow(); DWORD pid=0;
    if (owner) GetWindowThreadProcessId(owner,&pid);
    if (!owner || pid!=GetCurrentProcessId()) { owner=NULL; EnumWindows(find_window,(LPARAM)&owner); }
    return owner;
}
static int64_t verify(vault_presence_request *r) {
#ifdef HMJE_VAULT_WINDOWS_PRESENCE_TEST
    extern int64_t hmje_vp_win_test_verify(HANDLE);
    return verification_status((unsigned)hmje_vp_win_test_verify(r->cancel_event));
#else
    uint64_t reason;
    int64_t available=check_availability(&reason,r->cancel_event);
    if (available) return available;
    DWORD pid=0;
    if (!r->owner || !IsWindow(r->owner)) return VAULT_INVALID;
    GetWindowThreadProcessId(r->owner,&pid);
    if (pid!=GetCurrentProcessId()) return VAULT_INVALID;
    IUserConsentVerifierInterop *interop=NULL;
    HRESULT hr=factory(&vault_consent_interop_iid,(void **)&interop);
    if (FAILED(hr)) return hresult_status(hr);
    HSTRING message=NULL;
    hr=WindowsCreateString(r->prompt,(UINT32)wcslen(r->prompt),&message);
    VerificationOperation *operation=NULL;
    if (SUCCEEDED(hr)) hr=interop->lpVtbl->RequestVerificationForWindowAsync(interop,r->owner,message,
        &vault_consent_result_iid,(void **)&operation);
    WindowsDeleteString(message); interop->lpVtbl->Release(interop);
    if (FAILED(hr)) return hresult_status(hr);
    hr=wait_result((IUnknown *)operation,r->cancel_event,INFINITE);
    Verification value=(Verification)0;
    if (SUCCEEDED(hr)) hr=operation->lpVtbl->GetResults(operation,&value);
    close_operation((IUnknown *)operation);
    return FAILED(hr) ? hresult_status(hr) : verification_status((unsigned)value);
#endif
}
static DWORD WINAPI request_worker(void *argument) {
    vault_presence_request *r=argument;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    int64_t status=FAILED(hr) ? hresult_status(hr) : verify(r);
    AcquireSRWLockExclusive(&r->lock);
    if (!r->closed && !r->cancelled && WaitForSingleObject(r->cancel_event,0)!=WAIT_OBJECT_0) {
        r->status=status;
        if (!status) {
            uint64_t n=0;
            if (r->operation==4) r->migration_ready=1;
            else if (r->operation==1) r->status=vault_win_protected_blob(1,r->ns,r->nl,r->key,r->kl,
                r->input,r->input_length,&n,0);
            else if (r->operation==2) {
                r->result=malloc(VP_WIN_PAYLOAD_MAX);
                r->status=r->result ? vault_win_protected_blob(2,r->ns,r->nl,r->key,r->kl,
                    r->result,VP_WIN_PAYLOAD_MAX,&r->result_length,0) : VAULT_INTERNAL;
                if (r->status) { wipe_free(&r->result,VP_WIN_PAYLOAD_MAX); r->result_length=0; }
            } else r->status=vault_win_protected_blob(3,r->ns,r->nl,r->key,r->kl,NULL,0,&n,0);
        }
    }
    wipe_free(&r->input,(uint64_t)r->input_length);
    ReleaseSRWLockExclusive(&r->lock);
    if (SUCCEEDED(hr)) RoUninitialize();
    release_request(r); return 0;
}
uint64_t hmje_vault_protected_start(uint64_t op,const void *ns,int64_t nl,const void *key,int64_t kl,
    uint64_t policy,const void *prompt,int64_t pl,const void *value,int64_t size,uint64_t *status) {
    if (!status) return 0;
    *status=VAULT_INVALID;
    if (!vault_names(ns,nl,key,kl) || op<1 || op>4 || policy!=1 || pl<=0 || pl>4096 ||
        !hmje_vault_utf8_valid(prompt,pl,0) || size<0 || size>VP_WIN_PAYLOAD_MAX || (size && !value)) return 0;
    vault_presence_request *r=calloc(1,sizeof(*r));
    *status=VAULT_INTERNAL;
    if (!r) return 0;
    InitializeSRWLock(&r->lock);
    r->references=1; r->status=VP_PENDING; r->operation=op; r->nl=nl; r->kl=kl;
    memcpy(r->ns,ns,(size_t)nl); memcpy(r->key,key,(size_t)kl);
    r->input_length=size; r->cancel_event=CreateEventW(NULL,TRUE,FALSE,NULL);
    r->owner=owner_window();
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,prompt,(int)pl,NULL,0);
    r->prompt=calloc((size_t)count+1,sizeof(WCHAR));
    if (!r->cancel_event || !count || !r->prompt) { release_request(r); return 0; }
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,prompt,(int)pl,r->prompt,count);
    if (size) { r->input=malloc((size_t)size); if (!r->input) { release_request(r); return 0; } memcpy(r->input,value,(size_t)size); }
    AcquireSRWLockExclusive(&registry_lock);
    if (next_handle==UINT64_MAX) { ReleaseSRWLockExclusive(&registry_lock); release_request(r); return 0; }
    r->handle=++next_handle; r->next=requests; requests=r;
    InterlockedIncrement(&r->references);
    ReleaseSRWLockExclusive(&registry_lock);
    HANDLE thread=CreateThread(NULL,0,request_worker,r,0,NULL);
    uint64_t handle=r->handle;
    if (!thread) { release_request(r); hmje_vault_protected_close(handle); return 0; }
    CloseHandle(thread); *status=VP_PENDING; return handle;
}
int64_t hmje_vault_protected_exists(const void *ns,int64_t nl,const void *key,int64_t kl) {
    if (!vault_names(ns,nl,key,kl)) return VAULT_INVALID;
    uint64_t reason, n;
    int64_t s=hmje_vault_protected_availability(&reason);
    return s ? s : vault_win_protected_blob(0,ns,nl,key,kl,NULL,0,&n,0);
}
int64_t hmje_vault_protected_poll(uint64_t handle,void *out,int64_t capacity,uint64_t *length) {
    if (length) *length=0;
    if (!length || capacity<0 || (capacity && !out)) return VAULT_INVALID;
    vault_presence_request *r=lookup(handle);
    if (!r) return VP_CLOSED;
    AcquireSRWLockShared(&r->lock);
    int64_t s=r->closed ? VP_CLOSED : r->status;
    if (!s) {
        if (r->result_length>(uint64_t)capacity) s=VAULT_INVALID;
        else { if (r->result_length) memcpy(out,r->result,(size_t)r->result_length); *length=r->result_length; }
    }
    ReleaseSRWLockShared(&r->lock); release_request(r); return s;
}
int64_t hmje_vault_protected_cancel(uint64_t handle) {
    vault_presence_request *r=lookup(handle);
    if (!r) return VP_CLOSED;
    SetEvent(r->cancel_event);
    AcquireSRWLockExclusive(&r->lock);
    int64_t s=r->closed ? VP_CLOSED : VP_CANCEL;
    if (!r->closed) {
        r->cancelled=1; r->migration_ready=0; r->status=VP_CANCEL;
        wipe_free(&r->input,(uint64_t)r->input_length); wipe_free(&r->result,r->result_length); r->result_length=0;
    }
    ReleaseSRWLockExclusive(&r->lock); release_request(r); return s;
}
int64_t hmje_vault_protected_close(uint64_t handle) {
    vault_presence_request *r=lookup(handle);
    if (!r) return VP_CLOSED;
    SetEvent(r->cancel_event);
    AcquireSRWLockExclusive(&r->lock);
    r->closed=1; r->migration_ready=0;
    wipe_free(&r->input,(uint64_t)r->input_length); wipe_free(&r->result,r->result_length); r->result_length=0;
    ReleaseSRWLockExclusive(&r->lock);
    AcquireSRWLockExclusive(&registry_lock);
    vault_presence_request **p=&requests;
    while (*p && *p!=r) p=&(*p)->next;
    if (*p) { *p=r->next; release_request(r); }
    ReleaseSRWLockExclusive(&registry_lock);
    release_request(r); return VP_CLOSED;
}
uint64_t hmje_vault_protected_migration_ready(uint64_t handle) {
    vault_presence_request *r=lookup(handle);
    if (!r) return 0;
    AcquireSRWLockExclusive(&r->lock);
    uint64_t ready=r->migration_ready && !r->closed && !r->cancelled && WaitForSingleObject(r->cancel_event,0)!=WAIT_OBJECT_0;
    if (ready) { r->migration_ready=0; r->status=VP_PENDING; }
    ReleaseSRWLockExclusive(&r->lock); release_request(r); return ready;
}
int64_t hmje_vault_protected_transfer(uint64_t handle,uint64_t step,void *data,int64_t length,uint64_t *written) {
    if (written) *written=0;
    if (!written || length<0 || length>VP_WIN_PAYLOAD_MAX || (length && !data)) return VAULT_INVALID;
    vault_presence_request *r=lookup(handle);
    if (!r) return VP_CLOSED;
    AcquireSRWLockExclusive(&r->lock);
    int64_t s=VAULT_INVALID;
    if (r->closed) s=VP_CLOSED;
    else if (r->cancelled || WaitForSingleObject(r->cancel_event,0)==WAIT_OBJECT_0) s=VP_CANCEL;
    else if (r->operation==4 && r->status==VP_PENDING && r->migration_step==(int)step && step<=2) {
        if (!step) {
            s=hmje_vault_get(r->ns,r->nl,r->key,r->kl,data,length,written);
            if (s==VAULT_UNSUPPORTED) s=VAULT_INVALID; /* missing app identity, not Hello */
        }
        else s=vault_win_protected_blob(step==1 ? 1 : 2,r->ns,r->nl,r->key,r->kl,data,length,written,step==1);
        r->migration_step++;
    }
    ReleaseSRWLockExclusive(&r->lock); release_request(r); return s;
}
int64_t hmje_vault_protected_finish(uint64_t handle,uint64_t status) {
    vault_presence_request *r=lookup(handle);
    if (!r) return VP_CLOSED;
    AcquireSRWLockExclusive(&r->lock);
    int64_t s=VAULT_INVALID;
    if (r->closed) s=VP_CLOSED;
    else if (r->cancelled || WaitForSingleObject(r->cancel_event,0)==WAIT_OBJECT_0) s=VP_CANCEL;
    else if (r->operation==4 && r->status==VP_PENDING) { r->status=(int64_t)status; s=r->status; }
    ReleaseSRWLockExclusive(&r->lock); release_request(r); return s;
}

uint64_t hmje_vault_windows_request_operation(uint64_t handle) {
    vault_presence_request *r=lookup(handle);
    if (!r) return 0;
    AcquireSRWLockShared(&r->lock);
    uint64_t op=r->closed ? 0 : r->operation;
    ReleaseSRWLockShared(&r->lock); release_request(r); return op;
}
