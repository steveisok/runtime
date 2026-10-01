set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_definitions(-DFEATURE_NO_HOST)

include_directories(BEFORE ${VM_DIR})
include_directories(BEFORE ${VM_DIR}/${ARCH_SOURCES_DIR})
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${RUNTIME_DIR})

set(CORDBEE_SOURCES_DAC_AND_WKS
  controller.cpp
  debugger.cpp
  debuggermessagebox.cpp
  debuggermodule.cpp
  executioncontrol.cpp
  functioninfo.cpp
)

set(CORDBEE_HEADERS_DAC_AND_WKS
  controller.h
  controller.inl
  debugger.h
  debugger.inl
  executioncontrol.h
)

set(CORDBEE_SOURCES_WKS
  ${CORDBEE_SOURCES_DAC_AND_WKS}
  funceval.cpp
  rcthread.cpp
  canary.cpp
  shared.cpp
  frameinfo.cpp
  ${ARCH_SOURCES_DIR}/primitives.cpp
)

if(NOT CLR_CMAKE_TARGET_ARCH_WASM)
  list(APPEND CORDBEE_SOURCES_WKS dactable.cpp)
endif()

set(CORDBEE_HEADERS_WKS
  ${CORDBEE_HEADERS_DAC_AND_WKS}
  canary.h
  frameinfo.h
)

set(CORDBEE_SOURCES_DAC
  ${CORDBEE_SOURCES_DAC_AND_WKS}
)

set(CORDBEE_HEADERS_DAC
  ${CORDBEE_HEADERS_DAC_AND_WKS}
)

if(NOT CLR_CMAKE_TARGET_ARCH_WASM)
  list(APPEND CORDBEE_SOURCES_WKS ${ARCH_SOURCES_DIR}/walker.cpp)
endif()

if(FEATURE_INTERPRETER)
  list(APPEND CORDBEE_SOURCES_WKS interpreterwalker.cpp)
  list(APPEND CORDBEE_HEADERS_WKS interpreterwalker.h)
endif(FEATURE_INTERPRETER)

if (CLR_CMAKE_TARGET_ARCH_AMD64 OR CLR_CMAKE_TARGET_ARCH_I386)
  list(APPEND CORDBEE_SOURCES_WKS ${ARCH_SOURCES_DIR}/regdisplayhelper.cpp)
endif ()

convert_to_absolute_path(CORDBEE_SOURCES_DAC ${CORDBEE_SOURCES_DAC})
convert_to_absolute_path(CORDBEE_SOURCES_WKS ${CORDBEE_SOURCES_WKS})

if (CLR_CMAKE_TARGET_WIN32)
  convert_to_absolute_path(CORDBEE_HEADERS_DAC ${CORDBEE_HEADERS_DAC})
  convert_to_absolute_path(CORDBEE_HEADERS_WKS ${CORDBEE_HEADERS_WKS})
endif (CLR_CMAKE_TARGET_WIN32)

set(CORDBEE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT CLR_CMAKE_TARGET_ARCH_WASM)
  add_subdirectory(dac)
endif()
add_subdirectory(wks)
