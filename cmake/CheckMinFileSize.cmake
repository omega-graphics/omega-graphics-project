cmake_minimum_required(VERSION 3.14)  # file(SIZE)

# Asserts that FILE exists and is at least MIN_SIZE bytes.
# Used to catch silent build failures (e.g. ICU pkgdata producing only the
# stubdata DLL when Python 3 or the wrong msbuild Platform leaves makedata
# unable to run).
#
# Invoke as:
#   cmake -DFILE=<path> -DMIN_SIZE=<bytes> [-DLABEL=<msg>] -P CheckMinFileSize.cmake

if(NOT DEFINED FILE)
    message(FATAL_ERROR "CheckMinFileSize: -DFILE=<path> is required")
endif()
if(NOT DEFINED MIN_SIZE)
    message(FATAL_ERROR "CheckMinFileSize: -DMIN_SIZE=<bytes> is required")
endif()
if(NOT DEFINED LABEL)
    set(LABEL "${FILE}")
endif()

if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "CheckMinFileSize: ${LABEL}: file does not exist: ${FILE}")
endif()

file(SIZE "${FILE}" _actual_size)
if(_actual_size LESS MIN_SIZE)
    message(FATAL_ERROR
        "CheckMinFileSize: ${LABEL}: ${FILE} is ${_actual_size} bytes, "
        "expected at least ${MIN_SIZE}. "
        "This usually means the upstream build step silently produced a stub.")
endif()

message(STATUS "CheckMinFileSize: ${LABEL}: ${_actual_size} bytes (>= ${MIN_SIZE}) OK")
