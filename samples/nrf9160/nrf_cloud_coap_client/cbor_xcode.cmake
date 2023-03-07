#
# Generated using zcbor version 0.6.0
# https://github.com/NordicSemiconductor/zcbor
#

add_library(cbor_xcode)
target_sources(cbor_xcode PRIVATE
    C:\Python311\lib\zcbor\src\zcbor_decode.c
    C:\Python311\lib\zcbor\src\zcbor_encode.c
    C:\Python311\lib\zcbor\src\zcbor_common.c
    src\cbor_xcode.c
    )
target_include_directories(cbor_xcode PUBLIC
    C:\Python311\lib\zcbor\include

    src
    )
