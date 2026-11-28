# The library's layering, checked on its sources' #include lines
# (cmake -DSOURCE_DIR=<lib/orbis-core/orbis-core> -P check_layering.cmake):
#
# - a file format's folder (insta360/, dji/) is included only from inside
#   that folder: the core is camera-agnostic, the formats fill its types,
#   and one format doesn't build on another;
# - the library includes only itself, the C++ and POSIX headers and its
#   dependencies (FFmpeg, Vulkan, OpenCV, ONNX Runtime) -- no Qt, nothing of
#   the app's -- so it builds on its own.

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -P check_layering.cmake")
endif()

set(format_dirs insta360 dji gopro)
set(allowed
    "^<orbis-core/"
    "^<[a-z_]+>$"                    # the C++ standard library
    "^<(unistd|fcntl|dlfcn)\\.h>$"   # POSIX
    "^<lib(av|sw)[a-z]*/"            # FFmpeg
    "^<vulkan/"
    "^<opencv2/"
    "^<onnxruntime_[a-z_]+\\.h>$")

file(GLOB_RECURSE sources RELATIVE ${SOURCE_DIR} ${SOURCE_DIR}/*.h ${SOURCE_DIR}/*.cpp)
set(failures 0)
foreach(src IN LISTS sources)
    string(REGEX MATCH "^[^/]+" top "${src}")
    file(STRINGS ${SOURCE_DIR}/${src} lines REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*" "" inc "${line}")
        string(REGEX REPLACE "[ \t]*//.*$" "" inc "${inc}")
        set(ok FALSE)
        foreach(pattern IN LISTS allowed)
            if(inc MATCHES "${pattern}")
                set(ok TRUE)
            endif()
        endforeach()
        if(NOT ok)
            message("${src}: includes ${inc}, "
                    "which is neither the library's nor one of its dependencies'")
            math(EXPR failures "${failures} + 1")
        endif()
        foreach(dir IN LISTS format_dirs)
            if(inc MATCHES "^<orbis-core/${dir}/" AND NOT top STREQUAL dir)
                message("${src}: includes ${inc}; "
                        "only ${dir}/ itself may include ${dir}/'s headers")
                math(EXPR failures "${failures} + 1")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH sources count)
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} include(s) break the library's layering")
endif()
message("layering holds in ${count} files")
