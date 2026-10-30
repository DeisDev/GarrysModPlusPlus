# Removes the d3d9.dll of the retired Direct3D 9 build from bin/win64, only when it is ours (it logs "GarrysMod++
# disabled" without -insecure); any other d3d9.dll is left alone.
if(EXISTS "${TARGET}")
    file(STRINGS "${TARGET}" marker REGEX "GarrysMod[+][+] disabled: launch with -insecure" LIMIT_COUNT 1)
    if(marker)
        file(REMOVE "${TARGET}")
        message(STATUS "Removed the old GarrysMod++ ${TARGET}")
    endif()
endif()
