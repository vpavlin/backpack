# Reproducible recipe to rebuild liblogos_storage_jni.so when libstorage.so updates.
# (The app ships the prebuilt .so in jniLibs, like liblogosdelivery — no app-time CMake.)
# Manual equivalent used in the spike:
#   $NDK/.../bin/aarch64-linux-android30-clang -shared -fPIC -O2 -I. logos_storage_ffi.c \
#     -L../arm64-v8a -lstorage -llog -o ../arm64-v8a/liblogos_storage_jni.so
cmake_minimum_required(VERSION 3.18)
project(logos_storage_jni C)
add_library(storage SHARED IMPORTED)
set_target_properties(storage PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../${ANDROID_ABI}/libstorage.so)
add_library(logos_storage_jni SHARED logos_storage_ffi.c)
target_include_directories(logos_storage_jni PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
find_library(log-lib log)
target_link_libraries(logos_storage_jni storage ${log-lib})
