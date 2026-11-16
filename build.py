#!/usr/bin/env python3
"""Build Furbtendulator (the reference VT/OneBus emulator) for Linux: furb, the
GUI (GTK 3 / SDL 2), and furb_cli, the headless program for scripted runs.

    python3 build.py [--furb DIR] [--build DIR] [-j N] [--m32]

--furb   Furbtendulator source root (default: ./Furbtendulator-src next to this
         script if present (standalone package), else the PocketVT tree's
         gitignored reference/Furbtendulator-main/src)
--build  output dir (default: ./build)
--m32    32-bit x86 build (needs g++-multilib); the default is the native
         architecture (x86_64, arm64, ...).  Both produce identical output.
--no-gui build furb_cli only (the GUI needs libgtk-3-dev and libsdl2-dev)
--cxx, --cc, --sysroot   another compiler / system root (tools/build_bin32.sh
         uses them for the glibc 2.29-compatible 32-bit build in bin32/)
--touch  also build furb_touch: the Android front end (android/native) on
         desktop SDL 2, for testing it without a phone
--android ABI --ndk DIR --sdl DIR --sdl-lib DIR
         the Android libraries for one ABI (arm64-v8a, armeabi-v7a, x86,
         x86_64) with the NDK's clang: BUILD/android/libmain.so (emulator +
         front end) and libfurb_{iNES,FDS,NSF,VS}.so (the mapper packs),
         and furb_cli for the device (tools/adb_furb_cli.py runs it).
         --sdl is SDL 2's source (its include/), --sdl-lib the folder with
         libSDL2.so built for that ABI; --api the lowest Android version
         (default 21; tools/build_android.sh builds the 32-bit ABIs for 19,
         Android 4.4, with NDK r25c).  tools/build_android.sh does it all.

Produces BUILD/furb, BUILD/furb_cli and BUILD/Mappers/{iNES,FDS,NSF,VS}.so (the mapper packs,
loaded at run time exactly like Furbtendulator loads Mappers\*.dll).

The source file lists come from Furbtendulator's own Visual Studio projects.
The code assumes Win32's 32-bit 'long'; prep_src.py rewrites it to 'int' in
the build copy, so the native 64-bit build behaves exactly like the 32-bit one.
Incremental: only changed sources are recompiled.
"""
import argparse, os, re, shlex, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

ap = argparse.ArgumentParser()
_bundled = os.path.join(HERE, 'Furbtendulator-src')
ap.add_argument('--furb', default=_bundled if os.path.isdir(_bundled) else
                os.path.join(REPO, 'reference', 'Furbtendulator-main', 'src'))
ap.add_argument('--build', default=os.path.join(HERE, 'build'))
ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
ap.add_argument('--m32', action='store_true', help='32-bit x86 build (needs g++-multilib), as the original furb_cli')
ap.add_argument('--no-gui', action='store_true', help='build furb_cli only, not the GTK program (furb)')
ap.add_argument('--cxx', default='g++', help='C++ compiler (default g++)')
ap.add_argument('--cc', default='gcc', help='C compiler, for the bundled math (default gcc)')
ap.add_argument('--sysroot', help='build against this system root (tools/build_bin32.sh: an older glibc)')
ap.add_argument('--objcopy', default='objcopy', help='objcopy for the target (default objcopy)')
ap.add_argument('--cflags', default='', help='more flags for every compile and link (e.g. --target=... for clang)')
ap.add_argument('--touch', action='store_true', help='also build furb_touch, the Android front end on desktop SDL 2')
ap.add_argument('--android', metavar='ABI', choices=['arm64-v8a', 'armeabi-v7a', 'x86', 'x86_64'])
ap.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT'))
ap.add_argument('--api', type=int, default=21, help='lowest Android API level (default 21, Android 5.0; 19, Android 4.4, needs NDK r25c)')
ap.add_argument('--sdl', help='SDL 2 source tree (for --android)')
ap.add_argument('--sdl-lib', help='folder with libSDL2.so for the ABI (for --android)')
args = ap.parse_args()

