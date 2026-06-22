#define _GNU_SOURCE
#include "moonraker.h"
#include "cJSON.h"
#include "lodepng.h"

#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

/* Objects we ask Moonraker for in a single query. */
#define QUERY_PATH \
    "/printer/objects/query?extruder&heater_bed&print_stats&display_status&fan"

static printer_status_t g_status;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static char             g_host[128];
static int              g_port    = 7125;
static int              g_poll_ms = 1000;

/* ---- minimal HTTP/1.0 GET over a raw socket --------------------------- *
 * Moonraker speaks plain HTTP on localhost, so no TLS / libcurl needed.
 * HTTP/1.0 makes the server close the connection when done, so we just
 * read until EOF (no keep-alive, no chunked transfer-encoding to handle).
 * Returns a malloc'd response BODY (NUL-terminated for text convenience);
 * when out_len != NULL it receives the exact body length (binary-safe).   */
static char *http_get(const char *host, int port, const char *path,
                      size_t *out_len)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res); close(fd); return NULL;
    }
    freeaddrinfo(res);

    char req[768];
    int reqlen = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    for (int sent = 0; sent < reqlen; ) {
        ssize_t w = send(fd, req + sent, reqlen - sent, 0);
        if (w <= 0) { close(fd); return NULL; }
        sent += (int)w;
    }

    size_t cap = 8192, len = 0;
    char  *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    ssize_t r;
    while ((r = recv(fd, buf + len, cap - len - 1, 0)) > 0) {
        len += (size_t)r;
        if (cap - len < 2048) {
            char *p = realloc(buf, cap *= 2);
            if (!p) { free(buf); close(fd); return NULL; }
            buf = p;
        }
    }
    close(fd);

    /* split headers/body (binary-safe; body may contain NUL bytes) */
    char *hdr_end = memmem(buf, len, "\r\n\r\n", 4);
    if (!hdr_end) { free(buf); return NULL; }
    size_t off = (size_t)(hdr_end - buf) + 4;
    size_t blen = len - off;

    char *out = malloc(blen + 1);
    if (!out) { free(buf); return NULL; }
    memcpy(out, buf + off, blen);
    out[blen] = '\0';
    free(buf);

    if (out_len) *out_len = blen;
    return out;
}

/* ---- JSON helpers ----------------------------------------------------- */
static double get_num(const cJSON *obj, const char *key, double dflt)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : dflt;
}

/* percent-encode a string for use in a URL (path separators kept as-is) */
static void urlencode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in;
         *p && o + 4 < outsz; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* Target box the thumbnail is fit into (matches the UI's left column). */
#define THUMB_BOX_W 200
#define THUMB_BOX_H 240

/* Thumbnails are transparent; composite them over the job-card colour so the
 * background blends in. Keep in sync with COL_CARD in ui.c (0x1d2430). */
#define THUMB_BG_R 0x1d
#define THUMB_BG_G 0x24
#define THUMB_BG_B 0x30

/* Bilinear-resize an RGBA8888 image into dst (runs once per print job). */
static void resize_rgba(const unsigned char *src, int sw, int sh,
                        unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        float fy = (dh > 1) ? (float)y * (sh - 1) / (dh - 1) : 0.0f;
        int y0 = (int)fy, y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
        float wy = fy - y0;
        for (int x = 0; x < dw; x++) {
            float fx = (dw > 1) ? (float)x * (sw - 1) / (dw - 1) : 0.0f;
            int x0 = (int)fx, x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
            float wx = fx - x0;
            for (int c = 0; c < 4; c++) {
                float v00 = src[(y0 * sw + x0) * 4 + c];
                float v01 = src[(y0 * sw + x1) * 4 + c];
                float v10 = src[(y1 * sw + x0) * 4 + c];
                float v11 = src[(y1 * sw + x1) * 4 + c];
                float top = v00 + (v01 - v00) * wx;
                float bot = v10 + (v11 - v10) * wx;
                dst[(y * dw + x) * 4 + c] =
                    (unsigned char)(top + (bot - top) * wy + 0.5f);
            }
        }
    }
}

/* Write an RGBA8888 image as an LVGL v9 RGB565 ".bin" (12-byte header +
 * little-endian pixel rows; stride = w*2 since LV_DRAW_BUF_STRIDE_ALIGN=1). */
