# 에뮬 빌드 절차 (Windows x64)

2026-10-31에 실제로 성공한 절차다.

## 도구

| 도구 | 버전 | 설치 |
|---|---|---|
| Visual Studio 2022 Build Tools | 17.14.44 (MSVC 19.44, 도구 모음 v143) | `winget install Microsoft.VisualStudio.2022.BuildTools` + `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended` |
| NASM | 3.02 | `winget install NASM.NASM` → `%LOCALAPPDATA%\bin\NASM` |
| Strawberry Perl | 5.42 | `winget install StrawberryPerl.StrawberryPerl` → `C:\Strawberry\perl\bin` |
| CMake | VS Build Tools에 포함된 것 | `...\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |

## GGPO (정적 라이브러리)

```powershell
$cm = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cm -S emu\ggpo -B emu\ggpo\build -G "Visual Studio 17 2022" -A x64 -DBUILD_SHARED_LIBS=OFF -DGGPO_BUILD_VECTORWAR=OFF
& $cm --build emu\ggpo\build --config Release
```
산출물: `emu\ggpo\build\lib\x64\Release\GGPO.lib`

## FBNeo

```powershell
Remove-Item Env:NoDefaultCurrentDirectoryInExePath -ErrorAction SilentlyContinue   # 아래 주의 1
$env:PERLPATH = "C:\Strawberry\perl\bin"
$env:NASMPATH = "$env:LOCALAPPDATA\bin\NASM"
$env:PATH = "$env:PERLPATH;$env:NASMPATH;$env:PATH"
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  emu\fbneo\projectfiles\visualstudio-2022\fbneo_vs2022.sln /m /p:Configuration=Release /p:Platform=x64
```
산출물: `emu\fbneo\projectfiles\visualstudio-2022\x64\Release\fbneo64.exe` (처음 빌드는 약 3분)

**주의**
1. 환경변수 `NoDefaultCurrentDirectoryInExePath`가 설정돼 있으면 사전 빌드 단계에서 방금 만든 `ctv_make.exe`를 찾지 못해 `ctv.cpp`의 `CtvDo*` 오류가 난다. 빌드하는 셸에서만 지운다.
2. 업스트림 VS2022 프로젝트에 `hotkeys.cpp`, `aud_wasapi.cpp`가 빠져 있어 링크 오류(`Hotkeys*`, `AudOutWASAPI`)가 났다. 프로젝트 파일에 추가했다 (`emu/UPSTREAM.md` 수정 기록 1).

## 실행 확인

- `x64\Release\config\fbneo64.ini`에 `szAppRomPaths[0] <ROM 폴더>\` 한 줄을 넣는다. 첫 실행에는 FBNeo 라이선스 안내 창이 뜬다.
- `fbneo64.exe kof98 -w` → 창 모드로 KOF '98이 실행된다. `-w`를 빼면 전체화면을 시도하는데, 2560×1440에서는 16bpp 모드 설정 오류가 난다.
- 기본 키: 코인 `5`, 시작 `1`.
- 확인 결과 (2026-10-31): 타이틀 화면이 나오고, 코인 2회 입력에 `CREDITS 02`가 표시됐다.
