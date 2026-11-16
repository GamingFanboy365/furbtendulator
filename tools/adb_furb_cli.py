#!/usr/bin/env python3
"""adb_furb_cli.py -- run furb_cli on an Android device (or emulator) through
adb, as if it were a local furb_cli: the ROM is copied over, the output files
(--wav, --out, --data-dir, --save-config) come back to the paths given, and
the standard output and exit code are furb_cli's.  So tools/compare_builds.py
can compare the emulation inside Android with a PC build:

    tools/adb_furb_cli.py --setup BUILD/android      # once: furb_cli + mapper packs
    python3 tools/compare_builds.py build/furb_cli tools/adb_furb_cli.py ROMS...

BUILD/android is what build.py --android ABI makes (tools/build_android.sh:
build-android/build-ABI/android).  ANDROID_SERIAL picks the device, FURB_ADB
the adb program (default: adb), FURB_ADB_DIR the folder on the device
(default /data/local/tmp/furb).
"""
import os, random, shlex, subprocess, sys, tempfile

ADB = os.environ.get('FURB_ADB', 'adb')
RDIR = os.environ.get('FURB_ADB_DIR', '/data/local/tmp/furb')
PACKS = ('iNES', 'FDS', 'NSF', 'VS')


def adb(*args, **kw):
	return subprocess.run([ADB] + list(args), capture_output=True, **kw)


def shell(cmd):
	"""run a command on the device; its output (adb shell's exit code is not
	the command's on older Android, so callers echo $? themselves)"""
	r = adb('shell', cmd)
	return r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')


def setup(build):
	shell('rm -r %s; mkdir -p %s/Mappers' % (RDIR, RDIR))
	files = [os.path.join(build, 'furb_cli')] + [os.path.join(build, 'libfurb_%s.so' % p) for p in PACKS]
	r = adb('push', *files, RDIR + '/')
	if r.returncode:
		sys.exit('adb_furb_cli: push failed: %s' % r.stderr.decode(errors='replace'))
	# the Mappers folder names the packs, as in the app; furb_cli finds them
	# next to itself as libfurb_<pack>.so (compat.cpp's LoadLibrary)
	shell('cd %s && chmod 755 furb_cli && for p in %s; do echo > Mappers/$p.so; done' % (RDIR, ' '.join(PACKS)))
	print('set up %s on the device' % RDIR)


def main(argv):
	if argv[:1] == ['--setup']:
		return setup(argv[1] if len(argv) > 1 else '.')
	work = '%s/run-%d-%d' % (RDIR, os.getpid(), random.randrange(1 << 30))
	shell('mkdir -p %s' % work)
	pulls = []		# (remote, local): files to bring back
	prefixes = []		# (remote prefix, local prefix): --out
	args, i, rom_done = [], 0, False
	while i < len(argv):
		a = argv[i]
		if a in ('--data-dir', '--wav', '--avi', '--out', '--config', '--save-config') and i + 1 < len(argv):
			local = argv[i + 1]
			if a == '--data-dir':
				remote = work + '/data'
			elif a == '--config':
				remote = work + '/config.reg'
				adb('push', local, remote)
			elif a == '--out':
				remote = work + '/out'
				prefixes.append((remote, local))
			else:
				remote = work + '/' + a[2:] + os.path.splitext(local)[1]
				pulls.append((remote, local))
			args += [a, remote]
			i += 2
			continue
		if not a.startswith('-') and not rom_done and os.path.isfile(a):
			remote = work + '/' + os.path.basename(a)
			r = adb('push', a, remote)
			if r.returncode:
				sys.exit('adb_furb_cli: cannot copy %s: %s' % (a, r.stderr.decode(errors='replace')))
			args.append(remote)
			rom_done = True
		else:
			args.append(a)
		i += 1
	cmd = 'cd %s && ./furb_cli %s > %s/stdout 2> %s/stderr; echo "exit=$?"' % (
		RDIR, ' '.join(shlex.quote(a) for a in args), work, work)
	status = shell(cmd)
	code = 1
	for line in status.splitlines():
		if line.startswith('exit='):
			code = int(line[5:])
	with tempfile.TemporaryDirectory() as t:
		for name, stream in (('stdout', sys.stdout.buffer), ('stderr', sys.stderr.buffer)):
			if adb('pull', '%s/%s' % (work, name), os.path.join(t, name)).returncode == 0:
				stream.write(open(os.path.join(t, name), 'rb').read())
				stream.flush()
	for remote, local in pulls:
		adb('pull', remote, local)
	for remote, local in prefixes:
		base = os.path.basename(remote)
		for f in shell('ls %s' % work).split():
			if f.startswith(base):
				adb('pull', '%s/%s' % (work, f), local + f[len(base):])
	shell('rm -r %s' % work)
	return code


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
