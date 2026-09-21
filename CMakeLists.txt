cmake_minimum_required(VERSION 3.25)

# Let individual zero-CRT targets opt out of MSVC's Debug /RTC runtime.
if(POLICY CMP0184)
    cmake_policy(SET CMP0184 NEW)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Tools3000 — 高性能桌面效率工具 (Microkernel Architecture)
# ─────────────────────────────────────────────────────────────────────────────

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TOOLS3000_VERSION)
string(STRIP "${TOOLS3000_VERSION}" TOOLS3000_VERSION)
if(NOT TOOLS3000_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain exactly one stable SemVer value (for example 1.2.3)")
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed" CACHE PATH "Path to vcpkg installed directory")
endif()

project(Tools3000
    VERSION ${TOOLS3000_VERSION}
    DESCRIPTION "High-Performance Modular Windows Productivity Tool"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(TOOLS3000_ENABLE_MSVC_ANALYSIS "Run MSVC static analysis on security-sensitive targets" OFF)

if(MSVC)
    add_compile_options(/FS /utf-8)
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/core/Version.h.in"
    "${CMAKE_BINARY_DIR}/generated/Tools3000Version.h"
    @ONLY
)

# Generate Windows VERSIONINFO from the same project() version used by the UI,
# native APIs and installer. Keeping this as a function lets every shipped
# binary expose an accurate description and original filename in Explorer.
string(TIMESTAMP TOOLS3000_COPYRIGHT_YEAR "%Y" UTC)
function(tools3000_add_version_resource TARGET DESCRIPTION ORIGINAL_FILENAME FILE_TYPE)
    set(TOOLS3000_FILE_DESCRIPTION "${DESCRIPTION}")
    set(TOOLS3000_ORIGINAL_FILENAME "${ORIGINAL_FILENAME}")
    set(TOOLS3000_FILE_TYPE "${FILE_TYPE}")
    set(TOOLS3000_VERSION_RESOURCE
        "${CMAKE_BINARY_DIR}/generated/${TARGET}_version.rc")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/VersionInfo.rc.in"
        "${TOOLS3000_VERSION_RESOURCE}"
        @ONLY
    )
    target_sources(${TARGET} PRIVATE "${TOOLS3000_VERSION_RESOURCE}")
endfunction()

# Generate a metadata sidecar next to each plugin DLL. The host reads this file
# before LoadLibrary, so disabled or incompatible plugins never map their heavy
# dependencies into the process.
function(tools3000_add_plugin_manifest TARGET)
    set(MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/bin/plugins/${TARGET}.plugin.json")
    configure_file(
        "${CMAKE_SOURCE_DIR}/plugins/${TARGET}.plugin.json.in"
        "${MANIFEST_OUTPUT}"
        @ONLY
    )
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MANIFEST_OUTPUT}"
            "$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.plugin.json"
        COMMENT "Deploying ${TARGET} plugin manifest..."
    )
    install(FILES "${MANIFEST_OUTPUT}" DESTINATION bin/plugins)
endfunction()

# ── 输出目录 ──────────────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── 编译选项 ──────────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /utf-8 /W4 /WX- /permissive- /Zc:__cplusplus /MP /Z7
        /external:W0 /external:anglebrackets
    )
    add_link_options(/DEBUG)
    add_compile_definitions(
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    )
endif()

# ── 依赖库 ───────────────────────────────────────────────────────────────────
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(OpenCV CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)

set(WEBVIEW2_VERSION "1.0.4022.49" CACHE STRING "Pinned Microsoft.Web.WebView2 SDK version")
set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2.${WEBVIEW2_VERSION}")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
    message(FATAL_ERROR
        "Microsoft.Web.WebView2 ${WEBVIEW2_VERSION} is missing. Run deploy.ps1; CMake never downloads or executes build tools.")
endif()

# ── 文件收集 ──────────────────────────────────────────────────────────────────
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "src/core/*.cpp" "src/core/*.h")
file(GLOB_RECURSE GESTURE_SOURCES CONFIGURE_DEPENDS "src/gesture/*.cpp" "src/gesture/*.h")
file(GLOB_RECURSE CAPTURE_SOURCES CONFIGURE_DEPENDS "src/capture/*.cpp" "src/capture/*.h")
file(GLOB_RECURSE KEYCAST_SOURCES CONFIGURE_DEPENDS "src/keycast/*.cpp" "src/keycast/*.h")
file(GLOB_RECURSE TRAY_SOURCES CONFIGURE_DEPENDS "src/tray/*.cpp" "src/tray/*.h")
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "src/ui/*.cpp" "src/ui/*.h")
file(GLOB_RECURSE OCR_SOURCES CONFIGURE_DEPENDS "src/ocr/*.cpp" "src/ocr/*.h")
file(GLOB_RECURSE SEARCH_SOURCES CONFIGURE_DEPENDS "src/search/*.cpp" "src/search/*.h")
file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS "src/service/*.cpp" "src/service/*.h")
file(GLOB_RECURSE DIALOG_SOURCES CONFIGURE_DEPENDS "src/dialog/*.cpp" "src/dialog/*.h")

