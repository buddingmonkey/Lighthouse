# Copies Lighthouse's read-only data into the iOS .app bundle. Post-build: -DBUNDLE -DSOURCE_DIR -DBINARY_DIR.

file(MAKE_DIRECTORY "${BUNDLE}/assets")
file(COPY "${SOURCE_DIR}/assets/yaml" DESTINATION "${BUNDLE}/assets")
file(COPY "${SOURCE_DIR}/config.yml" DESTINATION "${BUNDLE}")

# Accept either tree since lighthouse.o2r lands in the source dir; zero-byte files count as missing.
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
