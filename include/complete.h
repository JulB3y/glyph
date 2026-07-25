#ifndef GLYPH_COMPLETE_H
#define GLYPH_COMPLETE_H

/* Shell-completion support: a hidden candidate backend plus the embedded
 * fish script delivered by `glyph completions fish|install`. */

/* Print completion candidates for `cmd` filtered by `prefix`, one per line
 * on stdout. Candidate source is keyed by cmd:
 *   info, install      -> catalog cache (glyph_path_catalog_cache)
 *   remove, upgrade    -> installed DB (glyph_db_load)
 *   anything else      -> no candidates
 * Always returns 0: missing/unparseable state or an unknown cmd yields empty
 * output, never an error (TAB must never fail). Takes no lock (read-only). */
int glyph_complete_emit(const char *cmd, const char *prefix);

/* The embedded fish completion script (single source of truth shared by
 * `completions fish` and `completions install`). NUL-terminated, never NULL. */
const char *glyph_complete_fish_script(void);

/* Write the fish script to <XDG_CONFIG_HOME>/fish/completions/glyph.fish,
 * creating the directory as needed, via atomic tmp+rename. On success
 * returns 0 and, if out_path is non-NULL, sets *out_path to the malloc'd
 * destination path (caller frees). Returns -1 on error (errno set). */
int glyph_complete_install_fish(char **out_path);

#endif /* GLYPH_COMPLETE_H */
