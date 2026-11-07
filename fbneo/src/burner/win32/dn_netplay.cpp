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
static DnArgs gArgs;
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
// Seats: participant p (room seat order) drives game player gSeat[p]. A word asks to move with bit 26 and the wanted player
// (0-3) in bits 24-25. Every PC applies the requests in participant order on the same frame and keeps the table in the saved
// state, so a move survives rollbacks the same everywhere. Inputs use bits 0-23 at most.
enum { DN_SEAT_SHIFT = 24, DN_SEAT_FLAG = 1u << 26, DN_SEAT_BITS = 7u << 24, DN_MAX_INPUTS = 24, DN_SEAT_WANT_FRAMES = 120 };
static int gSlots = 2;						// the game's player numbers
static UINT8 gSeat[GGPO_MAX_PLAYERS] = { 0, 1, 2, 3 };
static UINT8 gSeatShown[GGPO_MAX_PLAYERS] = { 0xFF, 0xFF, 0xFF, 0xFF };
static int gSeatWant = -1, gSeatWantFrames = 0;	// this PC's own request, held until it takes or times out
// Asking for a seat someone holds waits for their yes (10 s); a yes is held long enough to meet the ask (2 s).
enum { DN_SEAT_ASK_FRAMES = 600, DN_SEAT_YES_FRAMES = 120 };
static int gAsk[GGPO_MAX_PLAYERS] = { -1, -1, -1, -1 }, gAskShown[GGPO_MAX_PLAYERS] = { -2, -2, -2, -2 };	// seat each room player is asking for
static bool gSwapAsked = false;			// a fighting game: an agreed swap, reported once, for the lobby to restart with
static UINT8 gSwapNext[GGPO_MAX_PLAYERS];
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
		if (p >= 0 && p < players && gPlayerInp[p].size() < DN_MAX_INPUTS) gPlayerInp[p].push_back(i);
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
	int want[GGPO_MAX_PLAYERS];
	for (int p = 0; p < gPlayers; p++) {
		want[p] = (in[p] & DN_SEAT_FLAG) ? (int)((in[p] >> DN_SEAT_SHIFT) & 3) : -1;
		if (want[p] >= gSlots || want[p] == gSeat[p]) want[p] = -1;
	}
	// Both asking for each other's seat on the same frame is the holder's yes: they swap. A fighting game doesn't swap in
	// play: it reports the swap once and the lobby starts the match again with it.
	for (int p = 0; p < gPlayers; p++) {
		for (int q = p + 1; q < gPlayers; q++) {
			if (want[p] < 0 || want[p] != gSeat[q] || want[q] != gSeat[p]) continue;
			if (gArgs.seatRestart) {
				if (!gSwapAsked) { memcpy(gSwapNext, gSeat, sizeof(gSeat)); gSwapNext[p] = gSeat[q]; gSwapNext[q] = gSeat[p]; gSwapAsked = true; }
			} else {
				const UINT8 t = gSeat[p]; gSeat[p] = gSeat[q]; gSeat[q] = t;
			}
			want[p] = want[q] = -1;
		}
	}
	// A free seat is taken at once, in room order (co-op games; a fighting game has every seat filled)
	for (int p = 0; p < gPlayers; p++) {
		bool taken = want[p] < 0 || gArgs.seatRestart;
		for (int q = 0; q < gPlayers; q++) taken = taken || gSeat[q] == want[p];
		if (!taken) { gSeat[p] = (UINT8)want[p]; want[p] = -1; }
	}
	for (int p = 0; p < gPlayers; p++) gAsk[p] = want[p];	// what is left: asking for a seat someone holds
	for (int s = 0; s < gSlots; s++) {		// a free seat presses nothing
		for (size_t k = 0; k < gPlayerInp[s].size(); k++) { BurnDrvGetInputInfo(&bii, gPlayerInp[s][k]); *bii.pVal = 0; }
	}
	for (int p = 0; p < gPlayers; p++) {
		const int s = gSeat[p];
		for (size_t k = 0; k < gPlayerInp[s].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[s][k]);
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

// Start = coin + start: players never need a coin key or a joystick button for it (plan ÃÂ§4.6). One press runs this:
// coin held long enough for the game to count it, a wait (SF2 ignores start until its title screen is up, over 1 s), then start.
// ponytail: a press in the middle of a fight also puts in a coin. Knowing when a game wants one takes per-game RAM addresses.
enum { DN_ROLE_START = 4, DN_COIN_FRAMES = 6, DN_START_AT = 90, DN_START_END = 96 };
static uint32_t gCoinMask = 0;		// the local player's coin bit(s)
static int gStartStep = -1;
static bool gStartWasDown = false;
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

	const bool startMakesCoin = gKeymapSet && !gArgs.freeCoin && !gArgs.practice;	// practice: the game's own start, no coins
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
	return gScanLen + (int)sizeof(UINT32) + (int)sizeof(gSeat);
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
	memcpy(buf + sizeof(UINT32), gSeat, sizeof(gSeat));
	gScanPtr = buf + sizeof(UINT32) + sizeof(gSeat);
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
	memcpy(gSeat, buffer + sizeof(UINT32), sizeof(gSeat));
	gScanPtr = buffer + sizeof(UINT32) + sizeof(gSeat);
	BurnAcb = DnWriteAcb;
	BurnAreaScan(ACB_FULLSCAN | ACB_NET_OPT | ACB_WRITE, NULL);
	return true;
}

static void __cdecl cbFree(void* buffer) { free(buffer); }

static void DnEmitAsks()
{
	if (!memcmp(gAsk, gAskShown, sizeof(int) * gPlayers)) return;
	memcpy(gAskShown, gAsk, sizeof(int) * gPlayers);
	char j[200];
	int n = sprintf(j, "{\"ev\":\"asks\",\"asks\":[");
	bool first = true;
	for (int p = 0; p < gPlayers; p++) {
		if (gAsk[p] < 0) continue;
		n += sprintf(j + n, "%s{\"from\":%d,\"to\":%d}", first ? "" : ",", gSeat[p] + 1, gAsk[p] + 1);
		first = false;
	}
	sprintf(j + n, "]}");
	DnEmit(j);
}

