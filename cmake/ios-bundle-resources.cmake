# Copies the read-only data Lighthouse ships with into the iOS .app bundle.
# Run as a post-build step: cmake -DBUNDLE=... -DSOURCE_DIR=... -DBINARY_DIR=... -P this
#
# assets/yaml and config.yml drive the in-app Torch extractor; lighthouse.o2r holds the
# port's own assets. Only assets/yaml is copied so a ROM left in assets/ isn't bundled.

file(MAKE_DIRECTORY "${BUNDLE}/assets")
file(COPY "${SOURCE_DIR}/assets/yaml" DESTINATION "${BUNDLE}/assets")
file(COPY "${SOURCE_DIR}/config.yml" DESTINATION "${BUNDLE}")

# GeneratePortO2R writes lighthouse.o2r to the source dir and only copies it into the
# build tree that ran it, so accept either location. Empty files are treated as missing:
# an offline file(DOWNLOAD) leaves a zero-byte gamecontrollerdb.txt behind.
foreach(file gamecontrollerdb.txt lighthouse.o2r)
    set(found "")
    foreach(dir "${BINARY_DIR}" "${SOURCE_DIR}")
        if(EXISTS "${dir}/${file}" AND NOT found)
            file(SIZE "${dir}/${file}" size)
            if(size GREATER 0)
                set(found "${dir}/${file}")
            endif()
        endif()
    endforeach()
    if(found)
        file(COPY "${found}" DESTINATION "${BUNDLE}")
    elseif(file STREQUAL "lighthouse.o2r")
        # Produced by the GeneratePortO2R target, which is not part of `all`.
        message(WARNING "iOS bundle: lighthouse.o2r is missing; build the GeneratePortO2R "
                        "target or the app will exit on launch")
    else()
        message(WARNING "iOS bundle: ${file} is missing; controller mappings may be incomplete")
    endif()
endforeach()
