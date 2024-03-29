﻿/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef FFTOOLS_FFMPEG_H
#define FFTOOLS_FFMPEG_H

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <signal.h>

#include "cmdutils.h"

#include "libavformat/avformat.h"
#include "libavformat/avio.h"

#include "libavcodec/avcodec.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avutil.h"
#include "libavutil/dict.h"
#include "libavutil/eval.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"

#include "libswresample/swresample.h"

#define VSYNC_AUTO       -1
#define VSYNC_PASSTHROUGH 0         // 透传?
#define VSYNC_CFR         1         // 固定帧率(Const Frame-Rate).
#define VSYNC_VFR         2         // 可变帧率(Variable Frame-Rate)
#define VSYNC_VSCFR       0xfe      // video stream cfr. 1)固定帧率 且 只有一个输入视频流 && 输入偏移地址是0; 2)固定帧率且copy_ts=1时会使用?
#define VSYNC_DROP        0xff      // 音视频同步时，允许drop帧？

#define MAX_STREAMS 1024    /* arbitrary sanity check value */

#define mydebug av_log
//#define TYYCODE_TIMESTAMP_DEMUXER   // 用于debug解复用的时间戳
//#define TYYCODE_TIMESTAMP_DECODE
//#define TYYCODE_TIMESTAMP_ENCODER
//#define TYYCODE_TIMESTAMP_MUXER


enum HWAccelID {
    HWACCEL_NONE = 0,
    HWACCEL_AUTO,// 自动选择
    HWACCEL_GENERIC,// 选择通用的
    HWACCEL_VIDEOTOOLBOX,
    HWACCEL_QSV,
    HWACCEL_CUVID,
};

typedef struct HWAccel {
    const char *name;
    int (*init)(AVCodecContext *s);
    enum HWAccelID id;
    enum AVPixelFormat pix_fmt;
} HWAccel;

typedef struct HWDevice {
    const char *name;
    enum AVHWDeviceType type;
    AVBufferRef *device_ref;    /* 硬件设备引用 */
} HWDevice;

/* select an input stream for an output stream */
typedef struct StreamMap {
    int disabled;           /* 1 is this mapping is disabled by a negative map */
    int file_index;
    int stream_index;
    int sync_file_index;
    int sync_stream_index;
    char *linklabel;       /* name of an output link, for mapping lavfi outputs */
} StreamMap;

typedef struct {
    int  file_idx,  stream_idx,  channel_idx; // input
    int ofile_idx, ostream_idx;               // output
} AudioChannelMap;

