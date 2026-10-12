// Furbtendulator for Android -- and furb_touch, the same program for desktop
// Linux, used to test it.  A touch screen front end on SDL 2.
//
// The emulator runs the way furb_cli runs it: one thread, a frame at a time,
// on the headless host (compat/host_cli.cpp), here paced by the screen's
// refresh (pacing.h).  This file adds the picture, the sound, on-screen
// touch controls, gamepads and keyboards.  The menus are
// the Android activity's (FurbActivity.java); it sends the choices here as
// text commands (run_command below).  furb_touch reads the same commands
// from stdin and prints what the menu would show, for testing.
#include <SDL.h>
#include "StdAfx.h"
#include "Nintendulator.h"
#include "Settings.h"
#include "MapperInterface.h"
#include "NES.h"
#include "Sound.h"
#include "APU.h"
#include "CPU.h"
#include "PPU.h"
#include "GFX.h"
#include "Controllers.h"
#include "OneBus.h"
#include "OneBus_VT369.h"
#include "States.h"
#include "Movie.h"
#include "furb_host.h"
#include "font.h"
#include "pacing.h"
#include <locale.h>
#include <sys/stat.h>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <jni.h>
#endif

namespace GFX { extern int OFFSETX, OFFSETY; }
namespace Controllers { extern DIJOYSTATE2 JoyState[MAX_CONTROLLERS]; }
extern "C" int mkdir(const char *, mode_t);
extern "C" ssize_t readlink(const char *, char *, size_t);	// <unistd.h> clashes with Furb names

