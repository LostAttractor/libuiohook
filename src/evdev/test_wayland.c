// Build: gcc -o test_wayland test_wayland.c -pthread -lwayland-client

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

static volatile sig_atomic_t running = 1;

static void handle_sigint(int signum) {
    running = 0;
}


struct wlOutput {
    int x;
    int y;

    int width;
    int height;

    uint32_t registry_name;  // Track the registry name
    struct wl_output *wl_output;  // Track the wl_output object
};


struct wlContext {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;

    /* Monitors */
    struct wlOutput *outputs;
    size_t count;
};



static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {
    struct wlOutput *output = data;
    if (!output) {
	    printf("!!! Output not found\n");
	    return;
    }

    output->x = x;
    output->y = y;
    printf("WL: output at position %d,%d\n", x, y);
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    struct wlOutput *output = data;
    bool preferred = flags & WL_OUTPUT_MODE_PREFERRED;
    bool current = flags & WL_OUTPUT_MODE_CURRENT;

    printf("WL: %smode: %dx%d@%d%s\n", current ? "current " : "", width, height, refresh, preferred ? "*" : "");
    if (!output) {
        printf("!!! Output not found in list\n");
        return;
    }

    if (current) {
        if (!preferred) {
	    printf("Not using preferred mode on output -- check config\n");
        }

        output->width = width;
        output->height = height;
    }
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    // Do Nothing
}

static void output_done(void *data, struct wl_output *wl_output) {
    struct wlOutput *output = data;
    if (!output) {
	    printf("!!! Output not found in list\n");
	    return;
    }

    printf("Output updated: %dx%d at %d, %d\n",
		    output->width,
		    output->height,
		    output->x,
		    output->y);
}

static struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct wlContext *ctx = data;

    if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wlOutput *outputs = ctx->outputs;

        if (outputs == NULL) {
            outputs = malloc(sizeof(struct wlOutput));
        } else {
            outputs = realloc(ctx->outputs, sizeof(struct wlOutput) * (ctx->count + 1));
        }

    	if (outputs) {
            // Received monitor information, setup new listeners
    	    struct wl_output *wl_output = wl_registry_bind(registry, name, &wl_output_interface, 2);
    	    outputs[ctx->count].registry_name = name;  // Store the registry name
    	    outputs[ctx->count].wl_output = wl_output;  // Store the wl_output object
            wl_output_add_listener(wl_output, &output_listener, &outputs[ctx->count++]);
    	    ctx->outputs = outputs;
        }
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct wlContext *ctx = data;

    // Find the output with this registry name
    for (size_t i = 0; i < ctx->count; i++) {
        if (ctx->outputs[i].registry_name == name) {
            printf("Output removed: %dx%d at %d, %d\n",
                   ctx->outputs[i].width,
                   ctx->outputs[i].height,
                   ctx->outputs[i].x,
                   ctx->outputs[i].y);

            // Clean up the wl_output object
            if (ctx->outputs[i].wl_output) {
                wl_output_destroy(ctx->outputs[i].wl_output);
            }

            // Remove from array by shifting remaining elements
            ctx->count--;
            if (i < ctx->count) {
                memcpy(&ctx->outputs[i], &ctx->outputs[i + 1], sizeof(struct wlOutput) * (ctx->count - i));
            }

            // Optionally realloc to shrink the array
            if (ctx->count > 0) {
                ctx->outputs = realloc(ctx->outputs, sizeof(struct wlOutput) * ctx->count);
            } else {
                free(ctx->outputs);
                ctx->outputs = NULL;
            }

            break;
        }
    }
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void *event_loop_thread(void *arg) {
    struct wlContext *ctx = arg;

    // Main event loop with interruptible dispatch
    int wl_fd = wl_display_get_fd(ctx->wl_display);
    struct pollfd fds = {
        .fd = wl_fd,
        .events = POLLIN,
    };

    while (running) {
        // Flush outgoing requests
        while (wl_display_prepare_read(ctx->wl_display) != 0) {
            if (wl_display_dispatch_pending(ctx->wl_display) == -1) {
                running = 0;
                break;
            }
        }

        if (!running) {
            wl_display_cancel_read(ctx->wl_display);
            break;
        }

        wl_display_flush(ctx->wl_display);

        // Poll with timeout to allow checking the running flag periodically
        int ret = poll(&fds, 1, 250); // 250ms timeout
        if (ret == -1) {
            wl_display_cancel_read(ctx->wl_display);
            if (errno == EINTR) {
                continue; // Signal interrupted, check running flag
            }
            perror("poll");
            break;
        }

        if (ret == 0) {
            // Timeout - no events
            wl_display_cancel_read(ctx->wl_display);
            continue;
        }

        // Events available
        wl_display_read_events(ctx->wl_display);
        if (wl_display_dispatch_pending(ctx->wl_display) == -1) {
            break;
        }
    }

    return NULL;
}

int main() {
    // Setup signal handler for Ctrl+C
    struct sigaction sa = { 0 };
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    // Connect to the display server
    struct wlContext ctx = { 0 };
    ctx.wl_display = wl_display_connect(NULL);
    if (ctx.wl_display == NULL) {
        fprintf(stderr, "Failed to connect to display.\n");
        exit(1);
    }

    // Add the listener and pass state as the void *data.
    ctx.wl_registry = wl_display_get_registry(ctx.wl_display);
    wl_registry_add_listener(ctx.wl_registry, &wl_registry_listener, &ctx);
    wl_display_roundtrip(ctx.wl_display);

    // Start the event loop in a separate thread
    pthread_t thread;
    if (pthread_create(&thread, NULL, event_loop_thread, &ctx) != 0) {
        fprintf(stderr, "Failed to create thread.\n");
        wl_registry_destroy(ctx.wl_registry);
        wl_display_disconnect(ctx.wl_display);
        exit(1);
    }

    // Main thread blocks waiting for the event loop thread to complete
    pthread_join(thread, NULL);

    printf("\nShutting down gracefully...\n");

    wl_registry_destroy(ctx.wl_registry);

    // Disconnect from the display server
    wl_display_disconnect(ctx.wl_display);

    return 0;
}
