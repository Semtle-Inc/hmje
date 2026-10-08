/* Linux desktop host. X11 is also available through Xwayland on Wayland desktops.
 * The HMJE CPU command interpreter writes canonical XRGB8888 pixels here. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <time.h>
#include <ucontext.h>
#include <wchar.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <stddef.h>
#include <limits.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include "pointer_input.h"
#include "text_input_control.h"
#include "guarded_stack.h"

typedef int64_t i64;
typedef uint64_t u64;
#include "window_persist.h"
typedef struct DropRecord {
    struct DropRecord *next;
    size_t length;
    unsigned char bytes[];
} DropRecord;
#define KIND(n) ((u64)(n) << 56)
#define ENOSYS_ (-38LL)
static Display *display;
static Atom wm_delete;
static u64 pending_activation_id;
static XIM xim;
typedef struct XWindowState {
    struct XWindowState *next;
    struct XWindowState *opener;
    u64 id;
    Window w_window;
    GC w_gc;
    XIC w_xic;
    unsigned w_width, w_height;
    uint32_t *w_pixels;
    char *w_title;
    int w_close_sent, w_resize_pending, closing, linked, had_saved_frame, key_active;
    u64 w_events[1024];
    unsigned w_event_head, w_event_tail;
    hmje_pointer_ring w_pointers;
    u64 w_buttons_down;
    DropRecord *w_drop_head, *w_drop_tail;
    unsigned w_drop_count;
    Window w_drag_source;
    Atom w_drag_target;
    u64 w_drag_kind, w_drag_x, w_drag_y;
    int w_drag_entered;
    int w_drag_incremental;
    unsigned char *w_drag_bytes;
    size_t w_drag_length;
    Atom w_drag_data_type;
    uint32_t w_preedit[256];
    unsigned w_preedit_count, w_preedit_caret;
    ucontext_t context;
    HmjeGuardedStack stack;
    void *(*entry)(void *);
    void *argument;
    int state; /* 0=main/new, 1=running, 2=waiting, 3=parked, 4=dead */
    uint64_t deadline_ms;
} XWindowState;
static XWindowState *find_id(u64 id);
static XWindowState main_state;
static XWindowState *active = &main_state, *window_states;
static u64 next_window_id = 1;
static int window_events_aware, quit_requested;
static u64 last_window_policy = 1;
static Window selection_window;
static Atom clipboard_atom, primary_atom, utf8_atom, targets_atom, selection_property;
static Atom xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave;
static Atom xdnd_drop, xdnd_finished, xdnd_selection, xdnd_type_list;
static Atom xdnd_action_copy, xdnd_property, uri_list_atom, plain_utf8_atom;
static Atom png_atom, jpeg_atom, incr_atom;
static unsigned char *clipboard;
static size_t clipboard_length;
static unsigned char *selection_result;
static size_t selection_result_length;
static int selection_finished;
#define cur_window (active->w_window)
#define gc (active->w_gc)
#define xic (active->w_xic)
#define cur_width (active->w_width)
#define cur_height (active->w_height)
#define pixels (active->w_pixels)
#define title (active->w_title)
#define close_sent (active->w_close_sent)
#define resize_pending (active->w_resize_pending)
#define events (active->w_events)
#define event_head (active->w_event_head)
#define event_tail (active->w_event_tail)
#define pointers (active->w_pointers)
#define buttons_down (active->w_buttons_down)

static uint64_t now_ms(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static XWindowState *find_window(Window xid) {
    for (XWindowState *state = window_states; state; state = state->next)
        if (state->w_window == xid) return state;
    return NULL;
}
static int ensure_display(void) {
    if (display) return 1;
    setlocale(LC_CTYPE, ""); XSetLocaleModifiers("");
    display = XOpenDisplay(NULL);
    if (!display) { fprintf(stderr, "hmje-linux-gui: cannot open DISPLAY\n"); return 0; }
    int screen = DefaultScreen(display);
    Visual *visual = DefaultVisual(display, screen);
    if (DefaultDepth(display, screen) != 24 || visual->red_mask != 0xff0000 ||
        visual->green_mask != 0x00ff00 || visual->blue_mask != 0x0000ff) {
        fprintf(stderr, "hmje-linux-gui: XRGB8888 TrueColor visual required\n"); return 0;
    }
    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    primary_atom = XA_PRIMARY;
    utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    selection_property = XInternAtom(display, "HMJE_SELECTION_RESULT", False);
    xdnd_aware = XInternAtom(display, "XdndAware", False);
    xdnd_enter = XInternAtom(display, "XdndEnter", False);
    xdnd_position = XInternAtom(display, "XdndPosition", False);
    xdnd_status = XInternAtom(display, "XdndStatus", False);
    xdnd_leave = XInternAtom(display, "XdndLeave", False);
    xdnd_drop = XInternAtom(display, "XdndDrop", False);
    xdnd_finished = XInternAtom(display, "XdndFinished", False);
    xdnd_selection = XInternAtom(display, "XdndSelection", False);
    xdnd_type_list = XInternAtom(display, "XdndTypeList", False);
    xdnd_action_copy = XInternAtom(display, "XdndActionCopy", False);
    xdnd_property = XInternAtom(display, "HMJE_XDND_RESULT", False);
    uri_list_atom = XInternAtom(display, "text/uri-list", False);
    plain_utf8_atom = XInternAtom(display, "text/plain;charset=utf-8", False);
    png_atom = XInternAtom(display, "image/png", False);
    jpeg_atom = XInternAtom(display, "image/jpeg", False);
    incr_atom = XInternAtom(display, "INCR", False);
    selection_window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                            -1, -1, 1, 1, 0, 0, 0);
    if (!selection_window) return 0;
    XSelectInput(display, selection_window, PropertyChangeMask);
    xim = XOpenIM(display, NULL, NULL, NULL);
    return 1;
}

static void selection_event(const XEvent *event) {
    if (event->type == SelectionRequest) {
        const XSelectionRequestEvent *request = &event->xselectionrequest;
        XSelectionEvent reply = {0};
        reply.type = SelectionNotify; reply.display = display;
        reply.requestor = request->requestor; reply.selection = request->selection;
        reply.target = request->target; reply.time = request->time; reply.property = None;
        Atom property = request->property == None ? request->target : request->property;
        if (request->selection == clipboard_atom &&
            XGetSelectionOwner(display, clipboard_atom) == selection_window) {
            if (request->target == targets_atom) {
                Atom offered[] = {targets_atom, utf8_atom};
                XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                                PropModeReplace, (unsigned char *)offered, 2);
                reply.property = property;
            } else if (request->target == utf8_atom && clipboard_length <= 10000000) {
                XChangeProperty(display, request->requestor, property, utf8_atom, 8,
                                PropModeReplace, clipboard, (int)clipboard_length);
                reply.property = property;
            }
        }
        XSendEvent(display, request->requestor, False, NoEventMask, (XEvent *)&reply);
        XFlush(display);
    } else if (event->type == SelectionNotify &&
               event->xselection.requestor == selection_window) {
        selection_finished = 1;
        if (event->xselection.property == None) return;
        Atom type = None; int format = 0;
        unsigned long items = 0, after = 0; unsigned char *data = NULL;
        int result = XGetWindowProperty(display, selection_window, selection_property,
                                        0, 2500000, True, AnyPropertyType,
                                        &type, &format, &items, &after, &data);
        if (result == Success && format == 8 && after == 0 && items <= 10000000 &&
            (type == utf8_atom || type == XA_STRING)) {
            /* ICCCM STRING is Latin-1; convert it before handing text to HMJE. */
            size_t capacity = type == XA_STRING ? items * 2 : items;
            selection_result = malloc(capacity + 1);
            if (selection_result) {
                if (type == utf8_atom) {
                    memcpy(selection_result, data, items); selection_result_length = items;
                } else {
                    for (unsigned long i = 0; i < items; i++) {
                        unsigned char c = data[i];
                        if (c < 128) selection_result[selection_result_length++] = c;
                        else { selection_result[selection_result_length++] = 0xc0 | (c >> 6);
                               selection_result[selection_result_length++] = 0x80 | (c & 63); }
                    }
                }
                selection_result[selection_result_length] = 0;
            }
        }
        if (data) XFree(data);
    }
}

