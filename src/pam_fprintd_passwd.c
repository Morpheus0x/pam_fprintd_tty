/*
 * pam_fprintd_passwd.c – PAM module for seamless fingerprint-then-password auth
 *
 * Flow:
 *   1. Check fprintd availability + enrolled prints via D-Bus
 *   2. If available: display prompt, start verification, poll() for
 *      fingerprint result or keypress
 *   3. On fingerprint match      → PAM_SUCCESS
 *   4. On Ctrl+C / timeout       → PAM_AUTHINFO_UNAVAIL  (fall through to pam_unix)
 *   5. If fprintd unavailable    → PAM_AUTHINFO_UNAVAIL  (immediate fall through)
 *
 * Module arguments:
 *   timeout=N   seconds to wait for fingerprint (default 10)
 *   debug       enable syslog debug messages
 *
 * Ctrl+C handling:
 *   The terminal is placed in raw mode with ISIG disabled so that Ctrl+C
 *   arrives as byte 0x03 rather than generating SIGINT.  This is necessary
 *   because sudo blocks SIGINT via sigprocmask() during PAM authentication,
 *   which would cause the keypress to silently vanish.  Instead, we detect
 *   it as a regular byte.  Only Ctrl+C (0x03) triggers password fallback;
 *   all other keypresses are silently ignored.
 *
 * Safety:
 *   - Terminal settings are restored on every exit path, including
 *     signal delivery (SIGTERM, SIGHUP).
 *   - Original signal handlers are saved and reinstated on cleanup.
 *   - Stale terminal input is flushed before the poll loop starts.
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
#include <time.h>
#include <unistd.h>

#include "fprintd_dbus.h"

/* ── Defaults ─────────────────────────────────────────────────────── */

#define DEFAULT_TIMEOUT_SEC  10
#define PROMPT_MSG           "Place your finger on the fingerprint reader\n"

/* ── Module-wide state (only valid for the duration of one call) ─── */

typedef struct {
    int            timeout_sec;
    int            debug;
    int            tty_fd;       /* /dev/tty file descriptor            */
    struct termios orig_tio;     /* original terminal settings          */
    int            tio_saved;    /* whether orig_tio is valid           */
    fprintd_ctx    fp;           /* fprintd D-Bus context               */
} module_ctx;

/* ── Forward declarations ─────────────────────────────────────────── */

static void  parse_args(module_ctx *m, int argc, const char **argv);
static int   tty_open_raw(module_ctx *m);
static void  tty_restore(module_ctx *m);
static void  tty_write(module_ctx *m, const char *msg);
static void  install_signal_handlers(module_ctx *m);
static void  restore_signal_handlers(void);
static void  cleanup(module_ctx *m);
static void  dbg(const module_ctx *m, const char *fmt, ...);
static void  log_err(const char *fmt, ...);

/* ── Signal handling ──────────────────────────────────────────────── *
 *                                                                     *
 * We temporarily install handlers for SIGINT, SIGTERM, and SIGHUP     *
 * while the terminal is in raw mode.  The handlers:                   *
 *   1. Restore the terminal to its original settings                  *
 *   2. Re-install the original (saved) handler                        *
 *   3. Re-raise the signal so the calling process (sudo) sees it      *
 *                                                                     *
 * This guarantees the user's terminal is never left in a broken       *
 * state, even if the process is killed during fingerprint scanning.   *
 * ──────────────────────────────────────────────────────────────────── */

/*
 * File-scope pointer so the signal handler can find the module context.
 * Only set while the terminal is in raw mode; NULL otherwise.
 */
static volatile sig_atomic_t g_signal_active = 0;
static module_ctx           *g_signal_ctx    = NULL;

/* Saved original signal dispositions */
static struct sigaction g_old_sigint;
static struct sigaction g_old_sigterm;
static struct sigaction g_old_sighup;

static void signal_handler(int sig)
{
    /*
     * Restore terminal settings.  tty_restore() is safe here because
     * it only calls tcsetattr() and close(), both of which are
     * async-signal-safe (POSIX.1-2008).
     */
    if (g_signal_ctx)
        tty_restore(g_signal_ctx);

    g_signal_ctx    = NULL;
    g_signal_active = 0;

    /* Reinstate the original handler and re-raise so the caller sees it */
    const struct sigaction *old = NULL;
    switch (sig) {
    case SIGINT:  old = &g_old_sigint;  break;
    case SIGTERM: old = &g_old_sigterm; break;
    case SIGHUP:  old = &g_old_sighup;  break;
    default:      return;
    }

    sigaction(sig, old, NULL);
    raise(sig);
}

static void install_signal_handlers(module_ctx *m)
{
    g_signal_ctx    = m;
    g_signal_active = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags   = 0;            /* no SA_RESTART: we want EINTR in poll */
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT,  &sa, &g_old_sigint);
    sigaction(SIGTERM, &sa, &g_old_sigterm);
    sigaction(SIGHUP,  &sa, &g_old_sighup);
}

