// dn: GGPO rollback netplay for FBNeo (dn:synctest, dn:p2p)
// State save/load follows FBNeo's run-ahead code: ACB_READ copies emulator -> buffer, ACB_WRITE buffer -> emulator.
#include <winsock2.h>	// dn: before windows.h (burner.h) for the udp port check
#include "burner.h"
#include "dn.h"
#include "ggponet.h"
#include <stdint.h>
#include <algorithm>

static GGPOSession* gSession = NULL;
static int gPlayers = 2;
static std::vector<UINT32> gPlayerInp[GGPO_MAX_PLAYERS];	// driver input indices per player, bit k = gPlayerInp[p][k]
static std::vector<UINT32> gOtherDigital;					// reset/service/... : always forced to 0

// Lobby key settings. The lobby sends its own P1 mapping (keyboard and pad codes per role, plus hotkeys) and we
// read those controls ourselves: FBNeo's GameInp holds one code per input. Roles match the driver's szInfo "p1 <role>".
enum { DN_ROLES = 11, DN_ROLE_FIRE1 = 5 };
static const char* kRoleInfo[DN_ROLES] = { "up", "down", "left", "right", "start", "fire 1", "fire 2", "fire 3", "fire 4", "fire 5", "fire 6" };
static const char* kRoleKey[DN_ROLES] = { "up", "down", "left", "right", "start", "fire1", "fire2", "fire3", "fire4", "fire5", "fire6" };
static std::vector<int> gRoleCodes[DN_ROLES];
static bool gKeymapSet = false;			// until the lobby sends one, FBNeo's own mapping drives the roles
static std::vector<int> gHotkeys;		// per entry: code, fire mask (bit 0 = fire 1), turbo period in frames (0 = plain hotkey)
static std::vector<int> gTurboPhase;	// per entry: frames since its key went down
static std::vector<int> gBitRole;		// role of bit k in the local player's word, or -1 (FBNeo mapping: coin, select...)

// Flags riding along in every player's word, so all peers can show them next to the player: which controller
// the key settings use (28 keyboard, 29 joystick) and the badges (30 hotkey, 31 turbo). Drivers get bits 0-27.
enum { DN_BIT_KEYBOARD = 28, DN_BIT_JOYSTICK = 29, DN_BIT_HOTKEY = 30, DN_BIT_TURBO = 31, DN_GAME_BITS = 0x0FFFFFFF };
static bool gBadge[GGPO_MAX_PLAYERS][2];
static int gCtrl[GGPO_MAX_PLAYERS] = { -1, -1, -1, -1 };
static uint32_t gCtrlBits = 1u << DN_BIT_KEYBOARD;	// FBNeo's own mapping (keyboard) until the lobby sends one
static bool gSyncError = false;
static std::string gSyncErrorMsg;

extern "C" void DnOnSyncError(const char* msg)
{
	gSyncError = true;
	gSyncErrorMsg = msg;
}

// ---------------------------------------------------------------- inputs

static void DnBuildInputMap(int players)
{
	for (int p = 0; p < GGPO_MAX_PLAYERS; p++) gPlayerInp[p].clear();
	gOtherDigital.clear();

	struct BurnInputInfo bii;
	for (UINT32 i = 0; i < nGameInpCount; i++) {
		memset(&bii, 0, sizeof(bii));
		if (BurnDrvGetInputInfo(&bii, i) || bii.pVal == NULL || bii.nType != BIT_DIGITAL || bii.szName == NULL) continue;
		int p = (bii.szName[0] == 'P' && bii.szName[1] >= '1' && bii.szName[1] <= '4' && bii.szName[2] == ' ') ? bii.szName[1] - '1' : -1;
		if (p >= 0 && p < players && gPlayerInp[p].size() < 28) gPlayerInp[p].push_back(i);
		else gOtherDigital.push_back(i);
	}

	gBitRole.clear();
	for (size_t k = 0; k < gPlayerInp[0].size(); k++) {
		memset(&bii, 0, sizeof(bii));
		BurnDrvGetInputInfo(&bii, gPlayerInp[0][k]);
		int role = -1;
		for (int r = 0; r < DN_ROLES && bii.szInfo && !_strnicmp(bii.szInfo, "p1 ", 3); r++) {
			if (!_stricmp(bii.szInfo + 3, kRoleInfo[r])) role = r;
		}
		gBitRole.push_back(role);
	}
}

static void DnApplyConstants()
{
	struct GameInp* pgi = GameInp;
	for (UINT32 i = 0; i < nGameInpCount; i++, pgi++) {
		if (pgi->nInput == GIT_CONSTANT && pgi->Input.pVal) *(pgi->Input.pVal) = pgi->Input.Constant.nConst;
	}
}

