# Produce the `.omegasllib` inputs for the linker (Phase 3) tests, in one
# CTest fixture-setup step:
#   - compile link_a.omegasl  -> link_a.omegasllib
#   - compile link_b.omegasl  -> link_b.omegasllib
#   - `--link` the two        -> link_merged.omegasllib (positive test verifies it)
#   - backend-variant of A    -> link_a_variant.omegasllib (backend id bumped, for
#                                the backend-mismatch rejection test)
#
# Driven via `cmake -P` with -D args: OMEGASLC, ARCHIVE_TOOL, SRC_A, SRC_B,
# TEMPDIR, OUTDIR.

file(MAKE_DIRECTORY "${OUTDIR}")
file(MAKE_DIRECTORY "${TEMPDIR}")

function(run_or_die)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "link-fixture setup command failed (exit ${rc}): ${ARGN}")
    endif()
endfunction()

run_or_die(${OMEGASLC} -t "${TEMPDIR}" -o "${OUTDIR}/link_a.omegasllib" "${SRC_A}")
run_or_die(${OMEGASLC} -t "${TEMPDIR}" -o "${OUTDIR}/link_b.omegasllib" "${SRC_B}")
run_or_die(${OMEGASLC} --link
    "${OUTDIR}/link_a.omegasllib" "${OUTDIR}/link_b.omegasllib"
    -o "${OUTDIR}/link_merged.omegasllib")
run_or_die(${ARCHIVE_TOOL} backend-variant
    "${OUTDIR}/link_a.omegasllib" "${OUTDIR}/link_a_variant.omegasllib")
