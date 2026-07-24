/*
 * src/fc_cache.c — fontconfig cache refresh.
 *
 * Full implementation of the contract declared in include/fc_cache.h. Forks a
 * child process, execs fc-cache(1), and waits for it so the parent CLI can
 * continue afterwards. Best-effort: failures are logged but never abort the
 * program.
 *
 * Designed to compile cleanly under:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes
 *
 * Notes:
 *   - Feature-test macros are defined FIRST so every system header sees them.
 *   - The plan's raw execvp() snippet was illustrative; the header mandates
 *     fork()+waitpid() so the caller keeps running after the refresh.
 *   - Absolute fc-cache paths are probed first; if neither is executable the
 *     bare name "fc-cache" is handed to execvp() so $PATH is searched.
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
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int fc_cache_refresh(bool no_cache)
{
    if (no_cache) {
        return 0;
    }

    const char *path = choose_fc_cache_path();

    /* -fv forces a full rebuild of every cache; --really-force silences the
     * "already up to date" short-circuit so a refresh always runs. */
    char *argv[] = { "fc-cache", "-fv", "--really-force", NULL };

    pid_t child = fork();
    if (child == (pid_t)-1) {
        int saved_errno = errno;
        glyph_log_warn("fc-cache: fork failed: %s", strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }

    if (child == (pid_t)0) {
        /* Child: replace the image with fc-cache. execvp() only returns on
         * failure, so signal that to the parent via a non-zero status. */
        execvp(path, argv);
        _exit(127);
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
        glyph_log_warn("fc-cache refresh failed (exit %d)", code);
        return -1;
    }

    glyph_log_info("fc-cache refreshed fontconfig cache");
    return 0;
}
