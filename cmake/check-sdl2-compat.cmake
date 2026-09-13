option(LIGHTHOUSE_ALLOW_SDL2_COMPAT "Let the macOS build use sdl2-compat instead of SDL2" OFF)

function(lighthouse_check_sdl2_compat)
    if(NOT APPLE OR LIGHTHOUSE_APPLE_MOBILE OR LIGHTHOUSE_ALLOW_SDL2_COMPAT)
        return()
    endif()
    if(NOT SDL2_DIR)
        return()
    endif()

    get_filename_component(sdl2_resolved_dir "${SDL2_DIR}" REALPATH)
    get_filename_component(sdl2_prefix "${SDL2_DIR}/../../.." ABSOLUTE)

    set(evidence "")
    foreach(header "${sdl2_prefix}/include/SDL2/SDL_revision.h"
                   "${sdl2_prefix}/include/SDL_revision.h")
        if(EXISTS "${header}")
            file(STRINGS "${header}" compat_marker REGEX "SDL2COMPAT")
            if(compat_marker)
                set(evidence "${header} declares SDL2COMPAT_VENDOR_INFO")
                break()
            endif()
        endif()
    endforeach()

    if(NOT evidence AND sdl2_resolved_dir MATCHES "sdl2-compat")
        set(evidence "${SDL2_DIR} resolves to ${sdl2_resolved_dir}")
    endif()

    if(NOT evidence)
        return()
    endif()

    message(FATAL_ERROR
        "The SDL2 package at ${SDL2_DIR} is sdl2-compat, not SDL2.\n"
        "\n"
        "Evidence: ${evidence}\n"
        "\n"
        "sdl2-compat gives the SDL2 ABI on top of SDL3. Its library constructor loads SDL3 with "
        "dlopen(), and it calls abort() if the load fails. SDL3 is not a linked library, so the "
        "CMake bundle fixup can not copy it into Lighthouse.app/Contents/Frameworks. The app then "
        "starts correctly from a terminal, where the shell environment finds SDL3, but it aborts "
        "before main() when you start it from the Finder, because launchd gives it a clean "
        "environment. The crash report shows only dyld and abort. It does not name SDL.\n"
        "\n"
        "Homebrew supplies sdl2-compat as its \"sdl2\" formula. Use the MacPorts SDL2 instead. It "
        "is SDL2, and it is what CI uses.\n"
        "\n"
        "1. Install MacPorts: https://www.macports.org/install.php\n"
        "2. Install the ports that .github/macports.yml lists, for example:\n"
        "     sudo port install libsdl2 libsdl2_net libpng glew libzip nlohmann-json tinyxml2 \\\n"
        "                       libogg libopus opusfile libvorbis\n"
        "3. Delete this build directory. SDL2_DIR is a cache entry, and the cache keeps the "
        "Homebrew path.\n"
        "4. Configure again with:\n"
        "     -DCMAKE_PREFIX_PATH=/opt/local \\\n"
        "     -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \\\n"
        "     -DSDL2_DIR=/opt/local/lib/cmake/SDL2 \\\n"
        "     -DSDL2_net_DIR=/opt/local/lib/cmake/SDL2_net\n"
        "\n"
        "To build against sdl2-compat on purpose, configure with "
        "-DLIGHTHOUSE_ALLOW_SDL2_COMPAT=ON. You must then put libSDL3.dylib in the bundle "
        "yourself, or the app will not start from the Finder."
    )
endfunction()

lighthouse_check_sdl2_compat()
