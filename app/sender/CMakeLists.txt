add_library(llsender STATIC
    SenderPipeline.cpp
)
target_link_libraries(llsender PUBLIC llencode)

add_executable(lowlat_sender main.cpp)
target_link_libraries(lowlat_sender PRIVATE llsender)