static void DnApplyInputs(const uint32_t* in)
{
	struct BurnInputInfo bii;
	for (int p = 0; p < gPlayers; p++) {
		for (size_t k = 0; k < gPlayerInp[p].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[p][k]);
			*bii.pVal = (in[p] >> k) & 1;
		}
	}
	for (size_t k = 0; k < gOtherDigital.size(); k++) {
		BurnDrvGetInputInfo(&bii, gOtherDigital[k]);
		*bii.pVal = 0;
	}
	static const char* kCtrl[] = { "none", "keyboard", "joystick", "mixed" };
	for (int p = 0; p < gPlayers; p++) {
		int c = (in[p] >> DN_BIT_KEYBOARD) & 3;
		if (c != gCtrl[p]) {
			gCtrl[p] = c;
			char j[80];
			sprintf(j, "{\"ev\":\"controller\",\"player\":%d,\"type\":\"%s\"}", p + 1, kCtrl[c]);
			DnEmit(j);
		}
		for (int b = 0; b < 2; b++) {
			if (gBadge[p][b] || !((in[p] >> (DN_BIT_HOTKEY + b)) & 1)) continue;
			gBadge[p][b] = true;	// ponytail: stays for the whole session; per-match reset needs match-end detection (phase 1)
			char j[80];
			sprintf(j, "{\"ev\":\"badge\",\"player\":%d,\"kind\":\"%s\"}", p + 1, b ? "turbo" : "hotkey");
			DnEmit(j);
		}
	}
}

// Local player's controls: the lobby's mapping for the roles, FBNeo's P1 mapping for the rest (coin, select).
// Without focus (another window in front) only joystick controls count: keys typed elsewhere must not leak into the
// game, but a stick is only ever for the game (e.g. a stream or chat window kept in front).
static bool DnSwitch(int code, bool focus)
{
	return (focus || (code >= 0x4000 && code < 0x8000)) && InputReadSwitch(code);
}

// Start = coin + start: players never need a coin key or a joystick button for it (plan §4.6). One press runs this:
// coin held long enough for the game to count it, a wait (SF2 ignores start until its title screen is up, over 1 s), then start.
// ponytail: a press in the middle of a fight also puts in a coin. Knowing when a game wants one takes per-game RAM addresses.
enum { DN_ROLE_START = 4, DN_COIN_FRAMES = 6, DN_START_AT = 90, DN_START_END = 96 };
static uint32_t gCoinMask = 0;		// the local player's coin bit(s)
static int gStartStep = -1;
static bool gStartWasDown = false;
static DnArgs gArgs;
static int gFrame = 0;
extern int nVidSDisplayStatus;	// vid_directx_support.cpp

uint32_t DnPackLocalInput(bool focus)
{
	if (gArgs.testStart) focus = false;	// a test must not depend on what someone types while its window is in front
	uint32_t keys = 0, hot = 0, v = 0;
	for (int r = 0; r < DN_ROLES; r++) {
		for (size_t c = 0; c < gRoleCodes[r].size(); c++) {
			if (DnSwitch(gRoleCodes[r][c], focus)) keys |= 1u << r;
		}
	}
	if (gArgs.testStart && gFrame >= gArgs.testStart && gFrame < gArgs.testStart + 3) keys |= 1u << DN_ROLE_START;

	const bool startMakesCoin = gKeymapSet && !gArgs.freeCoin;
	bool coin = false;
	if (startMakesCoin) {
		const bool down = (keys >> DN_ROLE_START) & 1;
		if (down && !gStartWasDown && gStartStep < 0) gStartStep = 0;
		gStartWasDown = down;
		keys &= ~(1u << DN_ROLE_START);
		if (gStartStep >= 0) {
			coin = gStartStep < DN_COIN_FRAMES;
			if (gStartStep >= DN_START_AT) keys |= 1u << DN_ROLE_START;
			if (++gStartStep >= DN_START_END) gStartStep = -1;
		}
	}
	gTurboPhase.resize(gHotkeys.size() / 3);
	for (size_t i = 0, n = 0; i + 2 < gHotkeys.size(); i += 3, n++) {
		if (!DnSwitch(gHotkeys[i], focus)) { gTurboPhase[n] = 0; continue; }
		uint32_t mask = (gHotkeys[i + 1] & 0x3F) << DN_ROLE_FIRE1;
		int period = gHotkeys[i + 2];
		if (period > 1) {	// turbo: pressed for the first half of each period, starting with a press (2 = 30/s at 60fps)
			if (gTurboPhase[n] % period < (period + 1) / 2) hot |= mask;
			gTurboPhase[n]++;
			v |= 1u << DN_BIT_TURBO;
		} else {
			hot |= mask;
			v |= 1u << DN_BIT_HOTKEY;
		}
	}

	struct BurnInputInfo bii;
	for (size_t k = 0; k < gPlayerInp[0].size(); k++) {
		int r = gBitRole[k];
		bool on;
		if (r >= 0 && gKeymapSet) {
			on = (keys >> r) & 1;
		} else if (startMakesCoin && ((gCoinMask >> k) & 1)) {
			on = coin;		// the game's own coin key does nothing
		} else {
			BurnDrvGetInputInfo(&bii, gPlayerInp[0][k]);
			on = focus && *bii.pVal != 0;
		}
		if (on || (r >= 0 && ((hot >> r) & 1))) v |= 1u << k;
	}
	return v;
}

