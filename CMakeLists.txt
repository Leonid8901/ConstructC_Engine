cmake_minimum_required(VERSION 3.20)
project(CustomEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Linux: GLFW собирается и с X11, и с Wayland, если для Wayland есть пакеты; иначе только X11.
# Выбор во время запуска: Wayland-сессия -> Wayland, иначе X11 (переопределяется в engine.json).
if(UNIX AND NOT APPLE AND NOT DEFINED GLFW_BUILD_WAYLAND)
    set(CE_WAYLAND OFF)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(CE_WL QUIET wayland-client wayland-cursor wayland-egl xkbcommon)
        find_program(CE_WAYLAND_SCANNER wayland-scanner)
        if(CE_WL_FOUND AND CE_WAYLAND_SCANNER)
            set(CE_WAYLAND ON)
        endif()
    endif()
    set(GLFW_BUILD_WAYLAND ${CE_WAYLAND} CACHE BOOL "GLFW Wayland backend")
    if(NOT CE_WAYLAND)
        message(STATUS "CustomEngine: Wayland выключен (нужны libwayland-dev, libxkbcommon-dev, wayland-scanner) — только X11")
    endif()
endif()
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0)
FetchContent_MakeAvailable(glfw vma)

# ---------------- Движок (статическая библиотека) ----------------
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(CustomEngine STATIC ${ENGINE_SOURCES})
target_include_directories(CustomEngine
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include      # то, что видит игра: без Vulkan, glm и GLFW
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(CustomEngine PRIVATE Vulkan::Vulkan glfw GPUOpen::VulkanMemoryAllocator Threads::Threads)

# Профилирование через Tracy: cmake -DCE_PROFILE=ON (по умолчанию выключено и ничего не стоит)
option(CE_PROFILE "Профилирование через Tracy" OFF)
if(CE_PROFILE)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.11.1
        GIT_SHALLOW TRUE)
    # Данные собираются только пока подключён Tracy Profiler: без него память не растёт
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
    target_link_libraries(CustomEngine PRIVATE TracyClient)
    target_compile_definitions(CustomEngine PRIVATE CE_PROFILE=1)
endif()

if(MSVC)
    target_compile_options(CustomEngine PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(CustomEngine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_source_files_properties(src/Vulkan/VmaImpl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    target_compile_options(CustomEngine PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    set_source_files_properties(src/Vulkan/VmaImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------- Шейдеры: GLSL -> SPIR-V ----------------
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin REQUIRED)
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    ${SHADER_SRC_DIR}/*.vert ${SHADER_SRC_DIR}/*.frag ${SHADER_SRC_DIR}/*.comp)
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${SHADER_SRC_DIR}/*.glsl)

set(SPIRV_FILES "")
foreach(SRC ${SHADER_SOURCES})
    get_filename_component(NAME ${SRC} NAME)
    set(OUT ${SHADER_OUT_DIR}/${NAME}.spv)
    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -I ${SHADER_SRC_DIR} ${SRC} -o ${OUT}
        DEPENDS ${SRC} ${SHADER_INCLUDES}
        COMMENT "glslc ${NAME}"
        VERBATIM)
    list(APPEND SPIRV_FILES ${OUT})
endforeach()
add_custom_target(shaders DEPENDS ${SPIRV_FILES})
add_dependencies(CustomEngine shaders)
target_compile_definitions(CustomEngine PRIVATE SHADER_DIR="${SHADER_OUT_DIR}/")

# ---------------- Пример игры ----------------
add_executable(Sandbox sandbox/main.cpp)
target_link_libraries(Sandbox PRIVATE CustomEngine)
if(MSVC)
    target_compile_options(Sandbox PRIVATE /W4 /utf-8)
else()
    target_compile_options(Sandbox PRIVATE -Wall -Wextra)
endif()

# Раздача: cmake --install build --prefix <папка> кладёт Sandbox и shaders/ рядом (запуск откуда угодно)
install(TARGETS Sandbox RUNTIME DESTINATION .)
install(DIRECTORY ${SHADER_OUT_DIR}/ DESTINATION shaders FILES_MATCHING PATTERN "*.spv")

# ---------------- Бенчмарки (cmake -DCE_BUILD_BENCH=ON) ----------------
option(CE_BUILD_BENCH "Собрать бенчмарки UniXBench и SceneBench" OFF)
if(CE_BUILD_BENCH)
    add_executable(UniXBench bench/unix_bench.cpp)
    target_include_directories(UniXBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)  # внутренний Scheduler
    target_link_libraries(UniXBench PRIVATE CustomEngine)
    add_executable(SceneBench bench/scene_bench.cpp)
    target_link_libraries(SceneBench PRIVATE CustomEngine)
    add_executable(StressTest bench/stress.cpp)
    target_link_libraries(StressTest PRIVATE CustomEngine)
endif()