namespace {

// ================================================================ commands
// From the activity's menus (Android, any thread) or stdin (desktop), run
// between frames on this thread.
std::mutex command_lock;
std::deque<std::string> commands;
void queue_command(const std::string &c) {
	std::lock_guard<std::mutex> l(command_lock);
	commands.push_back(c);
}
bool next_command(std::string &c) {
	std::lock_guard<std::mutex> l(command_lock);
	if (commands.empty()) return false;
	c = commands.front();
	commands.pop_front();
	return true;
}

// ================================================================ Java side
#ifdef __ANDROID__
jstring to_java(JNIEnv *env, const std::string &s) {
	std::wstring w = furb_widen(s.c_str());
	std::vector<jchar> u;
	for (wchar_t c : w) {
		if (c >= 0x10000) { c -= 0x10000; u.push_back((jchar)(0xD800 | c >> 10)); u.push_back((jchar)(0xDC00 | (c & 0x3FF))); }
		else u.push_back((jchar)c);
	}
	return env->NewString(u.data(), (jsize)u.size());
}
std::string from_java(JNIEnv *env, jstring s) {
	const jchar *u = env->GetStringChars(s, NULL);
	jsize n = env->GetStringLength(s);
	std::wstring w;
	for (jsize i = 0; i < n; i++) {
		unsigned c = u[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
		w += (wchar_t)c;
	}
	env->ReleaseStringChars(s, u);
	return furb_narrow(w.c_str());
}
// FurbActivity.<method>(String)
void call_activity(const char *method, const std::string &arg) {
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject act = (jobject)SDL_AndroidGetActivity();
	if (!env || !act) return;
	jclass cls = env->GetObjectClass(act);
	jmethodID m = env->GetMethodID(cls, method, "(Ljava/lang/String;)V");
	if (m) {
		jstring s = to_java(env, arg);
		env->CallVoidMethod(act, m, s);
		env->DeleteLocalRef(s);
	}
	if (env->ExceptionCheck()) env->ExceptionClear();
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(act);
}
#endif

// ================================================================ paths, prefs
std::string prog_dir, data_dir, config_path;

// The front end's own settings live in the same registry file as
// Furbtendulator's, under names it does not use.
struct Pref { const wchar_t *name; int value; };
Pref prefs[] = {
	{L"TouchAspect", 0},	// 0: 8:7 pixels, like a TV; 1: square pixels
	{L"TouchSmooth", 0},	// filter the picture when scaling
	{L"TouchOpacity", 45},	// on-screen controls, percent
	{L"TouchHaptics", 1},	// buzz when an on-screen button is pressed
	{L"TouchShowFPS", 0},
	{L"TouchSound", 1},
	{L"TouchControls", 1},	// 0: never show the on-screen controls
};
enum { P_ASPECT, P_SMOOTH, P_OPACITY, P_HAPTICS, P_FPS, P_SOUND, P_CONTROLS, P_COUNT };
const char *pref_keys[P_COUNT] = {"aspect", "smooth", "opacity", "haptics", "fps", "sound", "controls"};
int pref(int i) { return prefs[i].value; }
void load_prefs(void) {
	for (auto &p : prefs) {
		DWORD v;
		if (FurbHost::get_dword(p.name, v)) p.value = (int)v;
	}
}
void save_settings(void) {
	for (auto &p : prefs) FurbHost::set_dword(p.name, (DWORD)p.value);
	Settings::SaveSettings();
	if (!FurbHost::save_config(config_path)) SDL_Log("furb: cannot write %s", config_path.c_str());
}

// ================================================================ messages
std::string osd_text;
Uint32 osd_until = 0;
void osd(const std::string &t, Uint32 ms = 2000) {
	osd_text = t;
	osd_until = SDL_GetTicks() + ms;
}
// Furbtendulator's own status messages (PrintTitlebar: "State saved", disk
// changes, ...) go to the title bar on Windows; here they are shown on screen.
void poll_titlebar(void) {
	if (TitlebarDelay > 0) {
		osd(furb_narrow(TitlebarBuffer));
		TitlebarDelay = 0;
	}
}

// ================================================================ the emulator
// Ports and buttons as furb_cli maps them: port i's buttons are the buttons
// of virtual joystick 2+i, whose state this file sets before every frame.
Controllers::StdPort **std_port(int i) {
	using namespace Controllers;
	Controllers::StdPort **t[6] = {&Port1, &Port2, &FSPort1, &FSPort2, &FSPort3, &FSPort4};
	return t[i];
}
DWORD *port_buttons(int i) {
	using namespace Controllers;
	DWORD *t[7] = {Port1_Buttons, Port2_Buttons, FSPort1_Buttons, FSPort2_Buttons, FSPort3_Buttons, FSPort4_Buttons, PortExp_Buttons};
	return t[i];
}
void map_buttons(void) {
	for (int p = 0; p < 7; p++)
		for (int i = 0; i < CONTROLLERS_MAXBUTTONS; i++)
			port_buttons(p)[i] = (DWORD)((2 + p) << 16 | i);
	for (int p = 0; p < 6; p++) (*std_port(p))->SetMasks();
	Controllers::PortExp->SetMasks();
}
// player 0..3 -> port index, following what is plugged in (furb_cli's resolve_pad)
int player_port(int n) {
	using namespace Controllers;
	bool fourscore = Port1->Type == STD_FOURSCORE || PortExp->Type == EXP_HORI4PLAY || VSDUAL;
	if (fourscore) return 2 + n;
	return n == 0 ? 0 : n == 1 ? 1 : 2 + n;
}
// the port whose device is a light gun, or -1
int gun_port(void) {
	using namespace Controllers;
	for (int p = 0; p < 2; p++) {
		int t = (*std_port(p))->Type;
		if (t == STD_ZAPPER || t == STD_VSZAPPER) return p;
	}
	if (PortExp->Type == EXP_ZAPPER) return 6;
	return -1;
}

enum { B_A, B_B, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_TURBO_A, B_TURBO_B };
uint32_t player_bits[4];		// this frame's buttons per player
bool gun_trigger = false;
int gun_x = 0, gun_y = 0;

std::string rom_path, rom_name;
std::vector<uint8_t> pending_audio;
std::vector<int16_t> scope;		// the last frame's sound, for the NSF player's waveform

// The NSF player: Furbtendulator's NSF pack runs the music and keeps its
// title, artist, track names and Play/Stop/song controls in its control
// window (IDD_NSF; headless here, see host_cli.cpp), which this reads and
// clicks like the Windows dialog's user would.
int nsf_song = 1;
bool nsf_playing = false;
long nsf_frames = 0;		// since Play
enum { NSF_DLG = 101, NSF_TITLE = 1000, NSF_ARTIST = 1001, NSF_COPYRIGHT = 1002, NSF_PLAY = 1005, NSF_STOP = 1006,
       NSF_SELECT = 1007, NSF_CHIP = 1013, NSF_TRACK_NAME = 1015 };

bool hires(void) { return RI.ConsoleType == CONSOLE_VT369 && (reg2000[0x1C] & 0x04); }

// One frame, exactly as furb_cli runs it (NES::Thread's loop without the
// debugger and the frame-step machinery).
void run_frame(void) {
	for (int p = 0; p < 7; p++) memset(Controllers::JoyState[2 + p].rgbButtons, 0, 128);
	for (int n = 0; n < 4; n++)
		for (int b = 0; b < CONTROLLERS_MAXBUTTONS; b++)
			if (player_bits[n] >> b & 1) Controllers::JoyState[2 + player_port(n)].rgbButtons[b] = 0x80;
	int g = gun_port();
	if (g >= 0) {
		FurbHost::set_cursor(gun_x, gun_y);
		if (gun_trigger) Controllers::JoyState[2 + g].rgbButtons[0] = 0x80;
	}
	for (;;) {
		if (CPU::CPU[1]) {
			if (CPU::CPU[0]->CycleCount <= CPU::CPU[1]->CycleCount) CPU::CPU[0]->ExecOp();
			else CPU::CPU[1]->ExecOp();
		} else
			CPU::CPU[0]->ExecOp();
		if (!NES::Scanline) continue;
		NES::Scanline = FALSE;
		if (PPU::PPU[0]->SLnum == 240) break;
		if (PPU::PPU[0]->SLnum == (RI.InputType == INPUT_FOURSCORE ? 10 : PPU::PPU[0]->SLStartNMI)) {
			if (NES::GenericMulticart) Controllers::SwitchControllersAutomatically();
			Controllers::UpdateInput();
		}
	}
}

// The visible picture (what GFX::SaveScreenshot saves: VT369's hi-res mode
// is twice the size) in 32-bit xRGB.
std::vector<uint32_t> picture;
int pic_w = 0, pic_h = 0;
void grab_picture(void) {
	if (hires()) {
		auto *p = dynamic_cast<PPU::PPU_VT369 *>(PPU::PPU[0]);
		pic_w = GFX::SIZEX * 2; pic_h = GFX::SIZEY * 2;
		picture.resize((size_t)pic_w * pic_h);
		const uint16_t *even = p->DrawArrayEven + 341 * 2 * GFX::OFFSETY + 2 * GFX::OFFSETX;
		const uint16_t *odd = p->DrawArrayOdd + 341 * 2 * GFX::OFFSETY + 2 * GFX::OFFSETX;
		for (int y = 0; y < pic_h; y++) {
			const uint16_t *src = (y & 1 ? odd : even) + y / 2 * 341 * 2;
			for (int x = 0; x < pic_w; x++) picture[(size_t)y * pic_w + x] = (uint32_t)GFX::Palette32[src[x]];
		}
	} else {
		pic_w = GFX::SIZEX; pic_h = GFX::SIZEY;
		picture.resize((size_t)pic_w * pic_h);
		const uint16_t *src = PPU::PPU[0]->DrawArray + GFX::OFFSETY * 341 + GFX::OFFSETX;
		for (int y = 0; y < pic_h; y++)
			for (int x = 0; x < pic_w; x++) picture[(size_t)y * pic_w + x] = (uint32_t)GFX::Palette32[src[y * 341 + x]];
	}
}

bool loaded(void) { return NES::ROMLoaded && PPU::PPU[0]; }
double game_fps(void) { return NES::CurRegion == Settings::REGION_NTSC ? 39375000.0 / 655171 : 26601712.0 / 531960; }
// Ask for a screen mode whose refresh is a whole multiple of the game's
// (60 Hz rather than 90 on such phones), so that every frame is shown for
// the same time (FurbActivity.preferRefresh).
void prefer_refresh(void) {
#ifdef __ANDROID__
	call_activity("preferRefresh", std::to_string(std::lround(game_fps() * 1000)));
#endif
}
bool is_nsf(void) { return loaded() && RI.ROMType == ROM_NSF; }
std::string nsf_field(int id) {
	HWND d = FurbHost::modeless(NSF_DLG);
	return d ? furb_narrow(FurbHost::get_text(d, id).c_str()) : "";
}

std::string base_name(const std::string &p) {
	std::string n = p.substr(p.find_last_of('/') + 1);
	size_t dot = n.find_last_of('.');
	return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

void nsf_select(int song);

bool open_rom(const std::string &path) {
	std::wstring w = furb_widen(path.c_str());
	NES::OpenFile(&w[0]);
	NES::Running = FALSE;		// (OpenFile "starts" NSFs; there is no emulation thread)
	if (!loaded()) {
		rom_path.clear();
		rom_name.clear();
		pic_w = pic_h = 0;
		return false;
	}
	map_buttons();
	FurbHost::set_client(GFX::SIZEX, GFX::SIZEY);
	rom_path = path;
	rom_name = base_name(path);
	nsf_song = RI.ROMType == ROM_NSF ? RI.NSF_InitSong : 1;
	nsf_playing = false;
	Sound::SoundON();
	pending_audio.clear();
	scope.clear();
	if (is_nsf()) nsf_select(nsf_song);		// (Furbtendulator waits for Play)
	else osd(rom_name);
	prefer_refresh();
	return true;
}

std::string state_path(int slot) {
	return data_dir + "/States/" + furb_narrow(States::BaseFilename) + ".ns" + std::to_string(slot);
}

// play song N (1-based), from its start
void nsf_select(int song) {
	HWND d = FurbHost::modeless(NSF_DLG);
	if (!d) return;
	song = SDL_clamp(song, 1, (int)RI.NSF_NumSongs);
	nsf_song = song;
	FurbHost::set_pos(d, NSF_SELECT, song - 1);
	SendMessage(d, WM_HSCROLL, 0, (LPARAM)GetDlgItem(d, NSF_SELECT));
	FurbHost::click(d, NSF_PLAY);
	nsf_playing = true;
	nsf_frames = 0;
}
void nsf_stop(void) {
	if (HWND d = FurbHost::modeless(NSF_DLG)) FurbHost::click(d, NSF_STOP);
	nsf_playing = false;
}
void nsf_toggle(void) {
	if (nsf_playing) nsf_stop();
	else nsf_select(nsf_song);
}

// "key=value;..." for the menu: what is loaded and what applies to it
std::string menu_state(void) {
	std::string s = "loaded=" + std::to_string(loaded() ? 1 : 0);
	if (loaded()) {
		s += ";name=" + rom_name;
		s += ";fds=" + std::to_string(RI.ROMType == ROM_FDS ? RI.FDS_NumSides : 0);
		s += ";vs=" + std::to_string(RI.ConsoleType == CONSOLE_VS ? (RI.INES2_VSFlags == VS_DUAL || RI.INES2_VSFlags == VS_BUNGELING ? 2 : 1) : 0);
		s += ";nsf=" + std::to_string(RI.ROMType == ROM_NSF ? RI.NSF_NumSongs : 0);
		s += ";song=" + std::to_string(nsf_song);
		s += ";playing=" + std::to_string(nsf_playing ? 1 : 0);
		s += ";region=" + std::to_string(NES::CurRegion == Settings::REGION_PAL ? 1 : NES::CurRegion == Settings::REGION_DENDY ? 2 : 0);
		s += ";slot=" + std::to_string(States::SelSlot);
		std::string used;
		for (int i = 0; i < 10; i++) {
			struct stat st;
			if (stat(state_path(i).c_str(), &st) == 0)
				used += (used.empty() ? "" : ",") + std::to_string(i) + ":" + std::to_string((long long)st.st_mtime);
		}
		s += ";states=" + used;
	}
	for (int i = 0; i < P_COUNT; i++) s += std::string(";") + pref_keys[i] + "=" + std::to_string(pref(i));
	s += ";swapduty=" + std::to_string(Settings::SwapDutyCycles ? 1 : 0);
	return s;
}

void keep_scope(void) {
	if (pending_audio.size() < 4) return;
	scope.assign((const int16_t *)pending_audio.data(), (const int16_t *)pending_audio.data() + pending_audio.size() / 2);
}

bool paused = false;		// the menu is open
bool quit = false;
bool fast_forward = false;

// ================================================================ pacing, sound
// A frame per refresh of the screen (pacing.h), and the sound stretched by a
// fraction of a percent to keep up with it.
SDL_AudioDeviceID audio_dev = 0;
int audio_block = 1024;		// samples the device takes at a time
Pacer pacer;
AudioRate audio_rate;
Resampler resampler;
std::vector<int16_t> sound_out;
long paced = 0, uneven = 0;	// refreshes with a game running, and those not showing one new frame when they should
int pacing_hz = 0;		// (for the pacing command)
bool pacing_vsync = false;

void restart_sound(void) {
	audio_rate.reset();
	resampler.reset();
	if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
}
// this refresh's frames' sound (pending_audio), dt seconds after the last
void queue_sound(double dt) {
	size_t made = pending_audio.size() / 2;
	if (!made) return;
	double frame = SAMPLING_RATE / 60.0, target = audio_block + 3 * frame;
	double queued = SDL_GetQueuedAudioSize(audio_dev) / 2.0;
	if (queued > 2 * target + frame) {	// far too much waiting (it can't happen, but in case)
		restart_sound();
		queued = 0;
	}
	if (queued == 0) {	// starting, or the sound ran out: a cushion of silence first
		std::vector<int16_t> quiet((size_t)(target - frame));
		SDL_QueueAudio(audio_dev, quiet.data(), (Uint32)quiet.size() * 2);
		queued = (double)quiet.size();
	}
	double k = audio_rate.ratio((double)made, dt, queued, target, SAMPLING_RATE);
	sound_out.clear();
	resampler.run((const int16_t *)pending_audio.data(), made, k, sound_out);
	if (!sound_out.empty()) SDL_QueueAudio(audio_dev, sound_out.data(), (Uint32)sound_out.size() * 2);
}
// the screen's refresh rate as the system reports it, 0 if it doesn't
int screen_hz(SDL_Window *win) {
	SDL_DisplayMode m;
	int d = SDL_GetWindowDisplayIndex(win);
	return d >= 0 && SDL_GetCurrentDisplayMode(d, &m) == 0 ? m.refresh_rate : 0;
}

void show_menu(void) {
#ifdef __ANDROID__
	paused = true;
	call_activity("showMenu", menu_state());
#else
	printf("menu: %s\n", menu_state().c_str());
	fflush(stdout);
#endif
}

void run_command(const std::string &line) {
	size_t sp = line.find(' ');
	std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
	int n = atoi(arg.c_str());
	TitlebarDelay = 0;
	if (cmd == "open") {
		if (!open_rom(arg)) osd("COULD NOT OPEN " + base_name(arg));
	} else if (cmd == "pause") paused = true;
	else if (cmd == "resume") paused = false;
	else if (cmd == "menu") show_menu();
	else if (cmd == "state") { printf("state: %s\n", menu_state().c_str()); fflush(stdout); }
	else if (cmd == "pacing") {
		printf("pacing: refreshes=%ld uneven=%ld hz=%.2f reported=%d vsync=%d lock=%d sound=%d\n", paced, uneven, pacer.refresh_hz(), pacing_hz, pacing_vsync, pacer.locked(), audio_dev ? 1 : 0);
		fflush(stdout);
	}
	else if (cmd == "quit") quit = true;
	else if (cmd == "pref") {
		std::string key = arg.substr(0, arg.find(' '));
		int v = atoi(arg.c_str() + key.size());
		for (int i = 0; i < P_COUNT; i++) if (key == pref_keys[i]) prefs[i].value = v;
		// Furbtendulator's own "swapped duty cycles" (its Sound menu): the 25%
		// and 50% square waves exchanged, as on some clone consoles.  The APU
		// applies it when a game next sets a duty, which most do every note.
		if (key == "swapduty") Settings::SwapDutyCycles = v != 0;
		save_settings();
	} else if (!loaded()) {
		return;
	} else if (cmd == "close") {
		NES::CloseFile();
		rom_path.clear(); rom_name.clear();
		pic_w = pic_h = 0;
	} else if (cmd == "reset" || cmd == "hardreset") {
		if (Movie::Mode) Movie::Stop();
		NES::Reset(cmd == "reset" ? RESET_SOFT : RESET_HARD);
		osd(cmd == "reset" ? "RESET" : "POWER CYCLE");
	} else if (cmd == "save" || cmd == "load") {
		States::SetSlot(n);
		std::wstring p = furb_widen(state_path(n).c_str()), shortp = furb_widen((furb_narrow(States::BaseFilename) + ".ns" + std::to_string(n)).c_str());
		TitlebarDelay = 0;
		if (cmd == "save") States::SaveState(p.c_str(), shortp.c_str());
		else States::LoadState(p.c_str(), shortp.c_str());
		if (!TitlebarDelay) osd(std::string(cmd == "save" ? "SAVED" : "LOADED") + " STATE " + std::to_string(n));
	} else if (cmd == "fds-insert") NES::insert28();
	else if (cmd == "fds-eject") NES::eject28();
	else if (cmd == "fds-next") NES::next28();
	else if (cmd == "fds-prev") NES::previous28();
	else if (cmd == "coin1") { if (!NES::coinDelay1) { NES::coin1 = 0x20; NES::coinDelay1 = 222222; } }
	else if (cmd == "coin2") { if (!NES::coinDelay2) { NES::coin2 = 0x20; NES::coinDelay2 = 222222; } }
	else if (cmd == "region") {
		NES::SetRegion(n == 1 ? Settings::REGION_PAL : n == 2 ? Settings::REGION_DENDY : Settings::REGION_NTSC);
		osd(n == 1 ? "PAL" : n == 2 ? "DENDY" : "NTSC");
		prefer_refresh();
	} else if (cmd == "nsf-song") nsf_select(n);
	else if (cmd == "nsf-next") nsf_select(nsf_song + 1);
	else if (cmd == "nsf-prev") nsf_select(nsf_song - 1);
	else if (cmd == "nsf-play") nsf_select(nsf_song);
	else if (cmd == "nsf-stop") nsf_stop();
	poll_titlebar();
}

// Battery saves and settings to disk: when the app goes to the background
// (Android may end it there without warning) and on exit.
void persist(void) {
	if (loaded()) NES::SaveSRAM();
	save_settings();
}

// ================================================================ input
// SDL scancodes -> DirectInput key codes, for Furbtendulator's keyboard
// devices (Family BASIC, Subor and other famiclone keyboards)
const std::map<int, int> dik_of = {
	{SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_1, 0x02}, {SDL_SCANCODE_2, 0x03}, {SDL_SCANCODE_3, 0x04},
	{SDL_SCANCODE_4, 0x05}, {SDL_SCANCODE_5, 0x06}, {SDL_SCANCODE_6, 0x07}, {SDL_SCANCODE_7, 0x08},
	{SDL_SCANCODE_8, 0x09}, {SDL_SCANCODE_9, 0x0A}, {SDL_SCANCODE_0, 0x0B}, {SDL_SCANCODE_MINUS, 0x0C},
	{SDL_SCANCODE_EQUALS, 0x0D}, {SDL_SCANCODE_BACKSPACE, 0x0E}, {SDL_SCANCODE_TAB, 0x0F}, {SDL_SCANCODE_Q, 0x10},
	{SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13}, {SDL_SCANCODE_T, 0x14},
	{SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17}, {SDL_SCANCODE_O, 0x18},
	{SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1A}, {SDL_SCANCODE_RIGHTBRACKET, 0x1B},
	{SDL_SCANCODE_RETURN, 0x1C}, {SDL_SCANCODE_LCTRL, 0x1D}, {SDL_SCANCODE_A, 0x1E}, {SDL_SCANCODE_S, 0x1F},
	{SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21}, {SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23},
	{SDL_SCANCODE_J, 0x24}, {SDL_SCANCODE_K, 0x25}, {SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27},
	{SDL_SCANCODE_APOSTROPHE, 0x28}, {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2A},
	{SDL_SCANCODE_BACKSLASH, 0x2B}, {SDL_SCANCODE_Z, 0x2C}, {SDL_SCANCODE_X, 0x2D}, {SDL_SCANCODE_C, 0x2E},
	{SDL_SCANCODE_V, 0x2F}, {SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31}, {SDL_SCANCODE_M, 0x32},
	{SDL_SCANCODE_COMMA, 0x33}, {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35},
	{SDL_SCANCODE_RSHIFT, 0x36}, {SDL_SCANCODE_KP_MULTIPLY, 0x37}, {SDL_SCANCODE_LALT, 0x38},
	{SDL_SCANCODE_SPACE, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x3A}, {SDL_SCANCODE_F1, 0x3B}, {SDL_SCANCODE_F2, 0x3C},
	{SDL_SCANCODE_F3, 0x3D}, {SDL_SCANCODE_F4, 0x3E}, {SDL_SCANCODE_F5, 0x3F}, {SDL_SCANCODE_F6, 0x40},
	{SDL_SCANCODE_F7, 0x41}, {SDL_SCANCODE_F8, 0x42}, {SDL_SCANCODE_F9, 0x43}, {SDL_SCANCODE_F10, 0x44},
	{SDL_SCANCODE_NUMLOCKCLEAR, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x46}, {SDL_SCANCODE_KP_7, 0x47},
	{SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49}, {SDL_SCANCODE_KP_MINUS, 0x4A}, {SDL_SCANCODE_KP_4, 0x4B},
	{SDL_SCANCODE_KP_5, 0x4C}, {SDL_SCANCODE_KP_6, 0x4D}, {SDL_SCANCODE_KP_PLUS, 0x4E}, {SDL_SCANCODE_KP_1, 0x4F},
	{SDL_SCANCODE_KP_2, 0x50}, {SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x53},
	{SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58}, {SDL_SCANCODE_KP_ENTER, 0x9C}, {SDL_SCANCODE_RCTRL, 0x9D},
	{SDL_SCANCODE_KP_DIVIDE, 0xB5}, {SDL_SCANCODE_RALT, 0xB8}, {SDL_SCANCODE_PAUSE, 0xC5}, {SDL_SCANCODE_HOME, 0xC7},
	{SDL_SCANCODE_UP, 0xC8}, {SDL_SCANCODE_PAGEUP, 0xC9}, {SDL_SCANCODE_LEFT, 0xCB}, {SDL_SCANCODE_RIGHT, 0xCD},
	{SDL_SCANCODE_END, 0xCF}, {SDL_SCANCODE_DOWN, 0xD0}, {SDL_SCANCODE_PAGEDOWN, 0xD1}, {SDL_SCANCODE_INSERT, 0xD2},
	{SDL_SCANCODE_DELETE, 0xD3},
};
// player 1 on a keyboard: the desktop GUI's defaults (X=A Z=B, right Shift =
// Select, Enter = Start, arrows; S/A = turbo A/B)
uint32_t keyboard_pad(const Uint8 *k) {
	uint32_t b = 0;
	if (k[SDL_SCANCODE_X]) b |= 1 << B_A;
	if (k[SDL_SCANCODE_Z]) b |= 1 << B_B;
	if (k[SDL_SCANCODE_RSHIFT]) b |= 1 << B_SELECT;
	if (k[SDL_SCANCODE_RETURN]) b |= 1 << B_START;
	if (k[SDL_SCANCODE_UP]) b |= 1 << B_UP;
	if (k[SDL_SCANCODE_DOWN]) b |= 1 << B_DOWN;
	if (k[SDL_SCANCODE_LEFT]) b |= 1 << B_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) b |= 1 << B_RIGHT;
	if (k[SDL_SCANCODE_S]) b |= 1 << B_TURBO_A;
	if (k[SDL_SCANCODE_A]) b |= 1 << B_TURBO_B;
	return b;
}

// Gamepads, in the order they were connected: the first is player 1 (along
// with the touch controls and the keyboard), the second player 2, ...
// NES B/A are the left/right face buttons, as on the NES pad.
std::vector<SDL_GameController *> pads;
uint32_t gamepad_bits(SDL_GameController *c) {
	auto btn = [c](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
	int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
	uint32_t b = 0;
	if (btn(SDL_CONTROLLER_BUTTON_B)) b |= 1 << B_A;
	if (btn(SDL_CONTROLLER_BUTTON_A)) b |= 1 << B_B;
	if (btn(SDL_CONTROLLER_BUTTON_Y)) b |= 1 << B_TURBO_A;
	if (btn(SDL_CONTROLLER_BUTTON_X)) b |= 1 << B_TURBO_B;
	bool menu_combo = btn(SDL_CONTROLLER_BUTTON_BACK) && btn(SDL_CONTROLLER_BUTTON_START);
	if (btn(SDL_CONTROLLER_BUTTON_BACK) && !menu_combo) b |= 1 << B_SELECT;
	if (btn(SDL_CONTROLLER_BUTTON_START) && !menu_combo) b |= 1 << B_START;
	if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) b |= 1 << B_UP;
	if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) b |= 1 << B_DOWN;
	if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) b |= 1 << B_LEFT;
	if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) b |= 1 << B_RIGHT;
	return b;
}