static void DnEmitKeys();

static void DnUpdateCtrlBits()
{
	gCtrlBits = 0;
	for (int r = 0; r < DN_ROLES; r++) {
		for (size_t c = 0; c < gRoleCodes[r].size(); c++) gCtrlBits |= 1u << (gRoleCodes[r][c] < 0x4000 ? DN_BIT_KEYBOARD : DN_BIT_JOYSTICK);
	}
	for (size_t i = 0; i + 2 < gHotkeys.size(); i += 3) gCtrlBits |= 1u << (gHotkeys[i] < 0x4000 ? DN_BIT_KEYBOARD : DN_BIT_JOYSTICK);
}

void DnSetKeymap(const std::string& line)
{
	for (int r = 0; r < DN_ROLES; r++) gRoleCodes[r] = DnJsonInts(line, kRoleKey[r]);
	gKeymapSet = true;
	DnUpdateCtrlBits();
	DnEmitKeys();
}

void DnSetHotkeys(const std::string& line)
{
	gHotkeys = DnJsonInts(line, "hk");
	gTurboPhase.clear();
	if (gKeymapSet) DnUpdateCtrlBits();
	DnEmitKeys();
}

// ---------------------------------------------------------------- state

static UINT8* gScanPtr = NULL;
static INT32 gScanLen = 0;

static INT32 __cdecl DnLenAcb(struct BurnArea* pba) { gScanLen += pba->nLen; return 0; }
static INT32 __cdecl DnReadAcb(struct BurnArea* pba) { memcpy(gScanPtr, pba->Data, pba->nLen); gScanPtr += pba->nLen; return 0; }
static INT32 __cdecl DnWriteAcb(struct BurnArea* pba) { memcpy(pba->Data, gScanPtr, pba->nLen); gScanPtr += pba->nLen; return 0; }

static int DnStateSize()
{
	gScanLen = 0;
	BurnAcb = DnLenAcb;
	BurnAreaScan(ACB_FULLSCAN | ACB_NET_OPT | ACB_READ, NULL);
	return gScanLen + (int)sizeof(UINT32);
}

static uint32_t Fnv1a(const UINT8* p, int n)
{
	uint32_t h = 2166136261u;
	while (n--) { h ^= *p++; h *= 16777619u; }
	return h;
}

static bool __cdecl cbSave(unsigned char** buffer, int* len, int* checksum, int)
{
	int n = DnStateSize();
	UINT8* buf = (UINT8*)malloc(n);
	if (!buf) return false;
	memcpy(buf, &nCurrentFrame, sizeof(UINT32));	// not part of the driver scan, but drivers read it
	gScanPtr = buf + sizeof(UINT32);
	BurnAcb = DnReadAcb;
	BurnAreaScan(ACB_FULLSCAN | ACB_NET_OPT | ACB_READ, NULL);
	*buffer = buf;
	*len = n;
	*checksum = (int)Fnv1a(buf, n);
	return true;
}

static bool __cdecl cbLoad(unsigned char* buffer, int)
{
	memcpy(&nCurrentFrame, buffer, sizeof(UINT32));
	gScanPtr = buffer + sizeof(UINT32);
	BurnAcb = DnWriteAcb;
	BurnAreaScan(ACB_FULLSCAN | ACB_NET_OPT | ACB_WRITE, NULL);
	return true;
}

static void __cdecl cbFree(void* buffer) { free(buffer); }
static bool __cdecl cbLog(char*, unsigned char*, int) { return true; }
static bool __cdecl cbBegin(const char*) { return true; }

// ---------------------------------------------------------------- frames

static void DnStepFrame(bool draw)
{
	if (!draw) {
		// Sound chips only clock while they render (e.g. YM2610Render returns early without pBurnSoundOut), so a
		// re-simulated frame still renders, into a buffer nobody plays: otherwise sample playback and envelopes come
		// out of a rollback frames behind, wrong until the next note, and different from the peer's chip.
		static std::vector<INT16> discard;
		if (nBurnSoundLen > 0 && discard.size() < (size_t)nBurnSoundLen * 2) discard.resize(nBurnSoundLen * 2);
		pBurnDraw = NULL;
		pBurnSoundOut = nBurnSoundLen > 0 ? discard.data() : NULL;	// synctest runs without sound (no rate)
	}
	nCurrentFrame++;
	BurnDrvFrame();
}

// Called by GGPO when it re-simulates frames during a rollback.
static int gRollbackFrames = 0;	// re-simulated frames since the last stats line

static bool __cdecl cbAdvance(int)
{
	gRollbackFrames++;
	uint32_t in[GGPO_MAX_PLAYERS] = { 0 };
	int disconnect = 0;
	ggpo_synchronize_input(gSession, in, sizeof(uint32_t) * gPlayers, &disconnect);
	DnApplyInputs(in);
	DnStepFrame(false);
	ggpo_advance_frame(gSession);
	return true;
}

