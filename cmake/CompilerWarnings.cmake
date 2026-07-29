# Centralised warning configuration.
#
# Rationale: warnings are the cheapest static analysis available. Bourse builds
# clean at a high warning level on both GCC and Clang; CI additionally flips
# BOURSE_WERROR=ON so regressions cannot land.

option(BOURSE_STRICT_WARNINGS "Enable the pedantic conversion/cast warning set" OFF)

function(bourse_set_warnings target)
  # The baseline set. Bourse builds clean against this on GCC and Clang, which
  # is why CI can run it with -Werror.
  set(gcc_like_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wformat=2
      -Wimplicit-fallthrough
      -Wnull-dereference)

  # Opt-in extras. These are valuable but noisy against <cstdint> arithmetic and
  # platform headers, so they are a deliberate review tool rather than a gate.
  if(BOURSE_STRICT_WARNINGS)
    list(APPEND gcc_like_warnings
         -Wconversion
         -Wsign-conversion
         -Wold-style-cast
         -Wdouble-promotion
         -Wuseless-cast)
  endif()

  set(msvc_warnings /W4 /permissive- /w14640 /w14826)

  if(MSVC)
    target_compile_options(${target} PRIVATE ${msvc_warnings})
    if(BOURSE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE ${gcc_like_warnings})
    if(BOURSE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
