# cgltf 1.15 (MIT, jkuhlmann/cgltf): glTF 2.0 / GLB parser, header kept
# verbatim. The viewer supplies its own file callbacks (Unicode paths).
add_library(cgltf STATIC cgltf.c cgltf.h)
target_include_directories(cgltf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(cgltf PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(cgltf PRIVATE /W0)
    target_compile_definitions(cgltf PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cgltf PRIVATE -w)
endif()