typedef struct OptionsContext {
    OptionGroup *g;

    /* input/output options */
    int64_t start_time;                         // -ss选项
    int64_t start_time_eof;
    int seek_timestamp;
    const char *format;                         // -f选项.注：多个-f选项时，只会取最后一个，例如ffmpeg xxx -f s16le -f flv xxx，那么format就是flv

    SpecifierOpt *codec_names;                  //offset:40,-c,-codec,-vcodec
    int        nb_codec_names;
    SpecifierOpt *audio_channels;
    int        nb_audio_channels;
    SpecifierOpt *audio_sample_rate;
    int        nb_audio_sample_rate;
    SpecifierOpt *frame_rates;                  //offset:88, -r选项
    int        nb_frame_rates;
    SpecifierOpt *frame_sizes;                  // -s选项
    int        nb_frame_sizes;
    SpecifierOpt *frame_pix_fmts;
    int        nb_frame_pix_fmts;

    /* input options */
    int64_t input_ts_offset;                    //offset:136. -itsoffset选项,设置输入时间戳偏移
    int loop;                                   //循环次数.例如无限次,-stream_loop -1
    int rate_emu;                               //offset:148 -re
    int accurate_seek;                          //-accurate_seek选项,是否开启精确查找,与-ss有关
    int thread_queue_size;

    SpecifierOpt *ts_scale;                     //-itsscale选项,默认是1.0,设置输入ts的刻度
    int        nb_ts_scale;
    SpecifierOpt *dump_attachment;
    int        nb_dump_attachment;
    SpecifierOpt *hwaccels;                     // -hwaccel选项
    int        nb_hwaccels;
    SpecifierOpt *hwaccel_devices;              // -hwaccel_device选项，是否指定硬件加速设备
    int        nb_hwaccel_devices;
    SpecifierOpt *hwaccel_output_formats;
    int        nb_hwaccel_output_formats;
    SpecifierOpt *autorotate;                       // -autorotate选项
    int        nb_autorotate;

    /* output options */
    StreamMap *stream_maps;
    int     nb_stream_maps;
    AudioChannelMap *audio_channel_maps; /* one info entry per -map_channel */
    int           nb_audio_channel_maps; /* number of (valid) -map_channel settings */
    int metadata_global_manual;
    int metadata_streams_manual;
    int metadata_chapters_manual;
    const char **attachments;
    int       nb_attachments;

    int chapters_input_file;

    int64_t recording_time;     // 对应输入输出文件的-t选项.注意区分输入输出文件.
                                // 例如输入没设-t,而输出设了10s,输入的recording_time是初始化时的值,而输出的则为10s.实际上这个结构体其它字段都是这样.
    int64_t stop_time;
    uint64_t limit_filesize;    // -fs选项，设置文件大小限制
    float mux_preload;
    float mux_max_delay;        // -muxdelay选项，在init_options()看到默认0.7
    int shortest;               // -shortest选项
    int bitexact;

    int video_disable;          // -vn选项
    int audio_disable;          // -an选项
    int subtitle_disable;
    int data_disable;

    /* indexed by output file stream index */
    int   *streamid_map;
    int nb_streamid_map;

    SpecifierOpt *metadata;     // -metadata选项
    int        nb_metadata;
    SpecifierOpt *max_frames;
    int        nb_max_frames;
    SpecifierOpt *bitstream_filters;
    int        nb_bitstream_filters;
    SpecifierOpt *codec_tags;
    int        nb_codec_tags;
    SpecifierOpt *sample_fmts;  // -sample_fmt选项
    int        nb_sample_fmts;
    SpecifierOpt *qscale;
    int        nb_qscale;
    SpecifierOpt *forced_key_frames;
    int        nb_forced_key_frames;
    SpecifierOpt *force_fps;
    int        nb_force_fps;
    SpecifierOpt *frame_aspect_ratios;      // -aspect选项
    int        nb_frame_aspect_ratios;
    SpecifierOpt *rc_overrides;
    int        nb_rc_overrides;
    SpecifierOpt *intra_matrices;
    int        nb_intra_matrices;
    SpecifierOpt *inter_matrices;
    int        nb_inter_matrices;
    SpecifierOpt *chroma_intra_matrices;
    int        nb_chroma_intra_matrices;
    SpecifierOpt *top_field_first;
    int        nb_top_field_first;
    SpecifierOpt *metadata_map;             // -map_metadata选项
    int        nb_metadata_map;
    SpecifierOpt *presets;
    int        nb_presets;
    SpecifierOpt *copy_initial_nonkeyframes;// -copyinkf选项
    int        nb_copy_initial_nonkeyframes;
    SpecifierOpt *copy_prior_start;         // -copypriorss选项,在开始时间之前复制或丢弃帧
    int        nb_copy_prior_start;
    SpecifierOpt *filters;
    int        nb_filters;
    SpecifierOpt *filter_scripts;
    int        nb_filter_scripts;
    SpecifierOpt *reinit_filters;           // -reinit_filter选项
    int        nb_reinit_filters;
    SpecifierOpt *fix_sub_duration;
    int        nb_fix_sub_duration;
    SpecifierOpt *canvas_sizes;
    int        nb_canvas_sizes;
    SpecifierOpt *pass;
    int        nb_pass;
    SpecifierOpt *passlogfiles;
    int        nb_passlogfiles;
    SpecifierOpt *max_muxing_queue_size;
    int        nb_max_muxing_queue_size;
    SpecifierOpt *guess_layout_max;
    int        nb_guess_layout_max;
    SpecifierOpt *apad;
    int        nb_apad;
    SpecifierOpt *discard;
    int        nb_discard;
    SpecifierOpt *disposition;
    int        nb_disposition;
    SpecifierOpt *program;
    int        nb_program;
    SpecifierOpt *time_bases;
    int        nb_time_bases;
    SpecifierOpt *enc_time_bases;
    int        nb_enc_time_bases;
} OptionsContext;

