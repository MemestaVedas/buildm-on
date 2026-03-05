cmake_minimum_required(VERSION 3.14)
project(buildm-on)

set(CMAKE_CXX_STANDARD 17)

# Fetch FTXUI automatically
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

add_executable(buildm-on src/main.cpp)

target_link_libraries(buildm-on
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
)

# Statically link C++ runtime on Windows so it doesn't need MinGW DLLs in PATH
if(WIN32)
  set_target_properties(buildm-on PROPERTIES LINK_FLAGS "-static-libgcc -static-libstdc++ -static -pthread")
endif()

install(TARGETS buildm-on RUNTIME DESTINATION bin)