# Android: the NDK's clang for the ABI, position-independent code throughout
# (the emulator is a library there, loaded by the app), no GTK program
ANDROID_TARGET = {'arm64-v8a': 'aarch64-linux-android', 'armeabi-v7a': 'armv7a-linux-androideabi',
                  'x86': 'i686-linux-android', 'x86_64': 'x86_64-linux-android'}
if args.android:
    if not (args.ndk and args.sdl and args.sdl_lib):
        sys.exit('build.py: --android needs --ndk, --sdl and --sdl-lib')
    tc = os.path.join(args.ndk, 'toolchains', 'llvm', 'prebuilt', 'linux-x86_64', 'bin')
    args.cxx, args.cc, args.objcopy = os.path.join(tc, 'clang++'), os.path.join(tc, 'clang'), os.path.join(tc, 'llvm-objcopy')
    args.no_gui = True

if not os.path.isdir(os.path.join(args.furb, 'src-main')):
    sys.exit('build.py: no Furbtendulator source at %s (pass --furb, or unzip Furbtendulator-main.zip '
             'into PocketVT\'s reference/)' % args.furb)

B = os.path.abspath(args.build)
SRC = os.path.join(B, 'src')
OBJ = os.path.join(B, 'obj')
os.makedirs(os.path.join(B, 'Mappers'), exist_ok=True)

# 1. copy + POSIX-fix the sources when the pristine tree is newer than the copy
stamp = os.path.join(B, '.prepped')
def newest(root):
    t = 0
    for d, _, fs in os.walk(root):
        for f in fs:
            t = max(t, os.path.getmtime(os.path.join(d, f)))
    return t
prep = os.path.join(HERE, 'prep_src.py')
if (not os.path.exists(stamp) or os.path.getmtime(stamp) < max(newest(args.furb), os.path.getmtime(prep))):
    print('preparing sources ...')
    subprocess.check_call([sys.executable, prep, args.furb, SRC])
    open(stamp, 'w').close()

def vcx_sources(proj, subdir):
    text = open(proj, encoding='utf-8', errors='replace').read()
    out = []
    for rel in re.findall(r'ClCompile Include="([^"]+)"', text):
        rel = rel.replace('\\', '/')
        assert rel.startswith('../src/'), rel
        p = os.path.join(SRC, subdir, 'src', rel[len('../src/'):])
        if not os.path.exists(p):		# case differences (e.g. DLL/ vs Dll/)
            d, f = os.path.split(p)
            parent, leaf = os.path.split(d)
            for n in os.listdir(parent):
                if n.lower() == leaf.lower():
                    p = os.path.join(parent, n, f)
        out.append(p)
    return out

main_srcs = vcx_sources(os.path.join(args.furb, 'src-main', 'msvc100', 'Nintendulator.vcxproj'), 'src-main')
# the four mapper packs, each its own shared library like the Windows DLLs
PACKS = [('iNES', 'INES.vcxproj', 'INES_EXPORTS'), ('FDS', 'FDS.vcxproj', 'FDS_EXPORTS'),
         ('NSF', 'NSF.vcxproj', 'NSF_EXPORTS'), ('VS', 'VS.vcxproj', 'VS_EXPORTS')]
pack_srcs = {name: vcx_sources(os.path.join(args.furb, 'src-mappers', 'msvc100', proj), 'src-mappers')
             for name, proj, _ in PACKS}

COMPAT = os.path.join(HERE, 'compat')
ARCH = ['-m32', '-msse2', '-mfpmath=sse'] if args.m32 else []
if args.sysroot:
    ARCH += ['--sysroot=' + os.path.abspath(args.sysroot)]
if args.android:
    ARCH += ['--target=%s%d' % (ANDROID_TARGET[args.android], args.api), '-fPIC']
    if args.android == 'x86':	# SSE arithmetic, as the 32-bit Linux build (the ABI has SSSE3)
        ARCH += ['-msse2', '-mfpmath=sse']