static bool __cdecl cbEvent(GGPOEvent* e);

static GGPOSessionCallbacks DnCallbacks()
{
	GGPOSessionCallbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.begin_game = cbBegin;
	cb.save_game_state = cbSave;
	cb.load_game_state = cbLoad;
	cb.log_game_state = cbLog;
	cb.free_buffer = cbFree;
	cb.advance_frame = cbAdvance;
	cb.on_event = cbEvent;
	return cb;
}

static uint32_t XorShift(uint32_t& s)
{
	s ^= s << 13; s ^= s >> 17; s ^= s << 5;
	return s;
}

// ---------------------------------------------------------------- modes

static int DnSyncTest(const DnArgs& a)
{
	GGPOSessionCallbacks cb = DnCallbacks();
	if (!GGPO_SUCCEEDED(ggpo_start_synctest(&gSession, &cb, (char*)"dongne", gPlayers, sizeof(uint32_t), 1))) {
		DnEmitError("ggpo_failed", "ggpo_start_synctest");
		return DN_EXIT_OTHER;
	}
	GGPOPlayerHandle h[GGPO_MAX_PLAYERS];
	for (int p = 0; p < gPlayers; p++) {
		GGPOPlayer pl;
		memset(&pl, 0, sizeof(pl));
		pl.size = sizeof(pl);
		pl.type = GGPO_PLAYERTYPE_LOCAL;
		pl.player_num = p + 1;
		ggpo_add_player(gSession, &pl, &h[p]);
	}

	uint32_t rng = a.seed ? a.seed : 1;
	int f = 0;
	for (; f < a.frames && !gSyncError; f++) {
		ggpo_idle(gSession, 0);
		for (int p = 0; p < gPlayers; p++) {
			uint32_t v = XorShift(rng) & DN_GAME_BITS;
			ggpo_add_local_input(gSession, h[p], &v, sizeof(v));
		}
		uint32_t in[GGPO_MAX_PLAYERS] = { 0 };
		int disconnect = 0;
		if (GGPO_SUCCEEDED(ggpo_synchronize_input(gSession, in, sizeof(uint32_t) * gPlayers, &disconnect))) {
			DnApplyInputs(in);
			DnStepFrame(false);
			ggpo_advance_frame(gSession);
		}
	}
	ggpo_close_session(gSession);
	gSession = NULL;

	char j[160];
	if (gSyncError) {
		sprintf(j, "{\"ev\":\"synctest\",\"ok\":false,\"frame\":%d}", f);
		DnEmit(j);
		DnEmitError("desync", gSyncErrorMsg.c_str());
		return DN_EXIT_DESYNC;
	}
	sprintf(j, "{\"ev\":\"synctest\",\"ok\":true,\"frames\":%d}", f);
	DnEmit(j);
	return DN_EXIT_OK;
}

// Rom path + driver init + netplay-safe input setup. Returns 0 or a DN_EXIT_* code (error already emitted).
static int DnLoadGame(const DnArgs& a)
{
	std::wstring rp = a.rompath;
	if (rp.back() != L'\\' && rp.back() != L'/') rp += L'\\';
	wcsncpy(szAppRomPaths[0], rp.c_str(), MAX_PATH - 1);

	int nGame = BurnDrvGetIndex((char*)a.game.c_str());
	if (nGame < 0) {
		DnEmitError("bad_args", ("unknown game: " + a.game).c_str());
		return DN_EXIT_OTHER;
	}
	if (DrvInit(nGame, false) || !bDrvOkay) {
		DnEmitError("rom_missing", ("cannot load " + a.game).c_str());
		return DN_EXIT_ROM;
	}

	InpDIPSWResetDIPs();	// driver default DIPs on every peer, whatever the local config says
	DnApplyConstants();
	gPlayers = a.players;
	DnBuildInputMap(gPlayers);
	return 0;
}

int DnRun(const DnArgs& a)
{
	bNoPopups = true;
	bQuietLoading = true;
	bDontInitMedia = true;

	BurnLibInit();
	ConfigAppLoad();

	int rc = DnLoadGame(a);
	if (rc == 0) {
		rc = DnSyncTest(a);
		DrvExit();
	}
	BurnLibExit();
	return rc;
}

// ---------------------------------------------------------------- dn:p2p

bool bDnNet = false;
static int gExitCode = DN_EXIT_OK;
static GGPOPlayerHandle gHandle[GGPO_MAX_PLAYERS];
static GGPOPlayerHandle gLocal;
static int gRemoteAlive = 0;
static bool gAlone = false;
static int gFramesAhead = 0;
static int gHeldFrames = 0, gStalledFrames = 0;	// since the last stats line: held for time sync / waiting on the peer's input
static bool gEnding = false;
static uint32_t gAutoRng = 0;
static bool gAutoHuman = false;	// --auto-input human:<seed>
// Where a game keeps its credit count (main CPU address), found by inserting coins and diffing RAM. The human bot
// inserts a coin only when it reaches 0: every start or continue uses one, so that is one coin per match.
static const struct { const char* game; UINT32 credits; } kCredits[] = {
	{ "sf2ce", 0xFF82D9 },
};
static UINT32 gCreditAddr = 0;

