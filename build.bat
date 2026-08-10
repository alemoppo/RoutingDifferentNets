@echo off
rem ============================================================
rem  build.bat - compila Network Route Manager (MinGW-w64 / MSYS2)
rem
rem  Requisiti:
rem    - gcc, windres nel PATH (MSYS2: pacman -S mingw-w64-x86_64-gcc)
rem    - SDL3 per MinGW in deps\SDL3-3.4.14 (vedi README)
rem ============================================================
setlocal enabledelayedexpansion

set SDL_DIR=deps\SDL3-3.4.14\x86_64-w64-mingw32
set SDL_INC=%SDL_DIR%\include
set SDL_LIB=%SDL_DIR%\lib

set TTF_DIR=deps\SDL3_ttf-3.2.2\SDL3_ttf-3.2.2\x86_64-w64-mingw32
set TTF_INC=%TTF_DIR%\include
set TTF_LIB=%TTF_DIR%\lib

if not exist "%SDL_INC%\SDL3\SDL.h" (
    echo [ERRORE] SDL3 non trovato in deps\. Scaricare il pacchetto
    echo          SDL3-devel-3.4.14-mingw.zip da https://github.com/libsdl-org/SDL/releases
    echo          ed estrarlo come deps\SDL3-3.4.14.
    exit /b 1
)
if not exist "%TTF_INC%\SDL3_ttf\SDL_ttf.h" (
    echo [ERRORE] SDL3_ttf non trovato in deps\. Scaricare
    echo          SDL3_ttf-devel-3.2.2-mingw.zip da https://github.com/libsdl-org/SDL_ttf/releases
    echo          ed estrarlo come deps\SDL3_ttf-3.2.2.
    exit /b 1
)

echo [1/2] compilazione dei sorgenti...
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\main.c    -o build\main.o    || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\gui.c     -o build\gui.o     || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\network.c -o build\network.o || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\routes.c  -o build\routes.o  || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\config.c  -o build\config.o  || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\monitor.c -o build\monitor.o || goto :err
gcc -std=c11 -O2 -Wall -Wextra -I src -I "%SDL_INC%" -I "%TTF_INC%" -c src\proc.c    -o build\proc.o    || goto :err

if not exist build mkdir build
echo [2/2] compile delle risorse e link...
windres -I src -O coff src\resources.rc -o build\resources.o        || goto :err
gcc -std=c11 -O2 -Wall -Wextra -mwindows -o NetworkRouteManager.exe ^
    build\main.o build\gui.o build\network.o build\routes.o ^
    build\config.o build\monitor.o build\proc.o build\resources.o ^
    -L "%SDL_LIB%" -L "%TTF_LIB%" -lSDL3 -lSDL3_ttf -liphlpapi -lws2_32   || goto :err

if not exist SDL3.dll     copy /y "%SDL_DIR%\bin\SDL3.dll"     SDL3.dll     >nul
if not exist SDL3_ttf.dll copy /y "%TTF_DIR%\bin\SDL3_ttf.dll" SDL3_ttf.dll >nul
echo.
echo Fatto: NetworkRouteManager.exe
echo Avviare con doppio clic; UAC richiedera' i privilegi amministrativi.
exit /b 0

:err
echo.
echo [ERRORE] compilazione fallita.
exit /b 1