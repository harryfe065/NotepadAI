cmake_minimum_required(VERSION 3.21)

project(NotepadAI
    VERSION 0.14
    DESCRIPTION "Cross-platform text editor"
    LANGUAGES C CXX
)

set(APP_DISTRIBUTION "" CACHE STRING "Distribution type")
string(TIMESTAMP CURRENT_YEAR "%Y")
set(APP_COPYRIGHT "Copyright 2019-${CURRENT_YEAR} Justin Dailey, modifications ${CURRENT_YEAR} nullmastermind")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(SCI_OWNREGEX)

if(MSVC)
    # https://stackoverflow.com/questions/78598141/first-stdmutexlock-crashes-in-application-built-with-latest-visual-studio
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    add_compile_options(/guard:cf)
    add_link_options(/guard:cf)
    # Emit PDB in all configs so Sentry can symbolicate release crash reports.
    add_compile_options(/Zi)
    add_link_options(/DEBUG:FULL /OPT:REF /OPT:ICF)
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang++ (GNU driver, MSVC ABI) leaves MSVC unset, so the block above is
    # skipped and Release links without a PDB. CodeView only changes the
    # symbols, not codegen; /DEBUG turns off /OPT:REF,ICF unless re-enabled.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-g> $<$<COMPILE_LANGUAGE:C,CXX>:-gcodeview>)
    add_link_options(LINKER:/DEBUG:FULL LINKER:/OPT:REF LINKER:/OPT:ICF)
endif()

if(UNIX AND NOT APPLE)
    add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2 -g)
    add_link_options(-Wl,-z,relro -Wl,-z,now)
elseif(APPLE)
    add_compile_options(-g)
endif()

find_package(Qt6 6.5
    REQUIRED
        COMPONENTS
            Core
            Gui
            Widgets
            Network
            PrintSupport
            Core5Compat
            Concurrent
            Svg
            OpenGL
)

option(NOTEPADAI_BUILD_TESTS "Build unit tests" OFF)
if(NOTEPADAI_BUILD_TESTS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    enable_testing()
endif()

set(CMAKE_AUTORCC ON)
qt_standard_project_setup()

set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM source cache directory")

add_subdirectory(thirdparty)
add_subdirectory(src)

if(NOTEPADAI_BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Packaging.cmake)
