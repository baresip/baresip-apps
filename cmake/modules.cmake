
if(DEFINED MODULES)
  return()
endif()

find_package(FVAD)

set(MODULES
  auloop
  autotest
  b2bua
  intercom
  kaoptions
  vidloop
  parcall
  qualify
)

if(FVAD_FOUND)
  list(APPEND MODULES fvad)
endif()

if(DEFINED EXTRA_MODULES)
  list(APPEND MODULES ${EXTRA_MODULES})
endif()
