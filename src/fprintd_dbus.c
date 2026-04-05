/*
 * fprintd_dbus.c – D-Bus helpers for communicating with fprintd
 *
 * Uses libdbus-1 (the reference C binding) to talk to fprintd over
 * the system bus.  All method calls are synchronous.  Signal reception
 * is driven by the caller's poll() loop via fprintd_get_fd() and
 * fprintd_poll_result().
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

/* Timeout for synchronous D-Bus method calls (ms) */
#define DBUS_CALL_TIMEOUT    5000

/* ── Static helpers ───────────────────────────────────────────────── */

/*
 * get_default_device – call Manager.GetDefaultDevice() and store the
 * returned object path in ctx->device_path.
 * Returns 0 on success, -1 on failure.
 */
static int get_default_device(fprintd_ctx *ctx)
{
    DBusMessage *msg = dbus_message_new_method_call(
        FPRINT_SERVICE,
        FPRINT_MANAGER_PATH,
        FPRINT_MANAGER_IFACE,
        "GetDefaultDevice");
    if (!msg) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: failed to create GetDefaultDevice message");
        return -1;
    }

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
    dbus_message_unref(msg);

    if (!reply) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: GetDefaultDevice failed: %s",
               err.message);
        dbus_error_free(&err);
        return -1;
    }

    const char *path = NULL;
    if (!dbus_message_get_args(reply, &err,
                               DBUS_TYPE_OBJECT_PATH, &path,
                               DBUS_TYPE_INVALID)) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: GetDefaultDevice bad reply: %s",
               err.message);
        dbus_error_free(&err);
        dbus_message_unref(reply);
        return -1;
    }

    ctx->device_path = strdup(path);
    dbus_message_unref(reply);

    if (!ctx->device_path) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: strdup failed for device path");
        return -1;
    }

    return 0;
}

/*
 * release_device – call Device.Release(). Safe to call if not claimed.
 */
static void release_device(fprintd_ctx *ctx)
{
    if (!ctx->claimed || !ctx->conn || !ctx->device_path)
        return;

    DBusMessage *msg = dbus_message_new_method_call(
        FPRINT_SERVICE,
        ctx->device_path,
        FPRINT_DEVICE_IFACE,
        "Release");
    if (!msg)
        return;

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
    dbus_message_unref(msg);

    if (reply)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&err))
        dbus_error_free(&err);

    ctx->claimed = false;
}

/*
 * add_verify_signal_match – register interest in VerifyStatus signals
 * so they arrive on our connection.
 */
static void add_verify_signal_match(fprintd_ctx *ctx)
{
    DBusError err;
    dbus_error_init(&err);

    char rule[512];
    snprintf(rule, sizeof(rule),
             "type='signal',"
             "sender='%s',"
             "interface='%s',"
             "member='VerifyStatus',"
             "path='%s'",
             FPRINT_SERVICE,
             FPRINT_DEVICE_IFACE,
             ctx->device_path);

    dbus_bus_add_match(ctx->conn, rule, &err);
    if (dbus_error_is_set(&err)) {
        syslog(LOG_AUTH | LOG_WARNING,
               "pam_fprint_fixed: add_match failed: %s", err.message);
        dbus_error_free(&err);
    }

    /* Flush so the match is active on the bus before we start verifying */
    dbus_connection_flush(ctx->conn);
}

/*
 * remove_verify_signal_match – unregister the VerifyStatus match.
 */
static void remove_verify_signal_match(fprintd_ctx *ctx)
{
    DBusError err;
    dbus_error_init(&err);

    char rule[512];
    snprintf(rule, sizeof(rule),
             "type='signal',"
             "sender='%s',"
             "interface='%s',"
             "member='VerifyStatus',"
             "path='%s'",
             FPRINT_SERVICE,
             FPRINT_DEVICE_IFACE,
             ctx->device_path);

    dbus_bus_remove_match(ctx->conn, rule, &err);
    if (dbus_error_is_set(&err))
        dbus_error_free(&err);

    dbus_connection_flush(ctx->conn);
}

