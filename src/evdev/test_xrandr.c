// gcc -o test_xrandr test_xrandr.c -pthread -lX11 -lXrandr

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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


static int get_screen_resources(Display *display, Window root, XRRScreenResources **out_res, int *out_crtc_count) {
    *out_res = XRRGetScreenResources(display, root);
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
        fprintf(stderr, "%s [%u]: Screen count overflow detected! Clamping to %u.\n",
                __FUNCTION__, __LINE__, UINT8_MAX);
        crtc_count = UINT8_MAX;
    }

    *out_crtc_count = crtc_count;
    return 0;
}

static uint8_t fill_monitors_from_crtcs(Display *display, XRRScreenResources *res, int crtc_count, screen_data *buffer) {
    uint8_t valid_count = 0;

    for (int i = 0; i < crtc_count; i++) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(display, res, res->crtcs[i]);
        if (crtc == NULL) {
            fprintf(stderr, "%s [%u]: Failed to get CRTC info (%#lX)\n",
                    __FUNCTION__, __LINE__, res->crtcs[i]);
            continue;
        }

        if (crtc->width == 0 || crtc->height == 0) {
            XRRFreeCrtcInfo(crtc);
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
        XRRFreeCrtcInfo(crtc);
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
            XRRFreeScreenResources(res);
        }
        return 0;
    }

    screen_data *tmp = malloc(sizeof(screen_data) * crtc_count);
    if (tmp == NULL) {
        fprintf(stderr, "%s [%u]: Failed to allocate monitor buffer!\n",
                __FUNCTION__, __LINE__);
        XRRFreeScreenResources(res);
        return -1;
    }

    uint8_t valid_count = fill_monitors_from_crtcs(display, res, crtc_count, tmp);
    XRRFreeScreenResources(res);

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
    Display *display = XOpenDisplay(XDisplayName(NULL));
    if (display == NULL) {
        fprintf(stderr, "%s [%u]: XOpenDisplay failure!\n",
                __FUNCTION__, __LINE__);
        return NULL;
    }

    int event_base = 0;
    int error_base = 0;
    if (!XRRQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr, "%s [%u]: xRandR extension is not available!\n",
                __FUNCTION__, __LINE__);
        return NULL;
    }

    Window root = XDefaultRootWindow(display);
    update_monitor_buffer(display, root);

    pthread_mutex_lock(&xrandr_mutex);
    xrandr_ready = true;
    pthread_cond_broadcast(&xrandr_cond);
    pthread_mutex_unlock(&xrandr_mutex);


    XRRSelectInput(display, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);

    while (!shutdown_requested) {
        while (XPending(display)) {
            XEvent ev;
            XNextEvent(display, &ev);

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
    (void) sig;
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

    return 0;
}
