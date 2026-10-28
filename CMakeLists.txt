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
if(LEGATO_HAS_FFMPEG)
  add_library(ffmpeg_avformat STATIC IMPORTED)
  add_library(ffmpeg_avcodec STATIC IMPORTED)
  add_library(ffmpeg_swresample STATIC IMPORTED)
  add_library(ffmpeg_swscale STATIC IMPORTED)
  add_library(ffmpeg_avutil STATIC IMPORTED)
  set_target_properties(ffmpeg_avformat PROPERTIES
    IMPORTED_LOCATION "${LEGATO_AVFORMAT}"
    INTERFACE_INCLUDE_DIRECTORIES "${LEGATO_FFMPEG_PREFIX}/include")
  set_target_properties(ffmpeg_avcodec PROPERTIES
    IMPORTED_LOCATION "${LEGATO_AVCODEC}"
    INTERFACE_INCLUDE_DIRECTORIES "${LEGATO_FFMPEG_PREFIX}/include")
  set_target_properties(ffmpeg_swresample PROPERTIES
    IMPORTED_LOCATION "${LEGATO_SWRESAMPLE}"
    INTERFACE_INCLUDE_DIRECTORIES "${LEGATO_FFMPEG_PREFIX}/include")
  set_target_properties(ffmpeg_swscale PROPERTIES
    IMPORTED_LOCATION "${LEGATO_SWSCALE}"
    INTERFACE_INCLUDE_DIRECTORIES "${LEGATO_FFMPEG_PREFIX}/include")
  set_target_properties(ffmpeg_avutil PROPERTIES
    IMPORTED_LOCATION "${LEGATO_AVUTIL}"
    INTERFACE_INCLUDE_DIRECTORIES "${LEGATO_FFMPEG_PREFIX}/include")
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
  core/src/alloc_wrap.cpp
  io/src/file_source.cpp
  io/src/callback_source.cpp
    render/src/null_sink.cpp
    render/src/gl_sink.cpp
  api/src/abi.cpp
)
if(LEGATO_HAS_FFMPEG)
  target_sources(legato PRIVATE
    demux/src/ffmpeg_demuxer.cpp
    codec/src/ffmpeg_sw_decoder.cpp
  )
  target_compile_definitions(legato PRIVATE LEGATO_HAS_FFMPEG=1)
  target_link_libraries(legato PRIVATE
    ffmpeg_avformat
    ffmpeg_avcodec
    ffmpeg_swresample
    ffmpeg_swscale
    ffmpeg_avutil
  )
  if(NOT MSVC)
    target_link_libraries(legato PRIVATE m)
  endif()
endif()
find_path(LEGATO_CURL_INCLUDE curl/curl.h
  PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl-7.61.1/include" /usr/include)
find_library(LEGATO_CURL NAMES curl libcurl.so.4 PATHS /usr/lib64 /usr/lib)
if(LEGATO_CURL_INCLUDE AND LEGATO_CURL)
  set(LEGATO_HAS_CURL ON)
  target_sources(legato PRIVATE io/src/http_source.cpp)
  target_include_directories(legato PRIVATE "${LEGATO_CURL_INCLUDE}")
  target_compile_definitions(legato PRIVATE LEGATO_HAS_CURL=1)
  target_link_libraries(legato PRIVATE "${LEGATO_CURL}")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(legato PRIVATE render/src/pipewire_sink.cpp render/src/linux_audio.cpp)
  target_link_libraries(legato PRIVATE ${CMAKE_DL_LIBS})
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
    "-Wl,--no-undefined"
    "-Wl,--wrap=malloc"
    "-Wl,--wrap=calloc"
    "-Wl,--wrap=realloc"
    "-Wl,--wrap=posix_memalign")
endif()

execute_process(
  COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
  OUTPUT_VARIABLE LEGATO_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT LEGATO_GIT_COMMIT)
  set(LEGATO_GIT_COMMIT "unknown")
endif()

add_executable(legato-bench bench/src/bench_main.cpp)
target_include_directories(legato-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench/src)
target_compile_definitions(legato-bench PRIVATE
  LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  LEGATO_BUILD_COMMIT="${LEGATO_GIT_COMMIT}"
  LEGATO_BUILD_CONFIG="play-demo"
  LEGATO_FFMPEG_VERSION="5.1.6")
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

add_executable(legato_clock_tests tests/clock_tests.cpp engine/src/clock.cpp pal/src/clock.cpp)
target_include_directories(legato_clock_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/include
  ${CMAKE_CURRENT_SOURCE_DIR}/pal/include)
