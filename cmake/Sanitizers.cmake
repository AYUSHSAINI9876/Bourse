# Sanitizer wiring.
#
# Usage:  cmake -B build -DBOURSE_SANITIZER=address
#         cmake -B build -DBOURSE_SANITIZER=thread
#         cmake -B build -DBOURSE_SANITIZER=undefined
#
# ASan and TSan are mutually exclusive, hence a single-choice variable rather
# than independent booleans. CI runs the full test suite under each in turn.

set(BOURSE_SANITIZER "none" CACHE STRING "Sanitizer: none|address|thread|undefined|address+undefined")
set_property(CACHE BOURSE_SANITIZER PROPERTY STRINGS none address thread undefined address+undefined)

function(bourse_enable_sanitizers target)
  if(BOURSE_SANITIZER STREQUAL "none")
    return()
  endif()

  if(MSVC)
    if(BOURSE_SANITIZER MATCHES "address")
      target_compile_options(${target} PRIVATE /fsanitize=address)
    endif()
    return()
  endif()

  set(flags "")
  if(BOURSE_SANITIZER STREQUAL "address")
    list(APPEND flags -fsanitize=address)
  elseif(BOURSE_SANITIZER STREQUAL "thread")
    list(APPEND flags -fsanitize=thread)
  elseif(BOURSE_SANITIZER STREQUAL "undefined")
    list(APPEND flags -fsanitize=undefined -fno-sanitize-recover=all)
  elseif(BOURSE_SANITIZER STREQUAL "address+undefined")
    list(APPEND flags -fsanitize=address,undefined -fno-sanitize-recover=all)
  else()
    message(FATAL_ERROR "Unknown BOURSE_SANITIZER value: ${BOURSE_SANITIZER}")
  endif()

  list(APPEND flags -fno-omit-frame-pointer -g)
  target_compile_options(${target} PRIVATE ${flags})
  target_link_options(${target} PRIVATE ${flags})
endfunction()
