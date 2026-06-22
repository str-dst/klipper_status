#include "lvgl/lvgl.h"
#include "ui.h"
#include "moonraker.h"

#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

/* LVGL tick source (LV_USE_OS == LV_OS_NONE). */
static uint32_t tick_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return v ? atoi(v) : dflt;
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    const char *fbdev = getenv("LV_LINUX_FBDEV_DEVICE");
    if (!fbdev) fbdev = "/dev/fb0";

    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fbdev);

    ui_init();

    const char *host = getenv("MOONRAKER_HOST");
    if (!host) host = "127.0.0.1";
    int port    = env_int("MOONRAKER_PORT", 7125);
    int poll_ms = env_int("MOONRAKER_POLL_MS", 1000);
    moonraker_start(host, port, poll_ms);

    while (1) {
        uint32_t idle = lv_timer_handler();
        if (idle > 16) idle = 16;          /* cap so the UI stays responsive */
        usleep(idle * 1000);
    }
    return 0;
}
