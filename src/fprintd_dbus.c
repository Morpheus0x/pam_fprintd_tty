/*
 * fprintd_dbus.c – D-Bus helpers for communicating with fprintd
 *
 * Uses libdbus-1 (the reference C binding) to talk to fprintd over
 * the system bus.  All functions are synchronous except for signal
 * handling which is driven by the caller's poll() loop.
 */

#include "fprintd_dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <dbus/dbus.h>

/* ── D-Bus constants ──────────────────────────────────────────────── */

#define FPRINT_SERVICE       "net.reactivated.Fprint"
#define FPRINT_MANAGER_PATH  "/net/reactivated/Fprint/Manager"
#define FPRINT_MANAGER_IFACE "net.reactivated.Fprint.Manager"
#define FPRINT_DEVICE_IFACE  "net.reactivated.Fprint.Device"

/* ── Forward declarations ─────────────────────────────────────────── */

static int  get_default_device(fprintd_ctx *ctx);

/* ── Implementation stubs (to be filled in task 2) ────────────────── */

int fprintd_open(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
    return -1;
}

int fprintd_has_enrolled_prints(fprintd_ctx *ctx, const char *username)
{
    /* TODO: implement in task 2 */
    (void)ctx; (void)username;
    return -1;
}

int fprintd_verify_start(fprintd_ctx *ctx, const char *username)
{
    /* TODO: implement in task 2 */
    (void)ctx; (void)username;
    return -1;
}

void fprintd_verify_stop(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
}

int fprintd_get_fd(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
    return -1;
}

fp_result_t fprintd_poll_result(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
    return FP_RESULT_ERROR;
}

void fprintd_close(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
}

/* ── Static helpers ───────────────────────────────────────────────── */

static int get_default_device(fprintd_ctx *ctx)
{
    /* TODO: implement in task 2 */
    (void)ctx;
    return -1;
}
