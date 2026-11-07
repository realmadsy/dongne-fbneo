// dn: ëë¤ì¤ë½ì¤ netplay mode (dn:synctest / dn:p2p). See docs/superpowers/plans in the k-arcade repo.
#pragma once

#include <string>
#include <vector>

struct DnPeer { int player; std::string ip; int port; };

struct DnArgs {
	std::string mode, game, autoInput;
	std::wstring rompath, soundDump, frameLog, dump;	// dump (dn:practice): both SF2 fighters' memory, 0x600 bytes a frame (tests)	// soundDump: raw 16-bit stereo, one block per emulated frame (tests). frameLog: see DnFrameLog
	int players = 2, me = 1, localPort = 7000, delay = 2, frames = 0;	// delay -1 = auto (--delay auto)
	int testStart = 0;		// --test-start <frame>: acts as if the start key went down at that frame, and reports the credits (tests)
	int testCrash = 0;		// --test-crash 1: crashes on the first input read (tests)
	bool fastStart = false;	// --fast-start 1: skip the boot, open on character select (DnFastStart)
	bool practice = false;	// --practice 1: alone, no session; SF2 Dash's other fighter follows the panel, KOF '98 opens its own practice mode
	std::string practiceSet;	// --practice-set: the settings to start with, as in the "practice" command
	std::string script;		// --script (dn:practice): frame,length,roles;... the player's presses, for tests
	std::vector<int> seats;	// --seats 2,1: the player number of each room player (room order) to start with; default 1, 2, ...
	bool seatRestart = false;	// --seat-restart 1: a fighting game: seats never swap in play, an agreed swap is reported to restart with
	bool freeCoin = false;	// --free-coin 1: the coin key works and start is only start (tools/latency.ps1 times the coin key)
	unsigned int seed = 1;
	HWND parent = NULL;
	std::vector<DnPeer> peers;
};

// dn_cli.cpp
bool DnParseArgs(const wchar_t* cmdline, DnArgs& out, std::string& err);

// dn_ipc.cpp
void DnEmit(const char* json);
void DnEmitError(const char* code, const char* msg);
void DnEmitDrain(DWORD ms);			// wait (up to ms) until queued lines are written
void DnIpcStart(HWND parent);		// stdin reader thread + child-window mode when parent != NULL
HWND DnParentWnd();					// lobby window we live in, or NULL
bool DnChildRect(RECT* rc);			// placement inside the parent (physical px), false if not in child mode
HWND DnInputWnd();					// hidden top-level window: DirectInput needs one, and it receives raw keyboard input
const unsigned char* DnKeyState();	// child mode keyboard, DIK-indexed (0x80 = down), fed by raw input (see dn_ipc.cpp)
bool DnInputOn();					// the lobby has not paused input (key settings panel open)
bool DnHasInputFocus();				// child mode: the lobby window is in front (and not in its key settings)
bool DnIpcPoll();					// main thread: apply queued stdin commands; true = quit requested
bool DnStdinClosed();
std::vector<int> DnJsonInts(const std::string& line, const char* key);	// "key":[1,2] or "key":1 from our flat command lines

// intf/input/inp_interface.cpp
INT32 InputReadSwitch(const INT32 nCode);

// dn_netplay.cpp
bool DnTryMain(const wchar_t* cmdline, int& exitCode);	// true: fully handled (synctest, bad args); false: continue normal init
int DnRun(const DnArgs& a);
void DnSetKeymap(const std::string& line);	// {"cmd":"keymap","up":[200,16386],...,"fire1":[44]}: codes per role (keyboard DIK / FBNeo joystick)
void DnSetSeat(const std::string& line);	// {"cmd":"seat","to":3}: move to that player number if nobody holds it
void DnSetPractice(const std::string& line);	// {"cmd":"practice","move":0,"guard":0,"stun":1}
void DnSetHotkeys(const std::string& line);	// {"cmd":"hotkeys","hk":[code,mask,turbo,...]}: mask bit 0 = fire 1, turbo = period in frames or 0

// dn:p2p runs inside FBNeo's normal app + message loop (main.cpp / run.cpp hooks)
extern bool bDnNet;
int DnP2PLoad();			// instead of ProcessCmdLine(): 0 = go on to the message loop
int DnNetGetInput();		// RunFrame(): 0 = inputs applied, run the frame; 1 = skip this frame
void DnNetAdvanceFrame();	// RunFrame(): after the emulated frame
void DnFrameLog(char kind);	// RunFrame(): after painting
void DnNetIdle();			// RunIdle(): network poll, rollbacks, stats
int DnShutdown();			// end of WinMain: closes the session, returns the exit code

enum { DN_EXIT_OK = 0, DN_EXIT_DESYNC = 2, DN_EXIT_ROM = 3, DN_EXIT_PORT = 4, DN_EXIT_OTHER = 5 };
