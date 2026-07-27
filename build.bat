@echo off

if not exist bin mkdir bin

set MODE=%1
if "%MODE%"=="" set MODE=debug

set SOURCES=pokaz.c

set COMMON_CFLAGS=/nologo /W4
set COMMON_LFLAGS=/link /ENTRY:wWinMainCRTStartup

if /i "%MODE%"=="release" (
    set CFLAGS=%COMMON_CFLAGS% /O2 /DNDEBUG
    set LFLAGS=%COMMON_LFLAGS% /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF
) else (
    set CFLAGS=%COMMON_CFLAGS% /Z7 /Od /DDEBUG
    set LFLAGS=%COMMON_LFLAGS% /SUBSYSTEM:CONSOLE
)

cl %CFLAGS% ^
    %SOURCES% ^
    /Fo"bin\\" ^
    /Fd"bin\\" ^
    /Fe"bin\\pokaz.exe" ^
    %LFLAGS%
