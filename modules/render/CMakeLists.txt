find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_library(llrender STATIC
    IRenderer.cpp
    NullRenderer.cpp
    SdlRenderer.cpp
)

# llcapture 走 PUBLIC: IRenderer.h include 了 Frame.h, 用到渲染器的 target 必然要它。
#
# SDL2 走 PRIVATE, 同 lldecode 对 FFmpeg 的处理: SdlRenderer.h 只前向声明了
# SDL_Window/SDL_Renderer/SDL_Texture, SDL.h 只出现在 SdlRenderer.cpp 里。
# 这一条是**必须**的而不是风格: SDL.h 里有 #define main SDL_main, 一旦以 PUBLIC
# 传出去, 每个 include 了渲染器头文件的 target 都会中招, 在 main.cpp 里表现为
# 莫名其妙的链接错误。
target_link_libraries(llrender
    PUBLIC  llcapture
    PRIVATE PkgConfig::SDL2
)