//自定义封装输入过滤器结构体
typedef struct InputFilter {
    AVFilterContext    *filter;         // 视频时:指向buffer.音频时:指向abuffer
    struct InputStream *ist;
    struct FilterGraph *graph;
    uint8_t            *name;
    enum AVMediaType    type;           // AVMEDIA_TYPE_SUBTITLE for sub2video

    /*
    typedef struct AVFifoBuffer {
        uint8_t *buffer;//开辟后的内存起始地址
        uint8_t *rptr, *wptr, *end;//rptr是指向可读地址，wptr指向可写地址，end指向开辟地址的末尾.初始化后rptr=wptr=buffer；
        uint32_t rndx, wndx;//初始化后默认都是0,rndx代表此次已经读取的字节数,wndx代表已经写入的字节数(看源码)
    } AVFifoBuffer;
    下面两个结论画图理解即可：
    所以: wndx - rndx就是代表还剩余可读取的字节数大小,即av_fifo_size函数的实现.
    f->end - f->buffer - av_fifo_size(f)代表fifo的剩余空间,f->end - f->buffer代表fifo队列的大小,即av_fifo_space函数的实现.
    */
    AVFifoBuffer *frame_queue;          // 输入过滤器帧队列的大小；初始化时是8帧，av_fifo_alloc(8 * sizeof(AVFrame*))。

    // parameters configured for this input(为此输入配置的参数)
    int format;                                 // 视频时:输入视频的像素格式

    int width, height;                          // 输入视频的宽高
    AVRational sample_aspect_ratio;             // 视频时:宽高比率,一般都是{0,1}

    int sample_rate;
    int channels;                               // 音频通道数
    uint64_t channel_layout;                    // 通道布局

    AVBufferRef *hw_frames_ctx;

    int eof;                                    // 解码完成遇到eof时,可能会通过send_filter_eof()置为1?(可看process_input_packet,configure_filtergraph)
} InputFilter;

typedef struct OutputFilter {
    AVFilterContext     *filter;                // 输出过滤器ctx, 视频时是:buffersink, 音频时是:abuffersink
    struct OutputStream *ost;                   // 输出流
    struct FilterGraph  *graph;                 // 指向FilterGraph封装的系统过滤器
    uint8_t             *name;

    /* temporary storage until stream maps are processed */
    AVFilterInOut       *out_tmp;
    enum AVMediaType     type;

    /* desired output stream properties */
    int width, height;                          // 分辨率，仅视频有效(see /* set the filter output constraints */)
    AVRational frame_rate;                      // 帧率(see /* set the filter output constraints */)
    int format;                                 // 视频时：保存着该编码器上下文支持的视频像素格式；
                                                // 音频时：保存着该编码器上下文支持的音频像素格式；初始化时为-1.(see /* set the filter output constraints */)

    int sample_rate;                            // 采样率，仅音频有效
    uint64_t channel_layout;                    // 通道布局，仅音频有效

    // those are only set if no format is specified and the encoder gives us multiple options
    // 只有在没有指定格式并且编码器提供多个选项的情况下才会设置这些选项
    int *formats;                               // 与format实际是一样的，不过有两个不同点：1.这里的内容是从编码器中获取；2.保存的是数组.
    uint64_t *channel_layouts;                  // 通道布局数组，仅音频有效
    int *sample_rates;                          // 采样率数组，仅音频有效
} OutputFilter;

typedef struct FilterGraph {
    int            index;                       // 过滤器下标.see init_simple_filtergraph()
    const char    *graph_desc;                  // 图形描述.为空表示是简单过滤器,不为空则不是. see filtergraph_is_simple()

    AVFilterGraph *graph;                       // 系统过滤器
    int reconfiguration;                        // =1标记配置了AVFilterGraph

    InputFilter   **inputs;                     // 输入文件过滤器描述，数组
    int          nb_inputs;                     // inputs数组的个数
    OutputFilter **outputs;                     // 输出文件过滤器描述，数组
    int         nb_outputs;                     // outputs数组的个数
} FilterGraph;

