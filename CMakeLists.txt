cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Sampler C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vita Sampler")
set(VITA_TITLEID  "SMPL00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")

add_executable(${PROJECT_NAME} main.c)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceAppMgr_stub
  SceLibKernel_stub
  SceTouch_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCommonDialog_stub
  ScePgf_stub
  ScePvf_stub
  freetype
  png
  jpeg
  z
  bz2
  m
  c
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/fonts/DejaVuSansCondensed-Bold.ttf fonts/cond.ttf
  FILE ${CMAKE_SOURCE_DIR}/fonts/DejaVuSansMono-Bold.ttf fonts/mono.ttf
  FILE ${CMAKE_SOURCE_DIR}/assets/knobs.png assets/knobs.png
  FILE ${CMAKE_SOURCE_DIR}/assets/keys.png assets/keys.png
  FILE ${CMAKE_SOURCE_DIR}/assets/lamps.png assets/lamps.png
  FILE ${CMAKE_SOURCE_DIR}/assets/body.png assets/body.png
)
