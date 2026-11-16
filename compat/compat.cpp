// Real implementations behind the Win32 shim (see windows.h): wide printf with
// MSVC semantics, UTF-8 paths, "DLL" loading through dlopen, directory
// helpers, zeroing operator new -- compiled into the executables and every
// mapper pack.  The host services live in the executable: host_reg.cpp
// (registry, export table) with host_cli.cpp (furb_cli) or gui/*.cpp (the
// GUI).  The mapper packs (built with -DFURB_PACK) forward to them.
#include <windows.h>
#include <dsound.h>
#include <endpointvolume.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <fnmatch.h>
#include <locale.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <vector>

std::string furb_narrow(const wchar_t *w) {
	std::string r;
	if (!w) return r;
	for (; *w; w++) {
		unsigned c = (unsigned)*w;
		if (c < 0x80) r += (char)c;
		else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
		else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | (c >> 12 & 0x3F)); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
	}
	return r;
}

std::wstring furb_widen(const char *s) {
	std::wstring r;
	if (!s) return r;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned c = *p++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra) c &= 0x3F >> extra;
		while (extra-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
		r += (wchar_t)c;
	}
	return r;
}

// Windows paths -> POSIX: backslashes become '/', and "*.dll" means our ".so".
static std::string posix_path(const wchar_t *w) {
	std::string p = furb_narrow(w);
	for (auto &c : p) if (c == '\\') c = '/';
	if (p.size() > 4 && !strcasecmp(p.c_str() + p.size() - 4, ".dll")) p.replace(p.size() - 4, 4, ".so");
	return p;
}

// Windows file names are case-insensitive (BIOS\\DISKSYS.ROM may be disksys.rom
// on disk): when the exact path is missing, match each component ignoring case.
static std::string resolve_case(const std::string &p) {
	if (p.empty() || access(p.c_str(), F_OK) == 0) return p;
	std::string out = p[0] == '/' ? "/" : "";
	size_t pos = p[0] == '/' ? 1 : 0;
	while (pos <= p.size()) {
		size_t slash = p.find('/', pos);
		std::string part = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
		std::string dir = out.empty() ? "." : out;
		std::string hit = part;
		if (!part.empty() && access((out + part).c_str(), F_OK) != 0)
			if (DIR *d = opendir(dir.c_str())) {
				while (struct dirent *e = readdir(d))
					if (!strcasecmp(e->d_name, part.c_str())) { hit = e->d_name; break; }
				closedir(d);
			}
		out += hit;
		if (slash == std::string::npos) break;
		out += '/';
		pos = slash + 1;
	}
	return out;
}

