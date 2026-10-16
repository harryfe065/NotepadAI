# tinyexr v1.0.12 (BSD-3-Clause, syoyo/tinyexr), last single-header release,
# kept verbatim. Inflate through the vendored miniz; block decode on threads.
find_package(Threads REQUIRED)
add_library(tinyexr STATIC tinyexr.cc tinyexr.h)
target_include_directories(tinyexr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tinyexr PUBLIC TINYEXR_USE_MINIZ=1 TINYEXR_USE_THREAD=1 TINYEXR_USE_OPENMP=0)
target_link_libraries(tinyexr PRIVATE miniz Threads::Threads)
set_target_properties(tinyexr PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(tinyexr PRIVATE /W0)
    target_compile_definitions(tinyexr PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(tinyexr PRIVATE -w)
endif()