target_compile_options(legato_clock_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(legato_core_tests tests/core_tests.cpp)
target_compile_options(legato_core_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_core_tests PRIVATE legato_core)

add_executable(legato_parse_tests tests/parse_tests.cpp)
target_include_directories(legato_parse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench/src)
target_compile_definitions(legato_parse_tests PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_parse_tests PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME core COMMAND legato_core_tests)
add_test(NAME audio_clock COMMAND legato_clock_tests)
add_test(NAME parse COMMAND legato_parse_tests)
add_test(NAME smoke COMMAND legato_smoke)
add_executable(legato_quiet_tests tests/quiet_tests.c)
target_compile_definitions(legato_quiet_tests PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_quiet_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_quiet_tests PRIVATE legato)
add_test(NAME quiet_idle
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_quiet.py $<TARGET_FILE:legato_quiet_tests> idle)
add_test(NAME quiet_pause
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_quiet.py $<TARGET_FILE:legato_quiet_tests> pause
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(quiet_idle PROPERTIES TIMEOUT 20)
set_tests_properties(quiet_pause PROPERTIES TIMEOUT 60)
add_executable(legato_m05_tests tests/m05_tests.c)
target_compile_definitions(legato_m05_tests PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(legato_m05_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(legato_m05_tests PRIVATE legato)
add_test(NAME m05 COMMAND legato_m05_tests)
add_test(NAME hello COMMAND legato_hello)
add_test(NAME idlgen
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/idlgen/idlgen.py --check --root ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ffmpeg_import
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ffmpeg_import.py)
add_test(NAME symbols
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_symbols.sh $<TARGET_FILE:legato>)
add_test(NAME symbols_dumpbin
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_symbols.sh --from-dumpbin
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dumpbin_ok.txt)
add_test(NAME symbols_dumpbin_extra
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_symbols.sh --from-dumpbin
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dumpbin_extra.txt)
set_tests_properties(symbols_dumpbin_extra PROPERTIES WILL_FAIL TRUE)
add_test(NAME synthetic_dry_run
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/generate_synthetic.py --dry-run)
add_test(NAME synthetic_corpus_flags
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/generate_synthetic.py --check-command)
add_test(NAME t1_seek
  COMMAND legato-bench run bench/scenarios/seek-local.yaml --golden bench/golden/seek-local.json
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(t1_seek PROPERTIES TIMEOUT 120)
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
add_test(NAME t1_alloc_pcm
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_gate.py
          $<TARGET_FILE:legato-bench>
          bench/scenarios/startup-local.yaml
          bench/golden/startup-local.json
          alloc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME t1_av_sync_offset
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_gate.py
          $<TARGET_FILE:legato-bench>
          bench/scenarios/startup-local.yaml
          bench/golden/startup-local.json
          sync
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(t1_alloc_pcm PROPERTIES
  ENVIRONMENT "LEGATO_TEST_PCM_STEADY_ALLOC=1"
  TIMEOUT 120)
set_tests_properties(t1_av_sync_offset PROPERTIES
  ENVIRONMENT "LEGATO_TEST_AV_SYNC_OFFSET_US=100000"
  TIMEOUT 120)
if(LEGATO_HAS_FFMPEG)
  add_executable(legato_av_sync_file tests/av_sync_file.c)
  target_compile_definitions(legato_av_sync_file PRIVATE LEGATO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(legato_av_sync_file PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(legato_av_sync_file PRIVATE legato)
  find_library(LEGATO_EGL EGL)
  find_library(LEGATO_GL GL)
  if(LEGATO_EGL AND LEGATO_GL)
    target_compile_definitions(legato_av_sync_file PRIVATE LEGATO_HAS_EGL=1)
    target_link_libraries(legato_av_sync_file PRIVATE "${LEGATO_EGL}" "${LEGATO_GL}")
  endif()
  add_test(NAME av_sync_file
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/flash_beep.py $<TARGET_FILE:legato_av_sync_file> ok
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME av_sync_file_offset
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/flash_beep.py $<TARGET_FILE:legato_av_sync_file> offset
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(av_sync_file PROPERTIES
    ENVIRONMENT "LEGATO_TEST_NO_AUDIO_DEVICE=1"
    TIMEOUT 60)
  set_tests_properties(av_sync_file_offset PROPERTIES
    ENVIRONMENT "LEGATO_TEST_NO_AUDIO_DEVICE=1;LEGATO_TEST_AV_SYNC_OFFSET_US=100000"
    TIMEOUT 60)
  if(LEGATO_HAS_CURL)
    add_executable(legato_http_playback tests/http_playback.c)
    target_compile_options(legato_http_playback PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(legato_http_playback PRIVATE legato)
    add_test(NAME http_range
      COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/http_playback.py $<TARGET_FILE:legato_http_playback> range
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME http_plain
      COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/http_playback.py $<TARGET_FILE:legato_http_playback> plain
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME http_callback
      COMMAND legato_http_playback callback
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(http_range http_plain http_callback PROPERTIES
      ENVIRONMENT "LEGATO_TEST_NO_AUDIO_DEVICE=1"
      TIMEOUT 60)
  endif()
endif()
add_test(NAME python_abi
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bindings/python/legato.py $<TARGET_FILE:legato>)
if(LEGATO_HAS_FFMPEG)
  add_executable(legato_play examples/play.c)
  target_compile_options(legato_play PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(legato_play PRIVATE legato)
  set(SDL2_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdl2-install/lib64/cmake/SDL2")
  find_package(SDL2 QUIET CONFIG)
  if(TARGET SDL2::SDL2-static)
    target_compile_definitions(legato_play PRIVATE LEGATO_HAS_SDL2=1)
    target_link_libraries(legato_play PRIVATE SDL2::SDL2-static SDL2::SDL2main)
  endif()
  add_test(NAME play_tone
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/testdata/play_tone.py $<TARGET_FILE:legato_play>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
