cmake_minimum_required(VERSION 3.14)
project(buildm-on)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# FTXUI — terminal UI
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# nlohmann/json — JSON serialization
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

find_package(Threads REQUIRED)

add_executable(buildm-on src/main.cpp)

target_link_libraries(buildm-on
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE Threads::Threads
)

if(WIN32)
  target_link_libraries(buildm-on PRIVATE ws2_32)
  set_target_properties(buildm-on PROPERTIES LINK_FLAGS "-static-libgcc -static-libstdc++ -static -pthread")
endif()

install(TARGETS buildm-on RUNTIME DESTINATION bin)

if(WIN32)
  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_NAME "Buildm-on")
  set(CPACK_PACKAGE_VENDOR "Buildm-on Developers")
  set(CPACK_PACKAGE_VERSION "2.0.0")
  include(CPack)
endif()
