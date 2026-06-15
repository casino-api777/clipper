cmake_minimum_required(VERSION 3.16)
project(clip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(clip WIN32 clip.cpp clip.rc)
target_compile_definitions(clip PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(clip PRIVATE user32 kernel32 shell32 advapi32 ole32 oleaut32 uiautomationcore uuid)

if(MSVC)
  target_compile_options(clip PRIVATE /W4 /EHsc)
  target_link_options(clip PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
else()
  target_compile_options(clip PRIVATE -Wall -Wextra -municode)
  target_link_options(clip PRIVATE -municode -mwindows)
endif()
