#!/usr/bin/env python3
"""Copy the Furbtendulator source tree into a build dir and make it build on a
case-sensitive POSIX filesystem: backslashes in #include become '/', and an
include whose path only matches case-insensitively is rewritten to the real
name.  The pristine tree (reference/Furbtendulator-main) is never modified."""
import os, re, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst, ignore=shutil.ignore_patterns('msvc100', '*.aps', '*.ico', '*.rc'))

def resolve(base, inc):
    """Return inc with each path component fixed to the on-disk case, or None."""
    cur = base
    out = []
    for part in inc.split('/'):
        if part in ('.', '..'):
            cur = os.path.normpath(os.path.join(cur, part)); out.append(part); continue
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = part if part in names else next((n for n in names if n.lower() == part.lower()), None)
        if hit is None:
            return None
        out.append(hit); cur = os.path.join(cur, hit)
    return '/'.join(out)

# MSVC-only constructs g++ rejects even with -fpermissive/-fms-extensions.
# Each fixup must match exactly once, so upstream changes are noticed.
FIXUPS = [
    # anonymous union of anonymous structs at file scope (Sunsoft 5B sound)
    ('src-mappers/src/Hardware/Sound/s_SUN5.cpp', r'static union\s*\{', 'static union SUN5_Regs {'),
    ('src-mappers/src/Hardware/Sound/s_SUN5.cpp', r'\};\s*\};\s*uint8_t select;',
     '};\n} SUN5_regs;\nuint8_t select;\n'
     + ''.join('#define %s SUN5_regs.%s\n' % (n, n) for n in
               ('tone', 'envelope', 'envhold', 'envaltr', 'envattk', 'envcont', 'byte7', 'byteB', 'byteC', 'byteD'))),
    # "const enum { ... };" declares no object (FDS sound)
    ('src-mappers/src/Hardware/Sound/s_FDS.cpp', r'const enum \{ TMOD=0, TWAV=1 \};', 'enum { TMOD=0, TWAV=1 };'),
    ('src-mappers/src/Hardware/Sound/s_FDS.cpp', r'const enum \{ EMOD=0, EVOL=1 \};', 'enum { EMOD=0, EVOL=1 };'),
    # variables of unnamed union type have no linkage in g++, so the extern
    # declaration in the _transfer file cannot bind; give the unions a tag
    ('src-main/src/plugThruDevice_SuperMagicCard.cpp', r'union \{(\s*uint8_t k8 \[64\]\[2\]\[4096\];)', 'union SMC_PRGRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard.cpp', r'union \{(\s*uint8_t k1 \[256\]\[1024\];)', 'union SMC_CHRRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard_transfer.cpp', r'extern union \{(\s*uint8_t k8 \[64\]\[2\]\[4096\];)', 'extern union SMC_PRGRAM {\\1'),
    ('src-main/src/plugThruDevice_SuperMagicCard_transfer.cpp', r'extern union \{(\s*uint8_t k1 \[256\]\[1024\];)', 'extern union SMC_CHRRAM {\\1'),
    # SSE intrinsics (x86 only): the flush-to-zero / denormals-are-zero
    # switch, done for every architecture by compat/furb_math.h
    ('src-main/src/Filter.cpp', r'#include <xmmintrin\.h>\n#include <pmmintrin\.h>\n', ''),
    ('src-main/src/Filter.cpp', r'_MM_SET_FLUSH_ZERO_MODE\(_MM_FLUSH_ZERO_ON\);\s*_MM_SET_DENORMALS_ZERO_MODE\(_MM_DENORMALS_ZERO_ON\);',
     'furb_flush_denormals();'),
    # what g++ -fpermissive lets through and clang (Android's compiler) does not:
    # string literals stored in non-const pointers of the mapper description
    ('src-main/src/MapperInterface.h', r'void \*\t\tMapperId;\n\tTCHAR \*\t\tDescription;', 'const void *\tMapperId;\n\tconst TCHAR *\tDescription;'),
    ('src-mappers/src/interface.h', r'void \*\t\tMapperId;\n\tTCHAR \*\t\tDescription;', 'const void *\tMapperId;\n\tconst TCHAR *\tDescription;'),
    # pointers cast to a smaller integer (the same truncation g++ does)
    ('src-mappers/src/Dll/d_iNES.cpp', r'\(unsigned int\)ROM->Filename', '(unsigned int)(uintptr_t)ROM->Filename'),
    ('src-mappers/src/Dll/d_VS.cpp', r'\(unsigned int\)ROM->Filename', '(unsigned int)(uintptr_t)ROM->Filename'),
    ('src-mappers/src/Dll/d_NSF.cpp', r'\(unsigned int\)ROM->Filename', '(unsigned int)(uintptr_t)ROM->Filename'),
    ('src-mappers/src/Dll/d_FDS.cpp', r'\(unsigned int\)ROM->Filename', '(unsigned int)(uintptr_t)ROM->Filename'),
    ('src-main/src/GFX.cpp', r'\(int\) PrimarySurf\)', '(int)(intptr_t) PrimarySurf)'),
    ('src-main/src/Controllers.cpp', r'int DevNum = \(int\)pvRef;', 'int DevNum = (int)(intptr_t)pvRef;'),
    ('src-mappers/src/iNES/MMC1-based/mapper001.cpp', r'TCHAR\*\* Description =', 'const TCHAR** Description ='),
    # a UTF-8 '®' in a char table: g++ keeps its last byte (0xAE, the
    # Windows-1252 code MSVC saw), clang refuses the multi-byte literal
    ('src-main/src/NES.cpp', "'®'", "'\\\\xAE'"),
    # an 'inline' member other files call: g++ keeps a copy for them
    # (-fkeep-inline-functions), clang has no such option
    ('src-main/src/PPU.h', r'inline void IncrementH \(\);', 'void IncrementH ();'),
    ('src-main/src/PPU.cpp', r'inline void PPU_RP2C02::IncrementH \(\)', 'void PPU_RP2C02::IncrementH ()'),
    # chunk IDs: a signed shift into the sign bit is not a constant expression
    ('src-main/src/NES.cpp', r'\(\(\(a\) <<  8\) & 0x00FF0000\) \| \\\n\t\t\(\(\(a\) << 24\)',
     '(((unsigned)(a) <<  8) & 0x00FF0000) | \\\\\\n\\t\\t(((unsigned)(a) << 24)'),
    # PRG_ROM, 256 MB of static array: reserved instead, so that only the
    # pages in use count against the device's memory (compat.cpp,
    # furb_reserve); a reference to the array keeps every use as it was
    ('src-main/src/NES.h', r'extern unsigned char PRG_ROM\[MAX_PRGROM_SIZE\]\[0x1000\];',
     'extern unsigned char (&PRG_ROM)[MAX_PRGROM_SIZE][0x1000];'),
    ('src-main/src/NES.cpp', r'\nunsigned char PRG_ROM\[MAX_PRGROM_SIZE\]\[0x1000\];',
     '\nunsigned char (&PRG_ROM)[MAX_PRGROM_SIZE][0x1000] = *(unsigned char (*)[MAX_PRGROM_SIZE][0x1000])'
     'furb_reserve(sizeof(unsigned char[MAX_PRGROM_SIZE][0x1000]));'),
    # not MSVC-only, but nondeterministic: buffer[0] of the first audio chunk
    # is never written, so the first sample was whatever the heap held
    ('src-main/src/Sound.cpp', r'buffer = new short\[buflen\];', 'buffer = new short[buflen]();'),
]
for rel, pat, rep in FIXUPS:
    p = os.path.join(dst, rel)
    text = open(p, encoding='utf-8', errors='surrogateescape').read()
    new, n = re.subn(pat, lambda m: m.expand(rep), text, count=1)
    if n != 1:
        sys.exit('prep_src: fixup did not apply to %s: %s' % (rel, pat))
    open(p, 'w', encoding='utf-8', errors='surrogateescape').write(new)