/* ── Public API ───────────────────────────────────────────────────── */

int fprintd_open(fprintd_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));

    DBusError err;
    dbus_error_init(&err);

    ctx->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!ctx->conn) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: cannot connect to system bus: %s",
               err.message);
        dbus_error_free(&err);
        return -1;
    }

    /*
     * Do NOT let libdbus call exit() if the connection drops.
     * We handle errors ourselves.
     */
    dbus_connection_set_exit_on_disconnect(ctx->conn, FALSE);

    if (get_default_device(ctx) < 0) {
        dbus_connection_unref(ctx->conn);
        ctx->conn = NULL;
        return -1;
    }

    return 0;
}

int fprintd_has_enrolled_prints(fprintd_ctx *ctx, const char *username)
{
    if (!ctx->conn || !ctx->device_path)
        return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        FPRINT_SERVICE,
        ctx->device_path,
        FPRINT_DEVICE_IFACE,
        "ListEnrolledFingers");
    if (!msg)
        return -1;

    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &username,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
    dbus_message_unref(msg);

    if (!reply) {
        /*
         * "NoEnrolledPrints" is a well-known fprintd error, not a
         * communication failure – treat it as "no prints" (return 0).
         */
        if (dbus_error_is_set(&err) &&
            strstr(err.message, "NoEnrolledPrints")) {
            dbus_error_free(&err);
            return 0;
        }
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: ListEnrolledFingers failed: %s",
               err.message);
        dbus_error_free(&err);
        return -1;
    }

    /* The reply is an array of strings (finger names).  We only care
     * whether the array is non-empty. */
    DBusMessageIter iter, sub;
    dbus_message_iter_init(reply, &iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &sub);
    int has_prints = (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID);

    dbus_message_unref(reply);
    return has_prints ? 1 : 0;
}

int fprintd_verify_start(fprintd_ctx *ctx, const char *username)
{
    if (!ctx->conn || !ctx->device_path)
        return -1;

    /* Claim the device first (pass username so fprintd knows who) */
    if (!ctx->claimed) {
        /* We need to claim with the actual username for verification */
        DBusMessage *msg = dbus_message_new_method_call(
            FPRINT_SERVICE,
            ctx->device_path,
            FPRINT_DEVICE_IFACE,
            "Claim");
        if (!msg)
            return -1;

        dbus_message_append_args(msg,
                                 DBUS_TYPE_STRING, &username,
                                 DBUS_TYPE_INVALID);

        DBusError err;
        dbus_error_init(&err);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
            ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
        dbus_message_unref(msg);

        if (!reply) {
            syslog(LOG_AUTH | LOG_ERR,
                   "pam_fprint_fixed: Claim(%s) failed: %s",
                   username, err.message);
            dbus_error_free(&err);
            return -1;
        }
        dbus_message_unref(reply);
        ctx->claimed = true;
    }

    /* Register for VerifyStatus signals before starting verification */
    add_verify_signal_match(ctx);

    /* Call VerifyStart("any") – accept any enrolled finger */
    DBusMessage *msg = dbus_message_new_method_call(
        FPRINT_SERVICE,
        ctx->device_path,
        FPRINT_DEVICE_IFACE,
        "VerifyStart");
    if (!msg) {
        remove_verify_signal_match(ctx);
        return -1;
    }

    const char *finger = "any";
    dbus_message_append_args(msg,
                             DBUS_TYPE_STRING, &finger,
                             DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
    dbus_message_unref(msg);

    if (!reply) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: VerifyStart failed: %s",
               err.message);
        dbus_error_free(&err);
        remove_verify_signal_match(ctx);
        return -1;
    }

    dbus_message_unref(reply);
    ctx->verify_active = true;
    return 0;
}