static void push(u64 event) {
    unsigned next = (event_head + 1) % 1024;
    if (next != event_tail) { events[event_head] = event; event_head = next; }
}
static u64 pop(void) {
    if (event_head == event_tail) return 0;
    u64 event = events[event_tail]; event_tail = (event_tail + 1) % 1024; return event;
}
static void resize_surface(unsigned w, unsigned h) {
    if (!w || !h || w > 16384 || h > 16384 || (w == cur_width && h == cur_height && pixels)) return;
    uint32_t *next = calloc((size_t)w * h, 4);
    if (!next) return;
    free(pixels); pixels = next; cur_width = w; cur_height = h; resize_pending = 1;
}
static void preedit_start(XIM im, XPointer client, XPointer call);
static void preedit_draw(XIM im, XPointer client, XPointer call);
static void preedit_caret(XIM im, XPointer client, XPointer call);
static void preedit_done(XIM im, XPointer client, XPointer call);
static void set_window_title(Window target, const char *value) {
    /* WM_NAME's STRING encoding is not UTF-8. EWMH window managers prefer
     * _NET_WM_NAME, whose declared type preserves Korean and other Unicode. */
    XStoreName(display, target, value);
    XChangeProperty(display, target, XInternAtom(display, "_NET_WM_NAME", False),
                    utf8_atom, 8, PropModeReplace,
                    (const unsigned char *)value, (int)strlen(value));
}
static int ensure_window(void) {
    if (cur_window) return 1;
    if (!ensure_display()) return 0;
    int screen = DefaultScreen(display);
    if (!cur_width) cur_width = 1180;
    if (!cur_height) cur_height = 860;
    cur_window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                                  cur_width, cur_height, 0, 0, 0);
    if (!cur_window) return 0;
    XSelectInput(display, cur_window, ExposureMask | StructureNotifyMask | KeyPressMask |
        KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
        FocusChangeMask | PropertyChangeMask);
    XSetWMProtocols(display, cur_window, &wm_delete, 1);
    unsigned long xdnd_version = 5;
    XChangeProperty(display, cur_window, xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&xdnd_version, 1);
    set_window_title(cur_window, title ? title : "HMJE");
    gc = XCreateGC(display, cur_window, 0, NULL);
    if (xim) {
        XIMCallback start = {(XPointer)active, (XIMProc)preedit_start};
        XIMCallback draw = {(XPointer)active, (XIMProc)preedit_draw};
        XIMCallback caret = {(XPointer)active, (XIMProc)preedit_caret};
        XIMCallback done = {(XPointer)active, (XIMProc)preedit_done};
        XVaNestedList callbacks = XVaCreateNestedList(0,
            XNPreeditStartCallback, &start, XNPreeditDrawCallback, &draw,
            XNPreeditCaretCallback, &caret, XNPreeditDoneCallback, &done, NULL);
        if (callbacks) {
            xic = XCreateIC(xim, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                            XNClientWindow, cur_window, XNFocusWindow, cur_window,
                            XNPreeditAttributes, callbacks, NULL);
            XFree(callbacks);
        }
        if (!xic) xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                  XNClientWindow, cur_window, XNFocusWindow, cur_window, NULL);
    }
    resize_surface(cur_width, cur_height);
    resize_pending = 0;
    XMapWindow(display, cur_window);
    XFlush(display);
    if (!active->linked) {
        if (!active->id) active->id = next_window_id++;
        active->next = window_states; window_states = active; active->linked = 1;
    }
    if (active == &main_state) {
        hmje_persist_record *saved = hmje_persist_bind(active->id, "__main__", 8, "", 0, 0);
        if (saved && saved->has_frame) {
            active->had_saved_frame = 1;
            XMoveResizeWindow(display, cur_window, saved->x, saved->y,
                              (unsigned)saved->width, (unsigned)saved->height);
            resize_surface((unsigned)saved->width, (unsigned)saved->height);
        }
    }
    return 1;
}
static void present(void) {
    if (!cur_window || !pixels) return;
    XImage *image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
        24, ZPixmap, 0, (char *)pixels, cur_width, cur_height, 32, (int)cur_width * 4);
    if (!image) return;
    XPutImage(display, cur_window, gc, image, 0, 0, 0, 0, cur_width, cur_height);
    image->data = NULL; XDestroyImage(image); XFlush(display);
}
static void emit_preedit(void) {
    for (unsigned i = 0; i < active->w_preedit_count; i++)
        push(KIND(8) | active->w_preedit[i]);
    unsigned caret = active->w_preedit_caret;
    if (caret > active->w_preedit_count) caret = active->w_preedit_count;
    push(KIND(8) | 4194304ULL | caret);
}
static void clear_preedit(void) {
    if (!active->w_preedit_count) return;
    active->w_preedit_count = active->w_preedit_caret = 0;
    emit_preedit();
}
static void push_text(const char *s, int n) {
    clear_preedit();
    int continuation = 0;
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i++];
        u64 cp = c;
        int extra = 0;
        if ((c & 0xe0) == 0xc0) { cp = c & 31; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 15; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 7; extra = 3; }
        else if (c >= 128) continue;
        if (i + extra > n) break;
        while (extra--) { unsigned char d = (unsigned char)s[i++]; cp = (cp << 6) | (d & 63); }
        if (cp >= 32 && cp != 127 && cp <= 0x10ffff) {
            push(KIND(4) | cp | (continuation ? 2097152ULL : 0)); continuation = 1;
        }
    }
}
static void preedit_start(XIM im, XPointer client, XPointer call) {
    (void)im; (void)call;
    XWindowState *previous = active; active = (XWindowState *)client;
    clear_preedit(); active = previous;
}
static void preedit_draw(XIM im, XPointer client, XPointer call) {
    (void)im;
    XIMPreeditDrawCallbackStruct *change = (XIMPreeditDrawCallbackStruct *)call;
    if (!change) return;
    XWindowState *previous = active; active = (XWindowState *)client;
    if (change->chg_first < 0 || change->chg_length < 0 ||
        (unsigned)change->chg_first > active->w_preedit_count ||
        (unsigned)change->chg_length > active->w_preedit_count - (unsigned)change->chg_first) {
        active = previous; return;
    }
    uint32_t inserted[256]; unsigned count = 0;
    XIMText *text = change->text;
    if (text && (text->encoding_is_wchar ? text->string.wide_char != NULL :
                                    text->string.multi_byte != NULL)) {
        if (text->length > 256) { active = previous; return; }
        if (text->encoding_is_wchar) {
            for (unsigned i = 0; i < text->length; i++) {
                uint32_t cp = (uint32_t)text->string.wide_char[i];
                inserted[count++] = cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) ? cp : 0xfffd;
            }
        } else {
            const char *cursor = text->string.multi_byte;
            mbstate_t state = {0};
            for (unsigned i = 0; i < text->length; i++) {
                wchar_t wc = 0;
                size_t width = mbrtowc(&wc, cursor, MB_CUR_MAX, &state);
                if (width == (size_t)-1 || width == (size_t)-2 || width == 0) {
                    inserted[count++] = 0xfffd; cursor++; memset(&state, 0, sizeof(state));
                } else { inserted[count++] = (uint32_t)wc; cursor += width; }
            }
        }
    } else if (text && text->feedback) {
        /* Feedback-only changes leave the displayed string intact. */
        active->w_preedit_caret = change->caret < 0 ? 0 : (unsigned)change->caret;
        emit_preedit(); active = previous; return;
    }
    unsigned first = (unsigned)change->chg_first, removed = (unsigned)change->chg_length;
    unsigned old = active->w_preedit_count;
    if (old - removed + count > 256) { active = previous; return; }
    memmove(active->w_preedit + first + count, active->w_preedit + first + removed,
            (old - first - removed) * sizeof(uint32_t));
    memcpy(active->w_preedit + first, inserted, count * sizeof(uint32_t));
    active->w_preedit_count = old - removed + count;
    active->w_preedit_caret = change->caret < 0 ? 0 : (unsigned)change->caret;
    emit_preedit(); active = previous;
}
static void preedit_caret(XIM im, XPointer client, XPointer call) {
    (void)im;
    XIMPreeditCaretCallbackStruct *caret = (XIMPreeditCaretCallbackStruct *)call;
    if (!caret) return;
    XWindowState *previous = active; active = (XWindowState *)client;
    active->w_preedit_caret = caret->position < 0 ? 0 : (unsigned)caret->position;
    emit_preedit(); active = previous;
}
static void preedit_done(XIM im, XPointer client, XPointer call) {
    (void)im; (void)call;
    XWindowState *previous = active; active = (XWindowState *)client;
    clear_preedit(); active = previous;
}
#ifdef HMJE_X11_TEST
void hmje_posix_test_preedit_wide(const wchar_t *value, unsigned count,
                                 int first, int removed, int caret) {
    if (!ensure_window()) abort();
    XIMText text = {0}; text.length = (unsigned short)count;
    text.encoding_is_wchar = True; text.string.wide_char = (wchar_t *)value;
    XIMPreeditDrawCallbackStruct change = {caret, first, removed, value ? &text : NULL};
    preedit_draw(xim, (XPointer)active, (XPointer)&change);
}
void hmje_posix_test_preedit_utf8(const char *value, unsigned count,
                                 int first, int removed, int caret) {
    if (!ensure_window()) abort();
    XIMText text = {0}; text.length = (unsigned short)count;
    text.encoding_is_wchar = False; text.string.multi_byte = (char *)value;
    XIMPreeditDrawCallbackStruct change = {caret, first, removed, value ? &text : NULL};
    preedit_draw(xim, (XPointer)active, (XPointer)&change);
}
void hmje_posix_test_preedit_done(void) {
    preedit_done(xim, (XPointer)active, NULL);
}
#endif
static u64 pointer_event(unsigned kind, int x, int y, unsigned button, int pressed) {
    return KIND(kind) | ((u64)(x < 0 ? 0 : x) & 0xffffff) |
        (((u64)(y < 0 ? 0 : y) & 0xffffff) << 24) |
        ((u64)button << 48) | (pressed ? (1ULL << 55) : 0);
}
/* XDND v5: local file URIs, UTF-8 text and PNG/JPEG are admitted. The host keeps
 * the same six-u64 record as AppKit. File paths are NFC before delivery. */