set(HOST_SOURCES src/main.cpp ${TRAY_SOURCES} ${UI_SOURCES} src/Tools3000.rc)
set_property(SOURCE src/Tools3000.rc PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/resources/app.ico"
    "${CMAKE_SOURCE_DIR}/resources/tray.ico"
    "${CMAKE_SOURCE_DIR}/resources/tray_dark.ico"
    "${CMAKE_SOURCE_DIR}/resources/tray_color.ico"
)

# ── 1. Tools3000Core (动态库) ───────────────────────────────────────────────────────
add_library(Tools3000Core SHARED ${CORE_SOURCES})
tools3000_add_version_resource(Tools3000Core "Tools3000 Core Runtime" "Tools3000Core.dll" VFT_DLL)
target_compile_definitions(Tools3000Core PRIVATE TOOLS3000CORE_EXPORTS)
target_include_directories(Tools3000Core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${LUA_INCLUDE_DIR}
)
target_link_libraries(Tools3000Core PUBLIC
    spdlog::spdlog nlohmann_json::nlohmann_json sol2::sol2 ${LUA_LIBRARIES} fmt::fmt
    user32 shell32 advapi32 dbghelp psapi shlwapi winhttp crypt32 uiautomationcore oleaut32 ole32 dwmapi gdiplus imm32 shcore propsys
    d2d1 dwrite d3d11 dxgi dcomp
)

# ── OBJECT 库定义 (消除 43 个核心源文件在单测中的二次编译) ───────────────────
# 1. capture_objects (15 个源文件)
set(CAPTURE_OBJECT_SOURCES
    src/capture/AudioCapture.cpp
    src/capture/CaptureBackend.cpp
    src/capture/CaptureToolbarLayout.cpp
    src/capture/CaptureDropdownMenu.cpp
    src/capture/CaptureVectorIcons.cpp
    src/capture/FloatingGlassBar.cpp
    src/capture/CornerRadiusHelper.cpp
    src/capture/BeautyShell.cpp
    src/capture/MarkupEngine.cpp
    src/capture/CursorOverlay.cpp
    src/capture/ScreenRecorder.cpp
    src/capture/ClipboardUtils.cpp
    src/capture/RecordingIndicator.cpp
    src/capture/RecordingGpuProbe.cpp
    src/capture/ScrollCapture.cpp
    src/capture/ShortcutHintOverlay.cpp
    src/capture/HudAvoidanceEngine.cpp
    src/capture/PinSpawnCalculator.cpp
    src/capture/PinPasteCoordinator.cpp
    src/capture/CaptureHistory.cpp
)
add_library(capture_objects OBJECT ${CAPTURE_OBJECT_SOURCES})
target_include_directories(capture_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(FFMPEG_INCLUDE_DIR)
    target_include_directories(capture_objects PUBLIC ${FFMPEG_INCLUDE_DIR})
endif()
if(OpenCV_FOUND)
    target_include_directories(capture_objects PUBLIC ${OpenCV_INCLUDE_DIRS})
endif()
if(MSVC)
    target_compile_options(capture_objects PRIVATE /bigobj)
endif()
target_link_libraries(capture_objects PUBLIC Tools3000Core ${OpenCV_LIBS}
    ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY}
    d2d1 dwrite d3d11 dxgi dxguid dwmapi mfplat mfreadwrite mfuuid
    shlwapi shell32 user32 gdi32 ole32 oleaut32 advapi32 comctl32 windowscodecs winhttp windowsapp Shcore imm32
)

# 2. gesture_objects (8 个源文件)
set(GESTURE_OBJECT_SOURCES
    src/gesture/GestureRecognizer.cpp
    src/gesture/ScopeRule.cpp
    src/gesture/GestureAction.cpp
    src/gesture/GestureProfile.cpp
    src/gesture/BuiltinCommands.cpp
    src/gesture/HotCornerEngine.cpp
    src/gesture/MouseHook.cpp
    src/gesture/GesturePacer.cpp
)
add_library(gesture_objects OBJECT ${GESTURE_OBJECT_SOURCES})
target_include_directories(gesture_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gesture_objects PUBLIC Tools3000Core
    d2d1 dwrite windowscodecs user32 shell32 shcore dwmapi dcomp d3d11 dxgi winmm
)

