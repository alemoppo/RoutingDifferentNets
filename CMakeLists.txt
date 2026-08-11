cmake_minimum_required(VERSION 3.16)
project(NetworkRouteManager C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Release e' il tipo di build predefinito (l'uso normale dell'utente deve dare
# una build ottimizzata). Per i generatori single-config (MinGW Makefiles /
# Unix Makefiles) un CMAKE_BUILD_TYPE esplicito in fase di configurazione fa
# selezionare i flag per-config.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo di build (default: Release)" FORCE)
endif()

if(NOT MINGW)
    message(FATAL_ERROR "Questo progetto richiede MinGW-w64 (vedi build.bat).")
endif()

# Percorso SDL3 e SDL3_ttf sotto deps\
set(SDL3_MINGW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL3-3.4.14/x86_64-w64-mingw32")
set(SDL3_INCLUDE_DIR "${SDL3_MINGW_ROOT}/include")
set(SDL3_LIBRARY "${SDL3_MINGW_ROOT}/lib/libSDL3.dll.a")

set(SDL3_TTF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL3_ttf-3.2.2/SDL3_ttf-3.2.2/x86_64-w64-mingw32")
set(SDL3_TTF_INCLUDE_DIR "${SDL3_TTF_ROOT}/include")
set(SDL3_TTF_LIBRARY "${SDL3_TTF_ROOT}/lib/libSDL3_ttf.dll.a")

if(NOT EXISTS "${SDL3_INCLUDE_DIR}/SDL3/SDL.h")
    message(FATAL_ERROR "SDL3 non trovato in deps/. Vedere README.")
endif()
if(NOT EXISTS "${SDL3_TTF_INCLUDE_DIR}/SDL3_ttf/SDL_ttf.h")
    message(FATAL_ERROR "SDL3_ttf non trovato in deps/. Vedere README.")
endif()

add_executable(NetworkRouteManager WIN32
    src/main.c
    src/gui.c
    src/network.c
    src/routes.c
    src/config.c
    src/monitor.c
    src/proc.c
    src/resources.rc
)

target_include_directories(NetworkRouteManager PRIVATE
    src ${SDL3_INCLUDE_DIR} ${SDL3_TTF_INCLUDE_DIR})
target_link_libraries(NetworkRouteManager PRIVATE
    ${SDL3_LIBRARY} ${SDL3_TTF_LIBRARY} iphlpapi ws2_32)

# Copia SDL3.dll e SDL3_ttf.dll accanto all'eseguibile dopo il build.
add_custom_command(TARGET NetworkRouteManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL3_MINGW_ROOT}/bin/SDL3.dll"
            "$<TARGET_FILE_DIR:NetworkRouteManager>/SDL3.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL3_TTF_ROOT}/bin/SDL3_ttf.dll"
            "$<TARGET_FILE_DIR:NetworkRouteManager>/SDL3_ttf.dll"
)

# windres viene gestito automaticamente da CMake per i file .rc.
set_source_files_properties(src/resources.rc PROPERTIES LANGUAGE RC)

# Opzioni di compilazione: Release ottimizzata, senza flags hardcoded globali.
# -Wall/-Wextra si applicano a tutte le build; -O2/-DNDEBUG solo in Release.
target_compile_options(NetworkRouteManager PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O2;-DNDEBUG>)