ARCH += shlex.split(args.cflags)
CXX = [args.cxx] + ARCH + ['-O2', '-std=gnu++17', '-fpermissive', '-w', '-fno-operator-names',
       '-fwrapv', '-fno-strict-aliasing', '-I' + COMPAT, '-DUNICODE', '-D_UNICODE',
       '-DWIN32', '-D_WINDOWS', '-DNDEBUG', '-include', 'stdexcept', '-include', 'cstring',
       '-include', 'locale', '-include', 'codecvt',	# MSVC's headers pull these in implicitly
       '-Dregister=',	# (removed in C++17: g++ only warns, clang refuses it)
       # the same arithmetic on every CPU: no fused multiply-add (ARM compilers
       # fuse a*b+c by default, and the palette generator's hue search then
       # picks other colours) and signed char as on x86 (ARM's is unsigned)
       '-ffp-contract=off', '-fsigned-char']
# keep-inline: MSVC emits 'inline' members that other files call (PPU IncrementH;
# prep_src.py also un-inlines it, for clang, which ignores the option)
MAIN_INC = ['-I' + os.path.join(SRC, 'src-main', 'src'), '-fkeep-inline-functions']
EMU = ['-include', 'furb_math.h']	# libm calls -> the bundled musl subset (compat/furb_math.h)
DLL_FLAGS = ['-fPIC', '-fvisibility=hidden', '-D_USRDLL', '-DFURB_PACK']

# Each module's .rc script (menus, accelerators, dialogs, strings) compiled to
# C++ tables by gui/rc2cpp.py; the address of a module's table is its
# HINSTANCE.  furb_cli links them too (the packs are shared with the GUI).
RC = {'main': ('src-main/src', 'Nintendulator.rc'), 'iNES': ('src-mappers/src/iNES', 'iNES.rc'),
      'FDS': ('src-mappers/src/FDS', 'FDS.rc'), 'NSF': ('src-mappers/src/NSF', 'NSF.rc'),
      'VS': ('src-mappers/src/Vs', 'VS.rc')}
RC2CPP = os.path.join(HERE, 'gui', 'rc2cpp.py')
def resources(module):
    d, rc = RC[module]
    d = os.path.join(args.furb, d)
    rc = os.path.join(d, next(f for f in os.listdir(d) if f.lower() == rc.lower()))
    hdr = os.path.join(d, 'resource.h')
    out = os.path.join(B, 'gen', 'res_%s.cpp' % module)
    if not os.path.exists(out) or os.path.getmtime(out) < max(map(os.path.getmtime, (rc, hdr, RC2CPP))):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.check_call([sys.executable, RC2CPP, rc, hdr, out])
    return out

def obj_for(src, tag):
    rel = os.path.relpath(src, SRC).replace('/', '__').replace(' ', '_')
    return os.path.join(OBJ, tag, os.path.splitext(rel)[0] + '.o')

jobs = []
for s in main_srcs:
    jobs.append((s, obj_for(s, 'main'), CXX + MAIN_INC + EMU))
jobs.append((os.path.join(HERE, 'furb_cli.cpp'), os.path.join(OBJ, 'main', 'furb_cli.o'), CXX + MAIN_INC))
jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, 'main', 'compat.o'), CXX))
jobs.append((os.path.join(COMPAT, 'host_reg.cpp'), os.path.join(OBJ, 'main', 'host_reg.o'), CXX))
jobs.append((os.path.join(COMPAT, 'host_cli.cpp'), os.path.join(OBJ, 'main', 'host_cli.o'), CXX))
jobs.append((resources('main'), os.path.join(OBJ, 'main', 'res_main.o'), CXX))
# The GUI program (gui/, furb): the same emulator objects with a GTK 3 / SDL 2
# Win32 layer instead of furb_cli's front end and headless host.
GUI = os.path.join(HERE, 'gui')
gui_flags = gui_libs = None
if not args.no_gui:
    try:
        env = dict(os.environ)
        triplet = 'i386-linux-gnu' if args.m32 else 'x86_64-linux-gnu'
        root = os.path.abspath(args.sysroot) if args.sysroot else ''
        if args.m32 or root:	# only the target's .pc files
            env['PKG_CONFIG_LIBDIR'] = '%s/usr/lib/%s/pkgconfig:%s/usr/share/pkgconfig' % (root, triplet, root)
        if root:
            env['PKG_CONFIG_SYSROOT_DIR'] = root
        gui_flags = subprocess.check_output(['pkg-config', '--cflags', 'gtk+-3.0', 'sdl2'], text=True, env=env).split()
        gui_libs = subprocess.check_output(['pkg-config', '--libs', 'gtk+-3.0', 'sdl2'], text=True, env=env).split()
        if root:	# GTK's own dependencies, found inside the sysroot
            gui_libs += ['-Wl,-rpath-link=%s/usr/lib/%s:%s/lib/%s' % (root, triplet, root, triplet)]
    except (OSError, subprocess.CalledProcessError):
        print('build.py: GTK 3 / SDL 2 development files not found (apt install libgtk-3-dev libsdl2-dev): building furb_cli only')
