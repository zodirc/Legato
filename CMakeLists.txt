cmake_minimum_required(VERSION 3.16)
project(legato VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fdiagnostics-color=always)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(FATAL_ERROR
    "GCC ${CMAKE_CXX_COMPILER_VERSION} cannot build this C++20 tree. "
    "Reconfigure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(legato_core STATIC
  core/src/log.cpp
  core/src/alloc.cpp
  core/src/actor.cpp
  pal/src/clock.cpp
  engine/src/scheduler.cpp
  engine/src/presets.gen.cpp
  engine/src/errors.gen.cpp
)
target_include_directories(legato_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
  ${CMAKE_CURRENT_SOURCE_DIR}/pal/include
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/io/include
  ${CMAKE_CURRENT_SOURCE_DIR}/demux/include
  ${CMAKE_CURRENT_SOURCE_DIR}/codec/include
  ${CMAKE_CURRENT_SOURCE_DIR}/render/include
)
if(MSVC)
  target_compile_options(legato_core PRIVATE /W4)
else()
  target_compile_options(legato_core PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
endif()
target_link_libraries(legato_core PUBLIC Threads::Threads)

set(LEGATO_FFMPEG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/dist" CACHE PATH
    "Prefix of a static LGPL FFmpeg built with play-demo or play-min")
set(LEGATO_HAS_FFMPEG OFF)
if(EXISTS "${LEGATO_FFMPEG_PREFIX}/lib/libavformat.a" OR EXISTS "${LEGATO_FFMPEG_PREFIX}/lib/avformat.lib")
  set(LEGATO_HAS_FFMPEG ON)
endif()
set(LEGATO_AVFORMAT "${LEGATO_FFMPEG_PREFIX}/lib/libavformat.a")
set(LEGATO_AVCODEC "${LEGATO_FFMPEG_PREFIX}/lib/libavcodec.a")
set(LEGATO_SWRESAMPLE "${LEGATO_FFMPEG_PREFIX}/lib/libswresample.a")
set(LEGATO_SWSCALE "${LEGATO_FFMPEG_PREFIX}/lib/libswscale.a")
set(LEGATO_AVUTIL "${LEGATO_FFMPEG_PREFIX}/lib/libavutil.a")
if(EXISTS "${LEGATO_FFMPEG_PREFIX}/lib/avformat.lib")
  set(LEGATO_AVFORMAT "${LEGATO_FFMPEG_PREFIX}/lib/avformat.lib")
  set(LEGATO_AVCODEC "${LEGATO_FFMPEG_PREFIX}/lib/avcodec.lib")
  set(LEGATO_SWRESAMPLE "${LEGATO_FFMPEG_PREFIX}/lib/swresample.lib")
  set(LEGATO_SWSCALE "${LEGATO_FFMPEG_PREFIX}/lib/swscale.lib")
  set(LEGATO_AVUTIL "${LEGATO_FFMPEG_PREFIX}/lib/avutil.lib")
endif()

add_library(legato SHARED
  engine/src/engine.cpp
  engine/src/player.cpp
  engine/src/file_playback.cpp
  engine/src/video_presenter.cpp
  engine/src/media_queue.cpp
  engine/src/clock.cpp
  engine/src/corpus.cpp
  engine/src/test_hooks.cpp
  io/src/file_source.cpp
  render/src/null_sink.cpp
  api/src/abi.cpp
)
if(LEGATO_HAS_FFMPEG)
  target_sources(legato PRIVATE
    demux/src/ffmpeg_demuxer.cpp
    codec/src/ffmpeg_sw_decoder.cpp
  )
  target_compile_definitions(legato PRIVATE LEGATO_HAS_FFMPEG=1)
  target_include_directories(legato PRIVATE "${LEGATO_FFMPEG_PREFIX}/include")
  target_link_libraries(legato PRIVATE
    "${LEGATO_AVFORMAT}"
    "${LEGATO_AVCODEC}"
    "${LEGATO_SWRESAMPLE}"
    "${LEGATO_SWSCALE}"
    "${LEGATO_AVUTIL}"
  )
  if(NOT MSVC)
    target_link_libraries(legato PRIVATE m)
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(LEGATO_ASOUND asound)
  if(LEGATO_HAS_FFMPEG AND LEGATO_ASOUND)
    target_sources(legato PRIVATE render/src/alsa_sink.cpp)
    target_compile_definitions(legato PRIVATE LEGATO_HAS_ALSA=1)
    target_link_libraries(legato PRIVATE "${LEGATO_ASOUND}")
  endif()
endif()
target_include_directories(legato PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(legato PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench/src)
target_compile_definitions(legato PRIVATE LEGATO_BUILDING)
if(MSVC)
  target_compile_options(legato PRIVATE /W4)
else()
  target_compile_options(legato PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(legato PRIVATE legato_core)
set_target_properties(legato PROPERTIES
  OUTPUT_NAME legato
  SOVERSION 0
  CXX_VISIBILITY_PRESET hidden
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

if(UNIX AND NOT APPLE)
  target_link_options(legato PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/liblegato.version"
    "-Wl,--no-undefined")
endif()

add_executable(legato-bench bench/src/bench_main.cpp)
target_include_directories(legato-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench/src)
target_compile_definitions(legato-bench PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato-bench PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato-bench PRIVATE legato)

add_executable(legato_hello examples/hello.c)
target_compile_definitions(legato_hello PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_hello PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_hello PRIVATE legato)

add_executable(legato_smoke tests/smoke.c)
target_compile_definitions(legato_smoke PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_smoke PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_smoke PRIVATE legato)

add_executable(legato_core_tests tests/core_tests.cpp)
target_compile_options(legato_core_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_core_tests PRIVATE legato_core)

add_executable(legato_parse_tests tests/parse_tests.cpp)
target_include_directories(legato_parse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench/src)
target_compile_definitions(legato_parse_tests PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_parse_tests PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME core COMMAND legato_core_tests)
add_test(NAME parse COMMAND legato_parse_tests)
add_test(NAME smoke COMMAND legato_smoke)
add_executable(legato_m05_tests tests/m05_tests.c)
target_compile_definitions(legato_m05_tests PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_m05_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_m05_tests PRIVATE legato)
add_test(NAME m05 COMMAND legato_m05_tests)
add_test(NAME hello COMMAND legato_hello)
add_test(NAME idlgen
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/idlgen/idlgen.py --check --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME symbols
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_symbols.sh $<TARGET_FILE:legato>)
add_test(NAME synthetic_dry_run
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/generate_synthetic.py --dry-run)
add_test(NAME t1_startup
  COMMAND legato-bench run bench/scenarios/startup-local.yaml --golden bench/golden/startup-local.json
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME t1_drop_frame
  COMMAND legato-bench run bench/scenarios/startup-local.yaml --golden bench/golden/startup-local.json
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(t1_drop_frame PROPERTIES
  ENVIRONMENT "LEGATO_TEST_DROP_VIDEO_FRAMES=1"
  WILL_FAIL TRUE
  TIMEOUT 120)
add_test(NAME t1_feed
  COMMAND legato-bench run bench/scenarios/feed-swipe-4g.yaml --golden bench/golden/feed-swipe-4g.json
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(t1_startup t1_feed PROPERTIES TIMEOUT 120)
add_test(NAME python_abi
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/legato.py $<TARGET_FILE:legato>)
if(LEGATO_HAS_FFMPEG)
  add_executable(legato_play examples/play.c)
  target_compile_options(legato_play PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(legato_play PRIVATE legato)
  add_test(NAME play_tone
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/play_tone.py $<TARGET_FILE:legato_play>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
