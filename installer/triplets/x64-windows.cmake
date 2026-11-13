set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

# MSVC 14.51 can crash while compiling Tesseract with precompiled headers.
if(PORT STREQUAL "tesseract")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON)
endif()

# Screen OCR consumes raw pixels and does not use the JPEG 2000 decoder.
if(PORT STREQUAL "leptonica")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DENABLE_OPENJPEG=OFF
        -DCMAKE_REQUIRE_FIND_PACKAGE_OpenJPEG=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenJPEG=ON
    )
endif()
