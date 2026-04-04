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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "fprintd_dbus.h"

/* ── Defaults ─────────────────────────────────────────────────────── */

#define DEFAULT_TIMEOUT_SEC  10
#define PROMPT_MSG           "Swipe finger on reader or press Enter for password:\n"

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
static void  tty_write(module_ctx *m, const char *msg);
static void  dbg(const module_ctx *m, const char *fmt, ...);
static void  cleanup(module_ctx *m);

/* ── PAM entry points ─────────────────────────────────────────────── */

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags,
                    int argc, const char **argv)
{
    module_ctx m;
    memset(&m, 0, sizeof(m));
    m.tty_fd = -1;

    (void)flags;

    parse_args(&m, argc, argv);

    /* ── 1. Get the username from PAM ─────────────────────────────── */

    const char *username = NULL;
    int rc = pam_get_user(pamh, &username, NULL);
    if (rc != PAM_SUCCESS || !username || !*username) {
        dbg(&m, "pam_fprint_fixed: could not determine username");
        return PAM_AUTHINFO_UNAVAIL;
    }

    dbg(&m, "pam_fprint_fixed: authenticating user '%s'", username);

    /* ── 2. Connect to fprintd ────────────────────────────────────── */

    if (fprintd_open(&m.fp) < 0) {
        dbg(&m, "pam_fprint_fixed: fprintd not available, skipping");
        return PAM_AUTHINFO_UNAVAIL;
    }

    /* ── 3. Check enrolled fingerprints ───────────────────────────── */

    int enrolled = fprintd_has_enrolled_prints(&m.fp, username);
    if (enrolled <= 0) {
        dbg(&m, "pam_fprint_fixed: no enrolled prints for '%s' (rc=%d)",
            username, enrolled);
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    dbg(&m, "pam_fprint_fixed: user '%s' has enrolled prints", username);

    /* ── 4. Open terminal for keypress detection ──────────────────── */

    if (tty_open_raw(&m) < 0) {
        dbg(&m, "pam_fprint_fixed: cannot open /dev/tty, skipping");
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    /* ── 5. Start fingerprint verification ────────────────────────── */

    if (fprintd_verify_start(&m.fp, username) < 0) {
        dbg(&m, "pam_fprint_fixed: VerifyStart failed");
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    /* ── 6. Display prompt ────────────────────────────────────────── */

    tty_write(&m, PROMPT_MSG);

    /* ── 7. Event loop: poll for fingerprint result or keypress ──── */

    int dbus_fd = fprintd_get_fd(&m.fp);
    if (dbus_fd < 0) {
        dbg(&m, "pam_fprint_fixed: cannot get D-Bus fd");
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    struct pollfd fds[2];
    fds[0].fd     = m.tty_fd;
    fds[0].events = POLLIN;
    fds[1].fd     = dbus_fd;
    fds[1].events = POLLIN;

    int pam_result = PAM_AUTHINFO_UNAVAIL;   /* default: fall through */
    int timeout_ms = m.timeout_sec * 1000;
    int remaining  = timeout_ms;

    struct timespec t_start, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (;;) {
        int n = poll(fds, 2, remaining);

        if (n < 0) {
            if (errno == EINTR) {
                /*
                 * Signal received (e.g. Ctrl+C → SIGINT delivered to
                 * the process).  Treat as "user wants to cancel
                 * fingerprint and fall through to password".
                 */
                dbg(&m, "pam_fprint_fixed: interrupted by signal, "
                    "falling back to password");
                break;
            }
            /* Unexpected poll error */
            dbg(&m, "pam_fprint_fixed: poll error: %s", strerror(errno));
            break;
        }

        if (n == 0) {
            /* Timeout – fall through to password */
            dbg(&m, "pam_fprint_fixed: timeout reached (%ds)",
                m.timeout_sec);
            tty_write(&m, "\npam_fprint_fixed: fingerprint timeout, "
                          "falling back to password\n");
            break;
        }

        /* ── Check for keypress on /dev/tty ───────────────────────── */

        if (fds[0].revents & POLLIN) {
            char buf[32];
            ssize_t nr = read(m.tty_fd, buf, sizeof(buf));
            if (nr > 0) {
                /*
                 * Any keypress means the user wants to type a password.
                 * We specifically document Enter, but reacting to any
                 * key avoids confusion if someone starts typing their
                 * password immediately.
                 */
                dbg(&m, "pam_fprint_fixed: keypress detected, "
                    "falling back to password");
                break;
            }
        }

        if (fds[0].revents & (POLLHUP | POLLERR)) {
            /* Terminal gone – bail out */
            dbg(&m, "pam_fprint_fixed: tty hangup/error");
            break;
        }

        /* ── Check for fprintd D-Bus messages ─────────────────────── */

        if (fds[1].revents & POLLIN) {
            fp_result_t fp_rc = fprintd_poll_result(&m.fp);

            switch (fp_rc) {
            case FP_RESULT_MATCH:
                dbg(&m, "pam_fprint_fixed: fingerprint matched!");
                tty_write(&m, "\n");
                pam_result = PAM_SUCCESS;
                goto done;

            case FP_RESULT_NO_MATCH:
                dbg(&m, "pam_fprint_fixed: fingerprint did not match");
                tty_write(&m, "Fingerprint did not match.\n");
                /* Fall through to password */
                goto done;

            case FP_RESULT_ERROR:
                dbg(&m, "pam_fprint_fixed: fprintd error");
                goto done;

            case FP_RESULT_PENDING:
                /* Retry events (swipe-too-short, etc.) – keep waiting */
                break;
            }
        }

        /* ── Recalculate remaining timeout ────────────────────────── */

        clock_gettime(CLOCK_MONOTONIC, &t_now);
        long elapsed_ms = (t_now.tv_sec  - t_start.tv_sec)  * 1000L
                        + (t_now.tv_nsec - t_start.tv_nsec) / 1000000L;
        remaining = timeout_ms - (int)elapsed_ms;
        if (remaining <= 0) {
            dbg(&m, "pam_fprint_fixed: timeout (elapsed)");
            tty_write(&m, "\npam_fprint_fixed: fingerprint timeout, "
                          "falling back to password\n");
            break;
        }
    }

done:
    cleanup(&m);
    return pam_result;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags,
               int argc, const char **argv)
{
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}

/* ── Argument parsing ─────────────────────────────────────────────── */

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

/* ── Terminal helpers ─────────────────────────────────────────────── */

/*
 * tty_open_raw – open /dev/tty and switch to non-canonical, no-echo
 * mode so that individual keypresses (especially Enter) are delivered
 * immediately to our poll() loop.
 *
 * Returns 0 on success, -1 on failure.
 */
static int tty_open_raw(module_ctx *m)
{
    m->tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (m->tty_fd < 0) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: open(/dev/tty): %s", strerror(errno));
        return -1;
    }

    /* Save the current terminal settings so we can restore them later */
    if (tcgetattr(m->tty_fd, &m->orig_tio) < 0) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: tcgetattr: %s", strerror(errno));
        close(m->tty_fd);
        m->tty_fd = -1;
        return -1;
    }
    m->tio_saved = 1;

    /*
     * Switch to raw-ish mode:
     *   - ICANON off  → don't wait for newline, deliver bytes immediately
     *   - ECHO   off  → don't echo keypresses (we're not collecting a password)
     *   - ISIG   on   → still deliver SIGINT on Ctrl+C so poll() gets EINTR
     *   - VMIN=1, VTIME=0 → read blocks until 1 byte (but we use poll)
     */
    struct termios raw = m->orig_tio;
    raw.c_lflag &= ~((tcflag_t)ICANON | (tcflag_t)ECHO);
    /* Keep ISIG so Ctrl+C still works */
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(m->tty_fd, TCSANOW, &raw) < 0) {
        syslog(LOG_AUTH | LOG_ERR,
               "pam_fprint_fixed: tcsetattr: %s", strerror(errno));
        close(m->tty_fd);
        m->tty_fd = -1;
        m->tio_saved = 0;
        return -1;
    }

    return 0;
}

/*
 * tty_restore – restore original terminal settings and close the fd.
 */
static void tty_restore(module_ctx *m)
{
    if (m->tty_fd < 0)
        return;

    if (m->tio_saved)
        tcsetattr(m->tty_fd, TCSANOW, &m->orig_tio);

    close(m->tty_fd);
    m->tty_fd    = -1;
    m->tio_saved = 0;
}

/*
 * tty_write – write a string to the controlling terminal.
 */
static void tty_write(module_ctx *m, const char *msg)
{
    if (m->tty_fd < 0)
        return;
    /* Best-effort write; ignore partial/failed writes. */
    (void)write(m->tty_fd, msg, strlen(msg));
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

/*
 * cleanup – stop fprintd, restore terminal, free resources.
 * Safe to call multiple times.
 */
static void cleanup(module_ctx *m)
{
    fprintd_close(&m->fp);
    tty_restore(m);
}

/* ── Debug logging ────────────────────────────────────────────────── */

static void dbg(const module_ctx *m, const char *fmt, ...)
{
    if (!m->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_AUTH | LOG_DEBUG, fmt, ap);
    va_end(ap);
}
