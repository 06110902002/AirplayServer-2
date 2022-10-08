cmake_minimum_required(VERSION 3.4.1)

add_subdirectory(lib/crypto)
add_subdirectory(lib/curve25519)
add_subdirectory(lib/ed25519)
add_subdirectory(lib/playfair)
add_subdirectory(lib/plist)
add_subdirectory(lib/fdk-aac)
add_subdirectory(lib)
add_subdirectory(mDNSResponder)

include_directories(lib
        lib/fdk-aac/libAACdec/include
        lib/fdk-aac/libAACenc/include
        lib/fdk-aac/libFDK/include
        lib/fdk-aac/libMpegTPDec/include
        lib/fdk-aac/libMpegTPEnc/include
        lib/fdk-aac/libPCMutils/include
        lib/fdk-aac/libSBRdec/include
        lib/fdk-aac/libSBRenc/include
        lib/fdk-aac/libSYS/include

        lib/fdk-aac/libArithCoding/include
        lib/fdk-aac/libDRCdec/include
        lib/fdk-aac/libSACdec/include
        lib/fdk-aac/libSACenc/include)

add_library(raop_server
        SHARED
        jni_raop_server.cpp)

find_library(log-lib
        log)

target_link_libraries(raop_server
        ${log-lib}
        play-lib
        jdns_sd
        ed25519
        fdk-aac)