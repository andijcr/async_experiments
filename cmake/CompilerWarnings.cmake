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
      # Not implied by -Wall/-Wextra: Clang's consumed-analysis (typestate)
      # warning, verifiably opt-in per type via [[clang::consumable]] -
      # est::extraction<T> (est/src/future.cppm) is the one type in this
      # codebase annotated with it, so enabling this repo-wide only ever
      # fires against that one type's own callers, not general code (issue
      # #116). Under -Werror below like everything else here; if a future
      # false positive from Clang's own (documented as not fully mature)
      # implementation ever blocks an otherwise-correct build, narrow this
      # flag to est_tests specifically rather than dropping -Werror.
      -Wconsumed
      -Werror
  )
endfunction()
