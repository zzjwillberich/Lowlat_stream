add_library(llcommon STATIC
    Logger.cpp
    Config.cpp
    Status.cpp
)

target_include_directories(llcommon PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(llcommon PUBLIC pthread)