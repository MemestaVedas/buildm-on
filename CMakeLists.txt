cmake_minimum_required(VERSION 3.14)
project(buildmon)

set(CMAKE_CXX_STANDARD 17)

# Fetch FTXUI automatically
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

add_executable(buildmon src/main.cpp)

target_link_libraries(buildmon
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
)
