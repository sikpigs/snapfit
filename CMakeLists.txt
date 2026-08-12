cmake_minimum_required(VERSION 3.28)

project(
    snapfit
    VERSION 0.1.0
    DESCRIPTION "A C++ project"
    LANGUAGES CXX
)

add_library(snapfit_lib INTERFACE)
target_include_directories(snapfit_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(snapfit_lib INTERFACE cxx_std_23)

add_executable(snapfit src/main.cpp)
target_link_libraries(snapfit PRIVATE snapfit_lib)

if(MSVC)
    target_compile_options(
        snapfit
        PRIVATE
            /W4
            /permissive-
            "$<$<CONFIG:Debug>:/Od;/Zi>"
            "$<$<CONFIG:Release>:/O3>"
    )
else()
    target_compile_options(
        snapfit
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wcast-align
            -Wconversion
            -Wdouble-promotion
            -Wextra-semi
            -Wformat=2
            -Wimplicit-fallthrough
            -Wmisleading-indentation
            -Wnon-virtual-dtor
            -Wnull-dereference
            -Wold-style-cast
            -Woverloaded-virtual
            -Wshadow
            -Wsign-conversion
            -Wundef
            "$<$<CONFIG:Debug>:-O0;-g>"
            "$<$<CONFIG:Release>:-O3>"
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(
            snapfit
            PRIVATE
                -Wduplicated-branches
                -Wduplicated-cond
                -Wlogical-op
                -Wuseless-cast
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            snapfit
            PRIVATE
                -Wcomma
                -Wdocumentation
                -Wheader-hygiene
        )
    endif()
endif()

include(CTest)

if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.15.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(snapfit_tests tests/registry_tests.cpp)
    target_link_libraries(snapfit_tests PRIVATE snapfit_lib Catch2::Catch2WithMain)

    include(Catch)
    catch_discover_tests(snapfit_tests)
endif()
