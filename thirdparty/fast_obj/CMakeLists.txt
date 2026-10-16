# fast_obj 1.3 (MIT, thisistherk/fast_obj): Wavefront OBJ/MTL parser, header
# kept verbatim. The viewer reads through its own callbacks (Unicode paths,
# cancellation).
add_library(fast_obj STATIC fast_obj.c fast_obj.h)
target_include_directories(fast_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(fast_obj PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(fast_obj PRIVATE /W0)
    target_compile_definitions(fast_obj PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(fast_obj PRIVATE -w)
endif()
