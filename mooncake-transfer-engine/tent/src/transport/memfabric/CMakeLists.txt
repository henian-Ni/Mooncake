if(USE_MEMFABRIC)
  add_library(tent_xport_memfabric OBJECT
    memfabric_transport.cpp
    memfabric_under_api.cpp)
  target_link_libraries(tent_xport_memfabric PUBLIC tent_interface)
  target_link_libraries(tent_xport_memfabric PUBLIC dl)
  target_include_directories(tent_xport_memfabric PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/tent/transport/memfabric)
endif()