// ================================================================ screen layout
struct Circle { float x, y, r; };
struct Box { float x, y, w, h; bool hit(float px, float py, float pad = 0) const { return px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad; } };
struct Layout {
	int w = 0, h = 0;
	Box game;
	Circle dpad, a, b;
	Box select, start, menu, ff;
	Box prev, play, next;		// the NSF player's
} L;

float picture_aspect(void) {
	int w = pic_w ? pic_w : 256, h = pic_h ? pic_h : 240;
	if (hires()) { w /= 2; h /= 2; }
	return (float)w * (pref(P_ASPECT) == 0 ? 8.0f / 7.0f : 1.0f) / h;
}

void layout(int w, int h) {
	L.w = w; L.h = h;
	float ar = picture_aspect();
	if (h > w) {		// portrait: the picture on top, the controls under it
		float gw = (float)w, gh = gw / ar;
		if (gh > h * 0.6f) { gh = h * 0.6f; gw = gh * ar; }
		L.game = {(w - gw) / 2, 0, gw, gh};
		float top = gh, u = (float)w, area = h - top;
		float cy = top + area * 0.45f;
		L.dpad = {u * 0.24f, cy, u * 0.18f};
		L.a = {u * 0.84f, cy - u * 0.05f, u * 0.095f};
		L.b = {u * 0.62f, cy + u * 0.07f, u * 0.095f};
		float sy = SDL_min(cy + u * 0.32f, (float)h - u * 0.1f);
		L.select = {u * 0.30f, sy, u * 0.17f, u * 0.075f};
		L.start = {u * 0.53f, sy, u * 0.17f, u * 0.075f};
		L.menu = {u * 0.03f, top + u * 0.03f, u * 0.17f, u * 0.075f};
		L.ff = {u * 0.80f, top + u * 0.03f, u * 0.17f, u * 0.075f};
		float bw = u * 0.27f, bh = u * 0.15f, gap = u * 0.04f;
		L.prev = {(w - 3 * bw - 2 * gap) / 2, cy - bh / 2, bw, bh};
		L.play = {L.prev.x + bw + gap, L.prev.y, bw, bh};
		L.next = {L.play.x + bw + gap, L.prev.y, bw, bh};
	} else {		// landscape: the picture in the middle, the controls over its sides
		float gh = (float)h, gw = gh * ar;
		if (gw > w) { gw = (float)w; gh = gw / ar; }
		L.game = {(w - gw) / 2, (h - gh) / 2, gw, gh};
		float u = (float)h;
		L.dpad = {u * 0.27f, u * 0.62f, u * 0.2f};
		L.a = {w - u * 0.14f, u * 0.56f, u * 0.1f};
		L.b = {w - u * 0.37f, u * 0.68f, u * 0.1f};
		L.select = {u * 0.05f, u * 0.89f, u * 0.2f, u * 0.08f};
		L.start = {w - u * 0.25f, u * 0.89f, u * 0.2f, u * 0.08f};
		L.menu = {u * 0.03f, u * 0.03f, u * 0.2f, u * 0.08f};
		L.ff = {w - u * 0.23f, u * 0.03f, u * 0.2f, u * 0.08f};
		float bw = u * 0.3f, bh = u * 0.13f, gap = u * 0.05f;
		L.prev = {(w - 3 * bw - 2 * gap) / 2, h - bh - u * 0.04f, bw, bh};
		L.play = {L.prev.x + bw + gap, L.prev.y, bw, bh};
		L.next = {L.play.x + bw + gap, L.prev.y, bw, bh};
	}
}