if gui_flags:
    for f in sorted(os.listdir(GUI)):
        if f.endswith('.cpp'):
            jobs.append((os.path.join(GUI, f), os.path.join(OBJ, 'gui', f[:-4] + '.o'), CXX + ['-UWIN32', '-U_WINDOWS'] + gui_flags + ['-I' + GUI]))

# The touch screen front end (android/native): the Android app's, or
# furb_touch on desktop SDL 2.  (-UWIN32: SDL's headers would think it Windows)
TOUCH = os.path.join(HERE, 'android', 'native')
touch_flags = touch_libs = None
if args.android:
    touch_flags = ['-I' + os.path.join(args.sdl, 'include')]
elif args.touch:
    try:
        touch_flags = subprocess.check_output(['pkg-config', '--cflags', 'sdl2'], text=True).split()
        touch_libs = subprocess.check_output(['pkg-config', '--libs', 'sdl2'], text=True).split()
    except (OSError, subprocess.CalledProcessError):
        sys.exit('build.py: --touch needs SDL 2 (apt install libsdl2-dev)')
if touch_flags is not None:
    jobs.append((os.path.join(TOUCH, 'main.cpp'), os.path.join(OBJ, 'touch', 'main.o'),
                 CXX + MAIN_INC + ['-UWIN32', '-U_WINDOWS', '-I' + TOUCH] + touch_flags))

for name, _, define in PACKS:
    flags = CXX + DLL_FLAGS + ['-D' + define]
    for s in pack_srcs[name]:
        jobs.append((s, obj_for(s, name), flags + EMU))
    jobs.append((os.path.join(COMPAT, 'compat.cpp'), os.path.join(OBJ, name, 'compat.o'), flags))
    jobs.append((resources(name), os.path.join(OBJ, name, 'res_%s.o' % name), flags))

hdr_time = max([os.path.getmtime(os.path.join(COMPAT, f)) for f in os.listdir(COMPAT)] + [os.path.getmtime(os.path.join(HERE, 'gui', 'gui.h')),
                os.path.getmtime(os.path.join(HERE, 'android', 'native', 'font.h'))])
# (furb_cli.cpp also depends on the headers; handled by the same rule)

def compile_one(job):
    src, obj, flags = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= max(os.path.getmtime(src), hdr_time):
        return None
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run(flags + ['-c', src, '-o', obj], capture_output=True, text=True)
    if r.returncode:
        return '%s\n%s' % (src, r.stderr[-4000:])
    return None

print('compiling %d sources (-j%d) ...' % (len(jobs), args.j))
with ThreadPoolExecutor(args.j) as ex:
    errors = [e for e in ex.map(compile_one, jobs) if e]
if errors:
    for e in errors[:5]:
        print(e, file=sys.stderr)
    open(os.path.join(B, 'compile_errors.log'), 'w').write('\n'.join(errors))
    sys.exit('build.py: %d file(s) failed to compile (all of them: %s)' % (len(errors), os.path.join(B, 'compile_errors.log')))

