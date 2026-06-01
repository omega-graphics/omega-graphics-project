@echo off
setlocal EnableDelayedExpansion

REM clean-windows-build-artifacts.bat
REM
REM Clear *.dll and *.exe from a Windows build tree, scoped to bin\,
REM lib\, and every "tests" output directory under the build root.
REM Intended for the "force-fresh test binaries before a debug run"
REM workflow -- leaves CMake metadata, object files, .lib/.pdb, and
REM third-party dependency builds (_deps\, any deps\) untouched so the
REM next `cmake --build` only relinks the project's own targets.
REM
REM Usage:
REM   utils\clean-windows-build-artifacts.bat [BUILD_DIR] [/n] [/y]
REM                                            [/f NAME [/f NAME ...]]
REM
REM   BUILD_DIR  Defaults to <repo>\build. Accepts absolute or relative.
REM   /n         Dry-run: print what would be deleted, delete nothing.
REM   /y         Skip the confirmation prompt.
REM   /f NAME    Restrict deletion to files matching NAME (a filename
REM              or glob, e.g. "OmegaWTK.dll", "SVGViewRenderTest.exe",
REM              or "Omega*.dll"). Repeat for multiple. Matching is
REM              recursive within the scoped roots and case-insensitive.
REM              Without /f, every *.dll and *.exe in scope is targeted.

set "BUILD_DIR="
set "DRY_RUN=0"
set "ASSUME_YES=0"
set "FILTER_COUNT=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="/n"        ( set "DRY_RUN=1"    & shift & goto parse_args )
if /I "%~1"=="--dry-run" ( set "DRY_RUN=1"    & shift & goto parse_args )
if /I "%~1"=="/y"        ( set "ASSUME_YES=1" & shift & goto parse_args )
if /I "%~1"=="--yes"     ( set "ASSUME_YES=1" & shift & goto parse_args )
if /I "%~1"=="/f"        ( call :add_filter "%~2" || exit /b 2 & shift & shift & goto parse_args )
if /I "%~1"=="--filter"  ( call :add_filter "%~2" || exit /b 2 & shift & shift & goto parse_args )
if /I "%~1"=="/h"        ( goto show_help )
if /I "%~1"=="/?"        ( goto show_help )
if /I "%~1"=="--help"    ( goto show_help )
if "!BUILD_DIR!"=="" (
    set "BUILD_DIR=%~1"
    shift
    goto parse_args
)
echo unknown extra argument: %~1 1>&2
exit /b 2

:show_help
echo Usage: %~nx0 [BUILD_DIR] [/n] [/y] [/f NAME [/f NAME ...]]
echo.
echo   BUILD_DIR  Defaults to ^<repo^>\build
echo   /n         Dry-run
echo   /y         Skip confirmation prompt
echo   /f NAME    Restrict to files matching NAME (filename or glob).
echo              Repeatable. Without /f, all *.dll and *.exe in scope
echo              are targeted.
echo.
echo Examples:
echo   %~nx0 /n
echo   %~nx0 /f OmegaWTK.dll /f SVGViewRenderTest.exe
echo   %~nx0 /f "Omega*.dll" /y
exit /b 0

:args_done

REM Default build dir = <repo>\build, where repo = parent of utils\.
if "!BUILD_DIR!"=="" (
    for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
    set "BUILD_DIR=!REPO_ROOT!\build"
)

if not exist "!BUILD_DIR!\" (
    echo build dir not found: !BUILD_DIR! 1>&2
    exit /b 1
)

REM Canonicalize.
for %%I in ("!BUILD_DIR!") do set "BUILD_DIR=%%~fI"

echo build dir: !BUILD_DIR!

REM ------------------------------------------------------------------
REM Build the list of scoped roots:
REM   <build>\bin
REM   <build>\lib
REM   every directory anywhere under <build> named "tests", excluding
REM   anything inside _deps\ or deps\ (those are vendored third-party
REM   build trees -- googletest, libpng, libtiff, icu, libjpeg-turbo
REM   etc. -- and wiping their DLLs/EXEs would force a dep rebuild,
REM   which is exactly what this script is meant to avoid).
REM ------------------------------------------------------------------
set "SCOPE_COUNT=0"
if exist "!BUILD_DIR!\bin\" call :add_scope "!BUILD_DIR!\bin"
if exist "!BUILD_DIR!\lib\" call :add_scope "!BUILD_DIR!\lib"

for /f "delims=" %%D in ('dir /b /s /ad "!BUILD_DIR!" 2^>nul ^| findstr /I /R "\\tests$"') do call :maybe_add_test_dir "%%D"

if !SCOPE_COUNT! EQU 0 (
    echo no scoped dirs found under !BUILD_DIR! ^(bin\, lib\, tests\^)
    exit /b 0
)

echo scoped dirs ^(!SCOPE_COUNT!^):
for /l %%I in (1,1,!SCOPE_COUNT!) do (
    set "D=!SCOPE_%%I!"
    echo   !D!
)
echo.

if !FILTER_COUNT! GTR 0 (
    echo name filters ^(!FILTER_COUNT!^):
    for /l %%I in (1,1,!FILTER_COUNT!) do (
        set "N=!FILTER_%%I!"
        echo   !N!
    )
    echo.
)

