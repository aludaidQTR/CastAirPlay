cmake_minimum_required(VERSION 3.20)
project(AiCamNative LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "AiCam native Windows targets require Windows.")
endif()

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BASE_CLASSES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/directshow_baseclasses")
file(GLOB BASE_CLASSES_SOURCES CONFIGURE_DEPENDS "${BASE_CLASSES_DIR}/*.cpp")
add_library(aicam_strmbase STATIC ${BASE_CLASSES_SOURCES})
target_include_directories(aicam_strmbase PUBLIC "${BASE_CLASSES_DIR}")
target_compile_definitions(aicam_strmbase PRIVATE _LIB UNICODE _UNICODE)
target_compile_options(aicam_strmbase PRIVATE
  /W3 /EHsc /Zc:strictStrings- /wd4100 /wd4201 /wd4511 /wd4512)
target_link_libraries(aicam_strmbase PUBLIC strmiids winmm ole32 oleaut32)

add_library(aicam_shared STATIC src/shared_memory.cpp)
target_include_directories(aicam_shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_shared PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_shared PRIVATE /W4 /EHsc)
target_link_libraries(aicam_shared PUBLIC advapi32)

add_library(aicam_core SHARED
  src/protocol.cpp
  src/crypto.cpp
  src/handshake.cpp
  src/session_cipher.cpp)
target_include_directories(aicam_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_core PRIVATE AICAM_CORE_BUILD UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_core PRIVATE /W4 /EHsc)
target_link_libraries(aicam_core PRIVATE bcrypt)
set_target_properties(aicam_core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(aicam_dsvcam SHARED src/dshow_filter.cpp src/aicam_dsvcam.def)
target_include_directories(aicam_dsvcam PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${BASE_CLASSES_DIR}")
target_compile_definitions(aicam_dsvcam PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_dsvcam PRIVATE /W4 /EHsc /wd4100 /wd4201)
target_link_libraries(aicam_dsvcam PRIVATE
  aicam_shared
  aicam_strmbase
  strmiids
  ole32
  oleaut32
  advapi32
  gdi32
  user32)
set_target_properties(aicam_dsvcam PROPERTIES OUTPUT_NAME "aicam_dsvcam")

add_executable(aicam_pattern_producer src/pattern_producer.cpp)
target_include_directories(aicam_pattern_producer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_pattern_producer PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_pattern_producer PRIVATE /W4 /EHsc)
target_link_libraries(aicam_pattern_producer PRIVATE aicam_shared)

add_executable(aicam_dshow_probe src/dshow_probe.cpp)
target_compile_definitions(aicam_dshow_probe PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_dshow_probe PRIVATE /W4 /EHsc)
target_link_libraries(aicam_dshow_probe PRIVATE strmiids ole32 oleaut32)

add_executable(aicam_shared_memory_test src/shared_memory_test.cpp)
target_include_directories(aicam_shared_memory_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_shared_memory_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_shared_memory_test PRIVATE /W4 /EHsc)
target_link_libraries(aicam_shared_memory_test PRIVATE aicam_shared)
add_test(NAME aicam_shared_memory COMMAND aicam_shared_memory_test)

add_executable(aicam_core_test src/core_crypto_protocol_test.cpp)
target_include_directories(aicam_core_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_core_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_core_test PRIVATE /W4 /EHsc)
target_link_libraries(aicam_core_test PRIVATE aicam_core)
add_test(NAME aicam_core_crypto_protocol COMMAND aicam_core_test)
set_tests_properties(aicam_core_crypto_protocol PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(aicam_testsender src/test_sender.cpp)
target_include_directories(aicam_testsender PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(aicam_testsender PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(aicam_testsender PRIVATE /W4 /EHsc)
target_link_libraries(aicam_testsender PRIVATE aicam_core ws2_32)
add_test(NAME aicam_testsender_loopback COMMAND aicam_testsender)
