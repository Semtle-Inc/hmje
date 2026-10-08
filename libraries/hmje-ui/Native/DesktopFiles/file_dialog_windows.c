#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <string.h>
#include "file_dialog.h"

int64_t hmje_ui_file_dialog_supported(void) { return 1; }

int64_t hmje_ui_choose_file(int64_t action, uint8_t *output,
                            uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (action != 1 && action != 2)) return -1;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) return -2;
    IFileDialog *dialog = NULL;
    IShellItem *selection = NULL;
    PWSTR wide_path = NULL;
    int64_t status = -2;
    const CLSID *clsid = action == 1 ? &CLSID_FileOpenDialog : &CLSID_FileSaveDialog;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) goto done;
    hr = IFileDialog_Show(dialog, NULL);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        status = 0;
        goto done;
    }
    if (FAILED(hr)) goto done;
    hr = IFileDialog_GetResult(dialog, &selection);
    if (FAILED(hr)) goto done;
    hr = IShellItem_GetDisplayName(selection, SIGDN_FILESYSPATH, &wide_path);
    if (FAILED(hr) || !wide_path) goto done;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path,
                                     -1, NULL, 0, NULL, NULL);
    if (length <= 1) goto done;
    uint64_t bytes = (uint64_t)(length - 1);
    if (bytes > capacity) {
        status = -3;
        goto done;
    }
    char *temporary = (char *)CoTaskMemAlloc((SIZE_T)length);
    if (!temporary) goto done;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path,
                            -1, temporary, length, NULL, NULL) == length) {
        memcpy(output, temporary, (size_t)bytes);
        *written = bytes;
        status = 1;
    }
    CoTaskMemFree(temporary);
done:
    if (wide_path) CoTaskMemFree(wide_path);
    if (selection) IShellItem_Release(selection);
    if (dialog) IFileDialog_Release(dialog);
    CoUninitialize();
    return status;
}

int64_t hmje_ui_choose_folder(const uint8_t *initial, uint64_t initial_length,
                              const uint8_t *prompt, uint64_t prompt_length,
                              uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (initial_length && !initial) ||
        (prompt_length && !prompt) || initial_length > INT_MAX || prompt_length > INT_MAX) return -1;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) return -2;
    IFileOpenDialog *dialog = NULL;
    IShellItem *folder = NULL, *selection = NULL;
    PWSTR wide_path = NULL;
    WCHAR *initial_wide = NULL, *prompt_wide = NULL;
    int64_t status = -2;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&dialog);
    if (FAILED(hr)) goto done_folder;
    DWORD options = 0;
    hr = IFileOpenDialog_GetOptions(dialog, &options);
    if (FAILED(hr) || FAILED(IFileOpenDialog_SetOptions(dialog,
        options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM))) goto done_folder;
    if (initial_length) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        (const char *)initial, (int)initial_length, NULL, 0);
        if (count <= 0) { status = -1; goto done_folder; }
        initial_wide = (WCHAR *)CoTaskMemAlloc(((SIZE_T)count + 1) * sizeof(WCHAR));
        if (!initial_wide) goto done_folder;
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)initial,
                            (int)initial_length, initial_wide, count);
        initial_wide[count] = 0;
        if (FAILED(SHCreateItemFromParsingName(initial_wide, NULL, &IID_IShellItem,
                                               (void **)&folder))) { status = -1; goto done_folder; }
        if (FAILED(IFileOpenDialog_SetFolder(dialog, folder))) goto done_folder;
    }
    if (prompt_length) {
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        (const char *)prompt, (int)prompt_length, NULL, 0);
        if (count <= 0) { status = -1; goto done_folder; }
        prompt_wide = (WCHAR *)CoTaskMemAlloc(((SIZE_T)count + 1) * sizeof(WCHAR));
        if (!prompt_wide) goto done_folder;
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)prompt,
                            (int)prompt_length, prompt_wide, count);
        prompt_wide[count] = 0;
        if (FAILED(IFileOpenDialog_SetOkButtonLabel(dialog, prompt_wide))) goto done_folder;
    }
    hr = IFileOpenDialog_Show(dialog, NULL);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { status = 0; goto done_folder; }
    if (FAILED(hr) || FAILED(IFileOpenDialog_GetResult(dialog, &selection)) ||
        FAILED(IShellItem_GetDisplayName(selection, SIGDN_FILESYSPATH, &wide_path))) goto done_folder;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path,
                                     -1, NULL, 0, NULL, NULL);
    if (length <= 1) goto done_folder;
    if ((uint64_t)(length - 1) > capacity) { status = -3; goto done_folder; }
    char *temporary = (char *)CoTaskMemAlloc((SIZE_T)length);
    if (!temporary) goto done_folder;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path,
                            -1, temporary, length, NULL, NULL) == length) {
        memcpy(output, temporary, (size_t)(length - 1));
        *written = (uint64_t)(length - 1);
        status = 1;
    }
    CoTaskMemFree(temporary);
done_folder:
    if (wide_path) CoTaskMemFree(wide_path);
    if (initial_wide) CoTaskMemFree(initial_wide);
    if (prompt_wide) CoTaskMemFree(prompt_wide);
    if (folder) IShellItem_Release(folder);
    if (selection) IShellItem_Release(selection);
    if (dialog) IFileOpenDialog_Release(dialog);
    CoUninitialize();
    return status;
}