typedef struct InputStream {
    int file_index;             // 输入文件的下标.例如-i 1.mp4 -i 2.mp4,两个输入文件下标依次是0,1
    AVStream *st;
    int discard;                /* true if stream data should be discarded */
                                // =1,该流读到的包都会被丢弃。输入流有效时,在new_output_stream()初始化为0
    int user_set_discard;
    int decoding_needed;        /* non zero if the packets must be decoded in 'raw_fifo', see DECODING_FOR_* */
                                // 非0表示输入流需要进行转码操作
#define DECODING_FOR_OST    1
#define DECODING_FOR_FILTER 2

    AVCodecContext *dec_ctx;    // 解码器上下文
    AVCodec *dec;               // choose_decoder找到的解码器
    AVFrame *decoded_frame;     // 存放解码后的一帧.视频在decode_video()开辟,音频在decode_audio()
    AVFrame *filter_frame; /* a ref of decoded_frame, to be sent to filters */ ///与decoded_frame类似

    int64_t       start;     /* time when read started */ // 指定-re选项时,在transcode_init会保存转码的开始时间,单位微秒
    /* predicted dts of the next packet read for this stream or (when there are
     * several frames in a packet) of the next frame in current packet (in AV_TIME_BASE units) */
    int64_t       next_dts;  ///单位微秒
    int64_t       dts;       ///< dts of the last packet read for this stream (in AV_TIME_BASE units)
                             ///(该流读取的最后一个包的dts,单位微秒)

    int64_t       next_pts;  ///< synthetic pts for the next decode frame (in AV_TIME_BASE units)(下一个解码帧的合成PTS)
    int64_t       pts;       ///< current pts of the decoded frame  (in AV_TIME_BASE units)
    int           wrap_correction_done;     // 启用的流更正开始时间是否完成, =1表示完成, 主要处理ts流, see process_input()

    int64_t filter_in_rescale_delta_last;   // 仅音频:统计样本数?debug看到该值一直会以样本数递增,例如0-1024-2048-3072...

    int64_t min_pts; /* pts with the smallest value in a current stream */
    int64_t max_pts; /* pts with the higher value in a current stream */

    // when forcing constant input framerate through -r,
    // this contains the pts that will be given to the next decoded frame
    // 当通过-r强制恒定的输入帧率时，这包含了将被赋予下一个解码帧的PTS
    int64_t cfr_next_pts;

    int64_t nb_samples; /* number of samples in the last decoded audio frame before looping.循环前最后解码音频帧中的采样数 */

    double ts_scale;                    // -itsscale选项,默认是1.0,设置输入ts的刻度
    int saw_first_ts;                   // 是否是第一次进来的时间戳,用于处理输入流InputStream的dts/pts(see process_input_packet).
    AVDictionary *decoder_opts;         // 用户输入的解码器选项,是从o中复制的
    AVRational framerate;               /* framerate forced with -r */
    int top_field_first;                // 是否优先显示顶部字段.
                                        // 该值视频时,会在decode_video给AVFrame->top_field_first赋值(如果内容是交错的，则首先显示顶部字段)
    int guess_layout_max;

    int autorotate;                     // -autorotate选项,与旋转角度有关

    int fix_sub_duration;
    struct { /* previous decoded subtitle and related variables */
        int got_output;
        int ret;
        AVSubtitle subtitle;
    } prev_sub;

    struct sub2video {
        int64_t last_pts;
        int64_t end_pts;
        AVFifoBuffer *sub_queue;    ///< queue of AVSubtitle* before filter init
        AVFrame *frame;
        int w, h;
    } sub2video;

    int dr1;

    /* decoded data from this stream goes into all those filters
     * currently video and audio only */
    InputFilter **filters;                      // 对比输出流OutputStream可以看到，输入流可以有多个输入过滤器,
                                                // 因为输出流的filters是一级指针，而这里输入流是二级指针
    int        nb_filters;                      // filters数组元素个数

    int reinit_filters;                         // -reinit_filter选项,ifilter参数改变是否重新初始化filtergraph,
                                                // =0时参数改变不会重新初始化,非0时会.默认值为-1,所以默认会重新初始化

    /* hwaccel options */
    enum HWAccelID hwaccel_id;                  // 硬件解码器id
    enum AVHWDeviceType hwaccel_device_type;    // 硬件设备类型，由hwaccel_id决定，hwaccel_id由OptionsContext.hwaccels(-hwaccel选项)决定
    char  *hwaccel_device;                      // -hwaccel_device选项
    enum AVPixelFormat hwaccel_output_format;

    /* hwaccel context */
    void  *hwaccel_ctx;
    void (*hwaccel_uninit)(AVCodecContext *s);
    int  (*hwaccel_get_buffer)(AVCodecContext *s, AVFrame *frame, int flags);// 自定义获取buffer回调
    int  (*hwaccel_retrieve_data)(AVCodecContext *s, AVFrame *frame);// 硬件检索数据回调，主要工作是硬解得到输出帧后，利用该函数进行处理
    enum AVPixelFormat hwaccel_pix_fmt;
    enum AVPixelFormat hwaccel_retrieved_pix_fmt;//硬件检索像素格式
    AVBufferRef *hw_frames_ctx;

    /* stats */
    // combined size of all the packets read(读取的所有包的大小之和)
    uint64_t data_size;             // 已经读取的包的总字节大小
    /* number of packets successfully read for this stream */
    uint64_t nb_packets;            // 已经读取的包的总数
    // number of frames/samples retrieved from the decoder(从解码器检索到的帧/样本数)
    uint64_t frames_decoded;        // 解码器已经解码的帧数
    uint64_t samples_decoded;       // 已经解码的样本数,仅音频有效

    int64_t *dts_buffer;            // eof时,统计有几个dts被保存到该数组.see decode_video()
    int nb_dts_buffer;              // dts_buffer的大小

    int got_output;                 // =1:标记该输入流至少已经成功解码一个pkt. see process_input_packet()
} InputStream;