REM ------------------------------------------------------------------
REM Enumerate target files inside each scoped root. Re-apply the
REM _deps\ / deps\ exclusion so a stray nested vendored tree inside a
REM tests\ folder doesn't get clobbered. When /f filters are given,
REM enumerate only those filename patterns; otherwise default to
REM every *.dll and *.exe in scope.
REM ------------------------------------------------------------------
set "TARGET_COUNT=0"
set "LIST_FILE=%TEMP%\clean-wba-%RANDOM%-%RANDOM%.txt"
if exist "!LIST_FILE!" del /f /q "!LIST_FILE!"

for /l %%I in (1,1,!SCOPE_COUNT!) do (
    set "D=!SCOPE_%%I!"
    if !FILTER_COUNT! EQU 0 (
        for /f "delims=" %%F in ('dir /b /s /a-d "!D!\*.dll" "!D!\*.exe" 2^>nul') do (
            set "P=%%F"
            call :emit_if_not_excluded "!P!"
        )
    ) else (
        for /l %%J in (1,1,!FILTER_COUNT!) do (
            set "N=!FILTER_%%J!"
            for /f "delims=" %%F in ('dir /b /s /a-d "!D!\!N!" 2^>nul') do (
                set "P=%%F"
                call :emit_if_not_excluded "!P!"
            )
        )
    )
)

if !TARGET_COUNT! EQU 0 (
    echo no .dll or .exe files found in scope under !BUILD_DIR!
    if exist "!LIST_FILE!" del /f /q "!LIST_FILE!"
    exit /b 0
)

echo files to delete ^(!TARGET_COUNT!^):
for /f "usebackq delims=" %%F in ("!LIST_FILE!") do echo   %%F

if "!DRY_RUN!"=="1" (
    echo.
    echo [dry-run] no files deleted.
    if exist "!LIST_FILE!" del /f /q "!LIST_FILE!"
    exit /b 0
)

if not "!ASSUME_YES!"=="1" (
    echo.
    set /p "REPLY=delete the !TARGET_COUNT! file(s) above? [y/N] "
    if /I not "!REPLY!"=="y"   if /I not "!REPLY!"=="yes" (
        echo aborted.
        if exist "!LIST_FILE!" del /f /q "!LIST_FILE!"
        exit /b 0
    )
)

set "DELETED=0"
set "FAILED=0"
for /f "usebackq delims=" %%F in ("!LIST_FILE!") do (
    del /f /q "%%F" >nul 2>&1
    if exist "%%F" (
        echo failed to delete: %%F 1>&2
        set /a FAILED+=1
    ) else (
        set /a DELETED+=1
    )
)

if exist "!LIST_FILE!" del /f /q "!LIST_FILE!"
echo deleted !DELETED! of !TARGET_COUNT! file(s).
if !FAILED! GTR 0 exit /b 1
exit /b 0


REM ==================================================================
REM Subroutines
REM ==================================================================

:add_scope
set /a SCOPE_COUNT+=1
set "SCOPE_!SCOPE_COUNT!=%~1"
exit /b 0

:maybe_add_test_dir
REM Skip the dir itself if it sits under a _deps\ or deps\ path.
set "CAND=%~1"
echo !CAND! | findstr /I /C:"\\_deps\\" >nul && exit /b 0
echo !CAND! | findstr /I /C:"\\deps\\"  >nul && exit /b 0
REM Skip duplicates of bin\ / lib\ in the unlikely event one lives
REM under a tests\ name (it shouldn't).
for /l %%I in (1,1,!SCOPE_COUNT!) do (
    if /I "!SCOPE_%%I!"=="!CAND!" exit /b 0
)
call :add_scope "!CAND!"
exit /b 0

:emit_if_not_excluded
set "P=%~1"
echo !P! | findstr /I /C:"\\_deps\\" >nul && exit /b 0
echo !P! | findstr /I /C:"\\deps\\"  >nul && exit /b 0
REM Skip duplicates. The same path can arrive twice when overlapping
REM /f patterns both match it (e.g. /f OmegaWTK.dll /f Omega*.dll).
REM Use a SEEN_<key> env var as the dedup set; sanitize the path
REM into a valid env-var name (paths have ':' and '\' which break
REM the SET parser, and spaces which break `defined`).
set "KEY=!P!"
set "KEY=!KEY::=_!"
set "KEY=!KEY:\=_!"
set "KEY=!KEY: =_!"
if defined SEEN_!KEY! exit /b 0
set "SEEN_!KEY!=1"
echo !P!>>"!LIST_FILE!"
set /a TARGET_COUNT+=1
exit /b 0

:add_filter
set "ARG=%~1"
if "!ARG!"=="" (
    echo /f requires an argument ^(filename or glob^) 1>&2
    exit /b 1
)
REM Reject names containing path separators -- /f scopes by filename
REM only; the scope dirs (bin\, lib\, tests\) define the where.
echo !ARG! | findstr /R /C:"[\\/]" >nul && (
    echo /f name must not contain a path separator: !ARG! 1>&2
    exit /b 1
)
REM Reject names without a .dll or .exe extension -- this script only
REM ever touches those, so a typo'd "OmegaWTK" (no extension) should
REM not silently match nothing. Compare the trailing 4 chars directly
REM (findstr has no regex alternation; a `for` loop would glob a
REM name like "Omega*.dll" against the current directory).
set "TAIL=!ARG:~-4!"
if /I "!TAIL!"==".dll" goto :add_filter_ok
if /I "!TAIL!"==".exe" goto :add_filter_ok
echo /f name must end in .dll or .exe: !ARG! 1>&2
exit /b 1
:add_filter_ok
set /a FILTER_COUNT+=1
set "FILTER_!FILTER_COUNT!=!ARG!"
exit /b 0