// What the key settings now are, and whether they are used at all (a bot ignores them): the lobby shows this,
// so settings that don't reach the game are visible right there instead of in a match.
static void DnEmitKeys()
{
	int kb = 0, joy = 0;
	for (int r = 0; r < DN_ROLES; r++) {
		for (size_t c = 0; c < gRoleCodes[r].size(); c++) (gRoleCodes[r][c] < 0x4000 ? kb : joy)++;
	}
	char j[160];
	sprintf(j, "{\"ev\":\"keys\",\"keyboard\":%d,\"joystick\":%d,\"hotkeys\":%d,\"input\":\"%s\"}",
		kb, joy, (int)(gHotkeys.size() / 3), gAutoRng ? "bot" : "local");
	DnEmit(j);
}
static DWORD gLastStats = 0;

static int DnPlayerOf(GGPOPlayerHandle h)
{
	for (int p = 0; p < gPlayers; p++) if (gHandle[p] == h) return p + 1;
	return 0;
}

static void DnEnd(const char* reason)
{
	if (gEnding) return;
	gEnding = true;
	char j[96];
	sprintf(j, "{\"ev\":\"ended\",\"reason\":\"%s\"}", reason);
	DnEmit(j);
	PostQuitMessage(0);
}

static bool __cdecl cbEvent(GGPOEvent* e)
{
	char j[96];
	switch (e->code) {
		case GGPO_EVENTCODE_CONNECTED_TO_PEER:
			sprintf(j, "{\"ev\":\"connected\",\"player\":%d}", DnPlayerOf(e->u.connected.player));
			DnEmit(j);
			break;
		case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
			sprintf(j, "{\"ev\":\"disconnected\",\"player\":%d}", DnPlayerOf(e->u.disconnected.player));
			DnEmit(j);
			if (--gRemoteAlive <= 0) DnEnd("peer_left");	// co-op keeps going while someone is left
			break;
		case GGPO_EVENTCODE_TIMESYNC:
			gFramesAhead = e->u.timesync.frames_ahead;
			break;
	}
	return true;
}

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

static bool DnPortFree(int port)
{
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) return true;	// can't tell; let GGPO try
	BOOL excl = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&excl, sizeof(excl));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((u_short)port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	bool ok = bind(s, (sockaddr*)&sa, sizeof(sa)) == 0;
	closesocket(s);
	return ok;
}

