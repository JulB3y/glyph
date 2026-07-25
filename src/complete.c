/*
 * src/complete.c — shell-completion candidate backend + embedded fish script.
 *
 * Implements include/complete.h. glyph_complete_emit() is the hidden engine
 * behind `glyph __complete <cmd> <prefix>`: it maps a command to an id source
 * (catalog cache for info/install, installed DB for remove/upgrade) and prints
 * prefix-matching ids one per line. It is deliberately tolerant: any missing
 * or unparseable state yields empty output and a 0 return, because a failing
 * completion backend makes TAB feel broken. Read-only, takes no lock.
 *
 * The fish script is embedded as a C string so `completions fish` (print) and
 * `completions install` (write) can never diverge.
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

#include "complete.h"
#include "db.h"
#include "manifest.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Candidate backend
 * ------------------------------------------------------------------------- */

int glyph_complete_emit(const char *cmd, const char *prefix)
{
    if (cmd == NULL) {
        return 0;
    }
    const char *pfx = (prefix != NULL) ? prefix : "";
    const size_t plen = strlen(pfx);

    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "install") == 0) {
        char *path = glyph_path_catalog_cache();
        if (path == NULL) {
            return 0;
        }
        glyph_catalog_t cat;
        memset(&cat, 0, sizeof(cat));
        if (glyph_catalog_load_file(path, &cat) == 0) {
            for (size_t i = 0; i < cat.n_fonts; i++) {
                const char *id = cat.fonts[i].id;
                if (id != NULL && strncmp(id, pfx, plen) == 0) {
                    puts(id);
                }
            }
            glyph_catalog_free(&cat);
        }
        free(path);
        return 0;
    }

    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "upgrade") == 0) {
        glyph_db_t db;
        memset(&db, 0, sizeof(db));
        if (glyph_db_load(&db) == 0) {
            for (size_t i = 0; i < db.n_fonts; i++) {
                const char *id = db.fonts[i].id;
                if (id != NULL && strncmp(id, pfx, plen) == 0) {
                    puts(id);
                }
            }
            glyph_db_free(&db);
        }
        return 0;
    }

    /* Unknown cmd: empty output, never an error. */
    return 0;
}

/* ---------------------------------------------------------------------------
 * Embedded fish script (single source of truth)
 * ------------------------------------------------------------------------- */

static const char GLYPH_FISH_SCRIPT[] =
    "# fish completion for glyph. Generate: `glyph completions fish`;\n"
    "# install: `glyph completions install`.\n"
    "\n"
    "complete -c glyph -f\n"
    "\n"
    "# Layer 1: subcommands at the first argument position.\n"
    "complete -c glyph -n __fish_use_subcommand -a index -d 'Manage the catalog index'\n"
    "complete -c glyph -n __fish_use_subcommand -a search -d 'Search the catalog'\n"
    "complete -c glyph -n __fish_use_subcommand -a list -d 'List fonts'\n"
    "complete -c glyph -n __fish_use_subcommand -a info -d 'Show font details'\n"
    "complete -c glyph -n __fish_use_subcommand -a install -d 'Install a font'\n"
    "complete -c glyph -n __fish_use_subcommand -a remove -d 'Remove an installed font'\n"
    "complete -c glyph -n __fish_use_subcommand -a upgrade -d 'Upgrade fonts'\n"
    "complete -c glyph -n __fish_use_subcommand -a completions -d 'Shell completion setup'\n"
    "complete -c glyph -n __fish_use_subcommand -a help -d 'Show help'\n"
    "\n"
    "# Layer 2: subcommand actions and flags.\n"
    "complete -c glyph -n '__fish_seen_subcommand_from index' -a update -d 'Download and verify the catalog'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from index' -a status -d 'Show cached catalog information'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from completions' -a fish -d 'Print the fish completion script'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from completions' -a install -d 'Install the fish completion script'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from install remove upgrade' -l no-cache -d 'Skip fontconfig cache refresh'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from install remove upgrade' -l verbose -d 'Show full fc-cache output'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from list' -l catalog -d 'List catalog fonts'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from upgrade' -l all -d 'Upgrade all installed fonts'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from index search list info install remove upgrade completions' -l help -d 'Show help'\n"
    "\n"
    "# Layer 3: live font ids from the hidden `__complete` backend.\n"
    "complete -c glyph -n '__fish_seen_subcommand_from info' -a '(glyph __complete info (commandline -ct))'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from install' -a '(glyph __complete install (commandline -ct))'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from remove' -a '(glyph __complete remove (commandline -ct))'\n"
    "complete -c glyph -n '__fish_seen_subcommand_from upgrade' -a '(glyph __complete upgrade (commandline -ct))'\n";

const char *glyph_complete_fish_script(void)
{
    return GLYPH_FISH_SCRIPT;
}

/* ---------------------------------------------------------------------------
 * Self-install (user scope, atomic)
 * ------------------------------------------------------------------------- */

int glyph_complete_install_fish(char **out_path)
{
    if (out_path != NULL) {
        *out_path = NULL;
    }

    char *cfg = glyph_xdg_config_home();
    if (cfg == NULL) {
        return -1;
    }
    char *dir = glyph_path_join(cfg, "fish/completions");
    free(cfg);
    if (dir == NULL) {
        return -1;
    }
    if (glyph_mkdir_p(dir, 0755) != 0) {
        free(dir);
        return -1;
    }
    char *dst = glyph_path_join(dir, "glyph.fish");
    free(dir);
    if (dst == NULL) {
        return -1;
    }
    const char *script = GLYPH_FISH_SCRIPT;
    if (glyph_write_file(dst, script, strlen(script), 0644) != 0) {
        free(dst);
        return -1;
    }
    if (out_path != NULL) {
        *out_path = dst;
    } else {
        free(dst);
    }
    return 0;
}
