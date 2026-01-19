project(fvad)

find_package(FVAD REQUIRED)

if(NOT FVAD_FOUND)
  return()
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS fvad.c)

if(STATIC)
  add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
  add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${FVAD_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${FVAD_LIBRARIES})