// ================================================================ touch
struct Finger { float x, y; };
std::map<SDL_FingerID, Finger> fingers;
uint32_t touch_bits = 0;
bool touch_ff = false;
bool controls_visible = true;		// hidden while a gamepad or keyboard is in use

float dist(float x, float y, const Circle &c) { return SDL_sqrtf((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y)); }

// What one finger at x,y presses
uint32_t touch_at(float x, float y) {
	uint32_t b = 0;
	float d = dist(x, y, L.dpad);
	if (d < L.dpad.r * 1.35f && d > L.dpad.r * 0.18f) {
		// eight directions, 45 degrees each
		float ang = SDL_atan2f(y - L.dpad.y, x - L.dpad.x) * 180.0f / (float)M_PI;	// 0 = right, 90 = down
		int sector = ((int)SDL_floorf((ang + 22.5f + 360.0f) / 45.0f)) % 8;
		static const uint32_t dirs[8] = {1u << B_RIGHT, 1u << B_RIGHT | 1u << B_DOWN, 1u << B_DOWN, 1u << B_DOWN | 1u << B_LEFT,
		                                 1u << B_LEFT, 1u << B_LEFT | 1u << B_UP, 1u << B_UP, 1u << B_UP | 1u << B_RIGHT};
		return dirs[sector];
	}
	float da = dist(x, y, L.a), db = dist(x, y, L.b);
	if (da < L.a.r * 1.3f) b |= 1 << B_A;
	if (db < L.b.r * 1.3f) b |= 1 << B_B;
	// between A and B presses both
	Circle mid = {(L.a.x + L.b.x) / 2, (L.a.y + L.b.y) / 2, L.a.r * 0.6f};
	if (!b && dist(x, y, mid) < mid.r) b = 1 << B_A | 1 << B_B;
	if (L.select.hit(x, y, L.select.h * 0.3f)) b |= 1 << B_SELECT;
	if (L.start.hit(x, y, L.start.h * 0.3f)) b |= 1 << B_START;
	return b;
}
bool on_control(float x, float y) {
	if (is_nsf()) return L.menu.hit(x, y) || L.prev.hit(x, y) || L.play.hit(x, y) || L.next.hit(x, y);
	if (L.menu.hit(x, y) || L.ff.hit(x, y)) return true;
	return pref(P_CONTROLS) && (touch_at(x, y) || dist(x, y, L.dpad) < L.dpad.r * 1.35f);
}

