// dn: command line parsing for dn:synctest / dn:p2p
#include "burner.h"
#include "dn.h"
#include <shellapi.h>

static std::string Narrow(const wchar_t* s)
{
	std::string r;
	for (; *s; s++) r += (*s < 128) ? (char)*s : '?';
	return r;
}

static bool ParseUInt(const wchar_t* s, unsigned long long lo, unsigned long long hi, unsigned long long& out, int base = 10)
{
	if (!*s) return false;
	wchar_t* end = NULL;
	out = wcstoull(s, &end, base);
	return *end == 0 && out >= lo && out <= hi;
}

static bool ParseInt(const wchar_t* s, int lo, int hi, int& out)
{
	unsigned long long v;
	if (*s == L'-' || !ParseUInt(s, lo, hi, v)) return false;
	out = (int)v;
	return true;
}

// "<player>=<ip>:<port>"
static bool ParsePeer(const wchar_t* s, DnPeer& p)
{
	std::string t = Narrow(s);
	size_t eq = t.find('='), colon = t.rfind(':');
	if (eq == std::string::npos || colon == std::string::npos || colon < eq) return false;
	std::wstring w(s);
	int player, port;
	if (!ParseInt(w.substr(0, eq).c_str(), 1, 4, player)) return false;
	if (!ParseInt(w.substr(colon + 1).c_str(), 1, 65535, port)) return false;
	p.player = player;
	p.ip = t.substr(eq + 1, colon - eq - 1);
	p.port = port;
	return !p.ip.empty();
}

