TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
        cmdutils.c \
        ffmpeg.c \
        ffmpeg_cuvid.c \
        ffmpeg_filter.c \
        ffmpeg_hw.c \
        ffmpeg_opt.c

HEADERS += \
    cmdutils.h \
    config.h \
    ffmpeg.h

win32 {

    #自定义变量.注释使用64位，否则使用32位
    #DEFINES += USE_X32

    #contains(QT_ARCH, i386) {#系统的变量，使用自己的变量去控制感觉更好
    contains(DEFINES, USE_X32) {
    # x86环境
    INCLUDEPATH += $$PWD/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/SDL2/include
    INCLUDEPATH += $$PWD/spdlog

    LIBS += $$PWD/ffmpeg-4.2/lib/x86/avformat.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avcodec.lib    \
            $$PWD/ffmpeg-4.2/lib/x86/avdevice.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avfilter.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/avutil.lib     \
            $$PWD/ffmpeg-4.2/lib/x86/postproc.lib   \
            $$PWD/ffmpeg-4.2/lib/x86/swresample.lib \
            $$PWD/ffmpeg-4.2/lib/x86/swscale.lib    \
            $$PWD/SDL2/lib/x86/SDL2.lib
    message("win32")

    }else{
    # x64环境
    INCLUDEPATH += $$PWD/ffmpeg-4.2/include
    INCLUDEPATH += $$PWD/SDL2/include
    INCLUDEPATH += $$PWD/spdlog
    # msvc编译器用到,否则会报找不到stdatomic.h错误
    INCLUDEPATH += $$PWD/ffmpeg-4.2/include/compat/atomics/win32

    LIBS += $$PWD/ffmpeg-4.2/lib/x64/avformat.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avcodec.lib    \
            $$PWD/ffmpeg-4.2/lib/x64/avdevice.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avfilter.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/avutil.lib     \
            $$PWD/ffmpeg-4.2/lib/x64/postproc.lib   \
            $$PWD/ffmpeg-4.2/lib/x64/swresample.lib \
            $$PWD/ffmpeg-4.2/lib/x64/swscale.lib    \
            $$PWD/SDL2/lib/x64/SDL2.lib
    message("win64")
    }

}
