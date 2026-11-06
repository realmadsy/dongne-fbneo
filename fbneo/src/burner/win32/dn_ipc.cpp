// dn: talking to the lobby. stdout: one JSON object per line. stdin: one command per line.
// With --parent-hwnd the screen window is a child of the lobby window (see scrn.cpp, inp_dinput.cpp).
#include "burner.h"
#include "dn.h"

// A blocking write would freeze the game whenever the lobby stops reading for a moment (on Windows
// its main thread is busy while a window is dragged or the mouse is held), and the peer freezes with
// us. So during a session a writer thread does the writing; the game thread only queues lines.
// The writer looks at the queue by itself: waking it from the game thread cost that frame up to 18 ms (measured on hits in practice).
static CRITICAL_SECTION gOutLock;
static bool gOutAsync = false;
static std::vector<std::string> gOutQueue;
static volatile LONG gOutBusy = 0;

static void DnWrite(const std::string& line)
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (h == NULL || h == INVALID_HANDLE_VALUE) return;
	DWORD n;
	WriteFile(h, line.data(), (DWORD)line.size(), &n, NULL);
}

enum { DN_OUT_POLL_MS = 5 };

static DWORD WINAPI StdoutThread(LPVOID)
{
	for (;;) {
		Sleep(DN_OUT_POLL_MS);
		std::vector<std::string> lines;
		EnterCriticalSection(&gOutLock);
		lines.swap(gOutQueue);
		InterlockedExchange(&gOutBusy, lines.empty() ? 0 : 1);
		LeaveCriticalSection(&gOutLock);
		for (size_t i = 0; i < lines.size(); i++) DnWrite(lines[i]);
		InterlockedExchange(&gOutBusy, 0);
	}
}

static void DnEmitAsyncStart()
{
	InitializeCriticalSection(&gOutLock);
	gOutAsync = true;
	CloseHandle(CreateThread(NULL, 0, StdoutThread, NULL, 0, NULL));
}

void DnEmit(const char* json)
{
	std::string line = std::string(json) + "\n";
	if (!gOutAsync) {				// outside a session (synctest, startup errors): write directly
		DnWrite(line);
		return;
	}
	EnterCriticalSection(&gOutLock);
	gOutQueue.push_back(line);
	InterlockedExchange(&gOutBusy, 1);
	LeaveCriticalSection(&gOutLock);
}

void DnEmitDrain(DWORD ms)
{
	if (!gOutAsync) return;
	for (DWORD waited = 0; waited < ms; waited += 10) {
		EnterCriticalSection(&gOutLock);
		bool empty = gOutQueue.empty();
		LeaveCriticalSection(&gOutLock);
		if (empty && !gOutBusy) return;
		Sleep(10);
	}
}

void DnEmitError(const char* code, const char* msg)
{
	std::string m;
	for (const char* p = msg; *p; p++) {
		if (*p == '"' || *p == '\\') m += '\\';
		if ((unsigned char)*p >= 0x20) m += *p;
	}
	std::string j = std::string("{\"ev\":\"error\",\"code\":\"") + code + "\",\"msg\":\"" + m + "\"}";
	DnEmit(j.c_str());
}

// ---------------------------------------------------------------- child window

static HWND gParent = NULL;
static RECT gRect = { 0, 0, 0, 0 };
static HWND gInputWnd = NULL;

HWND DnParentWnd() { return gParent; }

bool DnChildRect(RECT* rc)
{
	if (!gParent) return false;
	if (gRect.right <= gRect.left || gRect.bottom <= gRect.top) GetClientRect(gParent, &gRect);	// until the lobby sends one
	*rc = gRect;
	return true;
}

// Child mode keyboard: while the lobby (another process) is in front, Windows hides key state from us
// (DirectInput background mode and GetAsyncKeyState both read nothing). Raw input with RIDEV_INPUTSINK
// is delivered anyway, so we keep our own DIK-indexed key table from it.
static unsigned char gKeys[256];

const unsigned char* DnKeyState()
{
	static bool registered = false;
	if (!registered && DnInputWnd()) {	// first read happens after DirectInput set up its keyboard, so ours is the registration that sticks
		RAWINPUTDEVICE rid;
		rid.usUsagePage = 0x01;		// generic desktop
		rid.usUsage = 0x06;			// keyboard
		rid.dwFlags = RIDEV_INPUTSINK;
		rid.hwndTarget = DnInputWnd();
		registered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
	}
	// Key messages still queued (the main loop takes one message per turn, and a frame may have just waited for
	// vblank) would land a frame late: take them now, right when the frame reads its input. Measured ~10 ms.
	MSG m;
	while (gInputWnd && PeekMessage(&m, gInputWnd, WM_INPUT, WM_INPUT, PM_REMOVE)) {
		if (m.message == WM_QUIT) {			// PeekMessage hands out WM_QUIT whatever the filter: it's the main loop's
			PostQuitMessage((int)m.wParam);
			break;
		}
		DispatchMessage(&m);
	}
	return gKeys;
}

