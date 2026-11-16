// The executable's side shared by furb_cli and the GUI: the registry (one
// flat key, HKCU\SOFTWARE\Nintendulator, held in memory, loaded from and saved
// to .reg files), DirectInput's data-format objects, and the table through
// which the mapper packs find every winapi.def function in the executable.
#include <windows.h>
#include <dinput.h>
#include "furb_host.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace FurbHost {
bool verbose = false, quiet = false;

// ---- registry ----
struct RegVal { DWORD type; std::vector<BYTE> data; std::wstring name; };
static std::map<std::wstring, RegVal> reg;	// key: lower-case value name
static std::wstring lower(std::wstring s) { for (auto &c : s) c = towlower(c); return s; }
static void reg_put(const std::wstring &name, DWORD type, const void *data, size_t n) {
	RegVal v; v.type = type; v.name = name;
	v.data.assign((const BYTE *)data, (const BYTE *)data + n);
	reg[lower(name)] = v;
}
void set_dword(const std::wstring &name, DWORD v) { reg_put(name, REG_DWORD, &v, 4); }
void set_string(const std::wstring &name, const std::wstring &v) {
	reg_put(name, REG_SZ, v.c_str(), (v.size() + 1) * sizeof(wchar_t));	// TCHAR is wchar_t here
}
void set_binary(const std::wstring &name, const void *data, size_t n) { reg_put(name, REG_BINARY, data, n); }
bool get_dword(const std::wstring &name, DWORD &v) {
	auto it = reg.find(lower(name));
	if (it == reg.end() || it->second.data.size() < 4) return false;
	memcpy(&v, it->second.data.data(), 4);
	return true;
}

static std::wstring decode_file(const std::string &raw) {
	if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
		std::wstring w;
		for (size_t i = 2; i + 1 < raw.size(); i += 2) w += (wchar_t)((unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8);
		return w;
	}
	size_t skip = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	return furb_widen(raw.c_str() + skip);
}
static std::wstring unescape(const std::wstring &s) {
	std::wstring r;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == L'\\' && i + 1 < s.size()) r += s[++i];
		else r += s[i];
	}
	return r;
}
static bool parse_value(const std::wstring &name, std::wstring val, std::string &err) {
	while (!val.empty() && iswspace(val.back())) val.pop_back();
	std::wstring lv = lower(val);
	if (!val.empty() && val[0] == L'"') {
		size_t end = val.rfind(L'"');
		set_string(name, unescape(val.substr(1, end > 0 ? end - 1 : 0)));
	} else if (lv.compare(0, 6, L"dword:") == 0) {
		set_dword(name, (DWORD)wcstoul(val.c_str() + 6, NULL, 16));
	} else if (lv.compare(0, 3, L"hex") == 0) {
		size_t colon = val.find(L':');
		DWORD type = REG_BINARY;
		if (lv.compare(0, 4, L"hex(") == 0) type = (DWORD)wcstoul(val.c_str() + 4, NULL, 16);
		std::vector<BYTE> bytes;
		for (const wchar_t *p = val.c_str() + colon + 1; *p;) {
			while (*p && !iswxdigit(*p)) p++;
			if (!*p) break;
			wchar_t *e;
			bytes.push_back((BYTE)wcstoul(p, &e, 16));
			p = e;
		}
		if (type == 1 || type == 2) {		// REG_SZ / REG_EXPAND_SZ exported as UTF-16LE bytes
			std::wstring w;
			for (size_t i = 0; i + 1 < bytes.size(); i += 2) w += (wchar_t)(bytes[i] | bytes[i + 1] << 8);
			while (!w.empty() && w.back() == 0) w.pop_back();
			set_string(name, w);
		} else if (type == 4 && bytes.size() == 4) {
			DWORD d; memcpy(&d, bytes.data(), 4); set_dword(name, d);
		} else reg_put(name, type == 1 ? REG_SZ : REG_BINARY, bytes.data(), bytes.size());
	} else if (!val.empty() && (iswdigit(val[0]) || val[0] == L'-')) {
		set_dword(name, (DWORD)wcstol(val.c_str(), NULL, 0));
	} else {
		err = "bad value for " + furb_narrow(name.c_str()) + ": " + furb_narrow(val.c_str());
		return false;
	}
	return true;
}

