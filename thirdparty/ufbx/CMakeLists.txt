# ufbx 0.23.1 (MIT / public domain, ufbx/ufbx): FBX loader, sources kept
# verbatim. Loaded from memory, so stdio is only used for external files.
add_library(ufbx STATIC ufbx.c ufbx.h)
target_include_directories(ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(ufbx PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(ufbx PRIVATE /W0)
    target_compile_definitions(ufbx PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ufbx PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(ufbx PUBLIC m)
endif()
