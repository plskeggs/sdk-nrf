#
# Generated using zcbor version 0.6.0
# https://github.com/NordicSemiconductor/zcbor
#

add_library(cbor_encode)
target_sources(cbor_encode PRIVATE
    C:\Python311\lib\zcbor\src\zcbor_decode.c
    C:\Python311\lib\zcbor\src\zcbor_encode.c
    C:\Python311\lib\zcbor\src\zcbor_common.c
    src\cbor_encode.c
    )
target_include_directories(cbor_encode PUBLIC
    src
    C:\Python311\lib\zcbor\include
    )
