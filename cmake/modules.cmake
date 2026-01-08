
if(DEFINED MODULES)
  return()
endif()

find_package(FVAD)
find_package(MPA)

set(MODULES
  auloop
  autotest
  b2bua
  intercom
  kaoptions
  vidloop
  parcall
  qualify
  multicast
  redirect
  rtsp
  ebuacip
)

if(FVAD_FOUND)
  list(APPEND MODULES fvad)
endif()

if(MPA_FOUND)
  list(APPEND MODULES mpa)
endif()

if(DEFINED EXTRA_MODULES)
  list(APPEND MODULES ${EXTRA_MODULES})
endif()