bool DnParseArgs(const wchar_t* cmdline, DnArgs& a, std::string& err)
{
	if (wcsncmp(cmdline, L"dn:", 3) != 0) {
		err.clear();
		return false;
	}

	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(cmdline, &argc);
	if (!argv) { err = "cannot split command line"; return false; }

	bool ok = true;
	a.mode = Narrow(argv[0] + 3);
	if (a.mode != "synctest" && a.mode != "p2p" && a.mode != "roms" && a.mode != "practice") { err = "unknown mode: " + a.mode; ok = false; }

	for (int i = 1; ok && i < argc; i++) {
		std::wstring k = argv[i];
		const wchar_t* v = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (!v) { err = "missing value for " + Narrow(k.c_str()); ok = false; break; }
		i++;
		unsigned long long u;
		if (k == L"--game") a.game = Narrow(v);
		else if (k == L"--rompath") a.rompath = v;
		else if (k == L"--sound-dump") a.soundDump = v;
		else if (k == L"--frame-log") a.frameLog = v;
		else if (k == L"--ram-trace") a.ramTrace = v;
		else if (k == L"--gfx-dump") a.gfxDump = v;
		else if (k == L"--hp-trace") a.hpTrace = v;
		else if (k == L"--test-start") ok = ParseInt(v, 1, 100000000, a.testStart);
		else if (k == L"--test-crash") ok = ParseInt(v, 1, 1, a.testCrash);
		else if (k == L"--test-slow") ok = ParseInt(v, 1, 100, a.testSlow);
		else if (k == L"--free-coin") a.freeCoin = wcscmp(v, L"1") == 0;
		else if (k == L"--seat-restart") a.seatRestart = wcscmp(v, L"1") == 0;
		else if (k == L"--runahead") a.runAhead = wcscmp(v, L"1") == 0;
		else if (k == L"--seats") {
			a.seats.clear();
			for (const wchar_t* c = v; *c; c++) if (*c >= L'1' && *c <= L'4') a.seats.push_back(*c - L'0');
		}
		else if (k == L"--feed") a.feed = wcscmp(v, L"1") == 0;
		else if (k == L"--bout-end") ok = ParseInt(v, 1, 3600, a.boutEnd);
		else if (k == L"--set-first") ok = ParseInt(v, 1, 9, a.setFirst);
		else if (k == L"--set-won") ok = swscanf(v, L"%d,%d", &a.setWon[0], &a.setWon[1]) == 2 && a.setWon[0] >= 0 && a.setWon[1] >= 0 && a.setWon[0] < 9 && a.setWon[1] < 9;
		else if (k == L"--watch") a.watch = wcscmp(v, L"1") == 0;
		else if (k == L"--replay") { a.replay = wcscmp(v, L"1") == 0; if (a.replay) a.watch = true; }
		else if (k == L"--fast-start") a.fastStart = wcscmp(v, L"1") == 0;
		else if (k == L"--practice") a.practice = wcscmp(v, L"1") == 0;
		else if (k == L"--latency-probe") a.latencyProbe = wcscmp(v, L"1") == 0;
		else if (k == L"--fix-dirs") a.fixDirs = wcscmp(v, L"1") == 0;
		else if (k == L"--balance") a.balance = wcscmp(v, L"1") == 0;
		else if (k == L"--delay-trim") a.delayTrim = wcscmp(v, L"1") == 0;
		else if (k == L"--hold-smooth") a.holdSmooth = wcscmp(v, L"1") == 0;
		else if (k == L"--high-priority") a.highPriority = wcscmp(v, L"1") == 0;
		else if (k == L"--poll-late") a.pollLate = wcscmp(v, L"1") == 0;
		else if (k == L"--poll-wait") a.pollWait = wcscmp(v, L"1") == 0;
		else if (k == L"--vid-select") { ok = ParseUInt(v, 0, 5, u); a.vidSelect = (int)u; }
		else if (k == L"--dwm-sync") { ok = ParseUInt(v, 0, 1, u); a.dwmSync = (int)u; }
		else if (k == L"--aud-select") { ok = ParseUInt(v, 0, 2, u); a.audSelect = (int)u; }
		else if (k == L"--aud-segs") { ok = ParseUInt(v, 2, 16, u); a.audSegs = (int)u; }
		else if (k == L"--practice-set") a.practiceSet = Narrow(v);
		else if (k == L"--script") a.script = Narrow(v);
		else if (k == L"--script-file") {		// tests: a script longer than FBNeo's command line takes
			FILE* f = _wfopen(v, L"rb");
			ok = f != NULL;
			if (f) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) a.script.append(buf, n); fclose(f); }
		}
		else if (k == L"--dump") a.dump = v;
		else if (k == L"--ips") a.ips = v;
		else if (k == L"--frames") ok = ParseInt(v, 0, 100000000, a.frames);
		else if (k == L"--seed") { ok = ParseUInt(v, 0, 0xFFFFFFFFull, u); a.seed = (unsigned int)u; }
		else if (k == L"--players") ok = ParseInt(v, 1, 4, a.players);	// 1: alone in a room until someone joins
		else if (k == L"--me") ok = ParseInt(v, 1, 4, a.me);
		else if (k == L"--local-port") ok = ParseInt(v, 1, 65535, a.localPort);
		else if (k == L"--delay") { if (wcscmp(v, L"auto") == 0) a.delay = -1; else ok = ParseInt(v, 0, 10, a.delay); }
		else if (k == L"--auto-input") { a.autoInput = Narrow(v); size_t c = a.autoInput.find(':'); ok = (a.autoInput.rfind("random:", 0) == 0 || a.autoInput.rfind("human:", 0) == 0) && ParseUInt(v + c + 1, 0, 0xFFFFFFFFull, u); }
		else if (k == L"--parent-hwnd") { ok = wcsncmp(v, L"0x", 2) == 0 && ParseUInt(v + 2, 1, ~0ull, u, 16); a.parent = (HWND)(UINT_PTR)u; }
		else if (k == L"--peer") { DnPeer p; ok = ParsePeer(v, p); if (ok) a.peers.push_back(p); }
		else { err = "unknown option " + Narrow(k.c_str()); ok = false; break; }
		if (!ok && err.empty()) err = "bad value for " + Narrow(k.c_str()) + ": " + Narrow(v);
	}
	LocalFree(argv);

	if (ok && a.game.empty()) { err = "--game is required"; ok = false; }
	if (ok && a.rompath.empty() && a.mode != "roms") { err = "--rompath is required"; ok = false; }
	if (ok && a.mode == "synctest" && a.frames <= 0) { err = "--frames must be > 0 for synctest"; ok = false; }
	if (ok && a.me > a.players) { err = "--me exceeds --players"; ok = false; }
	if (ok && a.mode == "practice") a.practice = true;
	if (ok && a.mode == "p2p" && !a.practice && !a.watch) {
		if ((int)a.peers.size() != a.players - 1) { err = "p2p needs one --peer per remote player"; ok = false; }
		for (size_t i = 0; ok && i < a.peers.size(); i++) {
			if (a.peers[i].player == a.me || a.peers[i].player > a.players) { err = "bad --peer player number"; ok = false; }
		}
	}
	return ok;
}
