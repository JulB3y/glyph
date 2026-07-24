/*
 * src/fc_cache.c — fontconfig cache refresh.
 *
 * Full implementation of the contract declared in include/fc_cache.h. Forks a
 * child process, execs fc-cache(1), and waits for it so the parent CLI can
 * continue afterwards. Best-effort: failures are logged but never abort the
 * program.
 *
 * By default the child's stdout/stderr are redirected into a mkstemp() temp
 * file so fc-cache's verbose scan log never reaches the terminal; the capture
 * is replayed on stderr only when fc-cache fails. With verbose=true the child
 * inherits the terminal (legacy behavior).
 *
 * Designed to compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "glyph.h"
#include "fc_cache.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef P_tmpdir
#define P_tmpdir "/tmp"
#endif

/* Candidate absolute paths searched in order. NULL-terminated. */
static const char *const FC_CACHE_PATHS[] = {
    "/usr/bin/fc-cache",
    "/usr/local/bin/fc-cache",
    NULL,
};

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Pick an fc-cache executable to run. Returns the first absolute candidate
 * for which access(X_OK) succeeds; falls back to the bare name "fc-cache" so
 * that execvp() can search $PATH. Never returns NULL.
 */
static const char *choose_fc_cache_path(void)
{
    for (size_t i = 0; FC_CACHE_PATHS[i] != NULL; i++) {
        if (access(FC_CACHE_PATHS[i], X_OK) == 0) {
            return FC_CACHE_PATHS[i];
        }
    }
    return "fc-cache";
}

/* Dump the captured fc-cache log to stderr (failure context). */
static void replay_capture(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(STDERR_FILENO, buf + off, (size_t)(n - off));
            if (w <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return;
            }
            off += w;
        }
    }
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int fc_cache_refresh(bool no_cache, bool verbose)
{
    if (no_cache) {
        return 0;
    }

    const char *path = choose_fc_cache_path();

    /* -fv forces a full rebuild of every cache; --really-force silences the
     * "already up to date" short-circuit so a refresh always runs. */
    char *argv[] = { "fc-cache", "-fv", "--really-force", NULL };

    /* Non-verbose: capture child output into a 0600 temp file (mkstemp mode).
     * If mkstemp fails, degrade to inherited stdio rather than fail. */
    char tmpl[] = P_tmpdir "/glyph-fc-cache-XXXXXX";
    int cap_fd = -1;
    if (!verbose) {
        cap_fd = mkstemp(tmpl);
    }

    pid_t child = fork();
    if (child == (pid_t)-1) {
        int saved_errno = errno;
        if (cap_fd != -1) {
            close(cap_fd);
            unlink(tmpl);
        }
        glyph_log_warn("fc-cache: fork failed: %s", strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (child == (pid_t)0) {
        /* Child: replace the image with fc-cache. execvp() only returns on
         * failure, so signal that to the parent via a non-zero status. */
        if (cap_fd != -1) {
            (void)dup2(cap_fd, STDOUT_FILENO);
            (void)dup2(cap_fd, STDERR_FILENO);
            close(cap_fd);
        }
        execvp(path, argv);
        _exit(127);
    }

    if (cap_fd != -1) {
        close(cap_fd);
    }

    /* Parent: block until the child terminates, retrying on EINTR. */
    int status = 0;
    for (;;) {
        pid_t w = waitpid(child, &status, 0);
        if (w == (pid_t)-1) {
            if (errno == EINTR) {
                continue;
            }
            int saved_errno = errno;
            if (cap_fd != -1) {
                unlink(tmpl);
            }
            glyph_log_warn("fc-cache: waitpid failed: %s",
                           strerror(saved_errno));
            errno = saved_errno;
            return -1;
        }
        if (w == child) {
            break;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (cap_fd != -1) {
            replay_capture(tmpl);
            unlink(tmpl);
        }
        glyph_log_warn("fc-cache refresh failed (exit %d)", code);
        return -1;
    }

    if (cap_fd != -1) {
        unlink(tmpl);
    }
    glyph_log_debug("fc-cache: refresh ok");
    return 0;
}