static void drop_enqueue(XWindowState *state, u64 kind, u64 phase,
                         const unsigned char *data, size_t length) {
    if (!state || !kind || length > (8u << 20) || state->id == 0) return;
    DropRecord *record = malloc(sizeof(*record) + 48 + length);
    if (!record) return;
    record->next = NULL; record->length = 48 + length;
    u64 header[6] = {kind, phase, state->w_drag_x, state->w_drag_y, state->id, length};
    memcpy(record->bytes, header, 48);
    if (length) memcpy(record->bytes + 48, data, length);
    if (state->w_drop_tail) state->w_drop_tail->next = record;
    else state->w_drop_head = record;
    state->w_drop_tail = record;
    state->w_drop_count++;
    if (state->w_drop_count > 256) {
        DropRecord *old = state->w_drop_head;
        state->w_drop_head = old->next;
        free(old);
        state->w_drop_count--;
    }
}
static void xdnd_reply(Window source, Atom message, Window destination, long flags) {
    if (!source) return;
    XClientMessageEvent reply = {0};
    reply.type = ClientMessage; reply.display = display; reply.window = source;
    reply.message_type = message; reply.format = 32;
    reply.data.l[0] = (long)destination; reply.data.l[1] = flags;
    if (message == xdnd_status) reply.data.l[4] = (long)xdnd_action_copy;
    else reply.data.l[2] = (long)xdnd_action_copy;
    XSendEvent(display, source, False, NoEventMask, (XEvent *)&reply);
    XFlush(display);
}
static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static unsigned char *uri_paths(const unsigned char *input, size_t length, size_t *out_length) {
    unsigned char *out = malloc(length + 1);
    if (!out) return NULL;
    size_t used = 0;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && input[end] != '\n' && input[end] != '\r') end++;
        size_t path = start;
        if (end >= start + 7 && !memcmp(input + start, "file://", 7)) {
            path = start + 7;
            if (path + 9 <= end && !memcmp(input + path, "localhost", 9)) path += 9;
            if (path < end && input[path] == '/') {
                size_t mark = used;
                for (size_t i = path; i < end; i++) {
                    unsigned char c = input[i];
                    if (c == '%' && i + 2 < end) {
                        int hi = hex_digit(input[i + 1]), lo = hex_digit(input[i + 2]);
                        if (hi >= 0 && lo >= 0) { c = (unsigned char)((hi << 4) | lo); i += 2; }
                    }
                    if (c == 0) { used = mark; break; }
                    out[used++] = c;
                }
                if (used > mark) out[used++] = 0;
            }
        }
        start = end;
        while (start < length && (input[start] == '\n' || input[start] == '\r')) start++;
    }
    *out_length = used;
    return out;
}

/* libunistring is loaded at runtime so non-drop X11 apps do not gain a link
 * dependency. If it is absent, reject file drops instead of breaking the NFC
 * path contract. Each output segment remains NUL separated. */
