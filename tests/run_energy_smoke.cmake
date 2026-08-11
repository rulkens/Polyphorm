# Generates a synthetic dataset in a fresh dir and runs the sim headless.
# polyphorm exits nonzero if energy did not rise — that IS the assertion.
set(WORK ${CMAKE_CURRENT_BINARY_DIR}/energy_smoke_work)
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})
execute_process(COMMAND ${GEN} ${WORK} RESULT_VARIABLE r1)
if(NOT r1 EQUAL 0)
  message(FATAL_ERROR "dataset generator failed: ${r1}")
endif()
execute_process(COMMAND ${APP} --headless 400 --dataset ${WORK}/testdata
                WORKING_DIRECTORY ${WORK} RESULT_VARIABLE r2)
if(NOT r2 EQUAL 0)
  message(FATAL_ERROR "energy smoke failed (exit ${r2}) — energy not rising or startup failure")
endif()
