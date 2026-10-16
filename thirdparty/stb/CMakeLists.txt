# stb_image v2.30 (public domain / MIT, nothings/stb), header kept verbatim.
# Built as its own C library so it never enters the NotepadAI C++ PCH.
add_library(stb_image STATIC stb_image.c stb_image.h)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(stb_image PUBLIC STBI_NO_STDIO STBI_ONLY_TGA STBI_ONLY_PSD STBI_ONLY_HDR)
set_target_properties(stb_image PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(stb_image PRIVATE /W0)
else()
    target_compile_options(stb_image PRIVATE -w)
endif()