static void restore_signal_handlers(void)
{
    if (!g_signal_active)
        return;

    sigaction(SIGINT,  &g_old_sigint,  NULL);
    sigaction(SIGTERM, &g_old_sigterm, NULL);
    sigaction(SIGHUP,  &g_old_sighup,  NULL);

    g_signal_ctx    = NULL;
    g_signal_active = 0;
}

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

    dbg(&m, "pam_fprintd_passwd: module loaded (timeout=%d, debug=%d)",
        m.timeout_sec, m.debug);

    /* ── 1. Get the username from PAM ─────────────────────────────── */

    const char *username = NULL;
    int rc = pam_get_user(pamh, &username, NULL);
    if (rc != PAM_SUCCESS || !username || !*username) {
        log_err("pam_fprintd_passwd: could not determine username (rc=%d)", rc);
        return PAM_AUTHINFO_UNAVAIL;
    }

    dbg(&m, "pam_fprintd_passwd: authenticating user '%s'", username);

    /* ── 2. Connect to fprintd ────────────────────────────────────── */

    if (fprintd_open(&m.fp) < 0) {
        dbg(&m, "pam_fprintd_passwd: fprintd not available, skipping");
        return PAM_AUTHINFO_UNAVAIL;
    }

    /* ── 3. Check enrolled fingerprints ───────────────────────────── */

    int enrolled = fprintd_has_enrolled_prints(&m.fp, username);
    if (enrolled <= 0) {
        dbg(&m, "pam_fprintd_passwd: no enrolled prints for '%s' (rc=%d)",
            username, enrolled);
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    dbg(&m, "pam_fprintd_passwd: user '%s' has enrolled prints", username);

    /* ── 4. Open terminal for keypress detection ──────────────────── */

    if (tty_open_raw(&m) < 0) {
        log_err("pam_fprintd_passwd: cannot open /dev/tty, skipping");
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    /*
     * Install signal handlers AFTER opening the terminal in raw mode,
     * so they can restore it if we get killed.
     */
    install_signal_handlers(&m);

    /*
     * Flush any stale input that might be sitting in the tty buffer
     * (e.g. from a previous failed sudo attempt or accidental keypress).
     * Without this, a stale byte would trigger immediate fallback.
     */
    tcflush(m.tty_fd, TCIFLUSH);

    /* ── 5. Start fingerprint verification ────────────────────────── */

    if (fprintd_verify_start(&m.fp, username) < 0) {
        log_err("pam_fprintd_passwd: VerifyStart failed for '%s'", username);
        cleanup(&m);
        return PAM_AUTHINFO_UNAVAIL;
    }

    /* ── 6. Display prompt ────────────────────────────────────────── */

    tty_write(&m, PROMPT_MSG);

    /* ── 7. Event loop: poll for fingerprint result or keypress ──── */

    int dbus_fd = fprintd_get_fd(&m.fp);
    if (dbus_fd < 0) {
        log_err("pam_fprintd_passwd: cannot get D-Bus fd");
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

    dbg(&m, "pam_fprintd_passwd: entering poll loop (tty_fd=%d, dbus_fd=%d, "
        "timeout=%dms)", m.tty_fd, dbus_fd, timeout_ms);

    for (;;) {
        int n = poll(fds, 2, remaining);

        if (n < 0) {
            if (errno == EINTR) {
                /*
                 * Signal received (e.g. SIGTERM, SIGHUP).
                 * Our signal handler already restored the terminal.
                 * Fall through to password as a safe default.
                 *
                 * Note: Ctrl+C is handled as byte 0x03 (ISIG is off),
                 * not via SIGINT, so it doesn't come through here.
                 */
                dbg(&m, "pam_fprintd_passwd: interrupted by signal, "
                    "falling back to password");
                tty_write(&m, "\n");
                break;
            }
            /* Unexpected poll error */
            log_err("pam_fprintd_passwd: poll error: %s", strerror(errno));
            break;
        }

        if (n == 0) {
            /* Timeout – fall through to password */
            dbg(&m, "pam_fprintd_passwd: timeout reached (%ds)",
                m.timeout_sec);
            tty_write(&m, "Verification timed out\n");
            break;
        }

        /* ── Check for keypress on /dev/tty ───────────────────────── */

        if (fds[0].revents & POLLIN) {
            char buf[32];
            ssize_t nr = read(m.tty_fd, buf, sizeof(buf));
            if (nr > 0) {
                /*
                 * With ISIG disabled, Ctrl+C arrives as byte 0x03
                 * rather than generating SIGINT (which sudo blocks
                 * during PAM auth anyway).  Only Ctrl+C triggers
                 * fallback to password; all other keypresses are
                 * silently consumed so accidental input doesn't
                 * interrupt the fingerprint scan.
                 */
                if (buf[0] == 0x03) {
                    dbg(&m, "pam_fprintd_passwd: Ctrl+C detected, "
                        "falling back to password");
                    tty_write(&m, "\n");
                    break;
                }

                /* Any other keypress: silently ignore */
                dbg(&m, "pam_fprintd_passwd: ignoring keypress "
                    "(%d byte(s))", (int)nr);
            }
        }

        if (fds[0].revents & (POLLHUP | POLLERR)) {
            /* Terminal gone – bail out */
            dbg(&m, "pam_fprintd_passwd: tty hangup/error (revents=0x%x)",
                (unsigned)fds[0].revents);
            break;
        }

        /* ── Check for fprintd D-Bus messages ─────────────────────── */

        if (fds[1].revents & POLLIN) {
            fp_result_t fp_rc = fprintd_poll_result(&m.fp);

            switch (fp_rc) {
            case FP_RESULT_MATCH:
                dbg(&m, "pam_fprintd_passwd: fingerprint matched!");
                pam_result = PAM_SUCCESS;
                goto done;

            case FP_RESULT_NO_MATCH:
                dbg(&m, "pam_fprintd_passwd: fingerprint did not match");
                tty_write(&m, "Fingerprint did not match.\n");
                /* Fall through to password */
                goto done;

            case FP_RESULT_ERROR:
                dbg(&m, "pam_fprintd_passwd: fprintd reported an error");
                tty_write(&m, "pam_fprintd_passwd: fingerprint reader error\n");
                goto done;

            case FP_RESULT_PENDING:
                /* Retry events (swipe-too-short, etc.) – keep waiting */
                dbg(&m, "pam_fprintd_passwd: fprintd pending (retry event)");
                break;
            }
        }

        if (fds[1].revents & (POLLHUP | POLLERR)) {
            dbg(&m, "pam_fprintd_passwd: D-Bus fd hangup/error (revents=0x%x)",
                (unsigned)fds[1].revents);
            break;
        }

        /* ── Recalculate remaining timeout ────────────────────────── */

        clock_gettime(CLOCK_MONOTONIC, &t_now);
        long elapsed_ms = (t_now.tv_sec  - t_start.tv_sec)  * 1000L
                        + (t_now.tv_nsec - t_start.tv_nsec) / 1000000L;
        remaining = timeout_ms - (int)elapsed_ms;
        if (remaining <= 0) {
            dbg(&m, "pam_fprintd_passwd: timeout (elapsed after %ldms)",
                elapsed_ms);
            tty_write(&m, "Verification timed out\n");
            break;
        }
    }

done:
    dbg(&m, "pam_fprintd_passwd: returning %s",
        pam_result == PAM_SUCCESS ? "PAM_SUCCESS" : "PAM_AUTHINFO_UNAVAIL");
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
        log_err("pam_fprintd_passwd: open(/dev/tty): %s", strerror(errno));
        return -1;
    }

    /* Save the current terminal settings so we can restore them later */
    if (tcgetattr(m->tty_fd, &m->orig_tio) < 0) {
        log_err("pam_fprintd_passwd: tcgetattr: %s", strerror(errno));
        close(m->tty_fd);
        m->tty_fd = -1;
        return -1;
    }
    m->tio_saved = 1;

    /*
     * Switch to raw-ish mode:
     *   - ICANON off  → don't wait for newline, deliver bytes immediately
     *   - ECHO   off  → don't echo keypresses (we're not collecting a password)
     *   - ISIG   off  → Ctrl+C arrives as byte 0x03 in the input buffer
     *                    instead of generating SIGINT (which sudo blocks
     *                    via sigprocmask during PAM authentication, making
     *                    the keypress silently vanish)
     *   - VMIN=1, VTIME=0 → read blocks until 1 byte (but we use poll)
     */
    struct termios raw = m->orig_tio;
    raw.c_lflag &= ~((tcflag_t)ICANON | (tcflag_t)ECHO | (tcflag_t)ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(m->tty_fd, TCSANOW, &raw) < 0) {
        log_err("pam_fprintd_passwd: tcsetattr: %s", strerror(errno));
        close(m->tty_fd);
        m->tty_fd = -1;
        m->tio_saved = 0;
        return -1;
    }

    return 0;
}

/*
 * tty_restore – restore original terminal settings and close the fd.
 *
 * This function is async-signal-safe: it only calls tcsetattr() and
 * close(), both of which POSIX guarantees are safe in signal handlers.
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
    ssize_t ret = write(m->tty_fd, msg, strlen(msg));
    (void)ret;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

/*
 * cleanup – stop fprintd, restore terminal, restore signal handlers,
 * free resources.  Safe to call multiple times.
 */
static void cleanup(module_ctx *m)
{
    fprintd_close(&m->fp);
    tty_restore(m);
    restore_signal_handlers();
}

/* ── Logging ──────────────────────────────────────────────────────── */

/*
 * dbg – log at LOG_DEBUG level, only when the "debug" module arg is set.
 */
static void dbg(const module_ctx *m, const char *fmt, ...)
{
    if (!m->debug)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_AUTH | LOG_DEBUG, fmt, ap);
    va_end(ap);
}

/*
 * log_err – unconditionally log at LOG_ERR level.
 * Used for errors that should always be visible, regardless of debug flag.
 */
static void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsyslog(LOG_AUTH | LOG_ERR, fmt, ap);
    va_end(ap);
}