static LRESULT CALLBACK DnInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_INPUT) {
		RAWINPUT ri;
		UINT size = sizeof(ri);
		if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEKEYBOARD) {
			UINT code = ri.data.keyboard.MakeCode & 0x7F;
			if (ri.data.keyboard.Flags & RI_KEY_E0) code |= 0x80;		// DIK_UP = 0xC8 = E0 48, etc.
			gKeys[code] = (ri.data.keyboard.Flags & RI_KEY_BREAK) ? 0 : 0x80;
		}
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

HWND DnInputWnd()
{
	if (!gInputWnd) {
		WNDCLASS wc;
		memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = DnInputProc;
		wc.hInstance = hAppInst;
		wc.lpszClassName = _T("dnInput");
		RegisterClass(&wc);
		gInputWnd = CreateWindowEx(0, _T("dnInput"), _T("dn input"), WS_POPUP, 0, 0, 0, 0, NULL, NULL, hAppInst, NULL);
	}
	return gInputWnd;
}

static bool gInputOn = true;		// off while the lobby's key settings panel is open

bool DnInputOn() { return gInputOn; }

bool DnHasInputFocus()
{
	return gInputOn && (!gParent || GetForegroundWindow() == GetAncestor(gParent, GA_ROOT));
}

// ---------------------------------------------------------------- stdin commands

static CRITICAL_SECTION gLock;
static std::vector<std::string> gLines;
static bool gStdinClosed = false;

static DWORD WINAPI StdinThread(LPVOID)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	std::string buf;
	char chunk[512];
	DWORD n;
	while (ReadFile(h, chunk, sizeof(chunk), &n, NULL) && n > 0) {
		buf.append(chunk, n);
		size_t i;
		while ((i = buf.find('\n')) != std::string::npos) {
			EnterCriticalSection(&gLock);
			gLines.push_back(buf.substr(0, i));
			LeaveCriticalSection(&gLock);
			buf.erase(0, i + 1);
		}
	}
	EnterCriticalSection(&gLock);
	gStdinClosed = true;
	LeaveCriticalSection(&gLock);
	return 0;
}

void DnIpcStart(HWND parent)
{
	gParent = parent;
	DnEmitAsyncStart();
	InitializeCriticalSection(&gLock);
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	if (h != NULL && h != INVALID_HANDLE_VALUE) CloseHandle(CreateThread(NULL, 0, StdinThread, NULL, 0, NULL));
}

// Reads "key":number from a flat JSON line; good enough for our own fixed commands.
static bool JsonInt(const std::string& s, const char* key, int& out)
{
	std::string k = std::string("\"") + key + "\":";
	size_t i = s.find(k);
	if (i == std::string::npos) return false;
	out = atoi(s.c_str() + i + k.size());
	return true;
}

std::vector<int> DnJsonInts(const std::string& s, const char* key)
{
	std::vector<int> v;
	std::string k = std::string("\"") + key + "\":";
	size_t i = s.find(k);
	if (i == std::string::npos) return v;
	const char* p = s.c_str() + i + k.size();
	if (*p != '[') {
		v.push_back(atoi(p));
		return v;
	}
	for (p++; *p && *p != ']';) {
		char* e;
		long n = strtol(p, &e, 10);
		if (e == p) { p++; continue; }		// commas, spaces
		v.push_back((int)n);
		p = e;
	}
	return v;
}

bool DnStdinClosed()
{
	EnterCriticalSection(&gLock);
	bool closed = gStdinClosed;
	LeaveCriticalSection(&gLock);
	return closed;
}

bool DnIpcPoll()
{
	std::vector<std::string> lines;
	bool closed;
	EnterCriticalSection(&gLock);
	lines.swap(gLines);
	closed = gStdinClosed;
	LeaveCriticalSection(&gLock);

	for (size_t i = 0; i < lines.size(); i++) {
		const std::string& l = lines[i];
		if (l.find("\"cmd\":\"quit\"") != std::string::npos) return true;
		if (l.find("\"cmd\":\"input\"") != std::string::npos) {
			int on;
			if (JsonInt(l, "on", on)) gInputOn = on != 0;
		}
		if (l.find("\"cmd\":\"keymap\"") != std::string::npos) DnSetKeymap(l);
		if (l.find("\"cmd\":\"hotkeys\"") != std::string::npos) DnSetHotkeys(l);
		if (l.find("\"cmd\":\"practice\"") != std::string::npos) DnSetPractice(l);
		if (l.find("\"cmd\":\"rect\"") != std::string::npos) {
			int x, y, w, h;
			if (JsonInt(l, "x", x) && JsonInt(l, "y", y) && JsonInt(l, "w", w) && JsonInt(l, "h", h) && w > 0 && h > 0) {
				SetRect(&gRect, x, y, x + w, y + h);
				if (gParent && hScrnWnd) SetWindowPos(hScrnWnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
			}
		}
	}

	// Chromium's own child window ("Intermediate D3D Window") covers the whole lobby page and can end up
	// above us; whenever something is stacked over the game window, put it back on top.
	if (gParent && hScrnWnd && GetWindow(hScrnWnd, GW_HWNDPREV)) {
		SetWindowPos(hScrnWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	return closed && gParent != NULL;	// lobby went away: nobody will ever send quit
}
