cmake_minimum_required(VERSION 3.16)
project(pistar-dmr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# --- mbelib (AMBE+2 decoder), built from the vendored source -------------
set(MBE_DIR ${CMAKE_SOURCE_DIR}/third_party/mbelib)
add_library(mbe STATIC
  ${MBE_DIR}/ambe3600x2400.c ${MBE_DIR}/ambe3600x2450.c ${MBE_DIR}/ecc.c
  ${MBE_DIR}/imbe7100x4400.c ${MBE_DIR}/imbe7200x4400.c ${MBE_DIR}/mbelib.c)
target_include_directories(mbe PUBLIC ${MBE_DIR})
set_target_properties(mbe PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(mbe PRIVATE -w)

# --- embed remote_sniff.py so the binary is self-contained ------------------
file(READ ${CMAKE_SOURCE_DIR}/remote_sniff.py SNIFF_PY)
file(WRITE ${CMAKE_BINARY_DIR}/gen/remote_sniff.h
  "#pragma once\nstatic const char *kRemoteSniffPy = R\"PYSRC(${SNIFF_PY})PYSRC\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/remote_sniff.py)
file(READ ${CMAKE_SOURCE_DIR}/remote_inject.py INJECT_PY)
file(WRITE ${CMAKE_BINARY_DIR}/gen/remote_inject.h
  "#pragma once\nstatic const char *kRemoteInjectPy = R\"PYSRC(${INJECT_PY})PYSRC\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/remote_inject.py)
file(READ ${CMAKE_SOURCE_DIR}/remote_link.py LINK_PY)
file(WRITE ${CMAKE_BINARY_DIR}/gen/remote_link.h
  "#pragma once\nstatic const char *kRemoteLinkPy = R\"PYSRC(${LINK_PY})PYSRC\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/remote_link.py)
file(READ ${CMAKE_SOURCE_DIR}/remote_tx.py TX_PY)
file(WRITE ${CMAKE_BINARY_DIR}/gen/remote_tx.h
  "#pragma once\nstatic const char *kRemoteTxPy = R\"PYSRC(${TX_PY})PYSRC\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/remote_tx.py)

# --- OP25 AMBE/IMBE encoders (GPLv3), for synthetic test calls --------------
# OP25 carries its own modified mbelib, so everything except the op25voc_*
# wrapper is linked into one object and made local, keeping it apart from
# third_party/mbelib.
set(VOC_DIR ${CMAKE_SOURCE_DIR}/third_party/op25_vocoder)
file(GLOB VOC_IMBE ${VOC_DIR}/imbe_vocoder/*.cc)
add_library(op25voc_objs OBJECT
  ${VOC_DIR}/op25voc.cc ${VOC_DIR}/ambe_encoder.cc ${VOC_DIR}/ambe.c ${VOC_DIR}/mbelib.c
  ${VOC_DIR}/p25p2_vf.cc ${VOC_DIR}/rs.cc ${VOC_IMBE})
target_include_directories(op25voc_objs PRIVATE ${VOC_DIR} ${VOC_DIR}/imbe_vocoder)
set_target_properties(op25voc_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(op25voc_objs PRIVATE -w)
set(VOC_PRIVATE ${CMAKE_BINARY_DIR}/op25voc_private.o)
file(WRITE ${CMAKE_BINARY_DIR}/op25voc_keep.txt
  "op25voc_ambe_new\nop25voc_ambe_encode\nop25voc_ambe_free\nop25voc_imbe_new\nop25voc_imbe_encode\nop25voc_imbe_free\n")
add_custom_command(OUTPUT ${VOC_PRIVATE}
  COMMAND ${CMAKE_LINKER} -r -o ${CMAKE_BINARY_DIR}/op25voc_all.o $<TARGET_OBJECTS:op25voc_objs>
  COMMAND ${CMAKE_OBJCOPY} --keep-global-symbols=${CMAKE_BINARY_DIR}/op25voc_keep.txt
          ${CMAKE_BINARY_DIR}/op25voc_all.o ${VOC_PRIVATE}
  DEPENDS op25voc_objs $<TARGET_OBJECTS:op25voc_objs>
  COMMAND_EXPAND_LISTS
  COMMENT "Linking OP25 vocoder with private symbols")
set_source_files_properties(${VOC_PRIVATE} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(pistar-dmr
  src/main.cpp src/MainWindow.cpp src/MainWindow.h src/DmrDecoder.cpp src/DmrDecoder.h
  src/TestCall.cpp src/TestCall.h src/Settings.cpp src/Settings.h src/AudioOut.cpp src/AudioOut.h src/LinkDialog.cpp src/LinkDialog.h src/VoiceEncoder.cpp src/VoiceEncoder.h src/AudioIn.cpp src/AudioIn.h
  src/SettingsDialog.cpp src/SettingsDialog.h ${VOC_PRIVATE})
target_include_directories(pistar-dmr PRIVATE ${CMAKE_BINARY_DIR}/gen src)
find_package(Threads REQUIRED)
target_link_libraries(pistar-dmr PRIVATE Qt6::Widgets mbe m ${CMAKE_DL_LIBS} Threads::Threads)

# --- Codec2 (M17 voice), optional: install libcodec2-dev to enable M17 audio -
find_path(CODEC2_INCLUDE_DIR codec2/codec2.h)
find_library(CODEC2_LIBRARY codec2)
if(CODEC2_INCLUDE_DIR AND CODEC2_LIBRARY)
  message(STATUS "Codec2 found: M17 audio enabled")
  target_compile_definitions(pistar-dmr PRIVATE HAVE_CODEC2)
  target_include_directories(pistar-dmr PRIVATE ${CODEC2_INCLUDE_DIR})
  target_link_libraries(pistar-dmr PRIVATE ${CODEC2_LIBRARY})
else()
  message(STATUS "Codec2 not found: M17 calls are logged without audio")
endif()