int DnP2PLoad()
{
	DnIpcStart(gArgs.parent);	// before the screen window exists, so ScrnInit() sees the parent
	bNoPopups = true;
	bQuietLoading = true;
	bRunAhead = 0;			// its own save/load would fight the rollback
	bCheatsAllowed = false;
	nCmdOptUsed = 2;		// like "-w": windowed, and no game-select dialog at startup
	nVidFullscreen = 0;

	if ((gExitCode = DnLoadGame(gArgs)) != 0) return 1;

	gAlone = gPlayers == 1;		// alone in a room: no peer and no rollback, so no GGPO (it can't run a session of one)
	if (!gAlone) {
		if (!DnPortFree(gArgs.localPort)) {
			DnEmitError("port_in_use", ("udp port " + std::to_string(gArgs.localPort)).c_str());
			gExitCode = DN_EXIT_PORT;
			return 1;
		}

		GGPOSessionCallbacks cb = DnCallbacks();
		if (!GGPO_SUCCEEDED(ggpo_start_session(&gSession, &cb, (char*)"dongne", gPlayers, sizeof(uint32_t), gArgs.localPort))) {
			DnEmitError("ggpo_failed", "ggpo_start_session");
			gExitCode = DN_EXIT_OTHER;
			return 1;
		}
		ggpo_set_disconnect_timeout(gSession, 3000);
		ggpo_set_disconnect_notify_start(gSession, 1000);

		for (int p = 1; p <= gPlayers; p++) {
			GGPOPlayer pl;
			memset(&pl, 0, sizeof(pl));
			pl.size = sizeof(pl);
			pl.player_num = p;
			pl.type = GGPO_PLAYERTYPE_LOCAL;
			for (size_t i = 0; i < gArgs.peers.size(); i++) {
				if (gArgs.peers[i].player != p) continue;
				pl.type = GGPO_PLAYERTYPE_REMOTE;
				strncpy(pl.u.remote.ip_address, gArgs.peers[i].ip.c_str(), sizeof(pl.u.remote.ip_address) - 1);
				pl.u.remote.port = (unsigned short)gArgs.peers[i].port;
			}
			if (!GGPO_SUCCEEDED(ggpo_add_player(gSession, &pl, &gHandle[p - 1]))) {
				DnEmitError("ggpo_failed", "ggpo_add_player");
				gExitCode = DN_EXIT_OTHER;
				return 1;
			}
			if (pl.type == GGPO_PLAYERTYPE_LOCAL) gLocal = gHandle[p - 1];
		}
		gRemoteAlive = gPlayers - 1;
		ggpo_set_frame_delay(gSession, gLocal, gArgs.delay < 0 ? 0 : gArgs.delay);
		char j[64];
		sprintf(j, "{\"ev\":\"delay\",\"frames\":%d,\"auto\":%s}", gArgs.delay < 0 ? 0 : gArgs.delay, gArgs.delay < 0 ? "true" : "false");
		DnEmit(j);
	}

	{
		struct BurnInputInfo bii;
		for (size_t k = 0; k < gPlayerInp[gArgs.me - 1].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[gArgs.me - 1][k]);
			size_t n = bii.szInfo ? strlen(bii.szInfo) : 0;
			if (n >= 5 && !_stricmp(bii.szInfo + n - 5, " coin")) gCoinMask |= 1u << k;
		}
		for (size_t i = 0; i < sizeof(kCredits) / sizeof(kCredits[0]); i++) {
			if (gArgs.game == kCredits[i].game) gCreditAddr = kCredits[i].credits;
		}
	}

	if (!gArgs.autoInput.empty()) {
		gAutoRng = (uint32_t)strtoul(gArgs.autoInput.c_str() + gArgs.autoInput.find(':') + 1, NULL, 10) | 1;
		gAutoHuman = gArgs.autoInput.rfind("human:", 0) == 0;
		nAudVolume = 0;		// a bot's game is nobody's view; on one PC its sound would play over the player's
	}

	kNetGame = 1;			// FBNeo's netplay switches: no pause, ffwd, rewind or pacing tricks
	nVidSDisplayStatus = 0;	// no FBNeo "kaillera" icon: it sat on top of the game's 2P score, and the lobby shows the connection

	// Sound: WASAPI with 4 frames of buffer (~67 ms) instead of DirectSound's 6 (~100 ms); hits are heard sooner.
	// MediaInit() falls back to DirectSound if WASAPI can't start.
	nAudSelect = 2;
	nAudSegCount = 4;
	bAlwaysDrawFrames = 1;	// two frames due at one sound wakeup: show both, on schedule (RunPacePresent), instead of dropping the first
	gLastStats = GetTickCount();

	INT32 w = 0, h = 0;
	BurnDrvGetVisibleSize(&w, &h);
	char j[64];
	sprintf(j, "{\"ev\":\"ready\",\"w\":%d,\"h\":%d}", w, h);
	DnEmit(j);
	return 0;
}

// random: a new input every frame (stress: rolls back nearly every frame). human: each input held 4-15 frames
// like a person, so rollbacks come about as often as against a real opponent (measured 17/s vs 210/s at 115 ms ping),
// and no random coins (a coin sound every few seconds); see kCredits. Games not listed get no coins from the bot.
static uint32_t DnBotInput()
{
	static uint32_t v = 0;
	static int hold = 0, coinFrames = 0, coinWait = 0;
	if (!gAutoHuman || --hold <= 0) {
		v = XorShift(gAutoRng) & DN_GAME_BITS & (gAutoHuman ? ~gCoinMask : ~0u);
		hold = 4 + XorShift(gAutoRng) % 12;
	}
	if (!gAutoHuman) return v;
	if (coinWait > 0) coinWait--;
	else if (gCreditAddr && ReadValueAtHardwareAddress(gCreditAddr, 1, 0) == 0) {
		coinFrames = 6;		// held long enough for the game to count it
		DnEmit("{\"ev\":\"botcoin\"}");
		coinWait = 180;		// and give it 3 s to show up in the credits
	}
	if (coinFrames > 0) {
		coinFrames--;
		return v | gCoinMask;
	}
	return v;
}

// --frame-log: one line per event: time in ms, then d (drawn), s (run, not drawn), h (held: ahead of the peer), w (waiting for the peer).
void DnFrameLog(char kind)
{
	static FILE* f = gArgs.frameLog.empty() ? NULL : _wfopen(gArgs.frameLog.c_str(), L"w");
	if (!f) return;
	LARGE_INTEGER t, hz;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&hz);
	fprintf(f, "%.3f %c\n", t.QuadPart * 1000.0 / hz.QuadPart, kind);
}

// --fast-start: boot takes SF2 about 10 s, so every peer runs it unseen at full speed (about 0.4 s), with the same
// scripted coins and starts, and the session opens on character select. Same emulator + same ROM (the room server
// checks both) + same script = the same state: the sum is reported so a test can compare the peers.
// ponytail: the frames are per game, found by trying; a game not listed boots normally.
static const struct { const char* game; int coin1, coin2, start1, start2, end; } kFastStart[] = {
	{ "sf2ce", 900, 915, 1000, 1060, 1120 },
};