// 封装输入文件相关信息的结构体
typedef struct InputFile {
    AVFormatContext *ctx; // 输入文件的ctx
    int eof_reached;      /* true if eof reached */// =1: 输入文件遇到eof
    int eagain;           /* true if last read attempt returned EAGAIN(如果上次读取尝试返回EAGAIN则为真) */
                          //=1表示上一次读pkt时,返回了eagain

    int ist_index;        /* index of first stream in input_streams *///输入文件中第一个流的下标,一般为0
    int loop;             /* set number of times input stream should be looped *///循环次数.例如无限次,-stream_loop -1
    int64_t duration;     /* actual duration of the longest stream in a file
                             at the moment when looping happens(循环发生时文件中最长流的实际持续时间,即stream_loop选项) */
                          // 该输入文件中时长为最长的流的duration
    AVRational time_base; /* time base of the duration */ // 上面duration字段的时基
    int64_t input_ts_offset;  // -itsoffset选项,设置输入时间戳偏移

    int64_t ts_offset;    // 时间戳偏移地址,目前理解意思为与0相差的偏移地址,所以一般是负数值.
                          // 与input_ts_offset、copy_ts、start_at_zero、-ss选项有关,都没加默认是0.
    int64_t last_ts;      // 上一个ptk的dts
    int64_t start_time;   /* user-specified start time in AV_TIME_BASE or AV_NOPTS_VALUE */
    int seek_timestamp;
    int64_t recording_time;
    int nb_streams;       /* number of stream that ffmpeg is aware of; may be different
                             from ctx.nb_streams if new streams appear during av_read_frame() */
    int nb_streams_warn;  /* number of streams that the user was warned of(警告用户的流的数量) */
    int rate_emu;               // -re选项。从OptionsContext.rate_emu得到，用户输入-re选项，rate_emu的值为1
    int accurate_seek;

#if HAVE_THREADS
    AVThreadMessageQueue *in_thread_queue;      // 该输入文件的线程消息队列.
                                                // 多个输入文件时,会开启多个线程,每个线程通过av_read_frame读到的pkt会存放到该队列

    pthread_t thread;           /* thread reading from this file */
    int non_blocking;           /* reading packets from the thread should not block(从线程读取数据包不应该被阻塞) */
                                // 存在多个输入文件即多线程读取时,av_read_frame是否阻塞,0=阻塞,1=非阻塞

    int joined;                 /* the thread has been joined */ // 0=未被回收,1=已经回收该线程thread
    int thread_queue_size;      /* maximum number of queued packets */
#endif
} InputFile;

enum forced_keyframes_const {
    FKF_N,
    FKF_N_FORCED,
    FKF_PREV_FORCED_N,
    FKF_PREV_FORCED_T,
    FKF_T,
    FKF_NB
};

#define ABORT_ON_FLAG_EMPTY_OUTPUT (1 <<  0)

extern const char *const forced_keyframes_const_names[];

typedef enum {
    ENCODER_FINISHED = 1,
    MUXER_FINISHED = 2,
} OSTFinished ;

