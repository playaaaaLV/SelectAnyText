cmake_minimum_required(VERSION 3.21)
project(SelectAnyText LANGUAGES CXX RC)

add_executable(SelectAnyText WIN32
    main.cpp
    screen.cpp
    capture_region.cpp
    text_layout.cpp
    transfer.cpp
    multilingual_ocr.cpp
    settings.cpp
    settings_theme.cpp
    resources.rc
)
target_compile_features(SelectAnyText PRIVATE cxx_std_20)
target_compile_definitions(SelectAnyText PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SelectAnyText PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>)
target_link_options(SelectAnyText PRIVATE /MANIFEST:NO)
target_link_libraries(SelectAnyText PRIVATE advapi32 dwmapi gdi32 msimg32 ole32 shell32 user32)
set_property(TARGET SelectAnyText PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(GLOB ocr_models CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tessdata/*.traineddata")
add_custom_target(SelectAnyTextModels ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SelectAnyText>/tessdata"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ocr_models} "${CMAKE_CURRENT_SOURCE_DIR}/tessdata/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/tessdata/MODELS.json"
        "$<TARGET_FILE_DIR:SelectAnyText>/tessdata"
    VERBATIM
)
add_dependencies(SelectAnyText SelectAnyTextModels)

option(SELECTANYTEXT_BUILD_TESTS "Build core regression checks" OFF)
if(SELECTANYTEXT_BUILD_TESTS)
    enable_testing()
    add_executable(SelectAnyTextTests tests/core_tests.cpp text_layout.cpp settings.cpp capture_region.cpp)
    target_include_directories(SelectAnyTextTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(SelectAnyTextTests PRIVATE cxx_std_20)
    target_compile_definitions(SelectAnyTextTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(SelectAnyTextTests PRIVATE /W4 /utf-8)
    target_link_libraries(SelectAnyTextTests PRIVATE advapi32 user32)
    set_property(TARGET SelectAnyTextTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME core COMMAND SelectAnyTextTests)

    add_executable(SelectAnyTextOcrTests tests/ocr_tests.cpp transfer.cpp multilingual_ocr.cpp text_layout.cpp)
    target_include_directories(SelectAnyTextOcrTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(SelectAnyTextOcrTests PRIVATE cxx_std_20)
    target_compile_definitions(SelectAnyTextOcrTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(SelectAnyTextOcrTests PRIVATE /W4 /utf-8)
    target_link_libraries(SelectAnyTextOcrTests PRIVATE shell32 ole32 gdi32 user32)
    set_property(TARGET SelectAnyTextOcrTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME ocr COMMAND SelectAnyTextOcrTests)
    set_tests_properties(ocr PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_executable(SelectAnyTextRegionTests tests/region_tests.cpp screen.cpp capture_region.cpp text_layout.cpp)
    target_include_directories(SelectAnyTextRegionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(SelectAnyTextRegionTests PRIVATE cxx_std_20)
    target_compile_definitions(SelectAnyTextRegionTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(SelectAnyTextRegionTests PRIVATE /W4 /utf-8)
    target_link_libraries(SelectAnyTextRegionTests PRIVATE user32 gdi32 msimg32)
    set_property(TARGET SelectAnyTextRegionTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME region COMMAND SelectAnyTextRegionTests)
    set_tests_properties(region PROPERTIES TIMEOUT 30)
endif()