void buzz(void) {
#ifdef __ANDROID__
	if (pref(P_HAPTICS)) call_activity("buzz", "");
#endif
}

void update_touch(void) {
	uint32_t before = touch_bits;
	touch_bits = 0;
	touch_ff = false;
	gun_trigger = false;
	if (is_nsf()) return;		// (the player's buttons act when touched, in finger_event)
	for (auto &f : fingers) {
		float x = f.second.x, y = f.second.y;
		if (pref(P_CONTROLS)) touch_bits |= touch_at(x, y);
		if (L.ff.hit(x, y)) touch_ff = true;
		// a light gun aims and fires where the picture is touched
		if (gun_port() >= 0 && L.game.hit(x, y) && !on_control(x, y)) {
			gun_x = (int)((x - L.game.x) * GFX::SIZEX / L.game.w);
			gun_y = (int)((y - L.game.y) * GFX::SIZEY / L.game.h);
			gun_trigger = true;
		}
	}
	if (touch_bits & ~before) buzz();
}

void finger_event(const SDL_Event &e) {
	float x = e.tfinger.x * L.w, y = e.tfinger.y * L.h;
	if (e.type == SDL_FINGERUP) fingers.erase(e.tfinger.fingerId);
	else fingers[e.tfinger.fingerId] = {x, y};
	if (e.type == SDL_FINGERDOWN) {
		if (!controls_visible) controls_visible = true;
		else if (L.menu.hit(x, y)) { buzz(); show_menu(); }
		else if (is_nsf() && L.prev.hit(x, y)) { buzz(); nsf_select(nsf_song - 1); }
		else if (is_nsf() && L.play.hit(x, y)) { buzz(); nsf_toggle(); }
		else if (is_nsf() && L.next.hit(x, y)) { buzz(); nsf_select(nsf_song + 1); }
		else if (!loaded() && !on_control(x, y)) show_menu();	// "tap to open a game"
	}
	update_touch();
}

// ================================================================ drawing
SDL_Renderer *ren;
SDL_Texture *tex;
int tex_w = 0, tex_h = 0, tex_smooth = -1;