// MSVC text mode with an encoding ("rt,ccs=UTF-16LE", used for cheats.cfg /
// dip.cfg): the BOM decides UTF-8 or UTF-16LE, and fgetwc returns characters.
// glibc's own ccs= would force UTF-16, so decode here and hand back a UTF-8
// stream, which fgetwc decodes under the C.UTF-8 locale furb_cli sets.
static FILE *open_ccs_read(const std::string &path, const std::string &ccs) {
	FILE *in = fopen(path.c_str(), "rb");
	if (!in) in = fopen(resolve_case(path).c_str(), "rb");
	if (!in) return NULL;
	std::string raw;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) raw.append(buf, n);
	fclose(in);
	std::string utf8;
	bool utf16 = raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE;
	if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) utf8 = raw.substr(3);
	else if (utf16 || (strcasestr(ccs.c_str(), "UTF-16") && raw.size() >= 2 && raw[1] == 0)) {
		std::wstring w;
		for (size_t i = utf16 ? 2 : 0; i + 1 < raw.size(); i += 2) w += (wchar_t)((unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8);
		utf8 = furb_narrow(w.c_str());
	} else utf8 = raw;
	std::string text;	// text mode: CRLF -> LF
	for (size_t i = 0; i < utf8.size(); i++) if (!(utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n')) text += utf8[i];
	// Hand back a freshly opened real file: a stream we wrote to is byte-
	// oriented (glibc then fails fgetwc), and fmemopen streams cannot do wide
	// reads at all.  Written, closed, reopened, unlinked.
	char name[] = "/tmp/furb_cfg_XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0) return NULL;
	FILE *w = fdopen(fd, "wb");
	fwrite(text.data(), 1, text.size(), w);
	fclose(w);
	FILE *f = fopen(name, "r");
	unlink(name);
	return f;
}

FILE *furb_wfopen(const wchar_t *name, const wchar_t *mode) {
	std::string p = posix_path(name), m = furb_narrow(mode);
	size_t ccs = m.find(",ccs=");
	if (ccs != std::string::npos) {
		std::string enc = m.substr(ccs + 5);
		m = m.substr(0, ccs);
		if (m[0] == 'r' && m.find('+') == std::string::npos) return open_ccs_read(p, enc);
	}
	std::string plain;	// 't' (text) is the default on POSIX
	for (char c : m) if (c != 't') plain += c;
	FILE *f = fopen(p.c_str(), plain.c_str());
	if (!f && plain[0] == 'r') f = fopen(resolve_case(p).c_str(), plain.c_str());
	return f;
}

DWORD GetModuleFileName(HMODULE, wchar_t *buf, DWORD n) {
	char exe[4096];
	ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k <= 0 || !n) { if (n) buf[0] = 0; return 0; }
	exe[k] = 0;
	std::wstring w = furb_widen(exe);
	for (auto &c : w) if (c == L'/') c = L'\\';	// callers look for the last '\\'
	wcsncpy(buf, w.c_str(), n - 1);
	buf[n - 1] = 0;
	return (DWORD)wcslen(buf);
}
int furb_wremove(const wchar_t *name) { return remove(posix_path(name).c_str()); }
int furb_access(const char *p, int m) { return access(p, m); }

DWORD GetTickCount(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

void furb_splitpath(const wchar_t *path, wchar_t *drive, wchar_t *dir, wchar_t *fname, wchar_t *ext) {
	std::wstring p(path ? path : L"");
	size_t slash = p.find_last_of(L"/\\");
	std::wstring d = slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
	std::wstring f = slash == std::wstring::npos ? p : p.substr(slash + 1);
	size_t dot = f.find_last_of(L'.');
	std::wstring e = dot == std::wstring::npos ? L"" : f.substr(dot);
	if (dot != std::wstring::npos) f = f.substr(0, dot);
	if (drive) drive[0] = 0;
	if (dir) wcscpy(dir, d.c_str());
	if (fname) wcscpy(fname, f.c_str());
	if (ext) wcscpy(ext, e.c_str());
}

// MSVC wide formats: %s and %c take WIDE arguments, %S/%C narrow; %hs narrow.
std::wstring furb_fixfmt(const wchar_t *fmt) {
	std::wstring out;
	for (const wchar_t *p = fmt; *p; p++) {
		out += *p;
		if (*p != L'%') continue;
		if (p[1] == L'%') { out += *++p; continue; }
		// copy flags/width/precision/length until the conversion char
		while (p[1] && wcschr(L"-+ #0123456789.*", p[1])) out += *++p;
		bool narrow = false, wide = false;
		while (p[1] && wcschr(L"hlLqjztI", p[1])) {
			wchar_t c = *++p;
			if (c == L'h') { narrow = true; continue; }
			if (c == L'I') {	// I64 / I32 / I
				if (p[1] == L'6' && p[2] == L'4') { out += L"ll"; p += 2; }
				else if (p[1] == L'3' && p[2] == L'2') { p += 2; }
				continue;
			}
			if (c == L'l') wide = true;
			out += c;
		}
		wchar_t conv = p[1];
		if (!conv) break;
		p++;
		if (conv == L's' || conv == L'c') {
			if (narrow) out += conv;	// %hs -> narrow
			else { if (!wide) out += L'l'; out += conv; }
		} else if (conv == L'S' || conv == L'C') {
			out += (wchar_t)(conv == L'S' ? L's' : L'c');	// %S -> narrow
		} else {
			if (narrow) out += L'h';
			out += conv;
		}
	}
	return out;
}

#if defined(__ANDROID__) && __ANDROID_API__ < 21
// Android 4.4's printf writes only the first character of a wide string
// (%ls) or character (%lc), and so does the vswprintf the NDK supplies for
// it, which formats through it.  So format here, one conversion at a time
// through the narrow vsnprintf, wide strings and characters as UTF-8.
static std::wstring format_one(const char *spec, ...) {
	va_list ap, again;
	va_start(ap, spec);
	va_copy(again, ap);
	int n = vsnprintf(NULL, 0, spec, ap);
	std::string s(n > 0 ? n : 0, '\0');
	if (n > 0) vsnprintf(&s[0], n + 1, spec, again);
	va_end(again);
	va_end(ap);
	return furb_widen(s.c_str());
}
static int format_wide(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap) {
	std::wstring out;
	for (const wchar_t *p = fmt; *p; p++) {
		if (*p != L'%') { out += *p; continue; }
		if (p[1] == L'%') { out += L'%'; p++; continue; }
		// %[flags][width][.precision][length]conversion; '*' taken from the arguments
		std::string spec = "%", len;
		const wchar_t *q = p + 1;
		while (*q && wcschr(L"-+ #0", *q)) spec += (char)*q++;
		if (*q == L'*') { spec += std::to_string(va_arg(ap, int)); q++; }
		else while (*q >= L'0' && *q <= L'9') spec += (char)*q++;
		if (*q == L'.') {
			spec += '.'; q++;
			if (*q == L'*') { spec += std::to_string(va_arg(ap, int)); q++; }
			else while (*q >= L'0' && *q <= L'9') spec += (char)*q++;
		}
		while (*q && wcschr(L"hlLjzt", *q)) len += (char)*q++;
		wchar_t conv = *q;
		if (!conv) break;
		p = q;
		bool wide = len == "l";
		switch (conv) {
		case L's':
			if (wide) {
				const wchar_t *w = va_arg(ap, const wchar_t *);
				out += format_one((spec + "s").c_str(), w ? furb_narrow(w).c_str() : "(null)");
			} else {
				const char *c = va_arg(ap, const char *);
				out += format_one((spec + "s").c_str(), c ? c : "(null)");
			}
			break;
		case L'c':
			if (wide) out += format_one((spec + "s").c_str(), furb_narrow(std::wstring(1, (wchar_t)va_arg(ap, wint_t)).c_str()).c_str());
			else out += format_one((spec + "c").c_str(), va_arg(ap, int));
			break;
		case L'd': case L'i': case L'o': case L'u': case L'x': case L'X':
			if (len == "ll" || len == "j") out += format_one((spec + len + (char)conv).c_str(), va_arg(ap, long long));
			else if (len == "l" || len == "z" || len == "t") out += format_one((spec + len + (char)conv).c_str(), va_arg(ap, long));
			else out += format_one((spec + len + (char)conv).c_str(), va_arg(ap, int));
			break;
		case L'e': case L'E': case L'f': case L'F': case L'g': case L'G': case L'a': case L'A':
			if (len == "L") out += format_one((spec + "L" + (char)conv).c_str(), va_arg(ap, long double));
			else out += format_one((spec + (char)conv).c_str(), va_arg(ap, double));
			break;
		case L'p':
			out += format_one((spec + "p").c_str(), va_arg(ap, void *));
			break;
		case L'n':
			*va_arg(ap, int *) = (int)out.size();
			break;
		default:	// not a conversion: as written
			out += furb_widen((spec + len).c_str());
			out += conv;
		}
	}
	if (!n) return -1;
	size_t k = out.size() < n ? out.size() : n - 1;
	wmemcpy(buf, out.data(), k);
	buf[k] = 0;
	return out.size() < n ? (int)out.size() : -1;
}
#endif

int furb_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap) {
	std::wstring f = furb_fixfmt(fmt);
#if defined(__ANDROID__) && __ANDROID_API__ < 21
	int r = format_wide(buf, n, f.c_str(), ap);
#else
	int r = vswprintf(buf, n, f.c_str(), ap);
#endif
	if (r < 0 && n) buf[n - 1] = 0;
	return r;
}
int furb_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int furb_swprintf_nc(wchar_t *buf, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, 1u << 20, fmt, ap); va_end(ap); return r;
}
int furb_fwprintf(FILE *f, const wchar_t *fmt, ...) {
	wchar_t tmp[4096];
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(tmp, 4096, fmt, ap); va_end(ap);
	fputs(furb_narrow(tmp).c_str(), f);
	return r;
}
int furb_swscanf(const wchar_t *s, const wchar_t *fmt, ...) {
	std::wstring f = furb_fixfmt(fmt);
	va_list ap; va_start(ap, fmt);
#if defined(__ANDROID__) && __ANDROID_API__ < 21
	// (no vswscanf before Android 5.0: scan the UTF-8 text instead -- the
	// same for numbers; nothing in Furbtendulator scans strings with it)
	int r = vsscanf(furb_narrow(s).c_str(), furb_narrow(f.c_str()).c_str(), ap);
#else
	int r = vswscanf(s, f.c_str(), ap);
#endif
	va_end(ap); return r;
}