static int write_bin_rgb565(const char *path, const unsigned char *rgba,
                            int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    unsigned stride = (unsigned)w * 2u;
    unsigned char hdr[12] = {
        0x19,                                   /* magic                   */
        0x12,                                   /* cf = RGB565             */
        0x00, 0x00,                             /* flags                   */
        (unsigned char)(w & 0xFF),      (unsigned char)((w >> 8) & 0xFF),
        (unsigned char)(h & 0xFF),      (unsigned char)((h >> 8) & 0xFF),
        (unsigned char)(stride & 0xFF), (unsigned char)((stride >> 8) & 0xFF),
        0x00, 0x00                              /* reserved                */
    };
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return -1; }

    int ok = 1;
    for (int i = 0; i < w * h && ok; i++) {
        unsigned r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        unsigned short px = (unsigned short)(((r & 0xF8) << 8) |
                                             ((g & 0xFC) << 3) |
                                             ( b >> 3));
        unsigned char le[2] = { (unsigned char)(px & 0xFF),
                                (unsigned char)(px >> 8) };
        if (fwrite(le, 1, 2, f) != 2) ok = 0;
    }
    fclose(f);
    return ok ? 0 : -1;
}

/* Look up the largest thumbnail for `filename` via the Moonraker metadata
 * endpoint, download it, downscale to the UI box, and store it as a ready-to-
 * blit LVGL RGB565 ".bin" in /tmp. Reports the path + final size. */
static int fetch_thumbnail(const char *filename, char *out_path, size_t pathsz,
                           int *out_w, int *out_h)
{
    char enc[512], path[640];
    urlencode(filename, enc, sizeof enc);
    snprintf(path, sizeof path, "/server/files/metadata?filename=%s", enc);

    char *json = http_get(g_host, g_port, path, NULL);
    if (!json) return -1;
    cJSON *root = cJSON_Parse(json);
    free(json);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *thumbs = cJSON_GetObjectItemCaseSensitive(result, "thumbnails");

    char rel[512] = "";
    int best = -1;
    cJSON *t;
    cJSON_ArrayForEach(t, thumbs) {
        int w = (int)get_num(t, "width", 0);
        int h = (int)get_num(t, "height", 0);
        cJSON *rp = cJSON_GetObjectItemCaseSensitive(t, "relative_path");
        if (cJSON_IsString(rp) && rp->valuestring && w * h > best) {
            best = w * h;
            snprintf(rel, sizeof rel, "%s", rp->valuestring);
        }
    }
    cJSON_Delete(root);
    if (!rel[0]) return -1;

    char rel_enc[640], url[800];
    urlencode(rel, rel_enc, sizeof rel_enc);
    snprintf(url, sizeof url, "/server/files/gcodes/%s", rel_enc);

    size_t png_len = 0;
    char *png = http_get(g_host, g_port, url, &png_len);
    if (!png || png_len == 0) { free(png); return -1; }

    /* decode PNG -> RGBA8888 */
    unsigned char *rgba = NULL;
    unsigned sw = 0, sh = 0;
    unsigned err = lodepng_decode32(&rgba, &sw, &sh,
                                    (const unsigned char *)png, png_len);
    free(png);
    if (err || !rgba || sw == 0 || sh == 0) { free(rgba); return -1; }

    /* Composite over the card colour at full resolution (before resizing) so
     * transparent areas match the card and edges don't fringe dark. */
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        unsigned a = rgba[i * 4 + 3], ia = 255u - a;
        rgba[i * 4 + 0] = (unsigned char)((rgba[i * 4 + 0] * a + THUMB_BG_R * ia + 127) / 255);
        rgba[i * 4 + 1] = (unsigned char)((rgba[i * 4 + 1] * a + THUMB_BG_G * ia + 127) / 255);
        rgba[i * 4 + 2] = (unsigned char)((rgba[i * 4 + 2] * a + THUMB_BG_B * ia + 127) / 255);
        rgba[i * 4 + 3] = 255;
    }

    /* fit within the UI box, preserving aspect ratio */
    int dw, dh;
    if ((int)sw * THUMB_BOX_H > (int)sh * THUMB_BOX_W) {
        dw = THUMB_BOX_W; dh = (int)sh * THUMB_BOX_W / (int)sw;
    } else {
        dh = THUMB_BOX_H; dw = (int)sw * THUMB_BOX_H / (int)sh;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    unsigned char *dst = malloc((size_t)dw * dh * 4);
    if (!dst) { free(rgba); return -1; }
    resize_rgba(rgba, (int)sw, (int)sh, dst, dw, dh);
    free(rgba);

    static unsigned seq = 0;
    static char prev[256] = "";
    char tmp[256];
    snprintf(tmp, sizeof tmp, "/tmp/klipper_thumb_%u.bin", ++seq);

    int wr = write_bin_rgb565(tmp, dst, dw, dh);
    free(dst);
    if (wr != 0) { unlink(tmp); return -1; }

    if (prev[0]) unlink(prev);          /* drop the previous job's thumb */
    snprintf(prev, sizeof prev, "%s", tmp);

    snprintf(out_path, pathsz, "%s", tmp);
    *out_w = dw; *out_h = dh;
    return 0;
}

