# Copies the loader to the game root, refusing to replace a winmm.dll that is not ours (loader.rc names it).
if(EXISTS "${DESTINATION}")
    file(STRINGS "${DESTINATION}" marker ENCODING UTF-16LE REGEX "GarrysMod[+][+] loader" LIMIT_COUNT 1)
    if(NOT marker)
        message(FATAL_ERROR "${DESTINATION} is not the GarrysMod++ loader; not replacing it.")
    endif()
endif()
file(COPY_FILE "${SOURCE}" "${DESTINATION}" ONLY_IF_DIFFERENT)