bool load_config(const std::string &path, std::string &err) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) { err = "cannot open " + path; return false; }
	std::string raw;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.append(buf, n);
	fclose(f);
	std::wstring text = decode_file(raw);
	// join regedit's "\" line continuations
	std::wstring joined;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == L'\\' && i + 1 < text.size() && (text[i + 1] == L'\r' || text[i + 1] == L'\n')) {
			while (i + 1 < text.size() && (text[i + 1] == L'\r' || text[i + 1] == L'\n' || text[i + 1] == L' ')) i++;
			continue;
		}
		joined += text[i];
	}
	bool in_section = true, any_section = false;
	size_t pos = 0;
	while (pos < joined.size()) {
		size_t eol = joined.find(L'\n', pos);
		std::wstring line = joined.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
		pos = eol == std::wstring::npos ? joined.size() : eol + 1;
		while (!line.empty() && (line.back() == L'\r' || iswspace(line.back()))) line.pop_back();
		size_t b = 0;
		while (b < line.size() && iswspace(line[b])) b++;
		line = line.substr(b);
		if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
		if (lower(line).compare(0, 8, L"regedit4") == 0 || lower(line).compare(0, 16, L"windows registry") == 0) continue;
		if (line[0] == L'[') {
			any_section = true;
			std::wstring sec = lower(line);
			in_section = sec.find(L"\\software\\nintendulator]") != std::wstring::npos;
			continue;
		}
		if (any_section && !in_section) continue;
		std::wstring name, val;
		if (line[0] == L'"') {
			size_t q = line.find(L'"', 1);
			if (q == std::wstring::npos) continue;
			name = line.substr(1, q - 1);
			size_t eq = line.find(L'=', q);
			if (eq == std::wstring::npos) continue;
			val = line.substr(eq + 1);
		} else {
			size_t eq = line.find(L'=');
			if (eq == std::wstring::npos || line[0] == L'@') continue;
			name = line.substr(0, eq);
			while (!name.empty() && iswspace(name.back())) name.pop_back();
			val = line.substr(eq + 1);
			while (!val.empty() && iswspace(val[0])) val.erase(0, 1);
		}
		if (!parse_value(name, val, err)) return false;
	}
	return true;
}

bool save_config(const std::string &path) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fprintf(f, "REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\SOFTWARE\\Nintendulator]\r\n");
	std::vector<const RegVal *> vals;
	for (auto &kv : reg) vals.push_back(&kv.second);
	std::sort(vals.begin(), vals.end(), [](const RegVal *a, const RegVal *b) { return a->name < b->name; });
	for (const RegVal *v : vals) {
		std::string name = furb_narrow(v->name.c_str());
		if (v->type == REG_DWORD && v->data.size() == 4) {
			DWORD d; memcpy(&d, v->data.data(), 4);
			fprintf(f, "\"%s\"=dword:%08lx\r\n", name.c_str(), (unsigned long)d);
		} else if (v->type == REG_SZ) {
			std::string s = furb_narrow((const wchar_t *)v->data.data()), e;
			for (char c : s) { if (c == '\\' || c == '"') e += '\\'; e += c; }
			fprintf(f, "\"%s\"=\"%s\"\r\n", name.c_str(), e.c_str());
		} else {
			fprintf(f, "\"%s\"=hex:", name.c_str());
			for (size_t i = 0; i < v->data.size(); i++) fprintf(f, "%s%02x", i ? "," : "", v->data[i]);
			fprintf(f, "\r\n");
		}
	}
	fclose(f);
	return true;
}

} // namespace FurbHost

using namespace FurbHost;

// ---------------------------------------------------------------- registry API
LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY *out) { *out = (HKEY)1; return 0; }
LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void *, HKEY *out, DWORD *) { *out = (HKEY)1; return 0; }
LONG RegCloseKey(HKEY) { return 0; }
LONG RegQueryValueEx(HKEY, LPCTSTR name, DWORD *, DWORD *type, BYTE *data, DWORD *size) {
	auto it = reg.find(lower(name));
	if (it == reg.end()) return ERROR_FILE_NOT_FOUND;
	DWORD need = (DWORD)it->second.data.size();
	if (type) *type = it->second.type;
	if (!data) { if (size) *size = need; return 0; }
	if (!size) return 87;
	if (*size < need) { *size = need; return ERROR_MORE_DATA; }
	memcpy(data, it->second.data.data(), need);
	*size = need;
	return 0;
}
LONG RegSetValueEx(HKEY, LPCTSTR name, DWORD, DWORD type, const BYTE *data, DWORD size) {
	reg_put(name, type, data, size);
	return 0;
}


const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT), 0, 0, 256, 0, NULL};
const DIDATAFORMAT c_dfDIMouse2 = {sizeof(DIDATAFORMAT), 0, 0, sizeof(DIMOUSESTATE2), 0, NULL};
const DIDATAFORMAT c_dfDIJoystick2 = {sizeof(DIDATAFORMAT), 0, 0, sizeof(DIJOYSTATE2), 0, NULL};

// ---------------------------------------------------------------- export
// The one symbol the executable exports (--dynamic-list): the packs find the
// host functions through it.
extern "C" __attribute__((visibility("default"))) void *furb_host_lookup(const char *name) {
	static const std::map<std::string, void *> table = {
#define W(ret, name, params, args) {#name, (void *)(ret (*) params)&name},
#define H W
#define WR W
#define HR W
#include "winapi.def"
#undef W
#undef H
#undef WR
#undef HR
	};
	auto it = table.find(name);
	return it == table.end() ? NULL : it->second;
}