static void DnEmitSeats()
{
	static bool swapShown = false;
	if (gSwapAsked && !swapShown) {
		swapShown = true;
		char j[80];
		int n = sprintf(j, "{\"ev\":\"swap\",\"seats\":[");
		for (int p = 0; p < gPlayers; p++) n += sprintf(j + n, p ? ",%d" : "%d", gSwapNext[p] + 1);
		sprintf(j + n, "]}");
		DnEmit(j);
	}
	DnEmitAsks();
	if (!memcmp(gSeat, gSeatShown, gPlayers)) return;
	memcpy(gSeatShown, gSeat, gPlayers);
	char j[96];
	int n = sprintf(j, "{\"ev\":\"seats\",\"slots\":%d,\"seats\":[", gSlots);
	for (int p = 0; p < gPlayers; p++) n += sprintf(j + n, p ? ",%d" : "%d", gSeat[p] + 1);
	sprintf(j + n, "]}");
	DnEmit(j);
}

void DnSetSeat(const std::string& line)	// {"cmd":"seat","to":N}: move to player N if it is free
{
	std::vector<int> v = DnJsonInts(line, "to");
	if (v.size() == 1 && v[0] == 0) { gSeatWant = -1; return; }	// "to":0 takes the ask back
	if (v.size() != 1 || v[0] < 1 || v[0] > gSlots) return;
	gSeatWant = v[0] - 1;
	gSeatWantFrames = DN_SEAT_ASK_FRAMES;
	const int me = gArgs.me > 0 ? gArgs.me - 1 : 0;
	for (int q = 0; q < gPlayers; q++) {
		if (gSeat[q] == gSeatWant && gAsk[q] == gSeat[me]) gSeatWantFrames = DN_SEAT_YES_FRAMES;	// a yes to their ask
	}
}

static uint32_t DnWithSeatRequest(uint32_t local)
{
	local &= ~DN_SEAT_BITS;
	if (gSeatWant < 0) return local;
	if (gSeat[gArgs.me > 0 ? gArgs.me - 1 : 0] == gSeatWant || --gSeatWantFrames < 0) { gSeatWant = -1; return local; }
	return local | DN_SEAT_FLAG | ((uint32_t)gSeatWant << DN_SEAT_SHIFT);
}
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
			DnEmitSeats();
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

// SF2 Dash stage edit, in memory only: everyone keeps the standard game files, and nothing of the game goes in our source.
// Graphics are not part of the game's state or logic, so this can't affect a match.
// Zangief's stage: the fence, the lever and the chain ends stand in front of the fighters and hide their feet. They are
// 8x8 tiles on the top layer, used nowhere else in a match, and the floor is whole behind them.
extern UINT8* CpsGfx;
extern UINT32 nCpsGfxLen;
static int gStageTiles = 0;

static void DnStageEdits()
{
	if (gArgs.game != "sf2ce") return;
	static const struct { UINT32 from, to; } kFront[] = {
		{ 0x4284, 0x428a }, { 0x4294, 0x429a }, { 0x429e, 0x429f }, { 0x42a4, 0x42aa }, { 0x42ae, 0x42af }, { 0x42b4, 0x42b9 }, { 0x42be, 0x42bf },
		{ 0x42c5, 0x42c8 }, { 0x42ce, 0x42cf }, { 0x42d5, 0x42d8 }, { 0x42de, 0x42e0 }, { 0x42ed, 0x42f1 }, { 0x42fd, 0x430f }, { 0x4313, 0x4336 },
		{ 0x4338, 0x4346 }, { 0x4348, 0x4355 }, { 0x4358, 0x4365 }, { 0x4368, 0x4375 }, { 0x4378, 0x437f }, { 0x4446, 0x444a }, { 0x444c, 0x444f },
		{ 0x4456, 0x445a }, { 0x445c, 0x445f }, { 0x446c, 0x446f }, { 0x447c, 0x447f },
	};
	for (size_t i = 0; i < sizeof(kFront) / sizeof(kFront[0]); i++) {
		for (UINT32 t = kFront[i].from; t <= kFront[i].to; t++) {
			UINT32 at = (t + 0x10000) << 6;
			if (at + 64 <= nCpsGfxLen) { memset(CpsGfx + at, 0, 64); gStageTiles++; }
		}
	}
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

	DnStageEdits();
	InpDIPSWResetDIPs();	// driver default DIPs on every peer, whatever the local config says
	DnApplyConstants();
	gPlayers = a.players;
	gSlots = BurnDrvGetMaxPlayers();
	if (gSlots < gPlayers) gSlots = gPlayers;
	if (gSlots > GGPO_MAX_PLAYERS) gSlots = GGPO_MAX_PLAYERS;
	if ((int)a.seats.size() == gPlayers) {
		bool ok = true;
		for (int p = 0; p < gPlayers; p++) for (int q = 0; q < p; q++) ok = ok && a.seats[p] != a.seats[q];
		for (int p = 0; ok && p < gPlayers; p++) ok = a.seats[p] >= 1 && a.seats[p] <= gSlots;
		for (int p = 0; ok && p < gPlayers; p++) gSeat[p] = (UINT8)(a.seats[p] - 1);
	}
	DnBuildInputMap(gSlots);
	return 0;
}

static int DnPracticeTest(const DnArgs& a);

