// gcc -o test_xrandr test_xrandr.c -pthread -ldl

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

typedef struct _screen_data {
    uint8_t number;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} screen_data;

static unsigned char monitor_count = 0;
static screen_data *monitor_buffer = NULL;

static volatile sig_atomic_t shutdown_requested = 0;

static pthread_mutex_t xrandr_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t xrandr_cond = PTHREAD_COND_INITIALIZER;
static bool xrandr_ready = false;

// ---------------------------------------------------------------------------
// Dynamic loading of X11 / Xrandr
// ---------------------------------------------------------------------------

static void *libX11_handle = NULL;
static void *libXrandr_handle = NULL;

typedef Display *(*fn_XOpenDisplay)(const char *display_name);
typedef char *(*fn_XDisplayName)(const char *string);
typedef Window (*fn_XDefaultRootWindow)(Display *display);
typedef int (*fn_XPending)(Display *display);
typedef int (*fn_XNextEvent)(Display *display, XEvent *event_return);

typedef Bool (*fn_XRRQueryExtension)(Display *display, int *event_base, int *error_base);
typedef XRRScreenResources *(*fn_XRRGetScreenResources)(Display *display, Window window);
typedef void (*fn_XRRFreeScreenResources)(XRRScreenResources *resources);
typedef XRRCrtcInfo *(*fn_XRRGetCrtcInfo)(Display *display, XRRScreenResources *resources, RRCrtc crtc);
typedef void (*fn_XRRFreeCrtcInfo)(XRRCrtcInfo *crtc_info);
typedef void (*fn_XRRSelectInput)(Display *display, Window window, int mask);

static fn_XOpenDisplay           p_XOpenDisplay           = NULL;
static fn_XDisplayName           p_XDisplayName           = NULL;
static fn_XDefaultRootWindow     p_XDefaultRootWindow     = NULL;
static fn_XPending               p_XPending               = NULL;
static fn_XNextEvent             p_XNextEvent             = NULL;

static fn_XRRQueryExtension      p_XRRQueryExtension      = NULL;
static fn_XRRGetScreenResources  p_XRRGetScreenResources  = NULL;
static fn_XRRFreeScreenResources p_XRRFreeScreenResources = NULL;
static fn_XRRGetCrtcInfo         p_XRRGetCrtcInfo         = NULL;
static fn_XRRFreeCrtcInfo        p_XRRFreeCrtcInfo        = NULL;
static fn_XRRSelectInput         p_XRRSelectInput         = NULL;

