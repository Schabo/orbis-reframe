# Writes OUT (a header defining ORBIS_VERSION_STRING) from VERSION -- the
# project() version, which `cz bump` keeps in step with the vX.Y.Z tags -- and
# `git describe`:
#   on a release tag, clean      0.1.0
#   12 commits after it          0.1.0+12.gabc1234
#   no tag yet                   0.0.0+gabc1234
#   uncommitted changes          ... .dirty appended (0.1.0+dirty on a tag)
#   no git (a source tarball)    0.1.0
# Run on every build (cmake -P); OUT is only rewritten when it changes, so
# nothing recompiles needlessly.
# In: SOURCE_DIR, VERSION, OUT.
set(version "${VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --match "v[0-9]*"
                --long --dirty --always --abbrev=7
        OUTPUT_VARIABLE describe RESULT_VARIABLE result
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(result EQUAL 0)
        set(dirty FALSE)
        if(describe MATCHES "-dirty$")
            set(dirty TRUE)
            string(REGEX REPLACE "-dirty$" "" describe "${describe}")
        endif()
        set(meta "")
        if(describe MATCHES "^v.*-([0-9]+)-g([0-9a-f]+)$")
            if(NOT CMAKE_MATCH_1 EQUAL 0)
                set(meta "${CMAKE_MATCH_1}.g${CMAKE_MATCH_2}")
            endif()
        elseif(describe MATCHES "^([0-9a-f]+)$")
            set(meta "g${CMAKE_MATCH_1}")
        endif()
        if(dirty)
            if(meta)
                set(meta "${meta}.dirty")
            else()
                set(meta "dirty")
            endif()
        endif()
        if(meta)
            set(version "${version}+${meta}")
        endif()
    endif()
endif()

set(content "#pragma once\n#define ORBIS_VERSION_STRING \"${version}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