typedef struct OutputStream {
    int file_index;          /* file index */
    int index;               /* stream index in the output file */// 由oc->nb_streams - 1得到,输出流的下标
    int source_index;        /* InputStream index *///对应输入流的下标
                             /*这里说明一下，index与source_index是不一样的，前者是输出流的下标，一般是按顺序递增的。例如
                              由于在open_output_file的if (!o->nb_stream_maps)流程，输出视频流总是优先new，
                              所以输出视频流的index可以说是0，而此时若输入文件的视频流假设是1，那么source_index就是1.
                              这就是两者的区别*/

    AVStream *st;            /* stream in the output file */
    int encoding_needed;     /* true if encoding needed for this stream *///是否需要编码；0=不需要 1=需要.一般由!stream_copy得到
    int frame_number;        // 已经写帧的数量. 控制台就是打印这个值
                             // 视频:在do_video_out()统计写帧的个数;音频:在write_packet()统计写帧的个数;

    /* input pts and corresponding output pts
       for A/V sync */
    struct InputStream *sync_ist; /* input stream to sync against */
    int64_t sync_opts;       /* output frame counter, could be changed to some true timestamp */ // FIXME look at frame_number
                             /* 输出帧计数器，可以更改为一些真实的时间戳. FIXME：查看frame_number */
                             // 视频时: 代表下一帧的pts(see do_video_out()),不过是以自增的方式存储,除以帧率(保存在编码器时基)即得到对应的pts.
                             // 音频时:代表下一帧的pts(see do_audio_out()),不过是以采样点的形式存储,除以采样率即得到对应的pts.

    /* pts of the first frame encoded for this stream, used for limiting
     * recording time */
    int64_t first_pts;
    /* dts of the last packet sent to the muxer */
    int64_t last_mux_dts;       // 发送到muxer的最后一个包的DTS，即最近有效的dts,单位ost->st->time_base
    // the timebase of the packets sent to the muxer
    AVRational mux_timebase;    // 发送到muxer的数据包的时间基准
    AVRational enc_timebase;    // 由OptionsContext.enc_time_bases参数解析得到

    int                    nb_bitstream_filters;    // bsf_ctx数组大小.see init_output_bsfs()
    AVBSFContext            **bsf_ctx;              // 位流数组

    AVCodecContext *enc_ctx;    // 通过enc创建的编码器上下文
    AVCodecParameters *ref_par; /* associated input codec parameters with encoders options applied.
                                 (将输入编解码器参数与应用的编码器选项关联起来)*/

    AVCodec *enc;               // 通过choose_encoder得到的编码器
    int64_t max_frames;         // 通过OptionsContext.max_frames得到
    AVFrame *filtered_frame;    // 用于存储编码后的帧,在reap_filters时会给其开辟内存
    AVFrame *last_frame;
    int last_dropped;
    int last_nb0_frames[3];

    void  *hwaccel_ctx;

    /* video only */
    AVRational frame_rate;              // 帧率，由OptionsContext.frame_rates即-r选项得到
    int is_cfr;                         // 当format_video_sync 等于 VSYNC_CFR或者VSYNC_VSCFR时，为1.
    int force_fps;                      // -force_fps强制帧率选项.设置后不会再自动考虑编码器最好的帧率
    int top_field_first;                // ?
    int rotate_overridden;
    double rotate_override_value;

    AVRational frame_aspect_ratio;      // 对应OptionsContext.frame_aspect_ratios

    /* forced key frames */
    int64_t forced_kf_ref_pts;
    int64_t *forced_kf_pts;             // 强制关键帧pts数组.暂未深入研究
    int forced_kf_count;                // forced_kf_pts数组大小
    int forced_kf_index;                // 当前关键帧下标？
    char *forced_keyframes;
    AVExpr *forced_keyframes_pexpr;
    double forced_keyframes_expr_const_values[FKF_NB];

    /* audio only */
    int *audio_channels_map;             /* list of the channels id to pick from the source stream */
                                         // 要从源流中选择的通道id列表
    int audio_channels_mapped;           /* number of channels in audio_channels_map */

    char *logfile_prefix;
    FILE *logfile;                      // 日志文件句柄

    OutputFilter *filter;               // 指向输出过滤器
    char *avfilter;                     // 最终保存filters或者filters_script中过滤器描述的内容
    char *filters;         ///< filtergraph associated to the -filter option//与-filter选项相关联的Filtergraph
    char *filters_script;  ///< filtergraph script associated to the -filter_script option//与-filter_script选项相关联的Filtergraph脚本

    AVDictionary *encoder_opts;         // 保存着用户指定输出的编码器选项
    AVDictionary *sws_dict;             // 视频转码参数选项，一般用于avfilter滤镜相关.(最终被应用到AVFilterGraph的scale_sws_opts成员)
    AVDictionary *swr_opts;             // 音频相关配置选项.(最终会使用av_opt_set应用到AVFilterGraph的aresample_swr_opts成员)
    AVDictionary *resample_opts;        // 重采样选项.
    char *apad;
    OSTFinished finished;               /* no more packets should be written for this stream(输出流完成，则不应该再为该流写入任何信息包) */
                                        // 主要通过close_all_output_streams()/close_output_stream()/finish_output_stream()标记完成

    int unavailable;                    /* true if the steram is unavailable (possibly temporarily) */
                                        // 流是否可用,0-可用,1-不可用,循环时开始会重置它为0

    int stream_copy;                    // 是否不转码输出，例如-vcodec copy选项.0=转码 1=不转码.只要置为0，音视频都会进入转码的流程。see choose_encoder()

    // init_output_stream() has been called for this stream
    // The encoder and the bitstream filters have been initialized and the stream
    // parameters are set in the AVStream.
    int initialized;                    // =1表示init_output_stream()调用完成.see init_output_stream()

    int inputs_done;                    // =1表示输入流全部处理完成,主要用于选择输入流进行解码,see transcode_step()

    const char *attachment_filename;
    int copy_initial_nonkeyframes;      // 对应OptionsContext.copy_initial_nonkeyframes.复制最初的非关键帧
    int copy_prior_start;               // 对应OptionsContext.copy_prior_start.在开始时间之前复制或丢弃帧
    char *disposition;                  // 对应OptionsContext.disposition，即-disposition选项

    int keep_pix_fmt;                   // 当指定pix_fmt选项 且 *frame_pix_fmt == '+' 时,keep_pix_fmt=1

    /* stats */
    // combined size of all the packets written
    uint64_t data_size;                 // 写入的所有数据包的组合大小
    // number of packets send to the muxer
    uint64_t packets_written;
    // number of frames/samples sent to the encoder
    //(发送到编码器的帧/样本数)
    uint64_t frames_encoded;
    uint64_t samples_encoded;

    /* packet quality factor */
    int quality;                        // 编码质量，等价于ffmpeg命令行打印的q.在write_packet()得到

    int max_muxing_queue_size;          // 默认最大复用队列的大小为128，new_output_stream时指定

    /* the packets are buffered here until the muxer is ready to be initialized */
    AVFifoBuffer *muxing_queue;         // new_output_stream时开辟内存.

    /* packet picture type */
    int pict_type;                      // 帧类型

    /* frame encode sum of squared error values */
    int64_t error[4];                   // 用于存储编码时的错误
} OutputStream;

