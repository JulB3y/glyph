project(
    'glyph',
    'c',
    version: '0.1.0',
    default_options: [
        'c_std=c99',
        'warning_level=3',
        'werror=true',
    ],
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
#
# libcurl is always required (HTTPS downloads).
#
# cjson and miniz are resolved in this order:
#   1. System pkg-config dependency (libcjson-dev / libminiz-dev on Debian).
#   2. Meson WrapDB subproject (subprojects/cjson.wrap, subprojects/miniz.wrap).
#
# Pass `-Dvendor-deps=true` to force use of the vendored subprojects even when
# system packages are available — useful for reproducible / distro-agnostic
# builds.
#
# NOTE: This skeleton only ships src/util.c. The remaining src/*.c files are
# added by other agents; until they exist, `meson setup` will fail on the
# missing-source check, which is intentional.

libcurl_dep = dependency('libcurl', version: '>=7.68')

if get_option('vendor-deps')
    cjson_sub = subproject('cjson')
    cjson_dep = cjson_sub.get_variable('libcjson_dep')
else
    cjson_dep = dependency('cjson', fallback: ['cjson', 'libcjson_dep'])
endif

if get_option('vendor-deps')
    miniz_sub = subproject('miniz')
    miniz_dep = miniz_sub.get_variable('miniz_dep')
else
    miniz_dep = dependency('miniz', fallback: ['miniz', 'miniz_dep'])
endif

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
#
# `warning_level=3` + `werror=true` already give us -Wall -Wextra -Wpedantic
# and -Werror. We additionally turn on -Wstrict-prototypes and
# -Wmissing-prototypes (not enabled by any warning_level), and propagate the
# POSIX feature-test macros every translation unit relies on.

extra_c_args = [
    '-D_POSIX_C_SOURCE=200809L',
    '-D_DEFAULT_SOURCE',
    '-D_XOPEN_SOURCE=700',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
]

inc = include_directories('include')

# ---------------------------------------------------------------------------
# Build-time trust anchor
# ---------------------------------------------------------------------------
#
# Derive the DER SubjectPublicKeyInfo bytes from res/glyph.pem at build time
# and emit an embeddable C header (byte array + length). Deriving at build
# time means the embedded key can never be stale relative to res/glyph.pem.
# python3 is already a hard meson dependency, so this adds nothing new.

python3 = find_program('python3')

# NB: no backslash escapes in the script -- meson/ninja command mangling
# turns them into noise. chr(10) is the newline.
pem_to_der_script = '''
import base64, sys
nl = chr(10)
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, "r").read().splitlines()
der = base64.b64decode("".join(l for l in lines if not l.startswith("-----")))
if len(der) == 0:
    sys.exit("pem2der: empty DER from " + src)
out = [
    "#ifndef GLYPH_PUBKEY_DER_H",
    "#define GLYPH_PUBKEY_DER_H",
    "#include <stdint.h>",
    "static const uint8_t GLYPH_PUBKEY_DER[] = {",
    ",".join("0x%02x" % b for b in der),
    "};",
    "#define GLYPH_PUBKEY_DER_LEN (sizeof(GLYPH_PUBKEY_DER))",
    "#endif",
]
open(dst, "w").write(nl.join(out) + nl)
'''

glyph_pubkey_der_h = custom_target(
    'glyph_pubkey_der.h',
    input: 'res/glyph.pem',
    output: 'glyph_pubkey_der.h',
    command: [python3, '-c', pem_to_der_script, '@INPUT@', '@OUTPUT@'],
)

sources = files(
    'src/main.c',
    'src/complete.c',
    'src/manifest.c',
    'src/download.c',
    'src/verify.c',
    'src/extract.c',
    'src/db.c',
    'src/fc_cache.c',
    'src/util.c',
)

executable(
    'glyph',
    sources + [glyph_pubkey_der_h],
    include_directories: inc,
    dependencies: [libcurl_dep, cjson_dep, miniz_dep],
    c_args: extra_c_args,
    install: true,
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
#
# Generic pattern: any tests/test_<name>.c present becomes an executable plus a
# test() entry. Agents filling in modules should add their module's source
# files alongside each test entry as needed (e.g. link test_manifest.c against
# src/manifest.c + src/util.c).

if get_option('tests')
    fs = import('fs')
    test_srcs = {
        'test_manifest': files('src/manifest.c', 'src/util.c'),
        'test_verify':   files('src/verify.c', 'src/util.c'),
        'test_db':       files('src/db.c', 'src/util.c'),
        'test_extract':  files('src/extract.c', 'src/util.c'),
        'test_util':     files('src/util.c'),
        'test_complete': files('src/complete.c', 'src/manifest.c', 'src/db.c',
                               'src/util.c'),
    }
    foreach tname : ['test_manifest', 'test_verify', 'test_db', 'test_extract', 'test_util', 'test_complete']
        tpath = 'tests/@0@.c'.format(tname)
        if fs.exists(tpath)
            # test_verify compiles src/verify.c, which includes the generated
            # glyph_pubkey_der.h; listing the custom_target output as a source
            # orders the generation step and adds the build-dir include path.
            extra = (tname == 'test_verify') ? [glyph_pubkey_der_h] : []
            texe = executable(
                tname,
                [files(tpath)] + test_srcs[tname] + extra,
                include_directories: inc,
                dependencies: [libcurl_dep, cjson_dep, miniz_dep],
                c_args: extra_c_args,
            )
            test(tname, texe)
        endif
    endforeach
endif
