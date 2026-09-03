# Clang source-based coverage (-fprofile-instr-generate -fcoverage-mapping)
# for est targets, gated behind EST_ENABLE_COVERAGE so normal builds pay
# no instrumentation cost. See docs/PLAN.md, "gate CI on new-code
# coverage".

option(EST_ENABLE_COVERAGE "Build est with Clang source-based coverage instrumentation" OFF)

if(EST_ENABLE_COVERAGE)
  # LLVM_PROFILE_FILE (set per the "coverage" CTest preset) doesn't create
  # its own parent directory - it just fails to write silently otherwise.
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/profraw")
endif()

function(est_enable_coverage target)
  if(EST_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
  endif()
endfunction()
