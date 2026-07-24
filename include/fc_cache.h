#ifndef GLYPH_FC_CACHE_H
#define GLYPH_FC_CACHE_H

#include <stdbool.h>

/* Run fc-cache to refresh fontconfig. Spawns a child process (fork+execvp) and
   waits for completion. If no_cache is true, returns 0 immediately. Returns 0 on
   success, -1 if fc-cache not found or returns non-zero. */
int fc_cache_refresh(bool no_cache);

#endif /* GLYPH_FC_CACHE_H */