// ---------------------------------------------------------------- operator new
// Zero-filled, in the executable and in every pack.  Some constructors leave
// members unset (the CPU and PPU objects NES::CreateMachine allocates are read
// before they are written), so their start state was whatever the heap held
// -- which depends on earlier allocations, down to the length of the paths on
// the command line.  Zeroing makes every run start from the same state.
#include <new>
static void *zalloc(size_t n) {
	void *p = calloc(n ? n : 1, 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void *operator new(size_t n) { return zalloc(n); }
void *operator new[](size_t n) { return zalloc(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return calloc(n ? n : 1, 1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return calloc(n ? n : 1, 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

// ---------------------------------------------------------------- furb_reserve
// NES::PRG_ROM has room for the largest OneBus ROMs, 256 MB, as a static
// array on Windows.  As a static array here, Linux would count all of it
// against the free memory when the program (or the Android app's library)
// is loaded, and refuse to load on a phone or tablet with little memory
// left; reserved with MAP_NORESERVE, only the pages a game uses count.
#include <sys/mman.h>
void *furb_reserve(size_t n) {
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) p = calloc(n, 1);
	if (!p) {
		fprintf(stderr, "furb: cannot reserve %zu bytes\n", n);
		abort();
	}
	return p;
}

// ---------------------------------------------------------------- FindFirstFile
struct FindState { std::vector<std::string> names; size_t next; };

static bool fill(FindState *st, WIN32_FIND_DATA *d) {
	if (st->next >= st->names.size()) return false;
	std::string n = st->names[st->next++];
	// report ".so" packs under their Windows ".dll" names; LoadLibrary maps back
	if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) n.replace(n.size() - 3, 3, ".dll");
	memset(d, 0, sizeof *d);
	std::wstring w = furb_widen(n.c_str());
	wcsncpy(d->cFileName, w.c_str(), MAX_PATH - 1);
	return true;
}

HANDLE FindFirstFile(const wchar_t *pattern, WIN32_FIND_DATA *d) {
	std::string p = posix_path(pattern);
	size_t slash = p.find_last_of('/');
	std::string dir = slash == std::string::npos ? "." : p.substr(0, slash);
	std::string pat = slash == std::string::npos ? p : p.substr(slash + 1);
	DIR *dh = opendir(dir.c_str());
	if (!dh) return INVALID_HANDLE_VALUE;
	FindState *st = new FindState{{}, 0};
	while (struct dirent *e = readdir(dh))
		if (!fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD)) st->names.push_back(e->d_name);
	closedir(dh);
	if (!fill(st, d)) { delete st; return INVALID_HANDLE_VALUE; }
	return (HANDLE)st;
}
BOOL FindNextFile(HANDLE h, WIN32_FIND_DATA *d) { return fill((FindState *)h, d); }
BOOL FindClose(HANDLE h) { delete (FindState *)h; return TRUE; }

#ifndef FURB_PACK
extern "C" void *furb_host_lookup(const char *name);	// host_reg.cpp
#endif
#ifdef __ANDROID__
// The folder of the library this code is in (ending in '/'), from the
// process's memory map: dladdr gives only the file name on Android 4.4.
static std::string own_folder(void) {
	uintptr_t self = (uintptr_t)&own_folder;
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return "";
	char line[1024];
	std::string dir;
	while (dir.empty() && fgets(line, sizeof line, f)) {
		unsigned long long lo, hi;
		int at = 0;
		if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &lo, &hi, &at) < 2 || !at) continue;
		if (self < lo || self >= hi || line[at] != '/') continue;
		std::string p(line + at);
		p.erase(p.find_last_not_of("\r\n") + 1);
		dir = p.substr(0, p.find_last_of('/') + 1);
	}
	fclose(f);
	return dir;
}
#endif
HMODULE LoadLibrary(const wchar_t *name) {
	std::string path = posix_path(name);
	void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#ifdef __ANDROID__
	// Android installs the packs with the app's other native libraries, as
	// libfurb_<pack>.so; the Mappers folder only names them.  By full path,
	// from this library's own folder: Android 6 and older look a bare name
	// up only in the system's folders.
	if (!h) {
		std::string pack = "libfurb_" + path.substr(path.find_last_of('/') + 1), dir = own_folder();
		if (!dir.empty()) h = dlopen((dir + pack).c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!h) h = dlopen(pack.c_str(), RTLD_NOW | RTLD_LOCAL);
	}
#endif
	if (!h) {
		const char *err = dlerror();
		fprintf(stderr, "furb: dlopen %s: %s\n", path.c_str(), err);
#if defined(__ANDROID__) && !defined(FURB_PACK)
		__android_log_print(ANDROID_LOG_WARN, "furb", "dlopen %s: %s", path.c_str(), err ? err : "?");
#endif
		return NULL;
	}
#ifndef FURB_PACK
	// hand a mapper pack the executable's functions (see host() below)
	typedef void (*Attach)(void *(*)(const char *));
	if (Attach attach = (Attach)dlsym(h, "furb_pack_attach")) attach(furb_host_lookup);
#endif
	return (HMODULE)h;
}
void *GetProcAddress(HMODULE h, const char *name) { return h ? dlsym((void *)h, name) : NULL; }
BOOL FreeLibrary(HMODULE h) { return h && !dlclose((void *)h); }

// ---------------------------------------------------------------- file system
DWORD GetFileAttributes(LPCTSTR p) {
	struct stat st;
	if (stat(posix_path(p).c_str(), &st)) return (DWORD)INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL CreateDirectory(LPCTSTR p, void *) { return mkdir(posix_path(p).c_str(), 0777) == 0; }
BOOL PathFileExists(LPCTSTR p) { return access(posix_path(p).c_str(), F_OK) == 0; }
BOOL PathAppend(LPTSTR path, LPCTSTR more) {
	size_t n = wcslen(path);
	if (n && path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat(path, L"\\");
	while (*more == L'\\' || *more == L'/') more++;
	wcscat(path, more);
	return TRUE;
}

#ifdef FURB_PACK
// ---------------------------------------------------------------- pack side
// The window system, dialogs, GDI, cursor and threads live in the executable
// (furb_cli's headless ones or the GUI's); each API in winapi.def becomes a
// forwarder that looks the executable's function up once by name.  Resource
// APIs pass this pack's own resource table as the module handle.
#include "furb_rc.h"
// The executable's LoadLibrary passes its furb_host_lookup in right after
// loading the pack.  dlsym(RTLD_DEFAULT) also finds it on Linux, where the
// executable exports it, but not on Android, where the host is itself a
// library loaded without RTLD_GLOBAL.
typedef void *(*Lookup)(const char *);
static Lookup host_lookup;
extern "C" __attribute__((visibility("default"))) void furb_pack_attach(Lookup l) { host_lookup = l; }
static void *host(const char *name) {
	if (!host_lookup) host_lookup = (Lookup)dlsym(RTLD_DEFAULT, "furb_host_lookup");
	return host_lookup ? host_lookup(name) : NULL;
}
// (a function not found yet is looked up again next time: a call made while
// the pack loads, before furb_pack_attach, must not stick)
#define W(ret, name, params, args) ret name params { \
	typedef ret (*F) params; static F fn_; if (!fn_) fn_ = (F)host(#name); \
	if (!fn_) return furb_zero<ret>(); return fn_ args; }
#define H W
#define WR(ret, name, params, args) ret name params { \
	typedef ret (*F) params; static F fn_; if (!fn_) fn_ = (F)host(#name); \
	hinst = (HINSTANCE)&furb_module_resources; \
	if (!fn_) return furb_zero<ret>(); return fn_ args; }
#define HR WR
#include "winapi.def"
#undef W
#undef H
#undef WR
#undef HR
// The packs never touch the registry or DirectSound.
LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void *, HKEY *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCloseKey(HKEY) { return 0; }
LONG RegQueryValueEx(HKEY, LPCTSTR, DWORD *, DWORD *, BYTE *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegSetValueEx(HKEY, LPCTSTR, DWORD, DWORD, const BYTE *, DWORD) { return ERROR_FILE_NOT_FOUND; }
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) { *out = NULL; return E_FAIL; }
#endif