objs_of = lambda tag: [j[1] for j in jobs if os.sep + tag + os.sep in j[1]]
obj_for_host = lambda name: os.path.join(OBJ, 'main', name)

# The musl math subset (compat/libm): compiled as C without FMA contraction,
# merged into one object whose only global symbols are furb_sin, furb_pow, ...
# so it cannot interpose on the system libm.  Linked into the executable and
# every pack (each pack is self-contained, like a DLL).
LIBM = os.path.join(COMPAT, 'libm')
MATH_FUNCS = ['sin', 'cos', 'tan', 'exp', 'log', 'log2', 'log10', 'pow', 'sinf', 'cosf', 'powf']
libm_obj = os.path.join(OBJ, 'furb_libm.o')
libm_srcs = sorted(os.path.join(LIBM, f) for f in os.listdir(LIBM) if f.endswith('.c'))
if not os.path.exists(libm_obj) or os.path.getmtime(libm_obj) < max(
        os.path.getmtime(os.path.join(LIBM, f)) for f in os.listdir(LIBM)):
    print('compiling the musl math subset ...')
    mo = []
    for f in libm_srcs:
        o = os.path.join(OBJ, 'libm', os.path.basename(f)[:-2] + '.o')
        os.makedirs(os.path.dirname(o), exist_ok=True)
        subprocess.check_call([args.cc] + ARCH + ['-std=c99', '-D_XOPEN_SOURCE=700', '-O2', '-ffreestanding', '-fPIC',
            '-fexcess-precision=standard', '-frounding-math', '-ffp-contract=off', '-fno-strict-aliasing',
            '-U__FP_FAST_FMA', '-fno-builtin', '-fvisibility=hidden', '-include', os.path.join(LIBM, 'furb_musl.h'), '-I' + LIBM,
            '-c', f, '-o', o])
        mo.append(o)
    subprocess.check_call([args.cc] + ARCH + ['-r', '-nostdlib', '-o', libm_obj + '.tmp'] + mo)
    cmd = [args.objcopy, '--wildcard', '--keep-global-symbol=__x86.get_pc_thunk.*']	# i386 PIC thunks are COMDAT
    for fn in MATH_FUNCS:
        # (both names: GNU objcopy keeps by the new name, llvm-objcopy by the old)
        cmd += ['--redefine-sym', '%s=furb_%s' % (fn, fn), '--keep-global-symbol=furb_' + fn, '--keep-global-symbol=' + fn]
    subprocess.check_call(cmd + [libm_obj + '.tmp', libm_obj])
    os.remove(libm_obj + '.tmp')
exe = os.path.join(B, 'furb_cli')
def link(what, cmd):
    print('linking %s ...' % what)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        undef = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
        sys.exit('build.py: linking %s failed\n%s' % (what, '\n'.join(undef) or r.stderr[-4000:]))

emu = [o for o in objs_of('main') if os.path.basename(o) not in ('furb_cli.o', 'host_cli.o')]
if args.android:
    # The app's libraries.  The packs are named libfurb_<pack>.so, as Android
    # wants libraries in an app (compat.cpp's LoadLibrary finds them so).
    # 16 KiB pages: Android 15 devices may use them.
    AOUT = os.path.join(B, 'android')
    os.makedirs(AOUT, exist_ok=True)
    # (no-dependent-libraries: lld would follow the sources' #pragma comment(lib, "winmm.lib");
    # strip-debug: NDK r25c's C++ runtime brings its debug information along)
    ALINK = [args.cxx] + ARCH + ['-shared', '-static-libstdc++', '-Wl,-z,max-page-size=16384', '-Wl,--gc-sections',
                                 '-Wl,--no-dependent-libraries', '-Wl,--strip-debug']
    for name, _, _ in PACKS:
        link('libfurb_%s.so' % name, ALINK + ['-Wl,--exclude-libs,ALL', '-fvisibility=hidden', '-Wl,-z,defs',
             '-o', os.path.join(AOUT, 'libfurb_%s.so' % name)] + objs_of(name) + [libm_obj, '-ldl'])
    # libmain.so: SDL's Java side calls its SDL_main, FurbActivity its
    # nativeCommand; nothing else is exported (android/native/libmain.map)
    link('libmain.so', ALINK + ['-Wl,-z,defs', '-Wl,--version-script=' + os.path.join(TOUCH, 'libmain.map'),
         '-o', os.path.join(AOUT, 'libmain.so')] + emu + [obj_for_host('host_cli.o'), os.path.join(OBJ, 'touch', 'main.o'), libm_obj,
         '-L' + os.path.abspath(args.sdl_lib), '-lSDL2', '-llog', '-landroid', '-ldl', '-lm'])
    # furb_cli for the device, to run with adb (not part of the app): the
    # emulation as the app runs it, for comparing with a PC build
    # (tools/adb_furb_cli.py).  It finds the packs next to itself.
    link('furb_cli (Android)', [args.cxx] + ARCH + ['-pie', '-static-libstdc++', '-Wl,--gc-sections', '-Wl,--no-dependent-libraries',
         '-Wl,--dynamic-list=' + os.path.join(HERE, 'exports.list'), '-o', os.path.join(AOUT, 'furb_cli')] +
         objs_of('main') + [libm_obj, '-llog', '-ldl'])
