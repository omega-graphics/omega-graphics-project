cmake_minimum_required(VERSION 3.9)

if(NOT DEFINED OMEGASLC)
    message(FATAL_ERROR "OMEGASLC path not provided")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT shader file not provided")
endif()

if(NOT DEFINED OUTDIR)
    message(FATAL_ERROR "OUTDIR not provided")
endif()

if(NOT DEFINED OUTPUT_NAME)
    message(FATAL_ERROR "OUTPUT_NAME not provided")
endif()

if(NOT DEFINED GOLDEN)
    message(FATAL_ERROR "GOLDEN file not provided")
endif()

file(MAKE_DIRECTORY "${OUTDIR}")

execute_process(
    COMMAND "${OMEGASLC}" --hlsl -t "${OUTDIR}" -o "${OUTDIR}/out.omegasllib" "${INPUT}"
    RESULT_VARIABLE res
)

if(NOT res EQUAL 0)
    message(FATAL_ERROR "omegaslc failed with code ${res}")
endif()

set(GENERATED "${OUTDIR}/${OUTPUT_NAME}.hlsl")

if(NOT EXISTS "${GENERATED}")
    message(FATAL_ERROR "Expected generated file `${GENERATED}` not found")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${GENERATED}" "${GOLDEN}"
    RESULT_VARIABLE cmp_res
)

if(NOT cmp_res EQUAL 0)
    message(FATAL_ERROR "Golden-file mismatch between `${GENERATED}` and `${GOLDEN}`")
endif()