int DnRun(const DnArgs& a)
{
	bNoPopups = true;
	bQuietLoading = true;
	bDontInitMedia = true;

	BurnLibInit();
	ConfigAppLoad();

	int rc = DnLoadGame(a);
	if (rc == 0) {
		rc = a.mode == "practice" ? DnPracticeTest(a) : DnSyncTest(a);
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
	{ "kof98", 0xD00034 },
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

	// Sound: WASAPI with 4 frames of buffer (~67 ms) instead of DirectSound's 6 (~100 ms); hits are heard sooner.
	// MediaInit() falls back to DirectSound if WASAPI can't start. Before the game loads: its sound chips are set up for
	// this rate, and running them at another one writes past their buffers. One fixed rate, whatever the settings file
	// or the fallback, because the rate is part of what rollback saves: every peer needs the same one.
	nAudSelect = 2;
	nAudSegCount = 4;
	nAudSampleRate[0] = nAudSampleRate[2] = 48000;

	if ((gExitCode = DnLoadGame(gArgs)) != 0) return 1;

	gAlone = gPlayers == 1 || gArgs.practice;		// alone in a room, or practising: no peer and no rollback, so no GGPO (it can't run a session of one)
	if (gArgs.practice) DnSetPractice(gArgs.practiceSet);
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

	bAlwaysDrawFrames = 1;	// two frames due at one sound wakeup: show both, on schedule (RunPacePresent), instead of dropping the first
	gLastStats = GetTickCount();

	INT32 w = 0, h = 0;
	BurnDrvGetVisibleSize(&w, &h);
	char j[64];
	if (gStageTiles) { char e[64]; sprintf(e, "{\"ev\":\"stage-edits\",\"tiles\":%d}", gStageTiles); DnEmit(e); }
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
		v = XorShift(gAutoRng) & DN_GAME_BITS & ~DN_SEAT_BITS & (gAutoHuman ? ~gCoinMask : ~0u);
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
// skip: both press button 1 to leave a scene shown after the starts (KOF '98's how to play), or 0.
static const struct { const char* game; int coin1, coin2, start1, start2, skip, end; } kFastStart[] = {
	{ "sf2ce", 900, 915, 1000, 1060, 0, 1120 },
	{ "kof98", 720, 740, 820, 880, 960, 1000 },
	{ "wofj", 99999, 99999, 99999, 99999, 0, 500 },	// co-op: boot and warning skipped, on the title; everyone joins with their own start
	{ "machbrkr", 99999, 99999, 99999, 99999, 0, 150 },	// co-op: only the self test is skipped; everyone joins with their own start (coin + start)
};

// KOF '98's own PRACTICE, only in its home version: the standard file on the home BIOS (AES World: English). This opens on team select.
extern UINT8 NeoSystem, NeoReset;
enum { DN_BIOS_MASK = 0x3F, DN_BIOS_AES_WORLD = 0x10 };
static void DnKofPractice()
{
	NeoSystem = (NeoSystem & ~DN_BIOS_MASK) | DN_BIOS_AES_WORLD;
	NeoReset = 1;
	uint32_t startBit = 0, downBit = 0, fireBit = 0;
	struct BurnInputInfo bii;
	for (size_t k = 0; k < gPlayerInp[0].size(); k++) {
		BurnDrvGetInputInfo(&bii, gPlayerInp[0][k]);
		if (!bii.szInfo) continue;
		if (!_stricmp(bii.szInfo, "p1 start")) startBit = 1u << k;
		if (!_stricmp(bii.szInfo, "p1 down")) downBit = 1u << k;
		if (!_stricmp(bii.szInfo, "p1 fire 1")) fireBit = 1u << k;
	}
	for (int f = 0; f < 2160; f++) {
		uint32_t in = 0;
		if (f >= 1100 && f < 1106) in |= startBit;
		for (int d = 0; d < 5; d++) if (f >= 1300 + d * 40 && f < 1306 + d * 40) in |= downBit;		// the menu's sixth line
		if ((f >= 1600 && f < 1606) || (f >= 2000 && f < 2006)) in |= fireBit;
		DnApplyInputs(&in);
		DnStepFrame(false);
	}
	DnEmit("{\"ev\":\"faststart\",\"practice\":true}");
}

static void DnFastStart()
{
	if (gArgs.practice && gArgs.game == "kof98") { DnKofPractice(); return; }
	int coin1 = 0, coin2 = 0, start1 = 0, start2 = 0, skip = 0, end = 0;
	for (size_t i = 0; i < sizeof(kFastStart) / sizeof(kFastStart[0]); i++) {
		if (gArgs.game == kFastStart[i].game) {
			coin1 = kFastStart[i].coin1; coin2 = kFastStart[i].coin2; start1 = kFastStart[i].start1; start2 = kFastStart[i].start2; skip = kFastStart[i].skip; end = kFastStart[i].end;
		}
	}
	if (!end) return;
	uint32_t coinBits[2] = { 0, 0 }, startBits[2] = { 0, 0 }, fireBits[2] = { 0, 0 };
	struct BurnInputInfo bii;
	for (int p = 0; p < 2 && p < gPlayers; p++) {
		for (size_t k = 0; k < gPlayerInp[p].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[p][k]);
			size_t n = bii.szInfo ? strlen(bii.szInfo) : 0;
			if (n >= 5 && !_stricmp(bii.szInfo + n - 5, " coin")) coinBits[p] |= 1u << k;
			if (n >= 6 && !_stricmp(bii.szInfo + n - 6, " start")) startBits[p] |= 1u << k;
			if (n >= 7 && !_stricmp(bii.szInfo + n - 7, " fire 1")) fireBits[p] |= 1u << k;
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
		if (skip && f >= skip && f < skip + 6) { in[0] |= fireBits[0]; in[1] |= fireBits[1]; }
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

// ---------------------------------------------------------------- practice
// SF2 Dash alone: the other fighter follows the panel's settings by pressing buttons, as a player would. Everyone keeps the
// standard game file. Only practice writes to the game's memory, and it never runs in a match (no peer, no rollback).
// Addresses found by recording every frame of a scripted fight and comparing.
enum { PR_P1 = 0xFF83BE, PR_P2 = 0xFF86BE, PR_TIMER = 0xFF8ABE, PR_RAM = 0xFF0000 };
enum { PR_STATE = 0x03, PR_X = 0x06, PR_HP = 0x2A, PR_HP_LATER = 0x2C, PR_HP_BAR = 0x1BD, PR_STUN_TIME = 0x5D, PR_STUN = 0x5F, PR_BALL = 0x1D4, PR_FIGHTER = 0x291, PR_PICKED = 0x2B5 };
enum { ST_STAND = 0x00, ST_CROUCH = 0x02, ST_JUMP = 0x04, ST_BLOCK = 0x08, ST_ATTACK = 0x0A, ST_SPECIAL = 0x0C, ST_HURT = 0x0E, ST_THROWN = 0x14 };
enum { MOVE_STAND, MOVE_FORWARD, MOVE_BACK, MOVE_JUMP_FORWARD, MOVE_JUMP_BACK, MOVE_JUMP, MOVE_CROUCH };
enum { GUARD_OFF, GUARD_ON, GUARD_STEP, GUARD_REVERSAL };
enum { PR_FULL = 0x90, PR_FLOOR = 0x48, PR_SETTLE = 20, PR_CALM = 10, PR_QUIET = 30, PR_ATTACK_RANGE = 190, PR_JUMP_RANGE = 130, PR_BALL_RANGE = 110 };
enum { R_UP = 1 << 0, R_DOWN = 1 << 1, R_LEFT = 1 << 2, R_RIGHT = 1 << 3, R_LP = 1 << 5, R_MP = 1 << 6, R_HP = 1 << 7, R_LK = 1 << 8, R_MK = 1 << 9 };
// The back-charge moves first asked for lose their charge: back has to be let go or the first hit is blocked. These are their stand-ins.
enum { REV_PRESS, REV_UPPERCUT, REV_CHARGE_DOWN, REV_MASH };
static const struct { int kind; int buttons; } kReversal[12] = {
	{ REV_UPPERCUT, R_LP },				// Ryu
	{ REV_MASH, R_LP },					// E. Honda: light hundred hand slap
	{ REV_MASH, R_LP },					// Blanka: light electric thunder
	{ REV_CHARGE_DOWN, R_LK },			// Guile: light flash kick
	{ REV_UPPERCUT, R_LP },				// Ken
	{ REV_MASH, R_LK },					// Chun-Li: light lightning legs
	{ REV_PRESS, R_LP | R_MP | R_HP },	// Zangief: double lariat
	{ REV_PRESS, R_HP },				// Dhalsim: standing strong punch
	{ REV_PRESS, R_MK },				// M. Bison: standing medium kick
	{ REV_UPPERCUT, R_LP },				// Sagat: light tiger uppercut
	{ REV_PRESS, R_HP },				// Balrog: standing strong punch
	{ REV_PRESS, R_MP },				// Vega: standing medium punch
};
enum { WRITE_TIME = 1, WRITE_HEALTH = 2, WRITE_STUN = 4 };
static int gPrMove = MOVE_STAND, gPrGuard = GUARD_OFF, gPrStun = 1, gPrWrites = WRITE_TIME | WRITE_HEALTH | WRITE_STUN;	// writes: tests turn parts off
static uint32_t gPrBit[DN_ROLES];
static int gPrShowHp = 0, gPrShowStun = 0, gPrBoxes = 0, gPrBoxSide = 1;
static int gPrTestHp = 0;	// tests: what the dummy's health is refilled to, instead of full	// readouts and boxes drawn on the picture
// 1 STEP: 0 = waiting for the first hit, 1 = it landed; back to 0 once the fighter has been free for PR_QUIET frames.
static int gPrStep = 0, gPrQuiet = 0, gPrFired = 0;
static int gPrHits = 0, gPrDamage = 0, gPrLastHp = PR_FULL, gPrLastHp1 = PR_FULL, gPrSettle[2] = { 0, 0 }, gPrCalm[2] = { 0, 0 };
static bool gPrReported = false;

void DnSetPractice(const std::string& line)
{
	std::vector<int> v;
	if ((v = DnJsonInts(line, "move")).size() == 1 && v[0] >= MOVE_STAND && v[0] <= MOVE_CROUCH) gPrMove = v[0];
	if ((v = DnJsonInts(line, "guard")).size() == 1 && v[0] >= GUARD_OFF && v[0] <= GUARD_REVERSAL) { gPrGuard = v[0]; gPrStep = gPrFired = 0; }
	if ((v = DnJsonInts(line, "stun")).size() == 1) gPrStun = v[0] != 0;
	if ((v = DnJsonInts(line, "writes")).size() == 1) gPrWrites = v[0];
	if ((v = DnJsonInts(line, "hpnum")).size() == 1) gPrShowHp = v[0] != 0;
	if ((v = DnJsonInts(line, "testhp")).size() == 1) gPrTestHp = v[0];
	if ((v = DnJsonInts(line, "stunnum")).size() == 1) gPrShowStun = v[0] != 0;
	if ((v = DnJsonInts(line, "boxes")).size() == 1) gPrBoxes = v[0];
	if ((v = DnJsonInts(line, "boxside")).size() == 1 && (v[0] == 1 || v[0] == 2)) gPrBoxSide = v[0];
}

static int PrByte(UINT32 a) { return (int)ReadValueAtHardwareAddress(a, 1, 0); }
static int PrWord(UINT32 a) { return (int)ReadValueAtHardwareAddress(a, 2, 0); }

// Readouts drawn on the finished picture (VidFrameCallback): what the screen shows, never the game. A 5x7 font, one bit a pixel.
static const char kPrGlyphs[] = "0123456789HPSTUN/";
static const UINT8 kPrFont[17][7] = {
	{ 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 }, { 31, 2, 4, 2, 1, 17, 14 },
	{ 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 }, { 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
	{ 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 }, { 17, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 16, 16, 16 },
	{ 15, 16, 16, 14, 1, 1, 30 }, { 31, 4, 4, 4, 4, 4, 4 }, { 17, 17, 17, 17, 17, 17, 14 }, { 17, 25, 21, 19, 17, 17, 17 },
	{ 1, 1, 2, 4, 8, 16, 16 },
};
static void PrPixel(UINT8* img, int w, int h, int bpp, int pitch, int x, int y, UINT32 c)
{
	if (x < 0 || y < 0 || x >= w || y >= h) return;
	UINT8* p = img + y * pitch + x * bpp;
	if (bpp == 2) *(UINT16*)p = (UINT16)c;
	else if (bpp == 3) { p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF; }
	else *(UINT32*)p = c;
}
static void PrText(UINT8* img, int w, int h, int bpp, int pitch, int x, int y, const char* s, UINT32 ink, UINT32 shade)
{
	for (int pass = 0; pass < 2; pass++) {
		int cx = x;
		for (const char* q = s; *q; q++, cx += 6) {
			const char* g = strchr(kPrGlyphs, *q);
			if (!g || !*q) continue;
			const UINT8* rows = kPrFont[g - kPrGlyphs];
			for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++) if ((rows[r] >> (4 - b)) & 1) {
				if (pass == 0) { PrPixel(img, w, h, bpp, pitch, cx + b + 1, y + r, shade); PrPixel(img, w, h, bpp, pitch, cx + b, y + r + 1, shade); PrPixel(img, w, h, bpp, pitch, cx + b + 1, y + r + 1, shade); }
				else PrPixel(img, w, h, bpp, pitch, cx + b, y + r, ink);
			}
		}
	}
}
static int PrLong(UINT32 a) { return (int)ReadValueAtHardwareAddress(a, 4, 0); }
static void PrRect(UINT8* img, int w, int h, int bpp, int pitch, int x0, int y0, int x1, int y1, UINT32 c)
{
	for (int x = x0; x <= x1; x++) { PrPixel(img, w, h, bpp, pitch, x, y0, c); PrPixel(img, w, h, bpp, pitch, x, y1, c); }
	for (int y = y0; y <= y1; y++) { PrPixel(img, w, h, bpp, pitch, x0, y, c); PrPixel(img, w, h, bpp, pitch, x1, y, c); }
}
enum { PR_BAR_LEFT = 32, PR_BAR_RIGHT = 352, PR_HP_TEXT_Y = 12, PR_STUN_Y = 47, PR_STUN_BAR = 48, PR_DIZZY = 31, PR_STUN_BAR_TIME = 180 };
enum { PR_CAMERA_X = 0xFF8BC4, PR_CAMERA_Y = 0xFF8BC8, PR_Y = 0x0A, PR_FLIP = 0x12, PR_ANIM = 0x1A, PR_BOXTAB = 0x34 };
// Boxes from the game's own tables: the frame of animation (PR_ANIM) names a box in each table at +8.., the fighter's table list
// (PR_BOXTAB) gives where each table starts. Tables 0-2 are what can be hit (head, body, legs), 4 is the attack (12 bytes an entry),
// 5 is the body that pushes. Table 3 is left out: two entries, and nothing seen using it. Each box: centre x, y from the fighter's
// point (x mirrored by the facing), half width, half height. A fireball is an object of the same shape.
enum { BOX_ATTACK = 1, BOX_HURT = 2, BOX_BALL_ATTACK = 4, BOX_BALL_HURT = 8, BOX_PUSH = 16, BOX_PIVOT = 32, PR_GROUND = 233 };
static void DnPracticeBoxesOf(UINT8* img, int w, int h, int bpp, int pitch, UINT32 base, bool ball)
{
	const int camx = PrWord(PR_CAMERA_X), camy = PrWord(PR_CAMERA_Y);
	const int px = PrWord(base + PR_X) - camx, py = PrWord(base + PR_Y) - camy;
	const int flip = PrByte(base + PR_FLIP);
	const UINT32 anim = (UINT32)PrLong(base + PR_ANIM), tab = (UINT32)PrLong(base + PR_BOXTAB);
	for (int k = 0; k < 6; k++) {
		const int show = k == 4 ? (ball ? BOX_BALL_ATTACK : BOX_ATTACK) : k == 5 ? (ball ? 0 : BOX_PUSH) : k == 3 ? 0 : (ball ? BOX_BALL_HURT : BOX_HURT);
		const int id = show & gPrBoxes ? PrByte(anim + 8 + k) : 0;
		if (!id) continue;
		const UINT32 e = tab + (UINT32)PrWord(tab + k * 2) + id * (k == 4 ? 12 : 4);
		const int bx = (INT8)PrByte(e), by = (INT8)PrByte(e + 1), rw = PrByte(e + 2), rh = PrByte(e + 3);
		const int cx = px + (flip ? -bx : bx), cy = PR_GROUND - (py + by);
		const UINT32 c = show == BOX_ATTACK ? BurnHighCol(255, 70, 70, 0) : show == BOX_HURT ? BurnHighCol(90, 150, 255, 0) : show == BOX_BALL_ATTACK ? BurnHighCol(255, 110, 230, 0) : show == BOX_BALL_HURT ? BurnHighCol(80, 230, 255, 0) : BurnHighCol(90, 230, 110, 0);
		PrRect(img, w, h, bpp, pitch, cx - rw, cy - rh, cx + rw, cy + rh, c);
	}
	if (!ball && (gPrBoxes & BOX_PIVOT)) {
		const int gy = PR_GROUND - py;
		for (int d = -3; d <= 3; d++) { PrPixel(img, w, h, bpp, pitch, px + d, gy, BurnHighCol(255, 255, 255, 0)); PrPixel(img, w, h, bpp, pitch, px, gy + d, BurnHighCol(255, 255, 255, 0)); }
	}
}
void DnPracticeOverlay(UINT8* img, int w, int h, int bpp, int pitch)
{
	if (!gArgs.practice || gArgs.game != "sf2ce" || !img || !PrWord(PR_P2 + PR_HP)) return;
	if (gPrBoxes) {		// the chosen side's fighter and its fireball
		const UINT32 base = gPrBoxSide == 2 ? PR_P2 : PR_P1, ball = (UINT32)PrWord(base + PR_BALL);
		DnPracticeBoxesOf(img, w, h, bpp, pitch, base, false);
		if (ball) DnPracticeBoxesOf(img, w, h, bpp, pitch, PR_RAM + ball, true);
	}
	if (!gPrShowHp && !gPrShowStun) return;
	const UINT32 ink = BurnHighCol(0xFF, 0xFF, 0xFF, 0), shade = BurnHighCol(0, 0, 0, 0), well = BurnHighCol(0x22, 0x22, 0x22, 0);
	const UINT32 red = BurnHighCol(0xE8, 0x38, 0x2F, 0), amber = BurnHighCol(0xF0, 0xA0, 0x20, 0);
	char t[24];
	for (int p = 0; p < 2; p++) {
		const UINT32 base = p ? PR_P2 : PR_P1;
		if (gPrShowHp) {		// above the health bar, under the score line
			sprintf(t, "HP %d", PrWord(base + PR_HP));
			PrText(img, w, h, bpp, pitch, p ? PR_BAR_RIGHT - ((int)strlen(t) * 6 - 1) : PR_BAR_LEFT, PR_HP_TEXT_Y, t, ink, shade);
		}
		if (!gPrShowStun) continue;
		// Under the name: the stun meter out of PR_DIZZY (31: every fighter held 30 and got dizzy at 31), then the frames until it
		// empties (each hit adds 130; the bar is full at PR_STUN_BAR_TIME, the number keeps counting past it). 1P fills from the left, 2P from the right.
		const int values[2] = { PrByte(base + PR_STUN), PrByte(base + PR_STUN_TIME) }, full[2] = { PR_DIZZY, PR_STUN_BAR_TIME };
		for (int row = 0; row < 2; row++) {
			const int y = PR_STUN_Y + row * 9, x = p ? w - 26 - PR_STUN_BAR : 26;
			const int fill = (PR_STUN_BAR - 2) * (values[row] < full[row] ? values[row] : full[row]) / full[row];
			for (int yy = y - 1; yy <= y + 5; yy++) for (int xx = x - 1; xx <= x + PR_STUN_BAR; xx++) {
				const bool edge = yy == y - 1 || yy == y + 5 || xx == x - 1 || xx == x + PR_STUN_BAR;
				const bool frame = yy == y || yy == y + 4 || xx == x || xx == x + PR_STUN_BAR - 1;
				const int inner = xx - (x + 1);
				const bool filled = !frame && !edge && (p ? inner >= PR_STUN_BAR - 2 - fill : inner < fill);
				PrPixel(img, w, h, bpp, pitch, xx, yy, edge ? shade : frame ? ink : filled ? (row ? amber : red) : well);
			}
			if (row == 0) sprintf(t, "%03d/%03d", values[0], PR_DIZZY); else sprintf(t, "%03d", values[1]);
			const int tw = (int)strlen(t) * 6 - 1;
			PrText(img, w, h, bpp, pitch, p ? x - 4 - tw : x + PR_STUN_BAR + 4, y - 1, t, ink, shade);
		}
	}
}

static uint32_t DnPracticeDummy()
{
	const int s1 = PrByte(PR_P1 + PR_STATE), s2 = PrByte(PR_P2 + PR_STATE), x1 = PrWord(PR_P1 + PR_X), x2 = PrWord(PR_P2 + PR_X);
	const int back = x2 > x1 ? R_RIGHT : R_LEFT, forward = x2 > x1 ? R_LEFT : R_RIGHT;
	const bool free = s2 == ST_STAND || s2 == ST_CROUCH;
	const bool hurt = s2 == ST_HURT || s2 == ST_THROWN;
	const UINT32 ball = (UINT32)PrWord(PR_P1 + PR_BALL);
	const bool threat = ((s1 == ST_ATTACK || s1 == ST_SPECIAL) && abs(x1 - x2) < PR_ATTACK_RANGE)
		|| (s1 == ST_JUMP && abs(x1 - x2) < PR_JUMP_RANGE)
		|| (ball && abs(PrWord(PR_RAM + ball + PR_X) - x2) < PR_BALL_RANGE);

	if (hurt) { gPrStep = 1; gPrFired = 0; gPrReported = false; }	// a reversal after every hit
	gPrQuiet = free ? gPrQuiet + 1 : 0;
	if (gPrQuiet >= PR_QUIET) gPrStep = 0;

	if (gPrGuard == GUARD_REVERSAL) {
		const int rev = PrByte(PR_P2 + PR_FIGHTER) < 12 ? PrByte(PR_P2 + PR_FIGHTER) : 0;
		const int kind = kReversal[rev].kind, buttons = kReversal[rev].buttons;
				if (gPrStep == 0) {
			if (kind == REV_CHARGE_DOWN) return R_DOWN;
		} else if (!free && !gPrFired && s2 != ST_ATTACK && s2 != ST_SPECIAL) {
			static int phase = 0;
			phase++;
						if (kind == REV_CHARGE_DOWN) return R_DOWN;
			if (kind == REV_MASH) return phase & 1 ? buttons : 0;
			return 0;
		} else {
			gPrFired++;
			if (!gPrReported && (s2 == ST_ATTACK || s2 == ST_SPECIAL)) {
				gPrReported = true;
				char j[80];
				sprintf(j, "{\"ev\":\"reversal\",\"fighter\":%d,\"after\":%d}", rev, gPrFired);
				DnEmit(j);
			}
			if (kind == REV_MASH) return gPrFired < 30 && gPrFired & 1 ? buttons : 0;
			if (kind == REV_UPPERCUT) return gPrFired == 1 ? forward : gPrFired == 2 ? R_DOWN : gPrFired == 3 ? R_DOWN | forward : gPrFired < 9 ? R_DOWN | forward | buttons : 0;	// the game reads this motion only once free to act
			if (gPrFired > 6) return 0;
			if (kind == REV_CHARGE_DOWN) return gPrFired == 1 ? R_UP : R_UP | buttons;
			return buttons;
		}
	}

	// Crouching guard blocks every ground attack and never walks off when the attack is still out of reach; a jump-in needs a standing one.
	if ((gPrGuard == GUARD_ON || (gPrGuard == GUARD_STEP && gPrStep == 1)) && threat) return back | (s1 == ST_JUMP ? 0 : R_DOWN);
	switch (gPrMove) {
		case MOVE_FORWARD: return forward;
		case MOVE_BACK: return back;
		case MOVE_JUMP_FORWARD: return R_UP | forward;
		case MOVE_JUMP_BACK: return R_UP | back;
		case MOVE_JUMP: return R_UP;
		case MOVE_CROUCH: return R_DOWN;
	}
	return 0;
}

// Time never runs out. Health falls during a combo, so its damage can be seen, and is full again as soon as the fighter is free.
// PR_FLOOR keeps a long combo from ending the round: it is more than any one hit takes. The bar has its own number, which falls behind health.
// A fireball takes health some frames before the fighter reacts, and changing health in those frames cancels the reaction. So health is
// only refilled when nothing can still land: the fighter has been free for PR_CALM frames, the other one isn't attacking and has no
// fireball out, and PR_SETTLE frames have gone by since health last fell. The floor, which has to act in the middle of a combo, waits
// until the game has caught its own copy of health (PR_HP_LATER) up with the health a hit took: the game sizes the reaction from the
// gap between the two, so a floor write before that made a fireball crossing the floor land as a hit of no damage, with almost no
// push back.
static void DnPracticeHealth(UINT32 base, int hp)
{
	if (!(gPrWrites & WRITE_HEALTH)) return;
	WriteValueAtHardwareAddress(base + PR_HP, hp, 2, 0);
	WriteValueAtHardwareAddress(base + PR_HP_LATER, hp, 2, 0);
}

static void DnPracticeBody()
{
	const int s1 = PrByte(PR_P1 + PR_STATE), s2 = PrByte(PR_P2 + PR_STATE), hp1 = PrWord(PR_P1 + PR_HP), hp2 = PrWord(PR_P2 + PR_HP);
	if (hp2 == 0) return;		// no fight yet: the game sets health up when the round starts
	if (gPrWrites & WRITE_TIME) WriteValueAtHardwareAddress(PR_TIMER, 0x99, 1, 0);
	if (hp2 < gPrLastHp && s2 != ST_BLOCK) {
		gPrHits++;
		gPrDamage += gPrLastHp - hp2;
		char j[64];
		sprintf(j, "{\"ev\":\"combo\",\"hits\":%d,\"damage\":%d}", gPrHits, gPrDamage);
		DnEmit(j);
	}
	gPrSettle[1] = hp2 < gPrLastHp ? PR_SETTLE : gPrSettle[1] > 0 ? gPrSettle[1] - 1 : 0;
	gPrSettle[0] = hp1 < gPrLastHp1 ? PR_SETTLE : gPrSettle[0] > 0 ? gPrSettle[0] - 1 : 0;
	gPrLastHp = hp2;
	gPrLastHp1 = hp1;
	const bool free2 = s2 == ST_STAND || s2 == ST_CROUCH || s2 == ST_JUMP, free1 = s1 == ST_STAND || s1 == ST_CROUCH || s1 == ST_JUMP;
	const bool quiet1 = s1 != ST_ATTACK && s1 != ST_SPECIAL && !PrWord(PR_P1 + PR_BALL), quiet2 = s2 != ST_ATTACK && s2 != ST_SPECIAL && !PrWord(PR_P2 + PR_BALL);
	gPrCalm[1] = free2 && quiet1 ? gPrCalm[1] + 1 : 0;
	gPrCalm[0] = free1 && quiet2 ? gPrCalm[0] + 1 : 0;
	if (free2) gPrHits = gPrDamage = 0;
	if (gPrCalm[1] >= PR_CALM && !gPrSettle[1]) {
		const int full2 = gPrTestHp ? gPrTestHp : PR_FULL;
		if (hp2 != full2) {
			DnPracticeHealth(PR_P2, full2);
			if (gPrWrites & WRITE_HEALTH) WriteValueAtHardwareAddress(PR_P2 + PR_HP_BAR, full2, 1, 0);
			gPrLastHp = full2;
		}
	} else if (hp2 < PR_FLOOR && !free2 && PrWord(PR_P2 + PR_HP_LATER) == hp2) {
		DnPracticeHealth(PR_P2, PR_FLOOR);
		gPrLastHp = PR_FLOOR;
	}
	if (gPrCalm[0] >= PR_CALM && !gPrSettle[0] && hp1 != PR_FULL) {
		DnPracticeHealth(PR_P1, PR_FULL);
		if (gPrWrites & WRITE_HEALTH) WriteValueAtHardwareAddress(PR_P1 + PR_HP_BAR, PR_FULL, 1, 0);
		gPrLastHp1 = PR_FULL;
	} else if (hp1 < PR_FLOOR && !free1 && PrWord(PR_P1 + PR_HP_LATER) == hp1) {
		DnPracticeHealth(PR_P1, PR_FLOOR);
		gPrLastHp1 = PR_FLOOR;
	}
	if (!gPrStun && (gPrWrites & WRITE_STUN)) {
		for (UINT32 base = PR_P1; base <= PR_P2; base += PR_P2 - PR_P1) {
			WriteValueAtHardwareAddress(base + PR_STUN_TIME, 0, 1, 0);
			WriteValueAtHardwareAddress(base + PR_STUN, 0, 1, 0);
		}
	}
}

// The player's own presses for the panel's input list, by role (bit 0 up ... bit 10 fire 6), whenever they change. Start is left out.
static void DnPracticeInput(uint32_t local, int frame)
{
	static int last = -1;
	int roles = 0;
	for (size_t k = 0; k < gBitRole.size(); k++) if (gBitRole[k] >= 0 && gBitRole[k] != DN_ROLE_START && ((local >> k) & 1)) roles |= 1 << gBitRole[k];
	if (roles == last) return;
	last = roles;
	char j[64];
	sprintf(j, "{\"ev\":\"input\",\"f\":%d,\"r\":%d}", frame, roles);
	DnEmit(j);
}

static void DnPracticeFrame(uint32_t local, uint32_t* in)
{
	static bool mapped = false;
	if (!mapped) {
		mapped = true;
		struct BurnInputInfo bii;
		for (size_t k = 0; k < gPlayerInp[1].size(); k++) {
			BurnDrvGetInputInfo(&bii, gPlayerInp[1][k]);
			for (int r = 0; r < DN_ROLES && bii.szInfo && !_strnicmp(bii.szInfo, "p2 ", 3); r++) {
				if (!_stricmp(bii.szInfo + 3, kRoleInfo[r])) gPrBit[r] = 1u << k;
			}
		}
	}
	in[0] = local;
	in[1] = 0;
	if (!PrByte(PR_P1 + PR_PICKED)) return;		// the player picks their own fighter, then the other's
	if (!PrByte(PR_P2 + PR_PICKED)) {
		static bool released = false;		// the button that picked the first fighter must not pick the second too
		released = released || !(local & DN_GAME_BITS);
		in[0] = local & ~(uint32_t)DN_GAME_BITS;
		if (released) in[1] = local & DN_GAME_BITS;
		return;
	}
	DnPracticeBody();
	const uint32_t roles = DnPracticeDummy();
	for (int r = 0; r < DN_ROLES; r++) if ((roles >> r) & 1) in[1] |= gPrBit[r];
}

// --script: frame,length,roles;... the player's presses, for tests.
static uint32_t DnScriptInput(const std::string& script, int f)
{
	struct Press { int frame, len; uint32_t bits; };
	static std::vector<Press> presses;
	static bool parsed = false;
	if (!parsed) {
		parsed = true;
		for (size_t at = 0; at < script.size();) {
			size_t e = script.find(';', at);
			if (e == std::string::npos) e = script.size();
			int frame = 0, len = 0, roles = 0;
			if (sscanf(script.substr(at, e - at).c_str(), "%d,%d,%d", &frame, &len, &roles) == 3) {
				uint32_t bits = 0;
				for (size_t k = 0; k < gBitRole.size(); k++) if (gBitRole[k] >= 0 && ((roles >> gBitRole[k]) & 1)) bits |= 1u << k;
				presses.push_back({ frame, len, bits });
			}
			at = e + 1;
		}
	}
	uint32_t v = 0;
	for (size_t i = 0; i < presses.size(); i++) if (f >= presses[i].frame && f < presses[i].frame + presses[i].len) v |= presses[i].bits;
	return v;
}

// dn:practice: a whole session with no window, as fast as it goes: tests script the player and read what happened.
static int DnPracticeTest(const DnArgs& a)
{
	DnSetPractice(a.practiceSet);
	DnFastStart();
	int lastState = -1;
	for (int f = 0; f < a.frames; f++) {
		uint32_t local = DnScriptInput(a.script, f), in[GGPO_MAX_PLAYERS] = { 0 };
		DnPracticeInput(local, f);
		if (a.game == "sf2ce") DnPracticeFrame(local, in); else in[0] = local;
		DnApplyInputs(in);
		DnStepFrame(false);
		if (a.game != "sf2ce") continue;
		static FILE* dump = a.dump.empty() ? NULL : _wfopen(a.dump.c_str(), L"wb");
		if (dump) {		// both fighters, every frame, for tests that compare runs
			UINT8 b[0x600];
			for (int i = 0; i < 0x300; i++) { b[i] = (UINT8)PrByte(PR_P1 + i); b[0x300 + i] = (UINT8)PrByte(PR_P2 + i); }
			fwrite(b, 1, sizeof(b), dump);
		}
		static int lastPicked = -1;
		const int picked = PrByte(PR_P1 + PR_PICKED) + 2 * PrByte(PR_P2 + PR_PICKED);
		if (picked != lastPicked) { lastPicked = picked; char e[64]; sprintf(e, "{\"ev\":\"picked\",\"f\":%d,\"v\":%d}", f, picked); DnEmit(e); }
		int s2 = PrByte(PR_P2 + PR_STATE);
		if (s2 != lastState) {
			lastState = s2;
			char j[120];
			sprintf(j, "{\"ev\":\"state\",\"f\":%d,\"s\":%d,\"p1\":%d,\"p2\":%d,\"x\":%d,\"hp\":%d,\"stun\":%d,\"time\":%d}", f, s2, PrByte(PR_P1 + PR_FIGHTER), PrByte(PR_P2 + PR_FIGHTER),
				PrWord(PR_P2 + PR_X) - PrWord(PR_P1 + PR_X), PrWord(PR_P2 + PR_HP), PrByte(PR_P2 + PR_STUN), PrByte(PR_TIMER));
			DnEmit(j);
		}
	}
	char j[120];
	if (a.game != "sf2ce") return 0;
	sprintf(j, "{\"ev\":\"practice-end\",\"p1\":%d,\"p2\":%d,\"s\":%d,\"x\":%d,\"hp\":%d,\"bar\":%d,\"time\":%d}", PrByte(PR_P1 + PR_FIGHTER), PrByte(PR_P2 + PR_FIGHTER), PrByte(PR_P2 + PR_STATE), PrWord(PR_P2 + PR_X) - PrWord(PR_P1 + PR_X), PrWord(PR_P2 + PR_HP), PrByte(PR_P2 + PR_HP_BAR), PrByte(PR_TIMER));
	DnEmit(j);
	return 0;
}

int DnNetGetInput()
{
	if ((!gSession && !gAlone) || gEnding) return 1;
	if (gArgs.testCrash) *(volatile int*)0 = 0;
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
	uint32_t local = !gArgs.script.empty() ? DnScriptInput(gArgs.script, gFrame) | gCtrlBits : gAutoRng ? DnBotInput() : ((DnInputOn() ? DnPackLocalInput(DnHasInputFocus()) : 0) | gCtrlBits);	// the controller shows either way
	local = DnWithSeatRequest(local);
	if (gArgs.practice) DnPracticeInput(local, gFrame);
	if (gArgs.practice && gArgs.game == "sf2ce") {
		uint32_t in[GGPO_MAX_PLAYERS] = { 0 };
		DnPracticeFrame(local, in);
		DnApplyInputs(in);
		return 0;
	}
	if (gAlone) {
		DnApplyInputs(&local);
		DnEmitSeats();
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
	DnEmitSeats();
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

// dn:roms - no game loaded: the files a game needs, from FBNeo's own list, for the lobby to check a folder against.
// A clone's files that its parent has too (same CRC) are listed in the parent's zip: that is where a split set keeps them
// (packs and MAME-style sets), and FBNeo finds them there. Its own files stay in its zip.
static int DnRoms(const DnArgs& a)
{
	DnIpcStart(NULL);
	BurnLibInit();
	int nGame = BurnDrvGetIndex((char*)a.game.c_str());
	if (nGame < 0) {
		DnEmitError("bad_args", ("unknown game: " + a.game).c_str());
		DnEmitDrain(500);
		BurnLibExit();
		return DN_EXIT_OTHER;
	}
	std::vector<UINT32> parentCrcs;
	const char* parent = (nBurnDrvActive = nGame, BurnDrvGetTextA(DRV_PARENT));
	const int nParent = parent ? BurnDrvGetIndex((char*)parent) : -1;
	if (nParent >= 0) {
		nBurnDrvActive = nParent;
		for (UINT32 i = 0; i < 0x80; i++) {
			struct BurnRomInfo ri;
			memset(&ri, 0, sizeof(ri));
			if (BurnDrvGetRomInfo(&ri, i) == 0 && ri.nLen && ri.nCrc) parentCrcs.push_back(ri.nCrc);
		}
	}
	nBurnDrvActive = nGame;
	std::vector<std::string> zips;
	char* zipName = NULL;
	for (UINT32 i = 0; BurnDrvGetZipName(&zipName, i) == 0; i++) zips.push_back(std::string(zipName) + ".zip");
	std::string j = "{\"ev\":\"roms\",\"zips\":[";
	for (size_t i = 0; i < zips.size(); i++) j += (i ? ",\"" : "\"") + zips[i] + "\"";
	j += "],\"required\":[";
	bool first = true;
	for (UINT32 i = 0; i < 0x100; i++) {	// from 0x80 on: the board's files, the BIOS zip
		struct BurnRomInfo ri;
		memset(&ri, 0, sizeof(ri));
		char* romName = NULL;
		if (BurnDrvGetRomInfo(&ri, i) || !ri.nLen || (ri.nType & (BRF_OPT | BRF_NODUMP)) || BurnDrvGetRomName(&romName, i, 0) || !romName) continue;
		char crc[16];
		sprintf(crc, "%08x", ri.nCrc);
		const bool shared = i < 0x80 && zips.size() > 1 && std::find(parentCrcs.begin(), parentCrcs.end(), ri.nCrc) != parentCrcs.end();
		const std::string& zip = i >= 0x80 ? zips.back() : shared ? zips[1] : zips.front();
		j += std::string(first ? "" : ",") + "{\"zip\":\"" + zip + "\",\"name\":\"" + romName + "\",\"crc\":\"" + crc + "\"}";
		first = false;
	}
	j += "]}";
	DnEmit(j.c_str());
	DnEmitDrain(500);
	BurnLibExit();
	return 0;
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
	if (a.mode == "roms") {
		exitCode = DnRoms(a);
		return true;
	}
	if (a.mode == "p2p") {	// runs inside the normal app (main.cpp calls DnP2PLoad)
		gArgs = a;
		bDnNet = true;
		return false;
	}
	gArgs = a;
	exitCode = DnRun(a);
	return true;
}
