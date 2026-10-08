#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
namespace xml = winrt::Windows::Data::Xml::Dom;
namespace notifications = winrt::Windows::UI::Notifications;
static std::mutex clicks_lock;
static std::condition_variable clicks_changed;
static std::deque<std::string> clicks;
static std::vector<notifications::ToastNotification> retained;
static bool click_waiter_active;

static std::wstring app_id() {
    DWORD length = GetEnvironmentVariableW(L"HMJE_APP_USER_MODEL_ID", nullptr, 0);
    if (length > 1 && length < 256) {
        std::wstring id(length, L'\0');
        if (GetEnvironmentVariableW(L"HMJE_APP_USER_MODEL_ID", id.data(), length)) {
            id.resize(length - 1);
            return id;
        }
    }
    return L"net.semtle.HunminIDE";
}
static std::wstring wide(const uint8_t *bytes, uint64_t length) {
    if ((!bytes && length) || length > 131072) throw std::invalid_argument("utf8");
    if (!length) return {};
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char *>(bytes), static_cast<int>(length), nullptr, 0);
    if (chars <= 0) throw std::invalid_argument("utf8");
    std::wstring result(chars, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char *>(bytes), static_cast<int>(length), result.data(), chars) != chars ||
        result.find(L'\0') != std::wstring::npos) throw std::invalid_argument("utf8");
    return result;
}
static std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::invalid_argument("utf16");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
static std::wstring escaped(const std::wstring &value) {
    std::wstring result;
    for (wchar_t c : value) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: if (c >= 32 || c == L'\n' || c == L'\t') result += c;
        }
    }
    return result;
}
static void prepare_runtime() {
    static thread_local bool initialized = false;
    if (!initialized) {
        HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) winrt::check_hresult(result);
        initialized = true;
    }
    std::wstring id = app_id();
    winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(id.c_str()));
}
extern "C" void hmje_ui_notification_request_async(uint64_t token) {
    try {
        prepare_runtime();
        auto notifier = notifications::ToastNotificationManager::CreateToastNotifier(app_id());
        auto setting = notifier.Setting();
        hmje_async_complete_u64(token, 0, setting == notifications::NotificationSetting::Enabled ? 1 : 0);
    } catch (winrt::hresult_error const &error) {
        /* Classic unpackaged apps may post a toast even when Settings() has
         * no per-app entry yet (ERROR_NOT_FOUND). Show() reports real failures. */
        hmje_async_complete_u64(token,
            error.code() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? 0 : 5,
            error.code() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? 1 : 0);
    } catch (...) { hmje_async_complete_u64(token, 5, 0); }
}
extern "C" void hmje_ui_notification_post_async(const uint8_t *title, uint64_t title_length,
    const uint8_t *body, uint64_t body_length, const uint8_t *identifier,
    uint64_t identifier_length, uint64_t token) {
    try {
        std::wstring title_text = wide(title, title_length);
        std::wstring body_text = wide(body, body_length);
        std::wstring id_text = wide(identifier, identifier_length);
        if (id_text.empty() || id_text.size() > 64) {
            hmje_async_complete_u64(token, 22, 0); return;
        }
        prepare_runtime();
        /* The legacy text template invokes ToastNotification::Activated for a
         * running unpackaged desktop app without a COM toast activator. */
        std::wstring payload = L"<toast><visual><binding template=\"ToastText02\"><text id=\"1\">" +
            escaped(title_text) + L"</text><text id=\"2\">" + escaped(body_text) +
            L"</text></binding></visual></toast>";
        xml::XmlDocument document;
        document.LoadXml(payload);
        notifications::ToastNotification toast(document);
        toast.Tag(id_text);
        toast.Group(L"hmje");
        std::string captured = utf8(id_text);
        toast.Activated([captured](auto const &, auto const &) {
            std::lock_guard<std::mutex> guard(clicks_lock);
            if (clicks.size() >= 256) clicks.pop_front();
            clicks.push_back(captured);
            clicks_changed.notify_all();
        });
        auto notifier = notifications::ToastNotificationManager::CreateToastNotifier(app_id());
        notifications::ToastNotificationManager::History().Remove(id_text, L"hmje", app_id());
        notifier.Show(toast);
        {
            std::lock_guard<std::mutex> guard(clicks_lock);
            retained.push_back(toast);
            if (retained.size() > 256) retained.erase(retained.begin());
        }
        hmje_async_complete_u64(token, 0, 1);
    } catch (std::invalid_argument const &) { hmje_async_complete_u64(token, 22, 0); }
      catch (...) { hmje_async_complete_u64(token, 5, 0); }
}
extern "C" int64_t hmje_ui_notification_click_next(uint8_t *output, uint64_t capacity,
    uint64_t *written) {
    if (written) *written = 0;
    if (!output || !written || !capacity) return -22;
    std::lock_guard<std::mutex> guard(clicks_lock);
    if (clicks.empty()) return 0;
    if (clicks.front().size() > capacity) return -34;
    *written = clicks.front().size();
    memcpy(output, clicks.front().data(), static_cast<size_t>(*written));
    clicks.pop_front();
    return 1;
}
extern "C" void hmje_ui_notification_click_wait_async(uint64_t token) {
    bool busy = false;
    {
        std::lock_guard<std::mutex> guard(clicks_lock);
        busy = click_waiter_active;
        if (!busy) click_waiter_active = true;
    }
    if (busy) { hmje_async_complete_u64(token, 16, 0); return; }
    try {
        std::thread([token] {
            std::unique_lock<std::mutex> guard(clicks_lock);
            clicks_changed.wait(guard, [] { return !clicks.empty(); });
            click_waiter_active = false;
            guard.unlock();
            hmje_async_complete_u64(token, 0, 1);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> guard(clicks_lock);
        click_waiter_active = false;
        hmje_async_complete_u64(token, 12, 0);
    }
}
extern "C" int64_t hmje_ui_notification_badge(int64_t number) {
    (void)number; return -120;
}