#define LOAD_SYM(handle, name)                                                 \
    p_##name = (fn_##name) dlsym(handle, #name);                                \
    if (p_##name == NULL) {                                                    \
        fprintf(stderr, "dlsym failed for %s: %s\n", #name, dlerror());        \
        return -1;                                                             \
    }

static int init_x11_xrandr(void) {
    if (libX11_handle != NULL && libXrandr_handle != NULL) {
        return 0; // already initialized
    }

    dlerror(); // clear existing errors

    libX11_handle = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
    if (libX11_handle == NULL) {
        fprintf(stderr, "dlopen failed for libX11.so.6: %s\n", dlerror());
        return -1;
    }

    libXrandr_handle = dlopen("libXrandr.so", RTLD_LAZY | RTLD_LOCAL);
    if (libXrandr_handle == NULL) {
        fprintf(stderr, "dlopen failed for libXrandr.so: %s\n", dlerror());
        dlclose(libX11_handle);
        libX11_handle = NULL;
        return -1;
    }

    // X11 symbols
    LOAD_SYM(libX11_handle, XOpenDisplay);
    LOAD_SYM(libX11_handle, XDisplayName);
    LOAD_SYM(libX11_handle, XDefaultRootWindow);
    LOAD_SYM(libX11_handle, XPending);
    LOAD_SYM(libX11_handle, XNextEvent);

    // Xrandr symbols TODO XRRGetScreenInfo
    LOAD_SYM(libXrandr_handle, XRRQueryExtension);
    LOAD_SYM(libXrandr_handle, XRRGetScreenResources);
    LOAD_SYM(libXrandr_handle, XRRFreeScreenResources);
    LOAD_SYM(libXrandr_handle, XRRGetCrtcInfo);
    LOAD_SYM(libXrandr_handle, XRRFreeCrtcInfo);
    LOAD_SYM(libXrandr_handle, XRRSelectInput);

    return 0;
}

static void cleanup_x11_xrandr(void) {
    if (libXrandr_handle != NULL) {
        dlclose(libXrandr_handle);
        libXrandr_handle = NULL;
    }

    if (libX11_handle != NULL) {
        dlclose(libX11_handle);
        libX11_handle = NULL;
    }

    p_XOpenDisplay           = NULL;
    p_XDisplayName           = NULL;
    p_XDefaultRootWindow     = NULL;
    p_XPending               = NULL;
    p_XNextEvent             = NULL;
    p_XRRQueryExtension      = NULL;
    p_XRRGetScreenResources  = NULL;
    p_XRRFreeScreenResources = NULL;
    p_XRRGetCrtcInfo         = NULL;
    p_XRRFreeCrtcInfo        = NULL;
    p_XRRSelectInput         = NULL;
}

// ----------------------------------------------------------------及ひ
// Original logic using dynamically loaded functions
// ---------------------------------------------------------------------------

static int get_screen_resources(Display *display, Window root, XRRScreenResources **out_res, int *out_crtc_count) {
    *out_res = p_XRRGetScreenResources(display, root);
    if (*out_res == NULL) {
        fprintf(stderr, "%s [%u]: XRandR could not get screen resources!\n",
                __FUNCTION__, __LINE__);
        return -1;
    }

    int crtc_count = (*out_res)->ncrtc;
    if (crtc_count <= 0) {
        *out_crtc_count = 0;
        return 0;
    }

    if (crtc_count > UINT8_MAX) {
        fprintf(stderr, "%s [%u]: S        memset(&sa, 0, sizeof(sa));creen count overflow detected! Clamping to %u.\n",
                __FUNCTION__, __LINE__, UINT8_MAX);
        crtc_count = UINT8_MAX;
    }

    *out_crtc_count = crtc_count;
    return 0;
}

static uint8_t fill_monitors_from_crtcs(Display *display, XRRScreenResources *res, int crtc_count, screen_data *buffer) {
    uint8_t valid_count = 0;

    for (int i = 0; i < crtc_count; i++) {
        XRRCrtcInfo *crtc = p_XRRGetCrtcInfo(display, res, res->crtcs[i]);
        if (crtc == NULL) {
            fprintf(stderr, "%s [%u]: Failed to get CRTC info (%#lX)\n",
                    __FUNCTION__, __LINE__, res->crtcs[i]);
            continue;
        }

        if (crtc->width == 0 || crtc->height == 0) {
            p_XRRFreeCrtcInfo(crtc);
            continue;
        }

        buffer[valid_count] = (screen_data) {
            .number = valid_count + 1,
            .x      = (int16_t) crtc->x,
            .y      = (int16_t) crtc->y,
            .width  = (uint16_t) crtc->width,
            .height = (uint16_t) crtc->height
        };

        valid_count++;
        p_XRRFreeCrtcInfo(crtc);
    }

    return valid_count;
}

static int query_monitors(Display *display, Window root, screen_data **out_monitors, uint8_t *out_count) {
    *out_monitors = NULL;
    *out_count = 0;

    XRRScreenResources *res = NULL;
    int crtc_count = 0;

    int rc = get_screen_resources(display, root, &res, &crtc_count);
    if (rc != 0) {
        return rc;
    }

    if (crtc_count == 0) {
        if (res != NULL) {
            p_XRRFreeScreenResources(res);
        }
        return 0;
    }

    screen_data *tmp = malloc(sizeof(screen_data) * crtc_count);
    if (tmp == NULL) {
        fprintf(stderr, "%s [%u]: Failed to allocate monitor buffer!\n",
                __FUNCTION__, __LINE__);
        p_XRRFreeScreenResources(res);
        return -1;
    }

    uint8_t valid_count = fill_monitors_from_crtcs(display, res, crtc_count, tmp);
    p_XRRFreeScreenResources(res);

    if (valid_count == 0) {
        free(tmp);
        return 0;
    }

    *out_monitors = tmp;
    *out_count = valid_count;
    return 0;
}

static void replace_monitor_buffer(screen_data *new_buf, uint8_t new_count) {
    pthread_mutex_lock(&xrandr_mutex);

    free(monitor_buffer);
    monitor_buffer = new_buf;
    monitor_count = new_count;

    pthread_mutex_unlock(&xrandr_mutex);
}

static void update_monitor_buffer(Display *display, Window root) {
    screen_data *new_buf = NULL;
    uint8_t new_count = 0;

    if (query_monitors(display, root, &new_buf, &new_count) != 0) {
        free(new_buf);
        return;
    }

    replace_monitor_buffer(new_buf, new_count);
}

static void *xrandr_thread_proc(void *arg) {
    (void) arg;

    // Ensure libraries and symbols are loaded in this process before use.
    if (init_x11_xrandr() != 0) {
        fprintf(stderr, "%s [%u]: Failed to initialize X11/Xrandr via dlopen/dlsym!\n",
                __FUNCTION__, __LINE__);
        return NULL;
    }

    Display *display = p_XOpenDisplay(p_XDisplayName(NULL));
    if (display == NULL) {
        fprintf(stderr, "%s [%u]: XOpenDisplay failure!\n",
                __FUNCTION__, __LINE__);
        return NULL;
    }

    int event_base = 0;
    int error_base = 0;
    if (!p_XRRQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr, "%s [%u]: xRandR extension is not available!\n",
                __FUNCTION__, __LINE__);
        return NULL;
    }

    Window root = p_XDefaultRootWindow(display);
    update_monitor_buffer(display, root);

    pthread_mutex_lock(&xrandr_mutex);
    xrandr_ready = true;
    pthread_cond_broadcast(&xrandr_cond);
    pthread_mutex_unlock(&xrandr_mutex);

    p_XRRSelectInput(display, root,
                     RRScreenChangeNotifyMask |
                     RRCrtcChangeNotifyMask   |
                     RROutputChangeNotifyMask);

    while (!shutdown_requested) {
        while (p_XPending(display)) {
            XEvent ev;
            p_XNextEvent(display, &ev);

            fprintf(stderr, "X event received: type=%d\n", ev.type);

            update_monitor_buffer(display, root);
        }

        usleep(250 * 1000);
    }

    pthread_mutex_lock(&xrandr_mutex);
    xrandr_ready = false;
    pthread_mutex_unlock(&xrandr_mutex);

    return NULL;
}

