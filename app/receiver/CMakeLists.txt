add_library(llreceiver STATIC
    ReceiverPipeline.cpp
)
target_link_libraries(llreceiver PUBLIC lltransport)

add_executable(lowlat_receiver main.cpp)
target_link_libraries(lowlat_receiver PRIVATE llreceiver)
