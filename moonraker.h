#ifndef MOONRAKER_H
#define MOONRAKER_H

#include <stdbool.h>

/* Latest snapshot of printer status pulled from Moonraker. */
typedef struct {
    bool   online;          /* true if the last poll succeeded            */
    char   message[256];    /* m117 printer status message                */
    double nozzle_temp;     /* current hotend temperature  (deg C)        */
    double nozzle_target;   /* hotend target temperature   (deg C)        */
    double bed_temp;        /* current bed temperature     (deg C)        */
    double bed_target;      /* bed target temperature      (deg C)        */
    double fan_speed;       /* part cooling fan: 0.0 .. 1.0               */
    char   state[32];       /* print_stats.state: standby/printing/...    */
    char   filename[160];   /* currently loaded gcode filename            */
    double progress;        /* 0.0 .. 1.0                                 */
    double print_duration;  /* elapsed print time (seconds)               */
    double remaining;       /* estimated time left (s); <0 = unknown      */
    int    layer_current;   /* current layer (0 = unknown)                */
    int    layer_total;     /* total layers  (0 = unknown)                */
    char   thumb_path[256]; /* local fs path of downloaded thumb (or "")  */
    int    thumb_w;         /* source thumbnail width  (px)               */
    int    thumb_h;         /* source thumbnail height (px)               */
} printer_status_t;

/* Start the background polling thread. host/port point at Moonraker
 * (usually 127.0.0.1:7125 when this UI runs on the Klipper host). */
void moonraker_start(const char *host, int port, int poll_ms);

/* Thread-safe copy of the most recent status into *out. */
void moonraker_get(printer_status_t *out);

#endif /* MOONRAKER_H */
