# 원본 사본 (vendored)

| 사본 | 원본 | 커밋 | 커밋 날짜 |
|---|---|---|---|
| `emu/fbneo` | https://github.com/finalburnneo/FBNeo | `139cc70949a1fbf3d144b581aa841a7117d9591d` | 2026-10-31 |
| `emu/ggpo` | https://github.com/pond3r/ggpo | `7ddadef8546a7d99ff0b3530c6056bc8ee4b9c0a` | 2019-11-07 |

`git clone --depth 1`로 받은 뒤 `.git`을 지웠다.

## 동네오락실에서 수정한 부분

FBNeo 라이선스(`emu/fbneo/src/license.txt`)에 따라 수정분은 공개한다. 수정한 파일과 이유를 아래에 적는다.

1. `emu/fbneo/projectfiles/visualstudio-2022/fbneo_vs2022.vcxproj`: 업스트림 프로젝트에서 빠진 `src/burner/win32/hotkeys.cpp`와 `src/intf/audio/win32/aud_wasapi.cpp`를 `ClCompile`에 추가했다. 빠져 있으면 링크 오류가 난다.
2. `emu/fbneo/src/burner/win32/dn.h`, `dn_cli.cpp`, `dn_ipc.cpp`, `dn_netplay.cpp` (새 파일): `dn:synctest` / `dn:p2p` 넷플레이 모드.
3. `emu/fbneo/src/burner/win32/main.cpp`: `WinMain`에서 `dn:` 명령줄이면 `DnTryMain()`으로 분기 (기존 GUI 경로는 그대로), p2p는 `ProcessCmdLine()` 대신 `DnP2PLoad()`, 종료 코드는 `DnShutdown()`, `AppInit()` 첫 실행 창은 dn 모드에서 생략, `#include "dn.h"`.
4. `emu/fbneo/projectfiles/visualstudio-2022/fbneo_vs2022.vcxproj`: dn 소스 3개 추가, Release|x64에 GGPO 포함 경로(`..\..\..\ggpo\src\include`)와 `GGPO.lib`, `ws2_32.lib` 링크.
5. `emu/fbneo/src/burn/burn.cpp` `BurnStateMAMEScan()`: `ACB_NET_OPT | ACB_WRITE`일 때 postload 뒤에 저장값을 다시 써 넣는다. postload가 레지스터에서 값을 재계산하며 저장값을 덮어써서(YM2610 ADPCM `vol_shift`, FM `tl` 등) 롤백 후 상태가 달라지던 문제. `ACB_NET_OPT`가 없는 기존 경로(일반 세이브스테이트 등)는 동작이 같다.
6. `emu/ggpo/src/lib/ggpo/backends/synctest.cpp`: 불일치 시 `DebugBreak()` 대신 `DnOnSyncError(msg)` 호출, 프레임마다 출력하던 `printf` 삭제.
7. `emu/fbneo/src/burner/win32/run.cpp`: `RunFrame()`에서 `bDnNet`이면 프레임 카운터 증가 전에 `DnNetGetInput()`(GGPO 입력 동기화), 프레임 뒤에 `DnNetAdvanceFrame()`, `RunIdle()` 앞에서 `DnNetIdle()`. Kaillera 경로는 그대로.
8. `emu/fbneo/src/burner/win32/scrn.cpp`: `--parent-hwnd`이면 화면 창을 메뉴 없는 `WS_CHILD`로 로비 창 밑에 만들고(`ScrnInit`), 위치·크기는 로비가 보낸 rect를 따른다(`ScrnSize`). 이 모드에서는 더블클릭·메뉴의 전체화면 전환을 막고, 마우스 버튼 메시지를 무시하고, 키보드 포커스를 받으면 로비 창에 돌려준다(로비와 입력 큐를 공유해서, 포커스를 쥐고 있으면 로비 화면에 키가 안 들어감).
9. `emu/fbneo/src/intf/input/win32/inp_dinput.cpp`: 자식 창 모드에서 DirectInput 창을 숨긴 최상위 창(`DnInputWnd`)으로 바꾸고 배경 모드로 고정한다. 키보드는 DirectInput 장치를 만들지 않고 Raw Input 키 표(`DnKeyState`)를 읽는다. 로비가 앞에 있을 때 Windows가 키보드 상태를 숨기기 때문이다 (`docs/phase0-acceptance.md`). 화면 창이 없는 `dn:pads`도 숨긴 창을 쓰고, dn 모드에서는 패드를 비독점으로 잡는다(게임 중에도 `dn:pads`가 같은 패드를 읽게).
10. `emu/fbneo/src/intf/input/inp_interface.cpp`: `InputReadSwitch(code)` 추가. 로비 키 설정(키보드·패드 여러 개, 핫키)을 `dn_netplay.cpp`가 직접 읽는 데 쓴다. FBNeo의 입력 설정은 입력 하나에 코드 하나뿐이라서다.
11. `emu/fbneo/src/burner/win32/run.cpp` `RunFrame()`: 넷플레이(`bDnNet`)에서는 프레임을 만든 직후에 그린다. 원래는 다음 프레임이 시작될 때 그린다. 입력→화면 약 1프레임 단축(`docs/latency.md`).
12. `emu/fbneo/src/burner/win32/media.cpp` `MediaInit()`: 고른 소리 플러그인(넷플레이는 WASAPI)이 시작되지 않으면 DirectSound로 다시 시도한다.
13. `emu/ggpo/src/lib/ggpo/backends/p2p.cpp`·`p2p.h` `AddLocalInput()`: 세션 도중 프레임 지연을 올리면 입력 큐가 채운 사이 프레임도 상대에게 보낸다. 원래는 마지막 프레임만 보내서 상대가 멈췄다(자동 프레임 지연에 필요).
14. `emu/fbneo/src/burn/snd/fm.c` `YM2610_save_state()`·`YM2608_save_state()`: 엔벨로프·LFO 클럭(`eg_cnt`, `eg_timer*`, `lfo_cnt`, `lfo_inc`)과 3슬롯 `block_fnum`도 저장한다. YM2203·YM2612는 이미 저장하는데 이 둘만 빠져 있었다. 세이브스테이트 크기가 바뀌어서 이 두 칩을 쓰는 게임의 예전 세이브스테이트는 맞지 않는다.
15. `emu/fbneo/src/burner/win32/run.cpp` `RunFrame()`: 넷플레이가 상대를 기다리느라 프레임을 건너뛸 때, 사운드 버퍼에 남은 직전 프레임 소리를 다시 틀지 않고 마지막 샘플 값으로 채운다(지직거림 방지).
16. `emu/fbneo/src/burner/win32/main.cpp`: 넷플레이 모드(`bDnNet`)는 종료할 때 FBNeo 설정 파일을 저장하지 않는다. 넷플레이가 바꾼 값(WASAPI, 봇 음소거 등)이 일반 실행 설정에 남지 않게.
17. `emu/fbneo/src/burn/snd/msm6295.cpp` `MSM6295Scan()`: 리샘플러 위치(`nFractionalPosition`)와 보간용 이전 샘플도 저장한다. 위치는 샘플이 끝나는 시점(사운드 CPU가 읽는 상태 비트)도 정해서, 빠지면 상태를 불러온 뒤 칩이 상대와 달라진다.
18. `emu/fbneo/src/burn/snd/burn_ym2151.cpp` `BurnYM2151Scan()`: 다음 프레임으로 넘기는 샘플(개수, 리샘플러 위치, 버퍼 앞부분)도 저장한다. YM2610은 위치를 이미 저장한다.
19. `emu/fbneo/src/burner/win32/run.cpp` `RunPaceActive()`·`RunFrame()`, `dn_netplay.cpp` `bAlwaysDrawFrames = 1`: 넷플레이는 DWM 동기화와 상관없이 일정한 간격으로 프레임을 시작한다(FBNeo 자체의 `RunPacePresent`를 씀). 기다리는 자리는 입력을 읽기 전이다. 그려지기 직전에 기다리면 그 시간이 입력 지연이 된다(약 142ms → 125ms, `docs/latency.md`). 프레임은 사운드 장치의 10ms 주기에 맞춰 만들어져 간격이 20/20/10ms가 된다. 60Hz 모니터에서는 vblank 대기가 이를 가려 주지만 165Hz에서는 그대로 화면에 나온다. 한 번에 2프레임을 만들 때도 앞 프레임을 버리지 않고 둘 다 그린다.
20. `emu/ggpo/src/lib/ggpo/network/udp.cpp` `Udp::SendTo()`: 패킷 전송이 실패해도 메시지 상자를 띄우고 프로세스를 끝내지 않는다. Wi-Fi가 끊기는 순간처럼 인터넷에서는 흔한 일이고, GGPO는 입력을 다음 패킷에 다시 보내므로 패킷 손실과 같다.
21. `emu/ggpo/src/lib/ggpo/network/udp.cpp`, `udp_proto.cpp`, `input_queue.cpp` `Log()`: 이미 만들어진 문장을 다시 형식 문자열로 넘기지 않는다. 통계 줄의 `%` 때문에 `ggpo.log`를 켜면 프로세스가 끝났다. 동작은 로그를 켰을 때만 달라진다.
22. `emu/fbneo/src/burner/win32/dn_netplay.cpp` `DnP2PLoad()`: 사운드 장치와 표본 주파수(48 kHz 고정)를 게임을 불러오기 전에 정한다. 전에는 불러온 뒤에 WASAPI로 바꿔서, 설정 파일이 없는 새 설치본에서는 44.1 kHz로 준비된 사운드 칩이 48 kHz로 돌며 버퍼 끝을 넘어 썼다(`YM2151UpdateOne`).
23. `emu/fbneo/src/burner/win32/systeminfo.cpp` `ExceptionFilter()`, `emu/ggpo/src/lib/ggpo/platform_windows.h` `AssertFailed()`, `types.h` `ASSERT`: 넷플레이 모드에서는 충돌이나 어설션 때 대화상자를 띄우지 않고 프로세스를 끝낸다. 대화상자는 로비 창 뒤에 숨어 로비를 비활성화한 채 기다렸다. 에뮬은 `{"ev":"error","code":"crash"}`를 보내고, 어설션은 종료 코드 1로 끝난다.
24. `emu/fbneo/src/burner/win32/dn_netplay.cpp` `DnStageEdits()`: `sf2ce`의 장기에프 스테이지에서 캐릭터 앞을 가리는 울타리, 레버, 앞쪽 쇠사슬을 지운다. 게임 파일은 표준 그대로 두고, 불러온 그래픽 메모리에서 위쪽 레이어의 8×8 조각 197개를 비운다. 그래픽은 게임 상태에 들어가지 않으므로 대전에 영향이 없다. 소스에는 조각 번호만 있다.