# The PPUs' word accesses to byte buffers at any offset (tile and sprite data):
# fine on x86, a bus error on 32-bit ARM once the compiler merges two of them
# into one 64-bit load (compat/windows.h: furb_ua32)
for rel in ('src-main/src/PPU.cpp', 'src-main/src/OneBus.cpp', 'src-main/src/OneBus_VT369.cpp', 'src-main/src/UM6578.cpp'):
    p = os.path.join(dst, rel)
    text = open(p, encoding='utf-8', errors='surrogateescape').read()
    new, n = re.subn(r'\(\(unsigned long \*\) ?', '((furb_ua32 *)', text)
    if not n:
        sys.exit('prep_src: no (unsigned long *) casts in ' + rel)
    open(p, 'w', encoding='utf-8', errors='surrogateescape').write(new)

# Win32 is ILP32/LLP64: 'long' is 32 bits there, and the sources rely on it
# (pixel writes through 'unsigned long *', CRC tables, register structs,
# savestate fields).  On LP64 Linux it is 64 bits, so every 'long' that is
# not 'long long' / 'long double' becomes 'int' (same size and meaning on
# i386, so the 32-bit build is unchanged).  Comments and string/character
# literals are left alone.
long_re = re.compile(r'(//[^\n]*|/\*.*?\*/)|("(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\')'
                     r'|(?<![\w])(?<!long )(?:(unsigned|signed)\s+)?long(?:\s+int\b)?(?!\s+(?:long|double)\b)(?![\w])', re.S)
def fix_long(m):
    if m.group(1) or m.group(2):
        return m.group(0)
    return (m.group(3) + ' int') if m.group(3) else 'int'

inc_re = re.compile(r'(#\s*include\s*")([^"]+)(")')
for d, _, files in os.walk(dst):
    for f in files:
        if not f.endswith(('.cpp', '.h', '.hpp', '.c')):
            continue
        p = os.path.join(d, f)
        raw = open(p, 'rb').read()
        text = raw.decode('utf-8', errors='surrogateescape')
        def fix(m):
            inc = m.group(2).replace('\\', '/')
            real = resolve(d, inc)
            return m.group(1) + (real or inc) + m.group(3)
        new = inc_re.sub(fix, text)
        new = long_re.sub(fix_long, new)
        if new != text:
            open(p, 'wb').write(new.encode('utf-8', errors='surrogateescape'))
