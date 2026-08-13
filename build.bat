@echo off

if not exist bin mkdir bin

set MODE=%1
if "%MODE%"=="" set MODE=debug

set SOURCES=pokaz.c

set COMMON_CFLAGS=/nologo /W4 /MT
set COMMON_LFLAGS=/link /ENTRY:wWinMainCRTStartup /LIBPATH:deps

if /i "%MODE%"=="release" (
    set CFLAGS=%COMMON_CFLAGS% /O2 /Gy /Gw /GL /DNDEBUG
    set LFLAGS=%COMMON_LFLAGS% /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF
    set FILENAME=pokaz-v1.0.0-win64-release.exe
) else (
    set CFLAGS=%COMMON_CFLAGS% /Z7 /Od /DDEBUG
    set LFLAGS=%COMMON_LFLAGS% /SUBSYSTEM:CONSOLE
    set FILENAME=pokaz.exe
)

cl %CFLAGS% ^
    %SOURCES% ^
    /I "deps" ^
    /Fo"bin\\" ^
    /Fd"bin\\" ^
    /Fe"bin\\%FILENAME%" ^
    %LFLAGS%