static void DnFastStart()
{
	int coin1 = 0, coin2 = 0, start1 = 0, start2 = 0, end = 0;
	for (size_t i = 0; i < sizeof(kFastStart) / sizeof(kFastStart[0]); i++) {
		if (gArgs.game == kFastStart[i].game) {
			coin1 = kFastStart[i].coin1; coin2 = kFastStart[i].coin2; start1 = kFastStart[i].start1; start2 = kFastStart[i].start2; end = kFastStart[i].end;
		}
	}
	if (!end) return;
	uint32_t coinBits[2] = { 0, 0 }, startBits[2] = { 0, 0 };
	struct BurnInputInfo bii;
	for (int p = 0; p < 2 && p < gPlayers; p++) {
		for (size_t k = 0; k < gPlayerInp[p].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[p][k]);
			size_t n = bii.szInfo ? strlen(bii.szInfo) : 0;
			if (n >= 5 && !_stricmp(bii.szInfo + n - 5, " coin")) coinBits[p] |= 1u << k;
			if (n >= 6 && !_stricmp(bii.szInfo + n - 6, " start")) startBits[p] |= 1u << k;
		}
	}
	LARGE_INTEGER t0, t1, hz;
	QueryPerformanceCounter(&t0);
	for (int f = 0; f < end; f++) {
		uint32_t in[GGPO_MAX_PLAYERS] = { 0 };
		if (f >= coin1 && f < coin1 + 6) in[0] |= coinBits[0];
		if (gPlayers > 1 && f >= coin2 && f < coin2 + 6) in[0] |= coinBits[0];	// alone: one coin, one start
		if (f >= start1 && f < start1 + 6) in[0] |= startBits[0];
		if (f >= start2 && f < start2 + 6) in[1] |= startBits[1];
		DnApplyInputs(in);
		DnStepFrame(false);
	}
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&hz);
	unsigned char* buf = NULL; int len = 0, sum = 0;
	cbSave(&buf, &len, &sum, 0);
	free(buf);
	char j[128];
	sprintf(j, "{\"ev\":\"faststart\",\"frames\":%d,\"ms\":%d,\"sum\":%u,\"credits\":%d}", end, (int)((t1.QuadPart - t0.QuadPart) * 1000 / hz.QuadPart), (unsigned)sum, gCreditAddr ? (int)ReadValueAtHardwareAddress(gCreditAddr, 1, 0) : -1);
	DnEmit(j);
}

int DnNetGetInput()
{
	if ((!gSession && !gAlone) || gEnding) return 1;
	static bool fastStarted = false;
	if (gArgs.fastStart && !fastStarted) { fastStarted = true; DnFastStart(); }	// before GGPO saves its first frame
	if (gFramesAhead > 0) {	// GGPO says we're ahead of the peer: hold this frame
		gFramesAhead--;
		gHeldFrames++;
		DnFrameLog('h');
		return 1;
	}
	if (gArgs.testStart && gCreditAddr) {
		static int lastCredits = -1;
		int credits = (int)ReadValueAtHardwareAddress(gCreditAddr, 1, 0);
		if (credits != lastCredits) {
			lastCredits = credits;
			char j[40];
			sprintf(j, "{\"ev\":\"credits\",\"n\":%d}", credits);
			DnEmit(j);
		}
	}
	uint32_t local = gAutoRng ? DnBotInput() : ((DnInputOn() ? DnPackLocalInput(DnHasInputFocus()) : 0) | gCtrlBits);	// the controller shows either way
	if (gAlone) {
		DnApplyInputs(&local);
		return 0;
	}
	if (!GGPO_SUCCEEDED(ggpo_add_local_input(gSession, gLocal, &local, sizeof(local)))) {	// not synchronized yet, or too far ahead of the peer's input
		if (gFrame) { gStalledFrames++; DnFrameLog('w'); }
		return 1;
	}

	uint32_t in[GGPO_MAX_PLAYERS] = { 0 };
	int disconnect = 0;
	if (!GGPO_SUCCEEDED(ggpo_synchronize_input(gSession, in, sizeof(uint32_t) * gPlayers, &disconnect))) return 1;
	DnApplyInputs(in);
	return 0;
}

void DnNetAdvanceFrame()
{
	if (!gSession && !gAlone) return;
	static FILE* dump = gArgs.soundDump.empty() ? NULL : _wfopen(gArgs.soundDump.c_str(), L"wb");
	if (dump && pBurnSoundOut) fwrite(pBurnSoundOut, 4, nBurnSoundLen, dump);	// what this frame played; compared against the peer in tests
	if (gSession) ggpo_advance_frame(gSession);
	gFrame++;
	if (gArgs.frames && gFrame >= gArgs.frames) DnEnd("frames");
}