void fprintd_verify_stop(fprintd_ctx *ctx)
{
    if (!ctx->verify_active || !ctx->conn || !ctx->device_path)
        return;

    DBusMessage *msg = dbus_message_new_method_call(
        FPRINT_SERVICE,
        ctx->device_path,
        FPRINT_DEVICE_IFACE,
        "VerifyStop");
    if (!msg)
        goto out;

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        ctx->conn, msg, DBUS_CALL_TIMEOUT, &err);
    dbus_message_unref(msg);

    if (reply)
        dbus_message_unref(reply);
    if (dbus_error_is_set(&err))
        dbus_error_free(&err);

out:
    ctx->verify_active = false;
    remove_verify_signal_match(ctx);
}

int fprintd_get_fd(fprintd_ctx *ctx)
{
    if (!ctx->conn)
        return -1;

    int fd = -1;

    /*
     * libdbus doesn't expose a single "give me the fd" call, but the
     * watch API lets us find it.  For the simple single-threaded case
     * we use dbus_connection_get_unix_fd().
     */
    if (!dbus_connection_get_unix_fd(ctx->conn, &fd))
        return -1;

    return fd;
}

fp_result_t fprintd_poll_result(fprintd_ctx *ctx)
{
    if (!ctx->conn || !ctx->verify_active)
        return FP_RESULT_ERROR;

    /*
     * Drain all pending messages.  We are looking for a VerifyStatus
     * signal whose args are (string result, boolean done).
     *
     * Known result values from fprintd:
     *   "verify-match"            → success
     *   "verify-no-match"         → wrong finger
     *   "verify-retry-scan"       → bad scan, try again
     *   "verify-swipe-too-short"  → swipe too short
     *   "verify-finger-not-centered" → off-center
     *   "verify-remove-and-retry" → remove and retry
     *   "verify-disconnected"     → device gone
     *   "verify-unknown-error"    → ???
     */

    fp_result_t result = FP_RESULT_PENDING;

    /* Process all available data without blocking */
    dbus_connection_read_write(ctx->conn, 0);

    DBusMessage *msg;
    while ((msg = dbus_connection_pop_message(ctx->conn)) != NULL) {

        if (dbus_message_is_signal(msg,
                                   FPRINT_DEVICE_IFACE,
                                   "VerifyStatus")) {
            const char *status = NULL;
            dbus_bool_t done   = FALSE;
            DBusError err;
            dbus_error_init(&err);

            if (dbus_message_get_args(msg, &err,
                                      DBUS_TYPE_STRING,  &status,
                                      DBUS_TYPE_BOOLEAN, &done,
                                      DBUS_TYPE_INVALID)) {

                syslog(LOG_AUTH | LOG_DEBUG,
                       "pam_fprint_fixed: VerifyStatus: %s (done=%d)",
                       status, (int)done);

                if (strcmp(status, "verify-match") == 0) {
                    result = FP_RESULT_MATCH;
                } else if (strcmp(status, "verify-no-match") == 0) {
                    result = FP_RESULT_NO_MATCH;
                } else if (strcmp(status, "verify-disconnected") == 0 ||
                           strcmp(status, "verify-unknown-error") == 0) {
                    result = FP_RESULT_ERROR;
                }
                /* retry-scan, swipe-too-short, etc. → stay PENDING,
                 * fprintd will keep the reader active for another try */

            } else {
                if (dbus_error_is_set(&err))
                    dbus_error_free(&err);
                result = FP_RESULT_ERROR;
            }
        }

        dbus_message_unref(msg);

        /* If we got a definitive answer, stop processing */
        if (result != FP_RESULT_PENDING)
            break;
    }

    return result;
}

void fprintd_close(fprintd_ctx *ctx)
{
    if (!ctx)
        return;

    /* Stop verification if still running */
    if (ctx->verify_active)
        fprintd_verify_stop(ctx);

    /* Release the device */
    release_device(ctx);

    /* Free device path */
    if (ctx->device_path) {
        free(ctx->device_path);
        ctx->device_path = NULL;
    }

    /* Disconnect from D-Bus */
    if (ctx->conn) {
        dbus_connection_unref(ctx->conn);
        ctx->conn = NULL;
    }
}