void fill_circle(const Circle &c, SDL_Color col) {
	const int N = 40;
	SDL_Vertex v[N + 2];
	v[0] = {{c.x, c.y}, col, {0, 0}};
	for (int i = 0; i <= N; i++) {
		float t = (float)i / N * 2 * (float)M_PI;
		v[i + 1] = {{c.x + c.r * SDL_cosf(t), c.y + c.r * SDL_sinf(t)}, col, {0, 0}};
	}
	int idx[N * 3];
	for (int i = 0; i < N; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = i + 2; }
	SDL_RenderGeometry(ren, NULL, v, N + 2, idx, N * 3);
}
void fill_box(const Box &b, SDL_Color c) {
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
	SDL_FRect r = {b.x, b.y, b.w, b.h};
	SDL_RenderFillRectF(ren, &r);
}
float text_width(const std::string &s, float px) { return s.size() * 6 * px - px; }
void draw_text(const std::string &s, float x, float y, float px, SDL_Color c) {
	std::vector<SDL_FRect> rects;
	for (char ch : s) {
		char u = (char)toupper((unsigned char)ch);
		for (auto &g : font_glyphs)
			if (g.c == u) {
				for (int r = 0; r < 7; r++)
					for (int col = 0; col < 5; col++)
						if (g.rows[r][col] == '#') rects.push_back({x + col * px, y + r * px, px, px});
				break;
			}
		x += 6 * px;
	}
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
	if (!rects.empty()) SDL_RenderFillRectsF(ren, rects.data(), (int)rects.size());
}
void label(const Box &b, const std::string &s, SDL_Color c) {
	float px = SDL_max(1.0f, SDL_floorf(b.h * 0.4f / 7));
	draw_text(s, b.x + (b.w - text_width(s, px)) / 2, b.y + (b.h - 7 * px) / 2, px, c);
}
void label(const Circle &k, const std::string &s, SDL_Color c) {
	label(Box{k.x - k.r, k.y - k.r, 2 * k.r, 2 * k.r}, s, c);
}

// Dark, translucent buttons with a light edge and white labels, readable over
// bright and dark pictures alike; a pressed button lights up.  (With the
// on-screen pad turned off, only MENU and >> are drawn.)
void outline_circle(const Circle &c, SDL_Color col) {
	SDL_FPoint p[41];
	for (int i = 0; i <= 40; i++) {
		float t = (float)i / 40 * 2 * (float)M_PI;
		p[i] = {c.x + c.r * SDL_cosf(t), c.y + c.r * SDL_sinf(t)};
	}
	SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
	SDL_RenderDrawLinesF(ren, p, 41);
}
void outline_box(const Box &b, SDL_Color col) {
	SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
	SDL_FRect r = {b.x, b.y, b.w, b.h};
	SDL_RenderDrawRectF(ren, &r);
}
void draw_controls(uint32_t held) {
	int pct = SDL_clamp(pref(P_OPACITY), 5, 100);
	Uint8 a = (Uint8)(255 * pct / 100), a2 = (Uint8)SDL_min(255, 2 * 255 * pct / 100);
	SDL_Color base = {40, 40, 48, a}, lit = {235, 235, 245, a2}, edge = {220, 220, 230, a}, ink = {245, 245, 250, a2}, ink_lit = {20, 20, 30, a2};
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	auto button = [&](const Box &b, const char *text, bool on) {
		fill_box(b, on ? lit : base);
		outline_box(b, edge);
		label(b, text, on ? ink_lit : ink);
	};
	auto round = [&](const Circle &c, const char *text, bool on) {
		fill_circle(c, on ? lit : base);
		outline_circle(c, edge);
		label(c, text, on ? ink_lit : ink);
	};
	button(L.menu, "MENU", false);
	if (is_nsf()) {
		auto held = [&](const Box &b) { for (auto &f : fingers) if (b.hit(f.second.x, f.second.y)) return true; return false; };
		button(L.prev, "PREV", held(L.prev));
		button(L.play, nsf_playing ? "STOP" : "PLAY", held(L.play));
		button(L.next, "NEXT", held(L.next));
		return;
	}
	button(L.ff, ">>", fast_forward);
	if (!pref(P_CONTROLS)) return;
	// d-pad: a ring with a cross; the held directions light up
	fill_circle(L.dpad, {40, 40, 48, (Uint8)(a / 2)});
	outline_circle(L.dpad, edge);
	float arm = L.dpad.r * 0.34f, len = L.dpad.r * 0.92f;
	auto dir = [&](int bit, float dx, float dy) {
		Box r = dx ? Box{dx > 0 ? L.dpad.x : L.dpad.x - len, L.dpad.y - arm / 2, len, arm}
		           : Box{L.dpad.x - arm / 2, dy > 0 ? L.dpad.y : L.dpad.y - len, arm, len};
		fill_box(r, held >> bit & 1 ? lit : SDL_Color{95, 95, 108, a});
	};
	dir(B_UP, 0, -1); dir(B_DOWN, 0, 1); dir(B_LEFT, -1, 0); dir(B_RIGHT, 1, 0);
	round(L.a, "A", held >> B_A & 1);
	round(L.b, "B", held >> B_B & 1);
	button(L.select, "SELECT", held >> B_SELECT & 1);
	button(L.start, "START", held >> B_START & 1);
}

int shown_fps = 0, fps_frames = 0;
Uint32 fps_since = 0;

// Text the font can draw: other characters (accents, Japanese, ...) show as '?'
std::string printable(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) { out += (char)c; continue; }
		while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) i++;	// one UTF-8 character
		out += '?';
	}
	return out;
}
// one line, centred in r at y, as large as fits (up to maxpx font pixels)
void line(const Box &r, std::string t, float y, float maxpx, SDL_Color c) {
	t = printable(t);
	if (t.empty()) return;
	float room = r.w * 0.92f;
	float px = SDL_min(maxpx, SDL_floorf(room / (6.0f * t.size())));
	if (px < 2) {		// too long even small: cut it short
		px = SDL_min(maxpx, 2.0f);
		size_t fit = (size_t)(room / (6 * px));
		if (fit > 3 && t.size() > fit) t = t.substr(0, fit - 3) + "...";
	}
	draw_text(t, r.x + (r.w - text_width(t, px)) / 2, r.y + y, px, c);
}

// The NSF player: what is playing, and the sound's waveform
void draw_nsf(void) {
	const Box &r = L.game;
	fill_box(r, {18, 16, 34, 255});
	float h = r.h;
	SDL_Color white = {240, 240, 245, 255}, soft = {170, 170, 190, 255}, accent = {255, 205, 90, 255};
	auto field = [](int id) { std::string t = nsf_field(id); return t == "<?>" || t == "?" ? std::string() : t; };
	std::string title = field(NSF_TITLE);
	line(r, title.empty() ? rom_name : title, h * 0.06f, h / 90, white);
	line(r, field(NSF_ARTIST), h * 0.19f, h / 150, soft);
	line(r, field(NSF_COPYRIGHT), h * 0.25f, h / 150, soft);
	std::string chip = field(NSF_CHIP);
	if (!chip.empty()) line(r, "SOUND: " + chip, h * 0.31f, h / 170, soft);
	line(r, "SONG " + std::to_string(nsf_song) + " / " + std::to_string(RI.NSF_NumSongs), h * 0.40f, h / 75, accent);
	line(r, field(NSF_TRACK_NAME), h * 0.52f, h / 140, white);
	double fps = NES::CurRegion == Settings::REGION_NTSC ? 39375000.0 / 655171 : 26601712.0 / 531960;
	int secs = (int)(nsf_frames / fps);
	char t[32];
	snprintf(t, sizeof t, "%d:%02d", secs / 60, secs % 60);
	line(r, nsf_playing ? std::string("PLAYING  ") + t : "STOPPED", h * 0.60f, h / 150, nsf_playing ? white : soft);
	// waveform (above the buttons in landscape, where they lie over the picture)
	float top = r.y + h * 0.69f, bottom = r.y + h * (L.w > L.h ? 0.78f : 0.95f), mid = (top + bottom) / 2;
	SDL_SetRenderDrawColor(ren, 60, 60, 90, 255);
	SDL_RenderDrawLineF(ren, r.x + r.w * 0.05f, mid, r.x + r.w * 0.95f, mid);
	if (nsf_playing && scope.size() > 2) {
		const int N = 300;
		SDL_FPoint p[N];
		int peak = 1500;		// (scaled to the loudness, so quiet songs show too)
		for (int16_t v : scope) peak = SDL_max(peak, abs(v));
		for (int i = 0; i < N; i++) {
			int16_t v = scope[(size_t)i * scope.size() / N];
			p[i] = {r.x + r.w * (0.05f + 0.9f * i / (N - 1)), mid - 0.9f * v / peak * (bottom - top) / 2};
		}
		SDL_SetRenderDrawColor(ren, 120, 230, 160, 255);
		SDL_RenderDrawLinesF(ren, p, N);
	}
}