// --delay auto: play at 0, and once a few seconds of ping are in, take the smallest delay that keeps rollbacks
// within DN_ROLLBACK_BUDGET frames: 0 for nearby players, more across oceans, instead of one number for everyone.
// ponytail: chosen once per session; re-tuning mid-match (GGPO allows it) waits for real-world ping data.
enum { DN_ROLLBACK_BUDGET = 4, DN_AUTO_SAMPLES = 3, DN_MAX_AUTO_DELAY = 6 };
static int gAutoPings[DN_AUTO_SAMPLES], gAutoCount = 0;

static int DnAutoDelay(int pingMs)
{
	int oneWayFrames = (pingMs * 60 / 2 + 999) / 1000;		// ping is the round trip; 60 frames a second
	int d = oneWayFrames - DN_ROLLBACK_BUDGET;
	return d < 0 ? 0 : d > DN_MAX_AUTO_DELAY ? DN_MAX_AUTO_DELAY : d;
}

void DnNetIdle()
{
	if (DnIpcPoll()) DnEnd("quit");
	if (!gSession) return;
	ggpo_idle(gSession, 0);

	DWORD now = GetTickCount();
	if (now - gLastStats < 1000) return;
	gLastStats = now;
	int worstPing = -1;		// 3-4 players: the farthest one decides
	for (int p = 0; p < gPlayers; p++) {
		if (gHandle[p] == gLocal) continue;
		GGPONetworkStats st;
		if (!GGPO_SUCCEEDED(ggpo_get_network_stats(gSession, gHandle[p], &st))) continue;
		if (st.network.ping > worstPing) worstPing = st.network.ping;
		char j[192];
		sprintf(j, "{\"ev\":\"stats\",\"player\":%d,\"ping\":%d,\"framesAhead\":%d,\"rollbackFrames\":%d,\"held\":%d,\"stalled\":%d,\"frame\":%d}",
			p + 1, st.network.ping, st.timesync.remote_frames_behind, gRollbackFrames, gHeldFrames, gStalledFrames, gFrame);
		DnEmit(j);
	}
	gRollbackFrames = 0;
	gHeldFrames = gStalledFrames = 0;

	if (gArgs.delay < 0 && gAutoCount < DN_AUTO_SAMPLES && worstPing >= 0) {
		gAutoPings[gAutoCount++] = worstPing;
		if (gAutoCount == DN_AUTO_SAMPLES) {
			std::sort(gAutoPings, gAutoPings + DN_AUTO_SAMPLES);
			int ping = gAutoPings[DN_AUTO_SAMPLES / 2], d = DnAutoDelay(ping);
			ggpo_set_frame_delay(gSession, gLocal, d);
			char j[96];
			sprintf(j, "{\"ev\":\"delay\",\"frames\":%d,\"auto\":true,\"ping\":%d}", d, ping);
			DnEmit(j);
		}
	}
}

int DnShutdown()
{
	if (gSession) {
		ggpo_close_session(gSession);
		gSession = NULL;
	}
	DnEmitDrain(1000);	// let the writer thread send "ended"/"error" before the process goes away
	return gExitCode;
}

// dn:pads - no game: the lobby's key settings listen to gamepads through this (DirectInput lives here, not in
// the lobby). One line per press: {"ev":"pad","code":16513}, an FBNeo joystick code (0x4000 | pad << 8 | control).
static int DnPads()
{
	DnIpcStart(NULL);
	if (InputInit()) {
		DnEmitError("input_failed", "DirectInput init failed");
		DnEmitDrain(500);
		return DN_EXIT_OTHER;
	}
	DnEmit("{\"ev\":\"ready\"}");
	InputFind(3);			// poll, and take the resting axes (sticks, triggers) as the baseline
	int last = -1;
	while (!DnIpcPoll() && !DnStdinClosed()) {
		InputFind(1);		// poll
		int code = InputFind(4);
		if (code < 0x4000 || code >= 0x8000) code = -1;		// keyboard: the lobby page reads it; mouse: clicks on the panel
		if (code >= 0 && code != last) {
			char j[64];
			sprintf(j, "{\"ev\":\"pad\",\"code\":%d}", code);
			DnEmit(j);
		}
		last = code;
		Sleep(10);
	}
	InputExit();
	DnEmitDrain(500);
	return DN_EXIT_OK;
}

bool DnTryMain(const wchar_t* cmdline, int& exitCode)
{
	if (wcsncmp(cmdline, L"dn:", 3) != 0) return false;
	if (wcsncmp(cmdline, L"dn:pads", 7) == 0 && (cmdline[7] == 0 || cmdline[7] == L' ')) {
		exitCode = DnPads();
		return true;
	}

	DnArgs a;
	std::string err;
	if (!DnParseArgs(cmdline, a, err)) {
		DnEmitError("bad_args", err.c_str());
		exitCode = DN_EXIT_OTHER;
		return true;
	}
	if (a.mode == "p2p") {	// runs inside the normal app (main.cpp calls DnP2PLoad)
		gArgs = a;
		bDnNet = true;
		return false;
	}
	exitCode = DnRun(a);
	return true;
}
