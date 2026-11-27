cmake_minimum_required(VERSION 3.25)
project(QuestSearchOverlay VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QuestSearchOverlay requires Windows x64")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Pinned dependencies (paths passed in by tools/Build.ps1).
set(QSO_COMMONLIB_DIR "" CACHE PATH "Pinned CommonLibSSE-NG source")
set(QSO_HEADERS_DIR "" CACHE PATH "Pinned rapidcsv source parent")
if(NOT EXISTS "${QSO_COMMONLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set QSO_COMMONLIB_DIR to the pinned CommonLibSSE-NG source")
endif()

find_package(spdlog CONFIG REQUIRED)
set(RAPIDCSV_INCLUDE_DIRS "${QSO_HEADERS_DIR}/rapidcsv-8.84/src" CACHE PATH "" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# SKYRIM_SUPPORT_AE selects the AE layouts in CommonLibSSE-NG builds that test it (older versions laid out ControlMap,
# TES, BGSSaveLoadManager and CombatController for AE only with it; without it ControlMap is 8 bytes short and every
# field after controlMap[] is misread). Kept so the AE layout never depends on the library version. It must apply to
# CommonLib AND this plugin, so it is set before add_subdirectory.
add_compile_definitions(SKYRIM_SUPPORT_AE)
# Store source paths relative to this folder (no absolute build paths in the DLL: CommonLib's logging records
# std::source_location for every log call). Applies to CommonLib too, so it is set before add_subdirectory.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" QSO_TRIM_PREFIX)
add_compile_options("/d1trimfile:${QSO_TRIM_PREFIX}")
add_subdirectory("${QSO_COMMONLIB_DIR}" "${CMAKE_BINARY_DIR}/commonlib" EXCLUDE_FROM_ALL)

# Two parts. all = core + quests + capture -> QuestSearchOverlay.dll (shipped).
# notify = core + capture -> HUDNotificationReplacer.dll, its own INI / view folder / log (a notifications-only build;
# not shipped).
set(QSO_PARTS "all" CACHE STRING "all (quests + game notifications) or notify (game notifications only)")
set_property(CACHE QSO_PARTS PROPERTY STRINGS all notify)
set(QSO_CORE_SOURCES src/main.cpp src/core/View.cpp src/core/Boxes.cpp src/core/Settings.cpp src/core/History.cpp)
set(QSO_CAPTURE_SOURCES src/capture/Capture.cpp src/capture/B612.cpp)
set(QSO_QUEST_SOURCES src/quests/InputHook.cpp src/quests/QuestReader.cpp src/quests/Overlay.cpp src/quests/Nearby.cpp
    src/quests/Pins.cpp)
if(QSO_PARTS STREQUAL "all")
    set(QSO_TARGET QuestSearchOverlay)
    set(QSO_PART_QUESTS 1)
    set(QSO_DISPLAY "Quest Search and Notification Overlay")
    set(QSO_SOURCES ${QSO_CORE_SOURCES} ${QSO_QUEST_SOURCES} ${QSO_CAPTURE_SOURCES})
elseif(QSO_PARTS STREQUAL "notify")
    set(QSO_TARGET HUDNotificationReplacer)
    set(QSO_PART_QUESTS 0)
    set(QSO_DISPLAY "HUD Notification Replacer (from QSNO)")
    set(QSO_SOURCES ${QSO_CORE_SOURCES} ${QSO_CAPTURE_SOURCES})
else()
    message(FATAL_ERROR "QSO_PARTS must be all or notify, not '${QSO_PARTS}'")
endif()

add_library(${QSO_TARGET} SHARED ${QSO_SOURCES})
set(QSO_PRISMA_DIR "" CACHE PATH "Pinned PrismaUI SDK (PrismaUI_API.h)")
if(NOT EXISTS "${QSO_PRISMA_DIR}/PrismaUI_API.h")
    message(FATAL_ERROR "Set QSO_PRISMA_DIR to the folder holding PrismaUI_API.h")
endif()
target_include_directories(${QSO_TARGET} PRIVATE src src/core src/capture "${QSO_PRISMA_DIR}")
if(QSO_PART_QUESTS)
    target_include_directories(${QSO_TARGET} PRIVATE src/quests)
endif()
target_link_libraries(${QSO_TARGET} PRIVATE CommonLibSSE::CommonLibSSE)
target_precompile_headers(${QSO_TARGET} PRIVATE src/PCH.h)
# File names (DLL, INI, view folder, log, co-save ID QSOP) stay as before so saves and settings carry over; only the
# name the player reads changed (QSO_DISPLAY_NAME).
target_compile_definitions(${QSO_TARGET} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    QSO_VERSION_STRING="${PROJECT_VERSION}"
    QSO_PART_QUESTS=${QSO_PART_QUESTS}
    QSO_PLUGIN_NAME="${QSO_TARGET}"
    QSO_FILE_STEM="${QSO_TARGET}"
    QSO_VIEW_PAGE="${QSO_TARGET}/index.html"
    QSO_DISPLAY_NAME="${QSO_DISPLAY}")
target_compile_options(${QSO_TARGET} PRIVATE /W4 /permissive- /utf-8)
