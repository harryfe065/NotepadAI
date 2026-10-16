# bcdec 0.985 (MIT / public domain, iOrange/bcdec): BC1-BC7 block decoders,
# header kept verbatim. BCDEC_BC4BC5_PRECISE is public: it changes the BC4/BC5
# signatures (signed SNORM support).
add_library(bcdec STATIC bcdec.c bcdec.h)
target_include_directories(bcdec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bcdec PUBLIC BCDEC_BC4BC5_PRECISE)
set_target_properties(bcdec PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bcdec PRIVATE /W0)
else()
    target_compile_options(bcdec PRIVATE -w)
endif()
