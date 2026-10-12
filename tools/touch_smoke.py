#!/usr/bin/env python3
"""touch_smoke.py -- test the Android front end (android/native) on desktop
Linux.  furb_touch (build.py --touch) is the same program on desktop SDL 2:
the mouse is a finger, and the menu commands the Android activity would send
come from stdin.  This starts it on a private X server, in a landscape and a
portrait window, and checks that it opens a ROM from a command, draws frames,
that the on-screen A button and the keyboard reach the pad, that the MENU
button asks for the menu, that save states, reset and the settings work, and
that it exits cleanly with its settings saved; and that the NSF player plays a
(generated) NSF and changes and stops songs.  Needs Xvfb, xdotool,
openbox and ImageMagick's import; SDL's dummy audio driver stands in for a
sound card.

    python3 tools/touch_smoke.py [--furb build/furb_touch] [--screens DIR]
"""
import argparse, os, queue, shutil, struct, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--furb', default=os.path.join(HERE, '..', 'build', 'furb_touch'))
ap.add_argument('--screens')
a = ap.parse_args()
for tool in ('Xvfb', 'xdotool', 'openbox', 'import'):
	if not shutil.which(tool):
		sys.exit('touch_smoke: %s not found (apt install xvfb xdotool openbox imagemagick)' % tool)

T = tempfile.mkdtemp(prefix='furb_touch_')
fails = []
def check(name, ok, detail=''):
	print('%-58s %s %s' % (name, 'ok  ' if ok else 'FAIL', detail))
	if not ok:
		fails.append(name)

# the pad ROM of gui_smoke.py: the background is black, light blue while A is held
code = bytes([
	0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x80, 0x8D, 0x00, 0x20,
	0xA9, 0x08, 0x8D, 0x01, 0x20,
	0x4C, 0x0F, 0xC0,
	0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40,
	0xAD, 0x16, 0x40, 0x29, 0x01, 0xAA,
	0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,
	0xBD, 0x40, 0xC0, 0x8D, 0x07, 0x20,
	0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,
	0x40])
prg = bytearray(16384)
prg[:len(code)] = code
prg[0x40:0x42] = bytes([0x0F, 0x21])
prg[0x3FFA:0x4000] = struct.pack('<HHH', 0xC012, 0xC000, 0xC012)
rom = os.path.join(T, 'pad game.nes')		# (a space in the name, as Android file names often have)
open(rom, 'wb').write(b'NES\x1a\x01\x01' + bytes(10) + bytes(prg) + bytes(8192))

# an NSF of three songs: INIT starts a square wave, PLAY does nothing
nsf_code = bytes([0xA9, 0x0F, 0x8D, 0x15, 0x40, 0xA9, 0xBF, 0x8D, 0x00, 0x40, 0xA9, 0x40, 0x8D, 0x02, 0x40,
                  0xA9, 0x00, 0x8D, 0x03, 0x40, 0x60]).ljust(0x20, b'\xea') + b'\x60'
nsf = os.path.join(T, 'smoke.nsf')
open(nsf, 'wb').write(b'NESM\x1a\x01\x03\x01' + struct.pack('<HHH', 0x8000, 0x8000, 0x8020) +
                      b'Smoke Test'.ljust(32, b'\0') + b'touch_smoke'.ljust(32, b'\0') + b'2026'.ljust(32, b'\0') +
                      struct.pack('<H', 16639) + bytes(8) + struct.pack('<H', 19997) + bytes(6) + nsf_code)

