add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE erde_core)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME config COMMAND test_config)

add_executable(test_media test_media.cpp)
target_link_libraries(test_media PRIVATE erde_core)
target_include_directories(test_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME media COMMAND test_media)

add_executable(test_pipeline test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE erde_core)
target_include_directories(test_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pipeline COMMAND test_pipeline)

add_executable(test_filter test_filter.cpp)
target_link_libraries(test_filter PRIVATE erde_core)
target_include_directories(test_filter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME filter COMMAND test_filter)

add_executable(test_audio test_audio.cpp)
target_link_libraries(test_audio PRIVATE erde_core)
target_include_directories(test_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME audio COMMAND test_audio)

add_executable(test_encode test_encode.cpp)
target_link_libraries(test_encode PRIVATE erde_core)
target_include_directories(test_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME encode COMMAND test_encode)

add_executable(test_av test_av.cpp)
target_link_libraries(test_av PRIVATE erde_core)
target_include_directories(test_av PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME av COMMAND test_av)
