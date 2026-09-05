# Clang AddressSanitizer + UndefinedBehaviorSanitizer for est targets,
# gated behind EST_ENABLE_SANITIZERS so normal builds pay no
# instrumentation cost. Support is checked, not assumed: a toolchain
# whose compiler doesn't understand -fsanitize=address,undefined should
# get a clear configure-time warning and an uninstrumented build, not an
# obscure link failure or a silently-untested "pass".

option(EST_ENABLE_SANITIZERS "Build est with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(EST_ENABLE_SANITIZERS)
  include(CheckCXXCompilerFlag)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
  check_cxx_compiler_flag("-fsanitize=address,undefined" EST_COMPILER_SUPPORTS_SANITIZERS)
  unset(CMAKE_REQUIRED_FLAGS)

  if(NOT EST_COMPILER_SUPPORTS_SANITIZERS)
    message(WARNING
      "EST_ENABLE_SANITIZERS is ON but ${CMAKE_CXX_COMPILER_ID} "
      "${CMAKE_CXX_COMPILER_VERSION} doesn't accept "
      "-fsanitize=address,undefined - building without sanitizer "
      "instrumentation."
    )
  endif()
endif()

function(est_enable_sanitizers target)
  if(EST_ENABLE_SANITIZERS AND EST_COMPILER_SUPPORTS_SANITIZERS)
    # -fno-sanitize-recover=all: abort on the first violation instead of
    # printing a diagnostic and continuing - a sanitizer finding should
    # fail the test binary's exit code (and so the CI gate), not just its
    # stderr. -fno-omit-frame-pointer: both sanitizers' stack traces need
    # it to symbolize correctly.
    target_compile_options(${target} PRIVATE
      -fsanitize=address,undefined
      -fno-sanitize-recover=all
      -fno-omit-frame-pointer
    )
    target_link_options(${target} PRIVATE
      -fsanitize=address,undefined
      -fno-sanitize-recover=all
    )
  endif()
endfunction()
