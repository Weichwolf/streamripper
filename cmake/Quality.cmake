set(STREAMRIPPER_SANITIZER "none" CACHE STRING "none, address-undefined, thread")
set_property(CACHE STREAMRIPPER_SANITIZER PROPERTY STRINGS none address-undefined thread)
include(CheckPIESupported)
check_pie_supported(LANGUAGES C)
add_library(sr_quality INTERFACE)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(sr_quality INTERFACE
    -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Werror=format-security
    -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wvla
    -fstack-protector-strong -fstack-clash-protection -fno-omit-frame-pointer
    $<$<CONFIG:Debug>:-O1> $<$<CONFIG:Release>:-O2>
    "$<$<CONFIG:Release>:-ffile-prefix-map=${PROJECT_SOURCE_DIR}=.>")
  target_compile_definitions(sr_quality INTERFACE _FORTIFY_SOURCE=3)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(sr_quality INTERFACE
      -Wl,--fatal-warnings -Wl,--no-undefined -Wl,--as-needed
      -Wl,-z,relro,-z,now,-z,noexecstack)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
      target_compile_options(sr_quality INTERFACE -fcf-protection=full)
    endif()
  endif()
  if(STREAMRIPPER_SANITIZER STREQUAL "address-undefined")
    set(sr_sanitizers address,undefined)
  elseif(STREAMRIPPER_SANITIZER STREQUAL "thread")
    set(sr_sanitizers thread)
  elseif(NOT STREAMRIPPER_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported sanitizer: ${STREAMRIPPER_SANITIZER}")
  endif()
  if(DEFINED sr_sanitizers)
    target_compile_options(sr_quality INTERFACE -fsanitize=${sr_sanitizers} -fno-sanitize-recover=all -g)
    target_link_options(sr_quality INTERFACE -fsanitize=${sr_sanitizers})
  endif()
endif()