static unsigned char *nfc_paths(unsigned char *paths, size_t *length) {
    typedef unsigned char *(*normalize_fn)(const void *, const unsigned char *, size_t,
                                           unsigned char *, size_t *);
    static normalize_fn normalize;
    static const void *nfc;
    static int loaded;
    if (!loaded) {
        const char *names[] = {"libunistring.so.5", "libunistring.so.2",
                               "libunistring.so.6", "libunistring.so"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            void *library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (!library) continue;
            normalize_fn candidate = (normalize_fn)dlsym(library, "u8_normalize");
            const void *form = dlsym(library, "uninorm_nfc");
            if (candidate && form) {
                normalize = candidate;
                nfc = form;
                break;
            }
            dlclose(library);
        }
        loaded = 1;
    }
    if (!normalize) { free(paths); return NULL; }
    size_t capacity = *length + 1, used = 0;
    unsigned char *result = malloc(capacity);
    if (!result) { free(paths); return NULL; }
    for (size_t start = 0; start < *length;) {
        size_t end = start;
        while (end < *length && paths[end]) end++;
        size_t normalized_length = 0;
        unsigned char *normalized = normalize(nfc, paths + start, end - start,
                                               NULL, &normalized_length);
        if (!normalized || normalized_length > (8u << 20) ||
            used + normalized_length + 1 > (8u << 20)) {
            free(normalized); free(result); free(paths); return NULL;
        }
        if (used + normalized_length + 1 > capacity) {
            size_t next = used + normalized_length + 1;
            unsigned char *grown = realloc(result, next);
            if (!grown) { free(normalized); free(result); free(paths); return NULL; }
            result = grown; capacity = next;
        }
        memcpy(result + used, normalized, normalized_length);
        used += normalized_length;
        result[used++] = 0;
        free(normalized);
        start = end + 1;
    }
    free(paths);
    *length = used;
    return result;
}
#ifdef HMJE_X11_TEST
unsigned char *hmje_posix_test_nfc_paths(unsigned char *paths, size_t *length) {
    return nfc_paths(paths, length);
}
#endif
static void xdnd_clear_transfer(XWindowState *state) {
    free(state->w_drag_bytes);
    state->w_drag_bytes = NULL;
    state->w_drag_length = 0;
    state->w_drag_data_type = None;
    state->w_drag_incremental = 0;
}
static void xdnd_finish_transfer(XWindowState *state, Atom type,
                                 const unsigned char *data, size_t length) {
    int accepted = 0;
    if (state->w_drag_kind == 1 && type == uri_list_atom) {
        size_t paths_length = 0;
        unsigned char *paths = uri_paths(data, length, &paths_length);
        if (paths && paths_length) paths = nfc_paths(paths, &paths_length);
        if (paths && paths_length) { drop_enqueue(state, 1, 4, paths, paths_length); accepted = 1; }
        free(paths);
    } else if (state->w_drag_kind == 2 &&
               (type == utf8_atom || type == plain_utf8_atom || type == XA_STRING)) {
        drop_enqueue(state, 2, 4, data, length); accepted = 1;
    } else if (state->w_drag_kind == 3 && (type == png_atom || type == jpeg_atom)) {
        drop_enqueue(state, 3, 4, data, length); accepted = 1;
    }
    xdnd_reply(state->w_drag_source, xdnd_finished, state->w_window, accepted ? 1 : 0);
    xdnd_clear_transfer(state);
    state->w_drag_source = 0; state->w_drag_kind = 0; state->w_drag_target = None;
}
static void xdnd_selection_ready(XWindowState *state, const XSelectionEvent *event) {
    if (!state->w_drag_kind || event->property != xdnd_property) {
        xdnd_finish_transfer(state, None, NULL, 0); return;
    }
    Atom type = None; int format = 0; unsigned long items = 0, after = 0;
    unsigned char *data = NULL;
    int ok = XGetWindowProperty(display, state->w_window, xdnd_property, 0,
        (8u << 20) / 4, False, AnyPropertyType, &type, &format, &items, &after, &data) == Success;
    if (ok && type == incr_atom && format == 32 && items >= 1) {
        unsigned long announced = ((unsigned long *)data)[0];
        if (announced <= (8u << 20)) {
            state->w_drag_incremental = 1;
            XDeleteProperty(display, state->w_window, xdnd_property);
            XFlush(display);
        } else xdnd_finish_transfer(state, None, NULL, 0);
    } else if (ok && format == 8 && after == 0 && items <= (8u << 20)) {
        xdnd_finish_transfer(state, type, data, (size_t)items);
        XDeleteProperty(display, state->w_window, xdnd_property);
    } else {
        xdnd_finish_transfer(state, None, NULL, 0);
        XDeleteProperty(display, state->w_window, xdnd_property);
    }
    if (data) XFree(data);
}
static void xdnd_incremental_ready(XWindowState *state) {
    Atom type = None; int format = 0; unsigned long items = 0, after = 0;
    unsigned char *data = NULL;
    int ok = XGetWindowProperty(display, state->w_window, xdnd_property, 0,
        (8u << 20) / 4, True, AnyPropertyType, &type, &format, &items, &after, &data) == Success;
    if (!ok || format != 8 || after || items > (8u << 20) - state->w_drag_length ||
        (items && state->w_drag_data_type != None && type != state->w_drag_data_type)) {
        XDeleteProperty(display, state->w_window, xdnd_property);
        xdnd_finish_transfer(state, None, NULL, 0);
    } else if (!items) {
        xdnd_finish_transfer(state, state->w_drag_data_type,
                             state->w_drag_bytes, state->w_drag_length);
    } else {
        unsigned char *grown = realloc(state->w_drag_bytes, state->w_drag_length + items);
        if (!grown) xdnd_finish_transfer(state, None, NULL, 0);
        else {
            state->w_drag_bytes = grown;
            memcpy(grown + state->w_drag_length, data, items);
            state->w_drag_length += items;
            state->w_drag_data_type = type;
        }
    }
    if (data) XFree(data);
}
static void xdnd_message(XWindowState *state, const XClientMessageEvent *event) {
    if (event->message_type == xdnd_enter) {
        xdnd_clear_transfer(state);
        state->w_drag_source = (Window)event->data.l[0];
        state->w_drag_kind = 0; state->w_drag_target = None;
        state->w_drag_entered = 0;
        Atom offered[3] = {(Atom)event->data.l[2], (Atom)event->data.l[3], (Atom)event->data.l[4]};
        Atom *list = offered; unsigned long count = 3;
        unsigned char *allocated = NULL;
        if (event->data.l[1] & 1) {
            Atom type = None; int format = 0; unsigned long after = 0;
            if (XGetWindowProperty(display, state->w_drag_source, xdnd_type_list, 0, 256,
                                   False, XA_ATOM, &type, &format, &count, &after, &allocated) == Success &&
                format == 32 && allocated) list = (Atom *)allocated;
            else count = 0;
        }
        for (unsigned long i = 0; i < count; i++)
            if (list[i] == uri_list_atom) { state->w_drag_kind = 1; state->w_drag_target = uri_list_atom; break; }
        if (!state->w_drag_kind) for (unsigned long i = 0; i < count; i++)
            if (list[i] == png_atom || list[i] == jpeg_atom) {
                state->w_drag_kind = 3; state->w_drag_target = list[i]; break;
            }
        if (!state->w_drag_kind) for (unsigned long i = 0; i < count; i++)
            if (list[i] == utf8_atom || list[i] == plain_utf8_atom) {
                state->w_drag_kind = 2; state->w_drag_target = list[i]; break;
            }
        if (allocated) XFree(allocated);
    } else if (event->message_type == xdnd_position) {
        int root_x = (short)((event->data.l[2] >> 16) & 0xffff);
        int root_y = (short)(event->data.l[2] & 0xffff);
        Window child = None; int x = 0, y = 0;
        XTranslateCoordinates(display, DefaultRootWindow(display), state->w_window,
                              root_x, root_y, &x, &y, &child);
        state->w_drag_x = (u64)(x < 0 ? 0 : x);
        state->w_drag_y = (u64)(y < 0 ? 0 : y);
        drop_enqueue(state, state->w_drag_kind, state->w_drag_entered ? 2 : 1, NULL, 0);
        state->w_drag_entered = 1;
        xdnd_reply(state->w_drag_source, xdnd_status, state->w_window,
                   state->w_drag_kind ? 1 : 0);
    } else if (event->message_type == xdnd_leave) {
        drop_enqueue(state, state->w_drag_kind, 3, NULL, 0);
        xdnd_clear_transfer(state);
        state->w_drag_source = 0; state->w_drag_kind = 0; state->w_drag_target = None;
    } else if (event->message_type == xdnd_drop) {
        if (!state->w_drag_kind) {
            xdnd_reply(state->w_drag_source, xdnd_finished, state->w_window, 0);
            return;
        }
        XConvertSelection(display, xdnd_selection, state->w_drag_target, xdnd_property,
                          state->w_window, (Time)event->data.l[2]);
        XFlush(display);
    }
}
static void pointer_sample(unsigned phase, int x, int y, unsigned button, int sx, int sy) {
    hmje_pointer_record record = {0};
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    record.phase = phase; record.pointer_id = 1; record.x = x; record.y = y;
    record.time_us = (u64)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    record.changed_button = button; record.buttons = buttons_down;
    record.scroll_x = sx; record.scroll_y = sy; record.tool = HMJE_POINTER_TOOL_MOUSE;
    record.flags = HMJE_POINTER_CAP_BUTTONS |
        (phase == HMJE_POINTER_PHASE_SCROLL ? HMJE_POINTER_CAP_SCROLL : 0);
    hmje_pointer_ring_push(&pointers, record);
}
static void url_pump(void);
static void pump(void) {
    url_pump();
    if (!display) return;
    while (XPending(display)) {
        XEvent e; XNextEvent(display, &e);
        if (e.type == MappingNotify) { XRefreshKeyboardMapping(&e.xmapping); continue; }
        if (e.type == SelectionRequest || e.type == SelectionClear ||
            (e.type == SelectionNotify && e.xselection.requestor == selection_window)) {
            selection_event(&e); continue;
        }
        XWindowState *previous = active;
        XWindowState *target = find_window(e.xany.window);
        if (!target) {
            /* XIM transports (including IBus) use a private protocol window.
             * Its ClientMessages must reach Xlib even though it is not an app
             * window; otherwise the input context never enables conversion. */
            XFilterEvent(&e, None);
            continue;
        }
        active = target;
        /* XIM can consume focus events; keep the per-window state either way. */
        if (e.type == FocusIn) target->key_active = 1;
        if (e.type == FocusOut) target->key_active = 0;
        int was_preedit = target->w_preedit_count > 0;
        if (XFilterEvent(&e, cur_window)) { active = previous; continue; }
        switch (e.type) {
        case PropertyNotify:
            if (target->w_drag_incremental && e.xproperty.atom == xdnd_property &&
                e.xproperty.state == PropertyNewValue)
                xdnd_incremental_ready(target);
            break;
        case SelectionNotify:
            if (e.xselection.selection == xdnd_selection) xdnd_selection_ready(target, &e.xselection);
            break;
        case ConfigureNotify:
            resize_surface((unsigned)e.xconfigure.width, (unsigned)e.xconfigure.height);
            hmje_persist_frame(target->id, e.xconfigure.x, e.xconfigure.y,
                               e.xconfigure.width, e.xconfigure.height);
            break;
        case Expose: if (e.xexpose.count == 0) present(); break;
        case ClientMessage:
            if (e.xclient.message_type == xdnd_enter ||
                e.xclient.message_type == xdnd_position ||
                e.xclient.message_type == xdnd_leave ||
                e.xclient.message_type == xdnd_drop)
                xdnd_message(target, &e.xclient);
            else if ((Atom)e.xclient.data.l[0] == wm_delete)
                push(window_events_aware ? KIND(9) | 1 : KIND(6));
            break;
        case FocusIn: target->key_active = 1; if (xic) XSetICFocus(xic); if (window_events_aware) push(KIND(9) | 2); break;
        case FocusOut: target->key_active = 0; if (xic) XUnsetICFocus(xic); if (window_events_aware) push(KIND(9) | 3); break;
        case MotionNotify:
            pointer_sample(buttons_down ? HMJE_POINTER_PHASE_MOVE : HMJE_POINTER_PHASE_HOVER,
                           e.xmotion.x, e.xmotion.y, 0, 0, 0);
            push(pointer_event(1, e.xmotion.x, e.xmotion.y, 0, 0)); break;
        case ButtonPress: case ButtonRelease: {
            unsigned b = e.xbutton.button;
            if (b >= 4 && b <= 7) {
                if (e.type == ButtonPress) {
                    int sx = b == 6 ? -1 : b == 7 ? 1 : 0;
                    int sy = b == 4 ? -1 : b == 5 ? 1 : 0;
                    pointer_sample(HMJE_POINTER_PHASE_SCROLL, e.xbutton.x, e.xbutton.y, 0, sx, sy);
                    push(KIND(3) | 1 | ((b == 5 || b == 7) ? (1ULL << 55) : 0));
                }
            } else {
                unsigned mapped = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : b;
                if (mapped < 64) {
                    if (e.type == ButtonPress) buttons_down |= 1ULL << mapped;
                    else buttons_down &= ~(1ULL << mapped);
                }
                pointer_sample(e.type == ButtonPress ? HMJE_POINTER_PHASE_DOWN : HMJE_POINTER_PHASE_UP,
                               e.xbutton.x, e.xbutton.y, mapped, 0, 0);
                push(pointer_event(1, e.xbutton.x, e.xbutton.y, 0, 0));
                push(pointer_event(2, e.xbutton.x, e.xbutton.y, mapped, e.type == ButtonPress));
            }
            break;
        }
        case KeyPress: case KeyRelease: {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            unsigned scan = e.xkey.keycode >= 8 ? e.xkey.keycode - 8 : e.xkey.keycode;
            if (sym == XK_Tab) scan = 250; else if (sym == XK_Return || sym == XK_KP_Enter) scan = 251;
            else if (sym == XK_space) scan = 252; else if (sym == XK_comma) scan = 253;
            else if (sym == XK_Caps_Lock) scan = 254;
            /* X11's set-1 slash (53) collides with macOS Escape in HmjeUI. */
            else if (sym == XK_slash) scan = 255;
            u64 mods = 0; unsigned state = e.xkey.state;
            if (state & ShiftMask) mods |= 1ULL << 16;
            if (state & ControlMask) mods |= (1ULL << 17) | (1ULL << 20);
            if (state & Mod1Mask) mods |= 1ULL << 18;
            if (state & Mod4Mask) mods |= 1ULL << 19;
            int down = e.type == KeyPress;
            if (scan != 251) push((scan & 255) | (down ? 256 : 0) | mods);
            if (down && !(state & ControlMask)) {
                char buf[256]; KeySym unused; Status status; int n;
                if (xic) n = Xutf8LookupString(xic, &e.xkey, buf, sizeof(buf), &unused, &status);
                else n = XLookupString(&e.xkey, buf, sizeof(buf), &unused, NULL);
                if (n > 0) push_text(buf, n);
            }
            /* Enter actions can distinguish IME commit from ordinary Return. */
            if (scan == 251)
                push((scan & 255) | (down ? 256 : 0) | mods |
                     ((down && was_preedit) ? (1ULL << 21) : 0));
            break;
        }
        }
        active = previous;
    }
    if (pending_activation_id) {
        XWindowState *target = find_id(pending_activation_id);
        if (!target || !target->w_window || target->closing) {
            pending_activation_id = 0;
        } else {
            XWindowAttributes attributes;
            /* A window manager maps a new/restored client asynchronously. X11
             * rejects focus on an unmapped client with fatal BadMatch. */
            if (XGetWindowAttributes(display, target->w_window, &attributes) &&
                attributes.map_state == IsViewable) {
                XSetInputFocus(display, target->w_window, RevertToParent, CurrentTime);
                pending_activation_id = 0;
                XFlush(display);
            }
        }
    }
}
i64 hmje_posix_framebuffer_info(i64 address, i64 length) {
    if (!ensure_window()) return ENOSYS_;
    if (!address || length < 48) return -22;
    u64 *out = (u64 *)(uintptr_t)address;
    out[0] = cur_width; out[1] = cur_height; out[2] = (u64)cur_width * 4;
    out[3] = 4; out[4] = 1; out[5] = 1;
    if (length >= 56) { out[6] = 1; return 56; }
    return 48;
}
i64 hmje_posix_framebuffer_blit(i64 address, i64 w, i64 h, i64 pitch, i64 x, i64 y) {
    if (!ensure_window() || !pixels) return ENOSYS_;
    if (!address || w <= 0 || h <= 0 || pitch < w * 4 || x < 0 || y < 0) return 0;
    if ((u64)x >= cur_width || (u64)y >= cur_height) return 0;
    if (w > (i64)cur_width - x) w = cur_width - x;
    if (h > (i64)cur_height - y) h = cur_height - y;
    for (i64 row = 0; row < h; row++)
        memcpy((char *)pixels + ((size_t)y + row) * cur_width * 4 + (size_t)x * 4,
               (char *)(uintptr_t)address + row * pitch, (size_t)w * 4);
    return w * h * 4;
}
i64 hmje_posix_framebuffer_present(i64 rects, i64 count) {
    (void)rects; if (!ensure_window()) return ENOSYS_;
    pump(); if (count <= 0) return 0; present(); return 1;
}
i64 hmje_posix_input_poll(void) {
    if (!ensure_window()) return 0;
    pump(); if (resize_pending) { resize_pending = 0;
        return (i64)(KIND(5) | (u64)cur_width | ((u64)cur_height << 24) | (1ULL << 48)); }
    return (i64)pop();
}
i64 hmje_posix_input_poll2(i64 address, i64 length) {
    if (!ensure_window()) return 0;
    pump(); return hmje_pointer_ring_pop(&pointers, address, length);
}
i64 hmje_posix_gpu_command(i64 kind, i64 address, i64 length) {
    if (kind == 0 && address && length == 32) {
        u64 *out = (u64 *)(uintptr_t)address;
        out[0] = 2; out[1] = 4; out[2] = 0; out[3] = 1; return 32;
    }
    return ENOSYS_;
}
i64 hmje_posix_ui_window_config(i64 address, i64 length, i64 flags) {
    (void)flags;
    if (length < 0 || length > 4096 || (!address && length)) return -22;
    char *next = calloc((size_t)length + 1, 1); if (!next) return -12;
    if (length) memcpy(next, (void *)(uintptr_t)address, length);
    free(title); title = next; if (cur_window) set_window_title(cur_window, title);
    return 0;
}
i64 hmje_posix_text_input_control(i64 request) { (void)request; return 1; }
i64 hmje_posix_text_input_control2(i64 version, i64 client, i64 flags) {
    if (version == HMJE_TEXT_INPUT_CARET_RECT) return 1;
    int result = hmje_text_input_validate((u64)version, (u64)client, (u64)flags);
    if (result) return result;
    return ((u64)flags & HMJE_TEXT_INPUT_SECURE) ? ENOSYS_ : 1;
}
i64 hmje_posix_ui_clipboard(i64 operation, i64 address, i64 length) {
    if (operation == 0) {
        if (!ensure_display()) return ENOSYS_;
        unsigned char *text = NULL;
        size_t count = 0;
        Atom selections[] = {clipboard_atom, primary_atom};
        for (unsigned s = 0; s < 2 && !text; s++) {
            Window owner = XGetSelectionOwner(display, selections[s]);
            if (owner == None) continue;
            if (owner == selection_window && selections[s] == clipboard_atom) {
                text = clipboard; count = clipboard_length; break;
            }
            Atom formats[] = {utf8_atom, XA_STRING};
            for (unsigned f = 0; f < 2 && !text; f++) {
                free(selection_result); selection_result = NULL;
                selection_result_length = 0; selection_finished = 0;
                XConvertSelection(display, selections[s], formats[f], selection_property,
                                  selection_window, CurrentTime);
                XFlush(display);
                uint64_t deadline = now_ms() + 1000;
                while (!selection_finished && now_ms() < deadline) {
                    pump();
                    if (!selection_finished) {
                        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
                    }
                }
                if (selection_result) {
                    text = selection_result; count = selection_result_length;
                }
            }
        }
        if (text && address && length >= 0 && (u64)length >= count && count)
            memcpy((void *)(uintptr_t)address, text, count);
        return (i64)count;
    }
    if (operation == 1 && length >= 0 && length < 10000000 && (address || !length)) {
        if (!ensure_display()) return ENOSYS_;
        unsigned char *next = calloc((size_t)length + 1, 1); if (!next) return -12;
        if (length) memcpy(next, (void *)(uintptr_t)address, length);
        free(clipboard); clipboard = next; clipboard_length = (size_t)length;
        XSetSelectionOwner(display, clipboard_atom, selection_window, CurrentTime);
        XFlush(display);
        return XGetSelectionOwner(display, clipboard_atom) == selection_window ? length : -5;
    }
    return -22;
}

