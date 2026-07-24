#ifndef GLYPH_FC_CACHE_H
#define GLYPH_FC_CACHE_H

#include <stdbool.h>

/* Run fc-cache to refresh fontconfig. Spawns a child process (fork+execvp) and
   waits for completion. If no_cache is true, returns 0 immediately. If verbose
   is false, the child's stdout/stderr are captured into a temp file and
   replayed on stderr only when fc-cache fails; if verbose is true, fc-cache
   inherits the terminal. Returns 0 on success, -1 if fc-cache not found or
   returns non-zero. */
int fc_cache_refresh(bool no_cache, bool verbose);

#endif /* GLYPH_FC_CACHE_H */