typedef struct OutputFile {
    AVFormatContext *ctx;   // 输出文件的解复用上下文
    AVDictionary *opts;     // 解复用选项，由o->g->format_opts拷贝得到.see open_output_file()
    int ost_index;          /* index of the first stream in output_streams */
    int64_t recording_time;  ///< desired length of the resulting file in microseconds == AV_TIME_BASE units
                            //结果文件的期望长度(以微秒为单位)== AV_TIME_BASE单位.(录像时长)

    int64_t start_time;      ///< start time in microseconds == AV_TIME_BASE units
    uint64_t limit_filesize; /* filesize limit expressed in bytes(文件大小限制，以字节为单位),-fs选项.*/

    int shortest;           // -shortest选项得到的值,=1:最短时长的流完成输出时,其它流也要关闭.

    int header_written;     // =1表示调用avformat_write_header()成功.
} OutputFile;


extern InputStream **input_streams;         /*二维数组，用于保存每一个InputStream *输入文件里面的各个流，例如保存了视频流+音频流
那么input_streams[0]、input_streams[1]就是对应音视频流的信息*/
extern int        nb_input_streams;         // input_streams二维数组大小

extern InputFile   **input_files;           // 用于保存多个输入文件
extern int        nb_input_files;           // 输入文件个数

extern OutputStream **output_streams;       // 保存各个输出流的数组
extern int         nb_output_streams;       // output_streams二维数组大小

