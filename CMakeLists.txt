cmake_minimum_required(VERSION 3.28)
project(ygol_access VERSION 0.1.4 LANGUAGES CXX)  # scripts/release.ps1 bumps this

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT, so players don't need a particular VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GAME_DIR "D:/SteamLibrary/steamapps/common/Yu-Gi-Oh! Legacy of the Duelist Link Evolution"
    CACHE PATH "Game install folder (cmake --install target)")
set(PRISM_DIR "${CMAKE_SOURCE_DIR}/third_party/prism")

add_library(prism SHARED IMPORTED)
set_target_properties(prism PROPERTIES
    IMPORTED_LOCATION "${PRISM_DIR}/dynamic/release/bin/prism.dll"
    IMPORTED_IMPLIB "${PRISM_DIR}/dynamic/release/lib/prism.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${PRISM_DIR}/include")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    EXCLUDE_FROM_ALL)  # skips MinHook's own install rules
FetchContent_MakeAvailable(minhook)

add_library(ygol_access SHARED src/dllmain.cpp src/speech.cpp src/scan.cpp src/text.cpp src/rtti.cpp src/menu.cpp src/cards.cpp src/duel.cpp)
set_target_properties(ygol_access PROPERTIES OUTPUT_NAME xinput9_1_0)
target_compile_definitions(ygol_access PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ygol_access PRIVATE /W4 /utf-8)
target_link_libraries(ygol_access PRIVATE prism minhook)

enable_testing()
add_executable(scan_test tests/scan_test.cpp src/scan.cpp)
add_test(NAME scan_test COMMAND scan_test)
add_executable(cards_test tests/cards_test.cpp src/cards.cpp)
add_test(NAME cards_test COMMAND cards_test)

install(TARGETS ygol_access RUNTIME DESTINATION "${GAME_DIR}")
install(FILES "${PRISM_DIR}/dynamic/release/bin/prism.dll" DESTINATION "${GAME_DIR}")
