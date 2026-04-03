/*
 * fprintd_dbus.h – D-Bus helpers for communicating with fprintd
 *
 * Provides functions to:
 *   - Connect to the system D-Bus and obtain an fprintd device
 *   - Check whether the current user has enrolled fingerprints
 *   - Start / stop fingerprint verification
 *   - Retrieve the D-Bus file descriptor for poll()-based event loops
 *   - Process pending D-Bus messages and extract verify results
 */

#ifndef FPRINTD_DBUS_H
#define FPRINTD_DBUS_H

#include <dbus/dbus.h>
#include <stdbool.h>

/* Context for an fprintd session */
typedef struct fprintd_ctx {
    DBusConnection *conn;
    char           *device_path;   /* e.g. /net/reactivated/Fprint/Device/0 */
    bool            claimed;       /* true after Claim(), false after Release() */
    bool            verify_active; /* true between VerifyStart / VerifyStop  */
} fprintd_ctx;

/* Result of a single poll-cycle check */
typedef enum {
    FP_RESULT_PENDING,      /* no result yet, keep polling              */
    FP_RESULT_MATCH,        /* fingerprint matched                      */
    FP_RESULT_NO_MATCH,     /* fingerprint did not match                */
    FP_RESULT_ERROR,        /* communication / device error             */
} fp_result_t;

/*
 * fprintd_open – connect to system D-Bus, claim the default device.
 * Returns 0 on success, -1 on failure (fprintd not running, etc.).
 */
int fprintd_open(fprintd_ctx *ctx);

/*
 * fprintd_has_enrolled_prints – check whether `username` has at least
 * one enrolled fingerprint on the claimed device.
 * Returns  1  if yes,
 *          0  if no enrolled prints,
 *         -1  on D-Bus error.
 */
int fprintd_has_enrolled_prints(fprintd_ctx *ctx, const char *username);

/*
 * fprintd_verify_start – ask fprintd to begin verification for `username`.
 * The finger type is "any" so all enrolled fingers are accepted.
 * Returns 0 on success, -1 on failure.
 */
int fprintd_verify_start(fprintd_ctx *ctx, const char *username);

/*
 * fprintd_verify_stop – cancel / finish an ongoing verification.
 * Safe to call even if no verification is active.
 */
void fprintd_verify_stop(fprintd_ctx *ctx);

/*
 * fprintd_get_fd – return the file descriptor that should be monitored
 * with poll() / select() for incoming D-Bus messages.
 * Returns -1 if the connection is not open.
 */
int fprintd_get_fd(fprintd_ctx *ctx);

/*
 * fprintd_poll_result – call after poll() indicates the D-Bus fd is
 * readable.  Dispatches pending messages and returns the verification
 * result (or FP_RESULT_PENDING if no result yet).
 */
fp_result_t fprintd_poll_result(fprintd_ctx *ctx);

/*
 * fprintd_close – release the device, disconnect from D-Bus, free memory.
 */
void fprintd_close(fprintd_ctx *ctx);

#endif /* FPRINTD_DBUS_H */
