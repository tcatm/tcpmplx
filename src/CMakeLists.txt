include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${TCPMPLX_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR})
add_executable(tcpmplx main.c)
set_target_properties(tcpmplx PROPERTIES COMPILE_FLAGS "-std=gnu99 -Wall" LINK_FLAGS "")
install(TARGETS tcpmplx RUNTIME DESTINATION bin)