i64 hmje_posix_ui_drag_drop(i64 operation, i64 address, i64 capacity) {
    if (!ensure_window()) return ENOSYS_;
    DropRecord *head = active->w_drop_head;
    if (operation == 0) return head ? (i64)head->length : 0;
    if (operation != 1) return -22;
    if (!head) return 0;
    if (!address || capacity < (i64)head->length) return -34;
    memcpy((void *)(uintptr_t)address, head->bytes, head->length);
    i64 length = (i64)head->length;
    active->w_drop_head = head->next;
    if (!active->w_drop_head) active->w_drop_tail = NULL;
    active->w_drop_count--;
    free(head);
    return length;
}

/* HmjeUI keeps each window's view tree on its own stack. ucontext switches
 * these cooperative stacks on the X11 thread, so Xlib and XIM stay serialized. */
static XWindowState *find_id(u64 id) {
    for (XWindowState *state = window_states; state; state = state->next)
        if (state->id == id) return state;
    return NULL;
}
static unsigned live_count(void) {
    unsigned count = 0;
    for (XWindowState *state = window_states; state; state = state->next)
        if (state->w_window && state->state != 4) count++;
    return count;
}
static void destroy_active_window(void) {
    if (!cur_window) return;
    if (active->w_drag_source)
        xdnd_reply(active->w_drag_source, xdnd_finished, cur_window, 0);
    xdnd_clear_transfer(active);
    if (xic) { XDestroyIC(xic); xic = NULL; }
    if (gc) { XFreeGC(display, gc); gc = 0; }
    Window xid = cur_window; cur_window = 0;
    XDestroyWindow(display, xid); XFlush(display);
    free(pixels); pixels = NULL;
    free(title); title = NULL;
    free(pointers.grown_records); pointers.grown_records = NULL;
    while (active->w_drop_head) {
        DropRecord *next = active->w_drop_head->next;
        free(active->w_drop_head); active->w_drop_head = next;
    }
    active->w_drop_tail = NULL;
    active->w_drop_count = 0;
    close_sent = 1;
}
static void reap_dead(void) {
    XWindowState **link = &window_states;
    while (*link) {
        XWindowState *state = *link;
        if (state != &main_state && state->state == 4) {
            *link = state->next;
            hmje_guarded_stack_release(&state->stack); free(state);
        } else link = &state->next;
    }
}
static void sleep_ms(uint64_t milliseconds) {
    struct timespec delay = {(time_t)(milliseconds / 1000),
                             (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static void *(*entry_for(u64 address))(void *) {
    return (void *(*)(void *))(uintptr_t)address;
}
i64 hmje_posix_ui_window(i64 operation, i64 a, i64 b);
static void fiber_entry(int id) {
    XWindowState *state = find_id((u64)id);
    if (!state) abort();
    active = state;
    (void)state->entry(state->argument);
    (void)hmje_posix_ui_window(7, 0, 0);
    abort();
}
i64 hmje_posix_ui_window(i64 operation, i64 a, i64 b) {
    switch (operation) {
    case 0: /* QUERY: MULTIPLE | QUITTING */
        if (((u64)a & 1) != 0) window_events_aware = 1;
        return 1 | (quit_requested ? 4 : 0);
    case 1: { /* OPEN */
        if (!a || b < 80) return -22;
        const u64 *descriptor = (const u64 *)(uintptr_t)a;
        if (!descriptor[7] || descriptor[1] > 16384 || descriptor[2] > 16384 ||
            descriptor[6] > 4096 || (descriptor[6] && !descriptor[5])) return -22;
        XWindowState *opener = active;
        XWindowState *child = calloc(1, sizeof(*child));
        if (!child) return -12;
        child->id = next_window_id++;
        child->w_width = descriptor[1] ? (unsigned)descriptor[1] : 1180;
        child->w_height = descriptor[2] ? (unsigned)descriptor[2] : 860;
        child->w_title = calloc((size_t)descriptor[6] + 1, 1);
        int stack_error = hmje_guarded_stack_allocate(&child->stack, 8 * 1024 * 1024, 0);
        if (!child->w_title || stack_error != 0) {
            free(child->w_title); hmje_guarded_stack_release(&child->stack); free(child);
            return stack_error != 0 ? -(i64)stack_error : -12;
        }
        if (descriptor[6]) memcpy(child->w_title, (void *)(uintptr_t)descriptor[5], (size_t)descriptor[6]);
        child->entry = entry_for(descriptor[7]);
        child->argument = (void *)(uintptr_t)descriptor[8];
        child->opener = opener;
        active = child;
        if (!ensure_window()) { active = opener; free(child->w_title); hmje_guarded_stack_release(&child->stack); free(child); return ENOSYS_; }
        if ((descriptor[9] & 1) != 0)
            XMoveWindow(display, cur_window, (int)(i64)descriptor[3], (int)(i64)descriptor[4]);
        getcontext(&child->context);
        child->context.uc_stack.ss_sp = child->stack.usable;
        child->context.uc_stack.ss_size = child->stack.usable_size;
        child->context.uc_link = &main_state.context;
        makecontext(&child->context, (void (*)(void))fiber_entry, 1, (int)child->id);
        child->state = 1;
        opener->state = 3;
        swapcontext(&opener->context, &child->context);
        active = opener; opener->state = 1;
        return (i64)child->id;
    }
    case 2: { /* CLOSE */
        XWindowState *target = a ? find_id((u64)a) : active;
        if (!target || !target->w_window) return -2;
        if (!target->closing) {
            XWindowAttributes frame;
            if (XGetWindowAttributes(display, target->w_window, &frame))
                hmje_persist_frame(target->id, frame.x, frame.y, frame.width, frame.height);
            hmje_persist_close(target->id, quit_requested);
            target->closing = 1;
            XUnmapWindow(display, target->w_window);
            XWindowState *previous = active; active = target;
            push(KIND(9) | 4); active = previous;
        }
        return 0;
    }
    case 3: return active->w_window ? (i64)active->id : 0; /* CURRENT */
    case 4: { /* POLICY */
        u64 previous = last_window_policy;
        if (a == 1 || a == 2) last_window_policy = (u64)a;
        return (i64)previous;
    }
    case 5: return (i64)live_count();
    case 6: { /* ACTIVATE */
        XWindowState *target = find_id((u64)a);
        if (!target || !target->w_window || target->closing) return -2;
        pending_activation_id = target->id;
        XMapRaised(display, target->w_window);
        XFlush(display); return 0;
    }
    case 13: { /* SET_TITLE */
        XWindowState *target = find_id((u64)a);
        if (!target || !target->w_window || target->closing || !b) return -22;
        const u64 *value = (const u64 *)(uintptr_t)b;
        if (value[1] > 4096 || (value[1] && !value[0])) return -22;
        char *copy = calloc((size_t)value[1] + 1, 1);
        if (!copy) return -12;
        if (value[1]) memcpy(copy, (const void *)(uintptr_t)value[0], (size_t)value[1]);
        free(target->w_title);
        target->w_title = copy;
        set_window_title(target->w_window, copy);
        XFlush(display);
        return 0;
    }
    case 14: { /* APP_STATE */
        if (!display || !live_count()) return 3;
        Window focused = None;
        int revert = 0;
        XGetInputFocus(display, &focused, &revert);
        for (XWindowState *state = window_states; state; state = state->next) {
            if (!state->w_window || state->closing) continue;
            if (state->w_window == focused) return 1;
        }
        return 2;
    }
    case 15: { /* IS_OPEN */
        XWindowState *target = find_id((u64)a);
        return target && target->w_window && !target->closing ? 1 : 0;
    }
    case 21: { /* WINDOW_STATE */
        XWindowState *target = find_id((u64)a);
        if (!display || !target || !target->w_window || target->closing) return 2;
        return target->key_active ? 1 : 2;
    }
    case 16: { /* FIT_CONTENT: EWMH work area excludes panels/taskbars */
        if (!display || !active->w_window || !a) return -22;
        const u64 *size = (const u64 *)(uintptr_t)a;
        if (size[0] > 2 || size[1] > 2) return -22;
        if (active->had_saved_frame && (size[0] == 2 || size[1] == 2)) return 0;
        int screen = DefaultScreen(display);
        long left = 0, top = 0;
        unsigned work_w = (unsigned)DisplayWidth(display, screen);
        unsigned work_h = (unsigned)DisplayHeight(display, screen);
        Atom work_atom = XInternAtom(display, "_NET_WORKAREA", True);
        if (work_atom != None) {
            Atom actual; int format; unsigned long count, after; unsigned char *data = NULL;
            if (XGetWindowProperty(display, RootWindow(display, screen), work_atom, 0, 4, False,
                                   XA_CARDINAL, &actual, &format, &count, &after, &data) == Success &&
                format == 32 && count >= 4 && data != NULL) {
                unsigned long *v = (unsigned long *)data;
                left = (long)v[0]; top = (long)v[1];
                if (v[2] > 0 && v[2] <= 16384) work_w = (unsigned)v[2];
                if (v[3] > 0 && v[3] <= 16384) work_h = (unsigned)v[3];
            }
            if (data) XFree(data);
        }
        unsigned width = cur_width, height = cur_height;
        if (active == &main_state && size[0] == 2) width = work_w;
        else if (size[0] != 2 && size[2] > 0) width = (unsigned)size[2];
        if (active == &main_state && size[1] == 2) height = work_h;
        else if (size[1] != 2 && size[3] > 0) height = (unsigned)size[3];
        if (width == 0 || height == 0 || width > 16384 || height > 16384) return -22;
        XWindowAttributes attributes;
        XGetWindowAttributes(display, cur_window, &attributes);
        int x = active == &main_state && !active->had_saved_frame ? (int)(left + ((long)work_w - width) / 2) : attributes.x;
        int y = active == &main_state && !active->had_saved_frame ? (int)(top + ((long)work_h - height) / 2) : attributes.y;
        XMoveResizeWindow(display, cur_window, x, y, width, height);
        hmje_persist_frame(active->id, x, y, (int)width, (int)height);
        XSizeHints hints = {0};
        if (size[0] != 2 && size[1] != 2) {
            hints.flags = PMinSize | PMaxSize;
            hints.min_width = hints.max_width = (int)width;
            hints.min_height = hints.max_height = (int)height;
        }
        XSetWMNormalHints(display, cur_window, &hints);
        resize_surface(width, height);
        XFlush(display);
        return 0;
    }
    case 17: { /* BIND */
        XWindowState *target = find_id((u64)a);
        if (!target || !target->w_window || !b) return -22;
        const u64 *data = (const u64 *)(uintptr_t)b;
        if (!data[0] || !data[1] || data[1] > HMJE_PERSIST_MAX_TEXT || data[3] > HMJE_PERSIST_MAX_TEXT ||
            (data[3] && !data[2])) return -22;
        hmje_persist_record *record = hmje_persist_bind(target->id, (const char *)(uintptr_t)data[0],
            (size_t)data[1], (const char *)(uintptr_t)data[2], (size_t)data[3], data[4] != 0);
        if (!record) return -12;
        if (record->has_frame && record->width > 0 && record->height > 0 &&
            record->width <= 16384 && record->height <= 16384) {
            target->had_saved_frame = 1;
            XMoveResizeWindow(display, target->w_window, record->x, record->y,
                              (unsigned)record->width, (unsigned)record->height);
            XWindowState *previous = active; active = target;
            resize_surface((unsigned)record->width, (unsigned)record->height);
            active = previous;
        }
        if (getenv("HMJE_WINDOW_PERSIST_DEBUG"))
            fprintf(stderr, "x11-window: frame id=%llu restored=%d %d,%d %dx%d\n",
                    (unsigned long long)target->id, target->had_saved_frame,
                    record->x, record->y, record->width, record->height);
        return 0;
    }
    case 18: { /* RESTORE */
        if (!a) return -22;
        const u64 *data = (const u64 *)(uintptr_t)a;
        if (!data[0] || !data[1] || data[1] > HMJE_PERSIST_MAX_TEXT || !data[3]) return -22;
        return hmje_persist_restore((const char *)(uintptr_t)data[0], (size_t)data[1],
            (uint32_t)data[2], (char *)(uintptr_t)data[3], (size_t)data[4]);
    }
    case 7: { /* FINISH */
        XWindowState *me = active;
        destroy_active_window();
        /* The HMJE main-window loop hands remaining window fibers to FINISH.
         * Keep the process alive after a launcher transfers to a project. */
        if (me == &main_state) return hmje_posix_ui_window(8, 0, 0);
        me->state = 4;
        active = &main_state;
        swapcontext(&me->context, &main_state.context);
        abort();
    }
    case 8: { /* RUN */
        if (active != &main_state) return -22;
        while (live_count()) {
            pump(); reap_dead();
            uint64_t now = now_ms();
            XWindowState *ready = NULL;
            for (XWindowState *state = window_states; state; state = state->next) {
                if (state == &main_state || state->state != 2) continue;
                if (state->deadline_ms <= now || state->w_event_head != state->w_event_tail ||
                    state->w_resize_pending) { ready = state; break; }
            }
            if (!ready) { sleep_ms(1); continue; }
            ready->state = 1; active = ready;
            swapcontext(&main_state.context, &ready->context);
            active = &main_state;
        }
        reap_dead(); return 0;
    }
    case 9: quit_requested = 0; return 0; /* QUIT_CANCEL */
    case 10: /* QUIT */
        quit_requested = 1;
        for (XWindowState *state = window_states; state; state = state->next) {
            if (!state->w_window) continue;
            XWindowState *previous = active; active = state;
            push(KIND(9) | 1 | 256); active = previous;
        }
        return 0;
    case 12: { /* STARTED */
        XWindowState *me = active, *opener = me->opener;
        if (me == &main_state || !opener) return -22;
        me->opener = NULL; me->state = 2; me->deadline_ms = 0;
        opener->state = 1; active = opener;
        swapcontext(&me->context, &opener->context);
        active = me; return 0;
    }
    case 11: { /* WAIT */
        if (a < 0) return -22;
        if (active == &main_state) {
            pump();
            /* The first window's UI loop owns the main context. Additional
             * windows must run during its idle waits, not only after FINISH
             * enters RUN (which would leave them blank until it closes). */
            uint64_t now = now_ms();
            XWindowState *ready = NULL;
            for (XWindowState *state = window_states; state; state = state->next) {
                if (state == &main_state || state->state != 2) continue;
                if (state->deadline_ms <= now || state->w_event_head != state->w_event_tail ||
                    state->w_resize_pending) { ready = state; break; }
            }
            if (ready) {
                ready->state = 1;
                active = ready;
                swapcontext(&main_state.context, &ready->context);
                active = &main_state;
            } else sleep_ms((uint64_t)a);
            return 0;
        }
        XWindowState *me = active;
        me->state = 2; me->deadline_ms = now_ms() + (uint64_t)a;
        active = &main_state;
        swapcontext(&me->context, &main_state.context);
        active = me;
        return (me->w_event_head != me->w_event_tail || me->w_resize_pending ? 4 : 0) | 8;
    }
    default: return ENOSYS_;
    }
}

#include "posix_host_x11_url.inc"