void draw(void) {
	int w, h;
	SDL_GetRendererOutputSize(ren, &w, &h);
	layout(w, h);
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_RenderClear(ren);
	if (is_nsf()) {
		draw_nsf();
	} else if (loaded() && pic_w) {
		if (!tex || tex_w != pic_w || tex_h != pic_h || tex_smooth != pref(P_SMOOTH)) {
			if (tex) SDL_DestroyTexture(tex);
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, pref(P_SMOOTH) ? "1" : "0");
			tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, pic_w, pic_h);
			tex_w = pic_w; tex_h = pic_h; tex_smooth = pref(P_SMOOTH);
		}
		SDL_UpdateTexture(tex, NULL, picture.data(), pic_w * 4);
		SDL_FRect dst = {L.game.x, L.game.y, L.game.w, L.game.h};
		SDL_RenderCopyF(ren, tex, NULL, &dst);
	} else {
		float px = SDL_max(2.0f, SDL_floorf(w / 110.0f));
		std::string t1 = "FURBTENDULATOR", t2 = "TAP TO OPEN A GAME";
		draw_text(t1, (w - text_width(t1, px)) / 2, h * 0.3f, px, {230, 230, 230, 255});
		draw_text(t2, (w - text_width(t2, px * 0.6f)) / 2, h * 0.3f + 12 * px, px * 0.6f, {160, 160, 160, 255});
	}
	if (controls_visible)
		draw_controls(touch_bits);
	float px = SDL_max(2.0f, SDL_floorf(SDL_min(w, h) / 160.0f));
	if (!osd_text.empty() && SDL_TICKS_PASSED(SDL_GetTicks(), osd_until)) osd_text.clear();
	if (!osd_text.empty()) {
		float tw = text_width(osd_text, px);
		float x = SDL_max(px * 2, L.game.x + (L.game.w - tw) / 2), y = L.game.y + px * 4;
		fill_box({x - px * 2, y - px * 2, tw + px * 4, px * 11}, {0, 0, 0, 160});
		draw_text(osd_text, x, y, px, {255, 255, 255, 255});
	}
	if (pref(P_FPS) && loaded()) {
		std::string f = std::to_string(shown_fps) + " FPS  " + std::to_string((int)std::lround(pacer.refresh_hz())) + " HZ";
		draw_text(f, L.game.x + L.game.w - text_width(f, px) - px * 3, L.game.y + L.game.h - px * 10, px, {255, 255, 0, 255});
	}
	SDL_RenderPresent(ren);
}

// ================================================================ desktop stdin
#ifndef __ANDROID__
// (read(), not stdio: exit() flushes stdin, and would wait for a thread
// blocked in fgets holding its lock)
extern "C" ssize_t read(int, void *, size_t);
void read_stdin(void) {
	std::string line;
	char buf[4096];
	for (ssize_t n; (n = read(0, buf, sizeof buf)) > 0; )
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != '\n') { if (buf[i] != '\r') line += buf[i]; continue; }
			if (!line.empty()) queue_command(line);
			line.clear();
		}
}
#endif

void mkdirs(const std::string &p) {
	for (size_t i = 1; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777);
}

} // namespace

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_io_github_gamingfanboy365_furbtendulator_FurbActivity_nativeCommand(JNIEnv *env, jclass, jstring cmd) {
	queue_command(from_java(env, cmd));
}
#endif

// The emulator outlives SDL_main on Android: the activity can be destroyed and
// created again in the same process, and SDL_main runs again.
static bool core_ready = false;

int main(int argc, char **argv) {
	setlocale(LC_ALL, "C.UTF-8");
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");		// furb_touch: the mouse is a finger
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
		SDL_Log("furb: SDL_Init: %s", SDL_GetError());
		return 1;
	}
	std::string first_rom;
	int win_w = 960, win_h = 720;
#ifdef __ANDROID__
	// FurbActivity unpacks the data files (Mappers/, BIOS/, samples/, *.cfg)
	// into the app's files folder before starting this
	std::string files = SDL_AndroidGetInternalStoragePath();
	prog_dir = files + "/";
	data_dir = files + "/data";
	config_path = files + "/settings.reg";
	(void)argc; (void)argv;
#else
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[n > 0 ? n : 0] = 0;
	prog_dir = exe;
	prog_dir = prog_dir.substr(0, prog_dir.find_last_of('/') + 1);
	data_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.furb_touch";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
		else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
		else if (a == "-h" || a == "--help") {
			printf("usage: furb_touch [ROM] [--data-dir DIR] [--size WxH]\n"
			       "  commands on stdin: open PATH | save N | load N | reset | hardreset | region 0|1|2\n"
			       "  | fds-insert | fds-eject | fds-next | fds-prev | coin1 | coin2\n"
			       "  | nsf-song N | nsf-next | nsf-prev | nsf-play | nsf-stop\n"
			       "  | pref NAME VALUE | menu | state | pacing | pause | resume | close | quit\n");
			return 0;
		} else first_rom = a;
	}
	config_path = data_dir + "/settings.reg";
	std::thread(read_stdin).detach();
#endif

	if (!core_ready) {
		mkdirs(data_dir);
		struct stat st;
		if (stat(config_path.c_str(), &st) == 0) {
			std::string err;
			if (!FurbHost::load_config(config_path, err)) SDL_Log("furb: %s", err.c_str());
		}
		load_prefs();
		FurbHost::set_message_sink([](const std::wstring &text, const std::wstring &caption, UINT type) {
			SDL_ShowSimpleMessageBox((type & 0xF0) == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION,
			                         furb_narrow(caption.c_str()).c_str(), furb_narrow(text.c_str()).c_str(), NULL);
			UINT kind = type & 0xF;
			return (kind == MB_YESNO || kind == MB_YESNOCANCEL) ? IDNO : IDOK;	// never agree to anything unasked
		});
		FurbHost::set_audio_sink([](const void *d, size_t n) {
			pending_audio.insert(pending_audio.end(), (const uint8_t *)d, (const uint8_t *)d + n);
		});
#ifdef __ANDROID__
		// the mapper packs are app libraries (libfurb_<pack>.so); the Mappers
		// folder only names them for MapperInterface::Init's search
		mkdirs(prog_dir + "Mappers");
		for (const char *p : {"iNES", "FDS", "NSF", "VS"}) {
			std::string f = prog_dir + "Mappers/" + p + ".so";
			if (stat(f.c_str(), &st) != 0) fclose(fopen(f.c_str(), "w"));
		}
#endif
		wcscpy(ProgPath, furb_widen(prog_dir.c_str()).c_str());
		wcscpy(DataPath, furb_widen(data_dir.c_str()).c_str());
		NES::Init();
		Settings::AutoRun = FALSE;	// frames are run here; no emulation thread
		Settings::FSkip = 0;
		Settings::aFSkip = FALSE;
		map_buttons();
		core_ready = true;
	}

	Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
