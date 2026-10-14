@echo off
REM Builds both halves of the mod into dist\:
REM   winmm.dll          - the proxy shim (forwarders + one LoadLibrary)
REM   cpbm_identity.dll  - the mod itself
REM
REM Requires Visual Studio 2022 with the C++ desktop workload. Nothing else.
setlocal
REM Find vcvars on any VS2022 edition, so this also builds on a machine that
REM has only the Build Tools installed rather than the full IDE.
set VCVARS=
for %%P in (
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%P set VCVARS=%%P
if not defined VCVARS echo Could not find vcvars64.bat for VS2022. & exit /b 1
call %VCVARS% >nul 2>&1

set HERE=%~dp0
set OUT=%HERE%..\..\build
set DIST=%HERE%..\..\dist
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\core" mkdir "%OUT%\core"
if not exist "%OUT%\shim" mkdir "%OUT%\shim"
if not exist "%DIST%" mkdir "%DIST%"

REM Regenerated from the live System32 winmm.dll every build, so the forwarder
REM set can never drift from the machine's actual export table.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%gen-winmm-def.ps1"
if errorlevel 1 exit /b 1

echo.
REM cpbm_rules.c is deliberately not built. The trigger DSL it implements was
REM replaced by letting EU4 evaluate its own script and report through `log`
REM (see cpbm_main.c). Its source and tests stay in the repo; nothing links it.
echo === cpbm_identity.dll (the mod) ===
REM /wd4201: the Windows SDK's own d3d11.h and d3d10.h use nameless unions,
REM which /W4 flags and /WX would otherwise turn into build errors.
cl /nologo /W4 /WX /wd4201 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /DNDEBUG ^
   /Fo"%OUT%\core\\" ^
   "%HERE%core\cpbm_main.c" ^
   "%HERE%core\cpbm_env.c"  "%HERE%core\cpbm_sha256.c" ^
   "%HERE%core\cpbm_log.c"  "%HERE%core\cpbm_d3d9.c" ^
   "%HERE%core\cpbm_script.c" ^
   /link /DLL /OUT:"%DIST%\cpbm_identity.dll" ^
   /IMPLIB:"%OUT%\cpbm_identity.lib" ^
   shell32.lib ole32.lib d3d9.lib d3d11.lib
if errorlevel 1 exit /b 1

echo.
echo === winmm_orig.lib (stub, satisfies the linker only) ===
lib /nologo /DEF:"%HERE%shim\winmm_orig.def" /MACHINE:X64 ^
    /OUT:"%OUT%\shim\winmm_orig.lib"
if errorlevel 1 exit /b 1

echo.
echo === winmm.dll (proxy shim) ===
REM Compile and link as separate steps: cl mangles a linker response file passed
REM through /link, so link.exe is invoked directly to consume it.
cl /nologo /c /W4 /WX /O2 /GS- /D_CRT_SECURE_NO_WARNINGS /DNDEBUG ^
   /Fo"%OUT%\shim\\" "%HERE%shim\winmm_shim.c"
if errorlevel 1 exit /b 1

link /nologo /DLL /ENTRY:DllMain /NODEFAULTLIB /MACHINE:X64 ^
   /OUT:"%DIST%\winmm.dll" /IMPLIB:"%OUT%\shim\winmm.lib" ^
   "%OUT%\shim\winmm_shim.obj" ^
   @"%HERE%shim\winmm_exports.rsp" ^
   "%OUT%\shim\winmm_orig.lib" kernel32.lib user32.lib
if errorlevel 1 exit /b 1

echo.
echo Built into %DIST%:
dir /b "%DIST%\*.dll"
exit /b 0
