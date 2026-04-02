/*
 * pam_fprint_fixed.c – PAM module for sequential fingerprint-then-password auth
 *
 * Flow:
 *   1. Check fprintd availability + enrolled prints via D-Bus
 *   2. If available: display prompt, start verification, poll() for
 *      fingerprint result or Enter key press
 *   3. On fingerprint match  → PAM_SUCCESS
 *   4. On Enter / timeout    → PAM_AUTHINFO_UNAVAIL  (fall through to pam_unix)
 *   5. If fprintd unavailable→ PAM_AUTHINFO_UNAVAIL  (immediate fall through)
 *
 * Module arguments:
 *   timeout=N   seconds to wait for fingerprint (default 10)
 *   debug       enable syslog debug messages
 */

#define PAM_SM_AUTH

#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "fprintd_dbus.h"

/* ── Defaults ─────────────────────────────────────────────────────── */

#define DEFAULT_TIMEOUT_SEC  10
#define PROMPT_MSG           "Swipe finger on reader or press Enter for password: "

/* ── Module-wide state (only valid for the duration of one call) ─── */

typedef struct {
    int           timeout_sec;
    int           debug;
    int           tty_fd;       /* /dev/tty file descriptor            */
    struct termios orig_tio;    /* original terminal settings          */
    int           tio_saved;    /* whether orig_tio is valid           */
    fprintd_ctx   fp;           /* fprintd D-Bus context               */
} module_ctx;

/* ── Forward declarations ─────────────────────────────────────────── */

static void  parse_args(module_ctx *m, int argc, const char **argv);
static int   tty_open_raw(module_ctx *m);
static void  tty_restore(module_ctx *m);
static void  dbg(const module_ctx *m, const char *fmt, ...);

/* ── PAM entry point ──────────────────────────────────────────────── */

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags,
                    int argc, const char **argv)
{
    /* TODO: implement in task 3 */
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_AUTHINFO_UNAVAIL;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags,
               int argc, const char **argv)
{
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}

/* ── Helpers (stubs, filled in tasks 3–4) ─────────────────────────── */

static void parse_args(module_ctx *m, int argc, const char **argv)
{
    m->timeout_sec = DEFAULT_TIMEOUT_SEC;
    m->debug       = 0;

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "timeout=", 8) == 0) {
            m->timeout_sec = atoi(argv[i] + 8);
            if (m->timeout_sec <= 0)
                m->timeout_sec = DEFAULT_TIMEOUT_SEC;
        } else if (strcmp(argv[i], "debug") == 0) {
            m->debug = 1;
        }
    }
}

static int tty_open_raw(module_ctx *m)
{
    /* TODO: implement in task 3 */
    (void)m;
    return -1;
}

static void tty_restore(module_ctx *m)
{
    /* TODO: implement in task 4 */
    (void)m;
}

static void dbg(const module_ctx *m, const char *fmt, ...)
{
    if (!m->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_AUTH | LOG_DEBUG, fmt, ap);
    va_end(ap);
}
