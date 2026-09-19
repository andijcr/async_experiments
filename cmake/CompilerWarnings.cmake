# Shared warning flags for est targets. Kept in one place so every target
# (library, tests, examples) opts in the same way instead of repeating flags.

function(est_set_warnings target)
  target_compile_options(${target}
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Werror
  )
endfunction()