static void handle_sigint(int sig) {
    shutdown_requested = 1;
}

int main() {
    struct sigaction sa = { 0 };
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);


    // Create the thread attribute.
    pthread_attr_t xrandr_thread_attr;
    pthread_attr_init(&xrandr_thread_attr);

    pthread_t settings_thread_id;
    if (pthread_create(&settings_thread_id, &xrandr_thread_attr, xrandr_thread_proc, NULL) != 0) {
        fprintf(stderr, "%s [%u]: Failed to create settings thread!\n",
                __FUNCTION__, __LINE__);

        return 3;
    }

    // Wait for the XRandR thread to start.
    pthread_mutex_lock(&xrandr_mutex);
    while (!xrandr_ready) {
        pthread_cond_wait(&xrandr_cond, &xrandr_mutex);
    }
    pthread_mutex_unlock(&xrandr_mutex);

    while (!shutdown_requested) {
        pthread_mutex_lock(&xrandr_mutex);

        printf("monitor(s) detected: %u\n", monitor_count);

        if (monitor_buffer != NULL) {
            for (int i = 0; i < monitor_count; i++) {
                fprintf(stdout, "\t%3u) %4u x %-4u (%5d, %-5d)\n",
                    monitor_buffer[i].number,
                    monitor_buffer[i].width, monitor_buffer[i].height,
                    monitor_buffer[i].x, monitor_buffer[i].y);
            }
        }

        pthread_mutex_unlock(&xrandr_mutex);

        usleep(5 * 1000 * 1000);
    }

    pthread_join(settings_thread_id, NULL);
    printf("Cleanup complete.\n");

    pthread_attr_destroy(&xrandr_thread_attr);

    cleanup_x11_xrandr();
    return 0;
}
