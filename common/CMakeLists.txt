add_library(llcommon STATIC
    logger.cpp
    Config.cpp
)

target_include_directories(llcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(llcommon PUBLIC pthread)