add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE avpipe_core)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME config COMMAND test_config)

add_executable(test_media test_media.cpp)
target_link_libraries(test_media PRIVATE avpipe_core)
target_include_directories(test_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME media COMMAND test_media)

add_executable(test_pipeline test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE avpipe_core)
target_include_directories(test_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pipeline COMMAND test_pipeline)
