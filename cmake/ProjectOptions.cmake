# Attach a consistent, high-signal warning set to an interface target.
# Every real target that links this interface target inherits these options.
function(kc_enable_warnings target warnings_as_errors)
  if(MSVC)
    target_compile_options(${target} INTERFACE /W4 /permissive-)
    if(warnings_as_errors)
      target_compile_options(${target} INTERFACE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wold-style-cast
    )
    if(warnings_as_errors)
      target_compile_options(${target} INTERFACE -Werror)
    endif()
  endif()
endfunction()

# Enable runtime checks for invalid memory access and undefined C++ behavior.
# Sanitizers are developer/test instrumentation, not application features.
function(kc_enable_sanitizers target)
  if(MSVC)
    message(WARNING "KC_ENABLE_SANITIZERS is not configured for MSVC")
    return()
  endif()

  target_compile_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${target} INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
endfunction()