# (Xvfb's "vsync" is a timer of about 62 Hz: the front end's own 60 Hz clock instead)
env = dict(os.environ, DISPLAY=':78', SDL_AUDIODRIVER='dummy', SDL_RENDER_VSYNC='0')
xvfb = subprocess.Popen(['Xvfb', ':78', '-screen', '0', '1280x1024x24'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
wm = subprocess.Popen(['openbox'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

def xdo(*args):
	return subprocess.run(['xdotool'] + list(args), env=env, capture_output=True, text=True).stdout.strip()
def pixel(x, y):
	r = subprocess.run(['import', '-window', 'root', '-crop', '1x1+%d+%d' % (x, y), 'txt:-'], env=env, capture_output=True, text=True)
	for line in r.stdout.splitlines():
		if '(' in line and not line.startswith('#'):
			return tuple(int(float(v)) for v in line.split('(')[1].split(')')[0].split(',')[:3])
	return None
def shot(name):
	if a.screens:
		os.makedirs(a.screens, exist_ok=True)
		subprocess.run(['import', '-window', 'root', os.path.join(a.screens, name + '.png')], env=env)

# main.cpp's layout(), for a window of w x h showing a 256x240 picture with 8:7 pixels
def layout(w, h):
	ar = 256 * 8 / 7 / 240
	if h > w:
		gw = w; gh = gw / ar
		if gh > h * 0.6: gh = h * 0.6; gw = gh * ar
		game = ((w - gw) / 2, 0, gw, gh)
		u = w; area = h - gh; cy = gh + area * 0.45
		A = (u * 0.84, cy - u * 0.05)
		menu = (u * 0.03 + u * 0.085, gh + u * 0.03 + u * 0.0375)
	else:
		gh = h; gw = gh * ar
		game = ((w - gw) / 2, 0, gw, gh)
		u = h
		A = (w - u * 0.14, u * 0.56)
		menu = (u * 0.03 + u * 0.1, u * 0.03 + u * 0.04)
	return game, A, menu

def run(w, h, tag):
	data = os.path.join(T, 'data')
	furb = subprocess.Popen([a.furb, '--data-dir', data, '--size', '%dx%d' % (w, h)], env=env,
	                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(T, tag + '.log'), 'w'), text=True)
	lines = queue.Queue()
	threading.Thread(target=lambda: [lines.put(l.strip()) for l in furb.stdout], daemon=True).start()
	def send(c):
		furb.stdin.write(c + '\n')
		furb.stdin.flush()
	def expect(prefix, timeout=5):
		end = time.time() + timeout
		while time.time() < end:
			try:
				l = lines.get(timeout=0.2)
			except queue.Empty:
				continue
			if l.startswith(prefix):
				return l
		return None
	try:
		m = expect('menu:')
		check('[%s] with no game, asks for the menu at startup' % tag, m is not None and 'loaded=0' in m, m or '')
		if tag == 'portrait':
			check('[%s] swapped duty cycles kept from last time' % tag, m is not None and 'swapduty=1' in m)
			send('pref swapduty 0')
		win = None
		for _ in range(50):
			win = xdo('search', '--onlyvisible', '--classname', 'furb_touch')
			if win: break
			time.sleep(0.2)
		check('[%s] window shows' % tag, bool(win))
		if not win:
			return
		win = win.split()[0]
		send('open ' + rom)
		send('resume')
		time.sleep(1.5)
		xdo('windowactivate', '--sync', win)
		geo = xdo('getwindowgeometry', win)
		pos = [int(v) for v in geo.split('Position: ')[1].split()[0].split(',')]
		size = [int(v) for v in geo.split('Geometry: ')[1].split()[0].split('x')]
		check('[%s] window is %dx%d' % (tag, w, h), size == [w, h], 'x'.join(map(str, size)))
		game, A, menu = layout(*size)
		gx, gy = int(pos[0] + game[0] + game[2] / 2), int(pos[1] + game[1] + game[3] * 0.45)
		send('state')
		st = expect('state:')
		check('[%s] the game is loaded' % tag, st is not None and 'loaded=1' in st and 'name=pad game' in st, st or '')
		# a new frame every refresh (here a 60 Hz clock: Xvfb has no vsync)
		def pacing():
			send('pacing')
			p = expect('pacing:')
			return dict(kv.split('=') for kv in p.split()[1:]) if p else None
		p0 = pacing(); time.sleep(3); p1 = pacing()
		ok = p0 and p1 and int(p1['refreshes']) - int(p0['refreshes']) > 150 and int(p1['uneven']) - int(p0['uneven']) <= 2 and p1['lock'] == '1'
		check('[%s] one frame per refresh' % tag, ok, '%s -> %s' % (p0, p1))
		before = pixel(gx, gy)
		shot(tag + '-1-running')
		check('[%s] draws the game (black background)' % tag, before is not None and max(before) < 40, str(before))
		# the on-screen A button, pressed with the mouse (a finger)
		xdo('mousemove', str(int(pos[0] + A[0])), str(int(pos[1] + A[1])))
		xdo('mousedown', '1'); time.sleep(0.6)
		held = pixel(gx, gy)
		shot(tag + '-2-touch-A')
		xdo('mouseup', '1'); time.sleep(0.6)
		after = pixel(gx, gy)
		check('[%s] touching A presses A' % tag, held != before and after == before, '%s %s %s' % (before, held, after))
		# the keyboard (X = A)
		xdo('keydown', 'x'); time.sleep(0.6)
		held = pixel(gx, gy)
		xdo('keyup', 'x'); time.sleep(0.6)
		check('[%s] the X key presses A' % tag, held != before and pixel(gx, gy) == before, '%s %s' % (before, held))
		# MENU (the keyboard hid the controls: the first tap brings them back)
		mx, my = str(int(pos[0] + menu[0])), str(int(pos[1] + menu[1]))
		xdo('mousemove', mx, my); xdo('click', '1'); time.sleep(0.3)
		xdo('click', '1')
		m = expect('menu:')
		check('[%s] the MENU button asks for the menu' % tag, m is not None and 'loaded=1' in m, m or '')
		send('resume')
		# save states
		send('save 3'); send('state')
		st = expect('state:')
		check('[%s] save state 3' % tag, st is not None and 'states=3:' in st and os.path.exists(os.path.join(data, 'States', 'pad game.ns3')), st or '')
		send('load 3'); send('reset'); send('hardreset'); time.sleep(0.5)
		check('[%s] load state, reset, power cycle: still running' % tag, furb.poll() is None and pixel(gx, gy) == before)
		send('pref smooth 1'); send('pref aspect 1'); send('state')
		st = expect('state:')
		check('[%s] settings change' % tag, st is not None and 'smooth=1' in st and 'aspect=1' in st, st or '')
		if tag == 'landscape':
			send('pref swapduty 1'); send('state')
			st = expect('state:')
			check('[%s] swapped duty cycles on' % tag, st is not None and 'swapduty=1' in st, st or '')
		send('pref smooth 0'); send('pref aspect 0')
		send('quit')
		try:
			code = furb.wait(10)
		except subprocess.TimeoutExpired:
			code = None
		check('[%s] quits cleanly' % tag, code == 0, str(code))
		cfg = os.path.join(data, 'settings.reg')
		check('[%s] settings saved' % tag, os.path.exists(cfg) and 'TouchSmooth' in open(cfg, encoding='utf-8', errors='replace').read())
	finally:
		if furb.poll() is None:
			furb.kill()

# The NSF player: it plays on opening, NEXT changes the song, a stop stops it
def run_nsf():
	w, h = 540, 960
	furb = subprocess.Popen([a.furb, nsf, '--data-dir', os.path.join(T, 'data'), '--size', '%dx%d' % (w, h)], env=env,
	                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(os.path.join(T, 'nsf.log'), 'w'), text=True)
	lines = queue.Queue()
	threading.Thread(target=lambda: [lines.put(l.strip()) for l in furb.stdout], daemon=True).start()
	def state():
		furb.stdin.write('state\n')
		furb.stdin.flush()
		end = time.time() + 5
		while time.time() < end:
			try:
				l = lines.get(timeout=0.2)
			except queue.Empty:
				continue
			if l.startswith('state:'):
				return l
		return ''
	try:
		win = None
		for _ in range(50):
			win = xdo('search', '--onlyvisible', '--classname', 'furb_touch')
			if win: break
			time.sleep(0.2)
		check('[nsf] window shows', bool(win))
		if not win:
			return
		win = win.split()[0]
		time.sleep(1.5)
		st = state()
		check('[nsf] the NSF plays on opening', 'nsf=3' in st and 'song=1' in st and 'playing=1' in st, st)
		geo = xdo('getwindowgeometry', win)
		pos = [int(v) for v in geo.split('Position: ')[1].split()[0].split(',')]
		shot('nsf-1-player')
		check('[nsf] the player screen is drawn', pixel(pos[0] + 6, pos[1] + 6) == (18 * 257, 16 * 257, 34 * 257) or
		      pixel(pos[0] + 6, pos[1] + 6) == (18, 16, 34), str(pixel(pos[0] + 6, pos[1] + 6)))
		# NEXT, as main.cpp lays it out in a portrait window
		gh = w / (256 * 8 / 7 / 240); cy = gh + (h - gh) * 0.45
		bw, gap = w * 0.27, w * 0.04
		nx = (w - 3 * bw - 2 * gap) / 2 + 2 * (bw + gap) + bw / 2
		xdo('mousemove', str(int(pos[0] + nx)), str(int(pos[1] + cy))); xdo('click', '1'); time.sleep(1)
		st = state()
		check('[nsf] NEXT plays song 2', 'song=2' in st and 'playing=1' in st, st)
		furb.stdin.write('nsf-stop\n'); furb.stdin.flush(); time.sleep(0.5)
		st = state()
		check('[nsf] stop', 'playing=0' in st, st)
		furb.stdin.write('quit\n'); furb.stdin.flush()
		try:
			code = furb.wait(10)
		except subprocess.TimeoutExpired:
			code = None
		check('[nsf] quits cleanly', code == 0, str(code))
	finally:
		if furb.poll() is None:
			furb.kill()

try:
	run(960, 720, 'landscape')
	run(540, 960, 'portrait')
	run_nsf()
finally:
	wm.kill()
	xvfb.kill()
	xvfb.wait()	# (gone before another run starts its own :78)
print('\n%d failure(s); temp dir %s' % (len(fails), T))
sys.exit(1 if fails else 0)
