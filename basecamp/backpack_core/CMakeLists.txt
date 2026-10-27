cmake_minimum_required(VERSION 3.14)
project(BackpackCoreModule LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT or ensure logos-module-builder is in your flake inputs.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
# HTTP to the Storage node and pointer hosts. Qt Network is part of qtbase and ships with Basecamp/logosctl.
find_package(Qt6 REQUIRED COMPONENTS Network)

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/backpack_core_impl.h
        src/backpack_core_impl.cpp
        src/bp_crypto.hpp
        src/bp_apk.hpp
        src/bp_http.hpp
        src/bp_repo.hpp
        src/bp_fdroid.hpp
        src/qrcodegen.hpp
        src/qrcodegen.cpp
    INCLUDE_DIRS src
    FIND_PACKAGES OpenSSL ZLIB
    LINK_LIBRARIES OpenSSL::Crypto ZLIB::ZLIB Qt6::Network
)
