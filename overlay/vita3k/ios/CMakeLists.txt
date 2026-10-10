# iOS application shell for Vita3K.
#
# This target replaces the desktop (Qt) executable on iOS: it is a
# self-contained SDL3 launcher that lists installed titles and boots them
# through sdl_frontend::run, which renders with Vulkan. Since iOS has no
# native Vulkan driver, MoltenVK translates every Vulkan call to Metal — the
# only GPU API available on iOS. This is the same strategy Vita3K already uses
# on macOS.

add_executable(vita3k MACOSX_BUNDLE
    main_ios.cpp
    font.h
    ../interface.cpp
    ../interface.h)

target_include_directories(vita3k PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vita3k PRIVATE
    app audio compat config ctrl dialog display ime lang gdbstub gxm io miniz modules
    motion packages patch renderer sdl-frontend shader touch util psvpfsparser
    SDL3::SDL3)

# --- Metal rendering: MoltenVK translates the Vulkan API to Metal -----------
# The MoltenVK iOS release ships only static libraries
# (static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a) — there is no
# libMoltenVK.dylib for iOS, and iOS bundles must not embed dynamic libraries
# anyway. MOLTENVK_DYLIB (named after the macOS variable) therefore points at
# the static archive for iOS builds.
if(NOT DEFINED MOLTENVK_DYLIB)
    message(FATAL_ERROR "MOLTENVK_DYLIB was not set. The iOS build must be configured through "
        "the top-level project, which downloads MoltenVK in external/CMakeLists.txt.")
endif()
target_include_directories(vita3k PRIVATE "${CMAKE_BINARY_DIR}/external/MoltenVK/include")
target_link_libraries(vita3k PRIVATE "${MOLTENVK_DYLIB}")

# --- Apple frameworks used by SDL3, MoltenVK, ffmpeg and the shell ----------
target_link_libraries(vita3k PRIVATE
    "-framework UIKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework Foundation"
    "-framework CoreText"
    "-framework GameController"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework IOSurface"
    "-framework Security")

# MoltenVK 1.4.2 references MTLResidencySetDescriptor, a Metal class that only
# exists since iOS 17.0. Let the linker keep that symbol undefined (MoltenVK
# guards every use behind @available(iOS 17.0, *) at runtime) so the app keeps
# its iOS 15.0 deployment target and still links against the iOS 17 SDK.
target_link_options(vita3k PRIVATE "-Wl,-U,_OBJC_CLASS_$_MTLResidencySetDescriptor")

set_target_properties(vita3k PROPERTIES
    OUTPUT_NAME "Vita3K"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.vita3k.vita3k"
    MACOSX_BUNDLE_BUNDLE_NAME "Vita3K"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.2.1")

# iOS bundles have a flat layout (no Contents/), and the executable sits
# directly at the .app root, so TARGET_FILE_DIR is the bundle root.
add_custom_command(TARGET vita3k POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../../data" "$<TARGET_FILE_DIR:vita3k>/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../shaders-builtin" "$<TARGET_FILE_DIR:vita3k>/shaders-builtin")

# Ad-hoc signature so the bundle is structurally valid; a development or Apple
# ID signature is applied locally when sideloading (AltStore / Sideloadly).
add_custom_command(TARGET vita3k POST_BUILD
    COMMAND codesign --force --deep --sign - "$<TARGET_FILE_DIR:vita3k>")