#ifdef __ANDROID__
	win_flags |= SDL_WINDOW_FULLSCREEN;	// immersive: the status and navigation bars hidden
#endif
	SDL_Window *win = SDL_CreateWindow("Furbtendulator", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w, win_h, win_flags);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	if (!win || !ren) {
		SDL_Log("furb: no window: %s", SDL_GetError());
		return 1;
	}
	tex = NULL;
	tex_w = tex_h = 0;
	SDL_AudioSpec want = {}, have;
	want.freq = SAMPLING_RATE;	// what Sound.cpp mixes: 48 kHz, mono, 16-bit
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 1024;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);	// SDL converts to the device's format
	if (audio_dev) {
		audio_block = have.samples;
		SDL_PauseAudioDevice(audio_dev, 0);
	} else
		SDL_Log("furb: no sound: %s", SDL_GetError());

	SDL_RendererInfo info;
	bool vsync = SDL_GetRendererInfo(ren, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
	paused = false;		// (a menu left open when the activity went away)
	if (!first_rom.empty()) queue_command("open " + first_rom);
	std::string c;
	while (next_command(c)) run_command(c);
	if (!loaded() && !paused) show_menu();	// (paused: the activity is opening a game already)

	Uint64 freq = SDL_GetPerformanceFrequency(), last_present = SDL_GetPerformanceCounter(), next_tick = last_present;
	// (furb_touch on a display without vsync paces itself by a 60 Hz clock)
	auto refresh = [&](void) { return vsync ? screen_hz(win) : 60; };
	int hz = refresh();
	pacer.reset();
	restart_sound();
	bool menu_combo_held = false, ff_key = false;
	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT: quit = true; break;
			case SDL_APP_WILLENTERBACKGROUND:
				persist();
				if (audio_dev) SDL_PauseAudioDevice(audio_dev, 1);
				break;
			case SDL_APP_DIDENTERFOREGROUND:
				restart_sound();
				if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
				break;
			case SDL_APP_TERMINATING: persist(); break;
			case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: finger_event(e); break;
			case SDL_KEYDOWN:
				if (e.key.repeat) break;
				if (e.key.keysym.scancode != SDL_SCANCODE_AC_BACK) controls_visible = false;
				switch (e.key.keysym.scancode) {
				case SDL_SCANCODE_AC_BACK: case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1: show_menu(); break;
				case SDL_SCANCODE_F5: queue_command("save " + std::to_string(States::SelSlot)); break;
				case SDL_SCANCODE_F8: queue_command("load " + std::to_string(States::SelSlot)); break;
				case SDL_SCANCODE_TAB: ff_key = true; break;
				default: break;
				}
				break;
			case SDL_KEYUP:
				if (e.key.keysym.scancode == SDL_SCANCODE_TAB) ff_key = false;
				break;
			case SDL_CONTROLLERDEVICEADDED:
				if (SDL_GameController *gc = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(gc);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				for (size_t i = 0; i < pads.size(); i++)
					if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which) {
						SDL_GameControllerClose(pads[i]);
						pads.erase(pads.begin() + i);
						break;
					}
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				controls_visible = false;
				if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) show_menu();
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) fingers.clear(), update_touch();
				hz = refresh();	// (moved to another screen, or the screen's mode changed)
				break;
			case SDL_DISPLAYEVENT:
				hz = refresh();
				break;
			}
		}
		while (next_command(c)) run_command(c);
		if (quit) break;

		// this frame's input
		const Uint8 *keys = SDL_GetKeyboardState(NULL);
		memset(player_bits, 0, sizeof player_bits);
		player_bits[0] = touch_bits | keyboard_pad(keys);
		bool pad_ff = false, combo = false;
		for (size_t i = 0; i < pads.size() && i < 4; i++) {
			player_bits[i] |= gamepad_bits(pads[i]);
			if (SDL_GameControllerGetButton(pads[i], SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pad_ff = true;
			if (SDL_GameControllerGetButton(pads[i], SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(pads[i], SDL_CONTROLLER_BUTTON_START)) combo = true;
		}
		if (combo && !menu_combo_held) show_menu();	// Select+Start: pads without a menu button
		menu_combo_held = combo;
		if (is_nsf()) {
			// the pad drives the player: left/right the song (up/down by ten),
			// A or Start play/stop, B stop
			static uint32_t was = 0;
			uint32_t now = player_bits[0] | player_bits[1] | player_bits[2] | player_bits[3], hit = now & ~was;
			was = now;
			if (hit & 1 << B_LEFT) nsf_select(nsf_song - 1);
			if (hit & 1 << B_RIGHT) nsf_select(nsf_song + 1);
			if (hit & 1 << B_UP) nsf_select(nsf_song + 10);
			if (hit & 1 << B_DOWN) nsf_select(nsf_song - 10);
			if (hit & (1 << B_A | 1 << B_START)) nsf_toggle();
			if (hit & 1 << B_B) nsf_stop();
			memset(player_bits, 0, sizeof player_bits);
		}
		memset(Controllers::KeyState, 0, sizeof Controllers::KeyState);
		for (auto &k : dik_of) if (keys[k.first]) Controllers::KeyState[k.second] = 0x80;
		fast_forward = touch_ff || ff_key || pad_ff;

		// run frames: as many as are due by the screen's refreshes (one per
		// refresh at 60 Hz); several per refresh when fast-forwarding
		Uint64 now = SDL_GetPerformanceCounter();
		double dt = (double)(now - last_present) / freq;
		last_present = now;
		int ran = 0;
		if (loaded() && !paused) {
			double fps = game_fps();
			if (fast_forward) {
				for (; ran < 4; ran++) run_frame();
				pacer.reset();
			} else {
				for (int due = pacer.frames(dt, fps, hz); ran < due; ran++) run_frame();
				pacing_hz = hz;
				pacing_vsync = vsync;
				paced++;
				if (pacer.locked() == 1 && ran != 1) uneven++;
			}
			if (audio_dev && pref(P_SOUND) && !fast_forward) queue_sound(dt);
			else restart_sound();
			keep_scope();
			pending_audio.clear();
			if (nsf_playing) nsf_frames += ran;
			if (ran) {
				grab_picture();
				poll_titlebar();
			}
		} else {
			pacer.reset();
			restart_sound();
		}
		fps_frames += ran;
		if (SDL_TICKS_PASSED(SDL_GetTicks(), fps_since + 1000)) {
			shown_fps = fps_frames;
			fps_frames = 0;
			fps_since = SDL_GetTicks();
			hz = refresh();
		}
		draw();
		if (!vsync) {	// the 60 Hz clock, as if it were the screen's refresh
			Uint64 tick = freq / 60, t = SDL_GetPerformanceCounter();
			next_tick = next_tick + tick < t ? t + tick : next_tick + tick;
			while ((t = SDL_GetPerformanceCounter()) < next_tick)
				SDL_Delay((Uint32)((next_tick - t) * 1000 / freq > 2 ? (next_tick - t) * 1000 / freq - 1 : 0));
		}
	}

	persist();
	if (loaded()) NES::CloseFile();
	rom_path.clear();
	if (audio_dev) SDL_CloseAudioDevice(audio_dev);
	audio_dev = 0;
	for (auto *p : pads) SDL_GameControllerClose(p);
	pads.clear();
	if (tex) SDL_DestroyTexture(tex);
	tex = NULL;
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	quit = false;
	return 0;
}