# 3. service_objects (12 个源文件)
set(SERVICE_OBJECT_SOURCES
    src/service/PinyinEngine.cpp
    src/service/SearchExpression.cpp
    src/service/FileIndexStore.cpp
    src/service/MftParser.cpp
    src/service/db/RunHistoryManager.cpp
    src/service/db/SearchHistoryManager.cpp
    src/service/db/DatabaseManager.cpp
    src/service/content/PlainTextExtractor.cpp
    src/service/content/ZipXmlExtractor.cpp
    src/service/content/PsdAiExtractor.cpp
    src/service/content/DxfExtractor.cpp
    src/service/content/ContentSearchEngine.cpp
)
add_library(service_objects OBJECT ${SERVICE_OBJECT_SOURCES})
target_include_directories(service_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(service_objects PUBLIC spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json ZLIB::ZLIB advapi32 user32 shell32 ole32)

# 4. dialog_objects (3 个源文件)
set(DIALOG_OBJECT_SOURCES
    src/dialog/PathMemoryManager.cpp
    src/dialog/ExplorerTracker.cpp
    src/dialog/DialogNavigator.cpp
)
add_library(dialog_objects OBJECT ${DIALOG_OBJECT_SOURCES})
target_include_directories(dialog_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dialog_objects PUBLIC Tools3000Core
    d2d1 dwrite msimg32 user32 shell32 shcore ole32 oleaut32 shlwapi UIAutomationCore
)

# 5. keycast_objects (1 个源文件)
set(KEYCAST_OBJECT_SOURCES
    src/keycast/KeycastOverlay.cpp
)
add_library(keycast_objects OBJECT ${KEYCAST_OBJECT_SOURCES})
target_include_directories(keycast_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(keycast_objects PUBLIC Tools3000Core
    d2d1 dwrite msimg32 user32 shell32 shcore dwmapi
)

# 6. ui_objects (5 个源文件)
set(UI_OBJECT_SOURCES
    src/ui/WebViewSuspend.cpp
    src/ui/SpotlightOverlay.cpp
    src/ui/SpotlightOverlayInputController.cpp
    src/ui/SpotlightOverlayRenderer.cpp
    src/tray/TrayIcon.cpp
)
add_library(ui_objects OBJECT ${UI_OBJECT_SOURCES})
target_include_directories(ui_objects PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(EXISTS "${WEBVIEW2_DIR}")
    target_include_directories(ui_objects PUBLIC "${WEBVIEW2_DIR}/build/native/include")
endif()
target_link_libraries(ui_objects PUBLIC Tools3000Core
    shlwapi shell32 user32 gdi32 ole32 oleaut32 advapi32 comctl32 shcore d2d1 dwrite wtsapi32 dwmapi
)

# ── 2. Plugin_Gesture (动态库) ────────────────────────────────────────────────
set(GESTURE_OTHER_SOURCES
    src/gesture/GestureEngine.cpp
    src/gesture/GestureDispatchWorker.cpp
    src/gesture/GestureTrailOverlay.cpp
    src/gesture/RadialMenuOverlay.cpp
    src/gesture/PluginEntry.cpp
)
add_library(Plugin_Gesture SHARED ${GESTURE_OTHER_SOURCES} $<TARGET_OBJECTS:gesture_objects>)
tools3000_add_version_resource(Plugin_Gesture "Tools3000 Gesture Plugin" "Plugin_Gesture.dll" VFT_DLL)
tools3000_add_plugin_manifest(Plugin_Gesture)
target_compile_definitions(Plugin_Gesture PRIVATE PLUGIN_EXPORTS)
target_include_directories(Plugin_Gesture PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Plugin_Gesture PRIVATE Tools3000Core
    d2d1 dwrite windowscodecs user32 shell32 shcore dwmapi dcomp d3d11 dxgi winmm
)
set_target_properties(Plugin_Gesture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

# ── 3. Plugin_Capture (动态库) ────────────────────────────────────────────────
set(CAPTURE_OTHER_SOURCES
    src/capture/CaptureInput.cpp
    src/capture/CaptureOverlay.cpp
    src/capture/CaptureRenderer.cpp
    src/capture/PinWindow.cpp
    src/capture/PluginEntry.cpp
    src/capture/ScreenCapture.cpp
    src/capture/ScrollCaptureOverlay.cpp
    ${OCR_SOURCES}
)
add_library(Plugin_Capture SHARED ${CAPTURE_OTHER_SOURCES} $<TARGET_OBJECTS:capture_objects>)
tools3000_add_version_resource(Plugin_Capture "Tools3000 Capture Plugin" "Plugin_Capture.dll" VFT_DLL)
tools3000_add_plugin_manifest(Plugin_Capture)
target_compile_definitions(Plugin_Capture PRIVATE PLUGIN_EXPORTS)
target_include_directories(Plugin_Capture PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Plugin_Capture PRIVATE Tools3000Core
    opencv_core opencv_imgproc opencv_imgcodecs opencv_photo
    ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY}
    d2d1 dwrite d3d11 dxgi dxguid dwmapi mfplat mfreadwrite mfuuid
    shlwapi shell32 user32 gdi32 ole32 oleaut32 advapi32 comctl32 windowscodecs winhttp windowsapp Shcore imm32
)
if(FFMPEG_INCLUDE_DIR)
    target_include_directories(Plugin_Capture PRIVATE ${FFMPEG_INCLUDE_DIR})
endif()
if(OpenCV_FOUND)
    target_include_directories(Plugin_Capture PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
set_target_properties(Plugin_Capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

# ── 3.5. Plugin_Keycast (动态库) ────────────────────────────────────────────────
add_library(Plugin_Keycast SHARED src/keycast/PluginEntry.cpp $<TARGET_OBJECTS:keycast_objects>)
tools3000_add_version_resource(Plugin_Keycast "Tools3000 Keycast Plugin" "Plugin_Keycast.dll" VFT_DLL)
tools3000_add_plugin_manifest(Plugin_Keycast)
target_compile_definitions(Plugin_Keycast PRIVATE PLUGIN_EXPORTS)
target_include_directories(Plugin_Keycast PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Plugin_Keycast PRIVATE Tools3000Core
    d2d1 dwrite msimg32 user32 shell32 shcore dwmapi
)
set_target_properties(Plugin_Keycast PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

# ── 3.6. Plugin_Search (动态库) ────────────────────────────────────────────────
add_library(Plugin_Search SHARED ${SEARCH_SOURCES})
tools3000_add_version_resource(Plugin_Search "Tools3000 Search Plugin" "Plugin_Search.dll" VFT_DLL)
tools3000_add_plugin_manifest(Plugin_Search)
target_compile_definitions(Plugin_Search PRIVATE PLUGIN_EXPORTS)
target_include_directories(Plugin_Search PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Plugin_Search PRIVATE Tools3000Core nlohmann_json::nlohmann_json spdlog::spdlog advapi32 shell32 bcrypt)
set_target_properties(Plugin_Search PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

# ── 3.7. Plugin_DialogEnhancer (动态库) ────────────────────────────────────────
set(DIALOG_OTHER_SOURCES
    src/dialog/DialogEngine.cpp
    src/dialog/DialogRibbonOverlay.cpp
    src/dialog/PluginEntry.cpp
)
add_library(Plugin_DialogEnhancer SHARED ${DIALOG_OTHER_SOURCES} $<TARGET_OBJECTS:dialog_objects>)
tools3000_add_version_resource(Plugin_DialogEnhancer "Tools3000 Dialog Enhancer Plugin" "Plugin_DialogEnhancer.dll" VFT_DLL)
tools3000_add_plugin_manifest(Plugin_DialogEnhancer)
target_compile_definitions(Plugin_DialogEnhancer PRIVATE PLUGIN_EXPORTS)
target_include_directories(Plugin_DialogEnhancer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Plugin_DialogEnhancer PRIVATE Tools3000Core
    d2d1 dwrite msimg32 user32 shell32 shcore ole32 oleaut32 shlwapi
)
set_target_properties(Plugin_DialogEnhancer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")

# ── 4. Tools3000 Host (可执行文件) ─────────────────────────────────────────────
set(HOST_OTHER_SOURCES
    src/main.cpp
    src/ui/KeyboardPipeline.cpp
    src/ui/QuickLookWindow.cpp
    src/ui/SearchWindow.cpp
    src/ui/SettingsWindow.cpp
    src/ui/ToastOverlay.cpp
    src/ui/TrayWindow.cpp
    src/ui/WebViewEnvironmentManager.cpp
    src/Tools3000.rc
)
add_executable(Tools3000 WIN32 ${HOST_OTHER_SOURCES} $<TARGET_OBJECTS:ui_objects>)
set_target_properties(Tools3000 PROPERTIES OUTPUT_NAME "Tools3000")
tools3000_add_version_resource(Tools3000 "Tools3000 Desktop Productivity Toolkit" "Tools3000.exe" VFT_APP)
target_include_directories(Tools3000 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Tools3000 PRIVATE Tools3000Core
    shlwapi shell32 user32 gdi32 ole32 oleaut32 advapi32 comctl32 shcore d2d1 dwrite wtsapi32 dwmapi
)

# A runnable host build must contain the same validated plugin bundle that the
# installer ships. Depending on every plugin also prevents stale DLLs from a
# previous build being paired with missing or outdated manifests.
add_dependencies(Tools3000 Plugin_Gesture Plugin_Capture Plugin_Keycast Plugin_Search Plugin_DialogEnhancer)

add_custom_command(TARGET Tools3000 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Tools3000>/plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Plugin_Gesture>" "$<TARGET_FILE_DIR:Tools3000>/plugins/$<TARGET_FILE_NAME:Plugin_Gesture>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:Plugin_Gesture>/Plugin_Gesture.plugin.json" "$<TARGET_FILE_DIR:Tools3000>/plugins/Plugin_Gesture.plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Plugin_Capture>" "$<TARGET_FILE_DIR:Tools3000>/plugins/$<TARGET_FILE_NAME:Plugin_Capture>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:Plugin_Capture>/Plugin_Capture.plugin.json" "$<TARGET_FILE_DIR:Tools3000>/plugins/Plugin_Capture.plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Plugin_Keycast>" "$<TARGET_FILE_DIR:Tools3000>/plugins/$<TARGET_FILE_NAME:Plugin_Keycast>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:Plugin_Keycast>/Plugin_Keycast.plugin.json" "$<TARGET_FILE_DIR:Tools3000>/plugins/Plugin_Keycast.plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Plugin_Search>" "$<TARGET_FILE_DIR:Tools3000>/plugins/$<TARGET_FILE_NAME:Plugin_Search>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:Plugin_Search>/Plugin_Search.plugin.json" "$<TARGET_FILE_DIR:Tools3000>/plugins/Plugin_Search.plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Plugin_DialogEnhancer>" "$<TARGET_FILE_DIR:Tools3000>/plugins/$<TARGET_FILE_NAME:Plugin_DialogEnhancer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:Plugin_DialogEnhancer>/Plugin_DialogEnhancer.plugin.json" "$<TARGET_FILE_DIR:Tools3000>/plugins/Plugin_DialogEnhancer.plugin.json"
    COMMENT "Deploying validated plugin bundle beside Tools3000..."
)

if(EXISTS "${WEBVIEW2_DIR}")
    target_include_directories(Tools3000 PRIVATE "${WEBVIEW2_DIR}/build/native/include")
    if("${CMAKE_GENERATOR_PLATFORM}" STREQUAL "ARM64" OR "${CMAKE_SYSTEM_PROCESSOR}" MATCHES "^(ARM64|aarch64|arm64)$")
        target_link_libraries(Tools3000 PRIVATE "${WEBVIEW2_DIR}/build/native/arm64/WebView2LoaderStatic.lib")
    else()
        target_link_libraries(Tools3000 PRIVATE "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
    endif()
endif()

# ── 5. Tools3000_Service (后台高权限服务) ─────────────────────────────────────────────
add_executable(Tools3000_Service src/service/main.cpp $<TARGET_OBJECTS:service_objects>)
set_target_properties(Tools3000_Service PROPERTIES OUTPUT_NAME "Tools3000_Service")
tools3000_add_version_resource(Tools3000_Service "Tools3000 File Index Service" "Tools3000_Service.exe" VFT_APP)
target_compile_definitions(Tools3000_Service PRIVATE UNICODE _UNICODE)
target_link_libraries(Tools3000_Service PRIVATE spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json ZLIB::ZLIB advapi32 user32 shell32 ole32)

if(MSVC AND TOOLS3000_ENABLE_MSVC_ANALYSIS)
    foreach(ANALYSIS_TARGET Tools3000Core Plugin_Search Tools3000_Service service_objects)
        target_compile_options(${ANALYSIS_TARGET} PRIVATE /analyze /analyze:external-)
    endforeach()
endif()

# ── PCH 预编译头配置 ─────────────────────────────────────────────────────────
set(CORE_PCH_HEADER "${CMAKE_SOURCE_DIR}/src/core/CorePch.h")
set(PCH_TARGETS
    Tools3000Core
    gesture_objects
    capture_objects
    keycast_objects
    service_objects
    dialog_objects
    ui_objects
    Plugin_Gesture
    Plugin_Capture
    Plugin_Keycast
    Plugin_Search
    Plugin_DialogEnhancer
    Tools3000
    Tools3000_Service
)
foreach(PCH_TARGET ${PCH_TARGETS})
    if(TARGET ${PCH_TARGET})
        target_precompile_headers(${PCH_TARGET} PRIVATE "${CORE_PCH_HEADER}")
    endif()
endforeach()

# ── 6. 自动化测试 ────────────────────────────────────────────────────────────
include(CTest)
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)

    add_executable(Tools3000Tests
        tests/unit/test_main.cpp
        $<TARGET_OBJECTS:capture_objects>
        $<TARGET_OBJECTS:gesture_objects>
        $<TARGET_OBJECTS:service_objects>
        $<TARGET_OBJECTS:dialog_objects>
        $<TARGET_OBJECTS:keycast_objects>
        $<TARGET_OBJECTS:ui_objects>
    )
    target_precompile_headers(Tools3000Tests PRIVATE "${CORE_PCH_HEADER}")
    target_include_directories(Tools3000Tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(Tools3000Tests PRIVATE ${FFMPEG_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
    target_include_directories(Tools3000Tests PRIVATE "${WEBVIEW2_DIR}/build/native/include")
    if (MSVC)
        target_compile_options(Tools3000Tests PRIVATE /bigobj)
    endif()
    target_link_libraries(Tools3000Tests PRIVATE Tools3000Core GTest::gtest GTest::gmock ZLIB::ZLIB user32 shell32 gdi32 dwmapi d3d11 dxgi ole32 shcore advapi32 mfplat mfuuid shlwapi d2d1 dwrite winmm
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY} ${OpenCV_LIBS})
    add_test(NAME Tools3000.Unit COMMAND Tools3000Tests)

    # Manual visual QA helper. It uses the production Direct2D renderer, writes
    # a bitmap preview, and verifies repeated show/hide does not leak GDI/USER
    # objects. EXCLUDE_FROM_ALL keeps it out of normal builds and installers.
    add_executable(Tools3000ShortcutHintPreview EXCLUDE_FROM_ALL
        tests/visual/shortcut_hint_preview.cpp
        src/capture/ShortcutHintOverlay.cpp
    )
    target_include_directories(Tools3000ShortcutHintPreview PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(Tools3000ShortcutHintPreview PRIVATE Tools3000Core
        d2d1 dwrite shcore user32 gdi32 dwmapi)

    add_executable(Tools3000NativeOverlayPreview EXCLUDE_FROM_ALL
        tests/visual/native_overlay_preview.cpp
        src/keycast/KeycastOverlay.cpp
        src/ocr/OcrResultWindow.cpp
        src/ui/ToastOverlay.cpp
    )
    target_include_directories(Tools3000NativeOverlayPreview PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(Tools3000NativeOverlayPreview PRIVATE Tools3000Core
        d2d1 dwrite shcore user32 gdi32 dwmapi msimg32)

    # Cross-process IFileDialog integration gate. It is intentionally
    # excluded from normal builds because it briefly opens a real native dialog.
    add_executable(Tools3000DialogNavigatorIntegration EXCLUDE_FROM_ALL
        tests/integration/dialog_navigator_integration.cpp
        src/dialog/DialogNavigator.cpp
    )
    target_include_directories(Tools3000DialogNavigatorIntegration PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(Tools3000DialogNavigatorIntegration PRIVATE Tools3000Core
        user32 shell32 shlwapi ole32 UIAutomationCore)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/ui/dist")
    add_custom_command(TARGET Tools3000 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/ui/dist"
            "$<TARGET_FILE_DIR:Tools3000>/ui"
        COMMENT "Copying WebView2 UI assets..."
    )
endif()

# ── 安装规则 ──────────────────────────────────────────────────────────────────
install(TARGETS Tools3000 Tools3000_Service Tools3000Core RUNTIME DESTINATION bin)
install(TARGETS Plugin_Gesture Plugin_Capture Plugin_Keycast Plugin_Search Plugin_DialogEnhancer RUNTIME DESTINATION bin/plugins)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/" DESTINATION bin/resources)