else:
    # -z defs: the pack must be self-contained, like a DLL (fail at build, not dlopen)
    for name, _, _ in PACKS:
        link('Mappers/%s.so' % name, [args.cxx] + ARCH + ['-shared', '-static-libstdc++', '-static-libgcc',
             '-Wl,--exclude-libs,ALL', '-fvisibility=hidden', '-Wl,-z,defs',
             '-o', os.path.join(B, 'Mappers', name + '.so')] + objs_of(name) + [libm_obj, '-ldl'])
    # export exactly one symbol, furb_host_lookup, through which the packs reach
    # the executable's dialogs / cursor / file pickers (compat.cpp)
    link('furb_cli', [args.cxx] + ARCH + ['-static-libstdc++', '-static-libgcc',
         '-Wl,--dynamic-list=' + os.path.join(HERE, 'exports.list'), '-o', exe] + objs_of('main') + [libm_obj, '-ldl'])

# Furbtendulator's data files belong next to the program, as on Windows:
# cheats.cfg (cheat database), dip.cfg (DIP switch definitions), fastload.cfg,
# and the BIOS/ and samples/ folders (their dir.txt lists what goes there).
for data in (os.path.join(os.path.dirname(args.furb), 'bin'), os.path.join(args.furb, 'bin-data')):
    if os.path.isdir(data):
        for f in os.listdir(data):
            src = os.path.join(data, f)
            if f.endswith('.cfg'):
                shutil.copy(src, os.path.join(B, f))
            elif f in ('BIOS', 'samples') and os.path.isdir(src):
                os.makedirs(os.path.join(B, f), exist_ok=True)
                for g in os.listdir(src):
                    if not os.path.exists(os.path.join(B, f, g)):
                        shutil.copy(os.path.join(src, g), os.path.join(B, f, g))
        break
if gui_flags:
    gui_exe = os.path.join(B, 'furb')
    link('furb', [args.cxx] + ARCH + ['-static-libstdc++', '-static-libgcc',
         '-Wl,--dynamic-list=' + os.path.join(HERE, 'exports.list'), '-o', gui_exe] + emu + objs_of('gui') +
         [libm_obj, '-ldl', '-lpthread'] + gui_libs)
    print('built %s' % gui_exe)
if args.touch:
    touch_exe = os.path.join(B, 'furb_touch')
    link('furb_touch', [args.cxx] + ARCH + ['-static-libstdc++', '-static-libgcc',
         '-Wl,--dynamic-list=' + os.path.join(HERE, 'exports.list'), '-o', touch_exe] + emu +
         [obj_for_host('host_cli.o'), os.path.join(OBJ, 'touch', 'main.o'), libm_obj, '-ldl', '-lpthread'] + touch_libs)
    print('built %s' % touch_exe)
if args.android:
    print('built %s' % os.path.join(B, 'android'))
else:
    print('built %s' % exe)
