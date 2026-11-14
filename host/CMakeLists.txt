include(FetchContent)
FetchContent_Declare(webview2
  URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg
  URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(webview2)

set(WEBVIEW2_DIR ${CMAKE_CURRENT_BINARY_DIR}/webview2-sdk)
if(NOT EXISTS ${WEBVIEW2_DIR}/build/native/include/WebView2.h)
  file(ARCHIVE_EXTRACT INPUT ${webview2_SOURCE_DIR}/microsoft.web.webview2.1.0.4314.35.nupkg DESTINATION ${WEBVIEW2_DIR})
endif()

add_executable(houseguest WIN32 main.cpp houseguest.manifest houseguest.rc)
target_include_directories(houseguest PRIVATE ${WEBVIEW2_DIR}/build/native/include)
target_link_libraries(houseguest PRIVATE houseguest::system houseguest_warnings
  ${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib dwmapi shell32 ole32 user32 advapi32 version)
if(HOUSEGUEST_DEVTOOLS)
  target_compile_definitions(houseguest PRIVATE HOUSEGUEST_DEVTOOLS)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/ui/dist/index.html)
  add_custom_command(TARGET houseguest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:houseguest>/ui
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui/dist $<TARGET_FILE_DIR:houseguest>/ui)
endif()
