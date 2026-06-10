cmake_minimum_required(VERSION 3.22)

project(CookieLinkRelay VERSION 1.0.0 LANGUAGES C CXX)

if (WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(COOKIELINK_RELAY_ENABLE_TLS "Enable TLS for relay/client (requires OpenSSL)" OFF)
if (COOKIELINK_RELAY_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

set(CMAKE_XCODE_GENERATE_SCHEME OFF)
set(CMAKE_SUPPRESS_REGENERATION true)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Targets")

add_subdirectory(deps/juce EXCLUDE_FROM_ALL)

juce_add_console_app(CookieLinkRelay
    PRODUCT_NAME "cookielink-relay"
)

set(COOKIE_RELAY_AOO_ROOT deps/aoo)

set(COOKIE_RELAY_AOO_SOURCE_FILES
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/SLIP.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/client.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/client.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/codec_pcm.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/common.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/common.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/lockfree.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/net_utils.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/net_utils.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/server.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/server.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/sink.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/sink.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/source.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/source.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/sync.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/sync.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/time.cpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/time.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/src/time_dll.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo.h
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo_net.h
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo_net.hpp
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo_pcm.h
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo_types.h
    ${COOKIE_RELAY_AOO_ROOT}/lib/aoo/aoo_utils.hpp
    ${COOKIE_RELAY_AOO_ROOT}/deps/md5/md5.c
    ${COOKIE_RELAY_AOO_ROOT}/deps/md5/md5.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscOutboundPacketStream.cpp
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscPrintReceivedElements.cpp
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscReceivedElements.cpp
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscTypes.cpp
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/MessageMappingOscPacketListener.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscException.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscHostEndianness.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscOutboundPacketStream.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscPacketListener.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscPrintReceivedElements.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscReceivedElements.h
    ${COOKIE_RELAY_AOO_ROOT}/deps/oscpack/osc/OscTypes.h
)

target_sources(CookieLinkRelay PRIVATE
    server/TcpRelayServer.cpp
    ${COOKIE_RELAY_AOO_SOURCE_FILES}
)

target_include_directories(CookieLinkRelay PRIVATE
    Source
    ${COOKIE_RELAY_AOO_ROOT}/lib
    ${COOKIE_RELAY_AOO_ROOT}/deps
)

target_compile_features(CookieLinkRelay PRIVATE cxx_std_17)

target_compile_definitions(CookieLinkRelay PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    AOO_TIMEFILTER_CHECK=0
    AOO_STATIC
    USE_CODEC_OPUS=0
)

target_link_libraries(CookieLinkRelay PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)

if (COOKIELINK_RELAY_ENABLE_TLS)
    target_compile_definitions(CookieLinkRelay PRIVATE COOKIELINK_RELAY_USE_OPENSSL=1)
    target_link_libraries(CookieLinkRelay PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