static void parse_into(const char *json, printer_status_t *st)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *status = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "result"), "status");
    if (status) {
        cJSON *ext = cJSON_GetObjectItemCaseSensitive(status, "extruder");
        cJSON *bed = cJSON_GetObjectItemCaseSensitive(status, "heater_bed");
        cJSON *ps  = cJSON_GetObjectItemCaseSensitive(status, "print_stats");
        cJSON *ds  = cJSON_GetObjectItemCaseSensitive(status, "display_status");
        cJSON *fan = cJSON_GetObjectItemCaseSensitive(status, "fan");

        if (ext) {
            st->nozzle_temp   = get_num(ext, "temperature", st->nozzle_temp);
            st->nozzle_target = get_num(ext, "target",      st->nozzle_target);
        }
        if (bed) {
            st->bed_temp   = get_num(bed, "temperature", st->bed_temp);
            st->bed_target = get_num(bed, "target",      st->bed_target);
        }
        if (fan)
            st->fan_speed = get_num(fan, "speed", st->fan_speed);
        if (ps) {
            cJSON *state = cJSON_GetObjectItemCaseSensitive(ps, "state");
            cJSON *fn    = cJSON_GetObjectItemCaseSensitive(ps, "filename");
            cJSON *info  = cJSON_GetObjectItemCaseSensitive(ps, "info");
            if (cJSON_IsString(state) && state->valuestring)
                snprintf(st->state, sizeof st->state, "%s", state->valuestring);
            if (cJSON_IsString(fn) && fn->valuestring)
                snprintf(st->filename, sizeof st->filename, "%s", fn->valuestring);
            st->print_duration = get_num(ps, "print_duration", st->print_duration);
            /* layer counts are only present if the slicer emits them */
            st->layer_current = (int)get_num(info, "current_layer", 0);
            st->layer_total   = (int)get_num(info, "total_layer",   0);
        }
        if (ds) {
            st->progress = get_num(ds, "progress", st->progress);
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(ds, "message");
            if (cJSON_IsString(msg) && msg->valuestring)
                snprintf(st->message, sizeof st->message, "%s", msg->valuestring);
            else
                st->message[0] = '\0';   /* M117 with no text clears it */
        }

        /* progress-based ETA: total = elapsed / progress */
        if (st->progress > 0.001 && st->print_duration > 1.0)
            st->remaining = st->print_duration * (1.0 / st->progress - 1.0);
        else
            st->remaining = -1.0;

        st->online = true;
    }
    cJSON_Delete(root);
}

/* ---- polling thread --------------------------------------------------- */
static void *poll_loop(void *arg)
{
    (void)arg;
    char last_file[160] = "";
    for (;;) {
        printer_status_t local;
        pthread_mutex_lock(&g_lock);
        local = g_status;                 /* keep last values as defaults */
        pthread_mutex_unlock(&g_lock);

        char *body = http_get(g_host, g_port, QUERY_PATH, NULL);
        if (body) {
            local.online = false;
            parse_into(body, &local);
            free(body);
        } else {
            local.online = false;
        }

        /* (re)fetch the thumbnail only when the print file changes */
        if (strcmp(local.filename, last_file) != 0) {
            snprintf(last_file, sizeof last_file, "%s", local.filename);
            local.thumb_path[0] = '\0';
            local.thumb_w = local.thumb_h = 0;
            if (local.filename[0])
                fetch_thumbnail(local.filename, local.thumb_path,
                                sizeof local.thumb_path,
                                &local.thumb_w, &local.thumb_h);
        }

        pthread_mutex_lock(&g_lock);
        g_status = local;
        pthread_mutex_unlock(&g_lock);

        usleep(g_poll_ms * 1000);
    }
    return NULL;
}

/* ---- public API ------------------------------------------------------- */
void moonraker_start(const char *host, int port, int poll_ms)
{
    snprintf(g_host, sizeof g_host, "%s", host);
    g_port    = port    > 0 ? port    : 7125;
    g_poll_ms = poll_ms > 0 ? poll_ms : 1000;

    memset(&g_status, 0, sizeof g_status);
    snprintf(g_status.state, sizeof g_status.state, "connecting");

    pthread_t tid;
    pthread_create(&tid, NULL, poll_loop, NULL);
    pthread_detach(tid);
}

void moonraker_get(printer_status_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}
