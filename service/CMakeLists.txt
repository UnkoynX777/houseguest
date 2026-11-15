include(${CMAKE_SOURCE_DIR}/cmake/embed_panel.cmake)
houseguest_embed_panel(${CMAKE_BINARY_DIR}/generated/panel_assets.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/panel/index.html|/|text/html"
  "${CMAKE_CURRENT_SOURCE_DIR}/panel/panel.css|/panel.css|text/css"
  "${CMAKE_CURRENT_SOURCE_DIR}/panel/panel.js|/panel.js|text/javascript")

add_library(houseguest_service_lib STATIC
  src/store.cpp
  src/audit.cpp
  src/supervisor.cpp
  src/assessor.cpp
  src/continuity.cpp
  src/discovery.cpp
  src/lineage.cpp
  src/identity_resolver.cpp
  src/file_details.cpp
  src/change_watcher.cpp
  src/process_trace.cpp
  src/controller.cpp
  src/command_support.cpp
  src/commands_account.cpp
  src/commands_policy.cpp
  src/commands_session.cpp
  src/commands_system.cpp
  src/commands_apps.cpp
  src/commands_catalog.cpp
  src/catalog.cpp
  src/ipc_limits.cpp
  src/api/http.cpp
  src/api/devices.cpp
  src/api/router.cpp
  src/api/access.cpp
  src/ai/gemini.cpp
  src/ai/chat.cpp
  src/ai/assistant.cpp
  src/cloudflare/api.cpp
  src/cloudflare/tunnel.cpp
  src/cloudflare/process.cpp
  src/api/service.cpp
  src/commands_api.cpp
  ${CMAKE_BINARY_DIR}/generated/panel_assets.cpp
  src/remote.cpp
  src/api/listener.cpp
  src/pipe_server.cpp)
target_include_directories(houseguest_service_lib PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(houseguest_service_lib PUBLIC houseguest::system PRIVATE houseguest_warnings advapi32 wtsapi32 ws2_32)

add_executable(houseguest-service src/main.cpp)
target_link_libraries(houseguest-service PRIVATE houseguest_service_lib houseguest_warnings)

add_executable(houseguest-cli ../cli/main.cpp)
target_link_libraries(houseguest-cli PRIVATE houseguest::system houseguest_warnings)

if(HOUSEGUEST_BUILD_TESTS)
  add_executable(houseguest_service_tests tests/store_tests.cpp tests/audit_tests.cpp tests/ipc_limits_tests.cpp tests/continuity_tests.cpp tests/protection_tests.cpp tests/http_tests.cpp tests/listener_tests.cpp tests/router_tests.cpp tests/api_service_tests.cpp tests/access_tests.cpp tests/cloudflare_tests.cpp tests/remote_tests.cpp tests/ai_tests.cpp tests/assistant_tests.cpp)
  target_link_libraries(houseguest_service_tests PRIVATE houseguest_service_lib houseguest_warnings Catch2::Catch2WithMain)
  add_test(NAME houseguest_service_tests COMMAND houseguest_service_tests)
endif()