extern OutputFile   **output_files;         // 用于保存多个输出文件
extern int         nb_output_files;         // 输出文件个数

extern FilterGraph **filtergraphs;          // 封装好的系统过滤器数组，每个FilterGraph都会包含对应输入流与输出流的的输入输出过滤器。可看init_simple_filtergraph函数
extern int        nb_filtergraphs;          // filtergraphs数组的大小

extern char *vstats_filename;               //  由-vstats_file选项管理,设置后可以将视频流的相关信息保存到文件
extern char *sdp_filename;

extern float audio_drift_threshold;         // -adrift_threshold选项,默认0.1
extern float dts_delta_threshold;           // -dts_delta_threshold选项,默认10,时间戳不连续增量阈值
extern float dts_error_threshold;

extern int audio_volume;                    // -vol选项,默认256
extern int audio_sync_method;               // -async选项,音频同步方法.默认0
extern int video_sync_method;               // -vsync选项,视频同步方法,默认-1,表示自动
extern float frame_drop_threshold;
extern int do_benchmark;
extern int do_benchmark_all;                // -benchmark_all选项，默认0
extern int do_deinterlace;                  // -deinterlace选项,默认0.
extern int do_hex_dump;                     // -hex选项,当指定-dump选项dump pkt,也dump payload
extern int do_pkt_dump;                     // -dump选项,默认0
extern int copy_ts;                         // -copyts选项，默认0
extern int start_at_zero;                   // -start_at_zero选项，默认0
extern int copy_tb;
extern int debug_ts;                        // 是否打印相关时间戳.-debug_ts选项
extern int exit_on_error;                   // -xerror选项,默认是0
extern int abort_on_flags;
extern int print_stats;                     // -stats选项,默认-1,在编码期间打印进度报告.
extern int qp_hist;                         // -qphist选项,默认0,显示QP直方图
extern int stdin_interaction;
extern int frame_bits_per_raw_sample;
extern AVIOContext *progress_avio;
extern float max_error_rate;
extern char *videotoolbox_pixfmt;

extern int filter_nbthreads;                // -filter_threads选项,默认0,非复杂过滤器线程数.
extern int filter_complex_nbthreads;        // -filter_complex_threads选项,默认0,复杂过滤器线程数.
extern int vstats_version;                  // -vstats_version选项, 默认2, 要使用的vstats格式的版本

extern const AVIOInterruptCB int_cb;

extern const OptionDef options[];
extern const HWAccel hwaccels[];
extern AVBufferRef *hw_device_ctx;
#if CONFIG_QSV
extern char *qsv_device;
#endif
extern HWDevice *filter_hw_device;          // -filter_hw_device选项


void term_init(void);
void term_exit(void);

void reset_options(OptionsContext *o, int is_input);
void show_usage(void);

void opt_output_file(void *optctx, const char *filename);

void remove_avoptions(AVDictionary **a, AVDictionary *b);
void assert_avoptions(AVDictionary *m);

int guess_input_channel_layout(InputStream *ist);

enum AVPixelFormat choose_pixel_fmt(AVStream *st, AVCodecContext *avctx, AVCodec *codec, enum AVPixelFormat target);
void choose_sample_fmt(AVStream *st, AVCodec *codec);

int configure_filtergraph(FilterGraph *fg);
int configure_output_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out);
void check_filter_outputs(void);
int ist_in_filtergraph(FilterGraph *fg, InputStream *ist);
int filtergraph_is_simple(FilterGraph *fg);
int init_simple_filtergraph(InputStream *ist, OutputStream *ost);
int init_complex_filtergraph(FilterGraph *fg);

void sub2video_update(InputStream *ist, AVSubtitle *sub);

int ifilter_parameters_from_frame(InputFilter *ifilter, const AVFrame *frame);

int ffmpeg_parse_options(int argc, char **argv);

int videotoolbox_init(AVCodecContext *s);
int qsv_init(AVCodecContext *s);
int cuvid_init(AVCodecContext *s);

HWDevice *hw_device_get_by_name(const char *name);
int hw_device_init_from_string(const char *arg, HWDevice **dev);
void hw_device_free_all(void);

int hw_device_setup_for_decode(InputStream *ist);
int hw_device_setup_for_encode(OutputStream *ost);

int hwaccel_decode_init(AVCodecContext *avctx);

#endif /* FFTOOLS_FFMPEG_H */
