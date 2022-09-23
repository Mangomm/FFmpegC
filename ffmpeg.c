/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
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

/**
 * @file
 * multimedia converter based on the FFmpeg libraries
 */

#include "config.h"
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswresample/swresample.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/display.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/bprint.h"
#include "libavutil/time.h"
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"
#include "libavcodec/mathops.h"
#include "libavformat/os_support.h"

# include "libavfilter/avfilter.h"
# include "libavfilter/buffersrc.h"
# include "libavfilter/buffersink.h"

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif
#if HAVE_SETCONSOLECTRLHANDLER
#include <windows.h>
#endif


#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif

#include <time.h>

#include "ffmpeg.h"
#include "cmdutils.h"

#include "libavutil/avassert.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

static FILE *vstats_file;

const char *const forced_keyframes_const_names[] = {
    "n",
    "n_forced",
    "prev_forced_n",
    "prev_forced_t",
    "t",
    NULL
};

typedef struct BenchmarkTimeStamps {
    int64_t real_usec;          // transcode()调用前的实时时间，see av_gettime_relative().单位微秒
    int64_t user_usec;          // 进程在用户层消耗的时间.单位微秒
    int64_t sys_usec;           // 进程在内核层消耗的时间.单位微秒
} BenchmarkTimeStamps;

static void do_video_stats(OutputStream *ost, int frame_size);
static BenchmarkTimeStamps get_benchmark_time_stamps(void);
static int64_t getmaxrss(void);
static int ifilter_has_all_input_formats(FilterGraph *fg);

static int run_as_daemon  = 0;  // 0=前台运行；1=后台运行
static int nb_frames_dup = 0;
static unsigned dup_warning = 1000;
static int nb_frames_drop = 0;
static int64_t decode_error_stat[2];// 记录解码的状态.下标0记录的是成功解码的参数,下标1是失败的次数.

static int want_sdp = 1;

static BenchmarkTimeStamps current_time;
AVIOContext *progress_avio = NULL;  // 指定-progress选项,会在回调为该全局对象初始化

static uint8_t *subtitle_out;

InputStream **input_streams = NULL;
int        nb_input_streams = 0;
InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputStream **output_streams = NULL;
int         nb_output_streams = 0;
OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

#if HAVE_TERMIOS_H

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int restore_tty;
#endif

#if HAVE_THREADS
static void free_input_threads(void);
#endif

/* sub2video hack:
   Convert subtitles to video with alpha to insert them in filter graphs.
   This is a temporary solution until libavfilter gets real subtitles support.
 */

static int sub2video_get_blank_frame(InputStream *ist)
{
    int ret;
    AVFrame *frame = ist->sub2video.frame;

    av_frame_unref(frame);
    ist->sub2video.frame->width  = ist->dec_ctx->width  ? ist->dec_ctx->width  : ist->sub2video.w;
    ist->sub2video.frame->height = ist->dec_ctx->height ? ist->dec_ctx->height : ist->sub2video.h;
    ist->sub2video.frame->format = AV_PIX_FMT_RGB32;
    if ((ret = av_frame_get_buffer(frame, 32)) < 0)
        return ret;
    memset(frame->data[0], 0, frame->height * frame->linesize[0]);
    return 0;
}

static void sub2video_copy_rect(uint8_t *dst, int dst_linesize, int w, int h,
                                AVSubtitleRect *r)
{
    uint32_t *pal, *dst2;
    uint8_t *src, *src2;
    int x, y;

    if (r->type != SUBTITLE_BITMAP) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: non-bitmap subtitle\n");
        return;
    }
    if (r->x < 0 || r->x + r->w > w || r->y < 0 || r->y + r->h > h) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: rectangle (%d %d %d %d) overflowing %d %d\n",
            r->x, r->y, r->w, r->h, w, h
        );
        return;
    }

    dst += r->y * dst_linesize + r->x * 4;
    src = r->data[0];
    pal = (uint32_t *)r->data[1];
    for (y = 0; y < r->h; y++) {
        dst2 = (uint32_t *)dst;
        src2 = src;
        for (x = 0; x < r->w; x++)
            *(dst2++) = pal[*(src2++)];
        dst += dst_linesize;
        src += r->linesize[0];
    }
}

static void sub2video_push_ref(InputStream *ist, int64_t pts)
{
    AVFrame *frame = ist->sub2video.frame;
    int i;
    int ret;

    av_assert1(frame->data[0]);
    ist->sub2video.last_pts = frame->pts = pts;
    for (i = 0; i < ist->nb_filters; i++) {
        ret = av_buffersrc_add_frame_flags(ist->filters[i]->filter, frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF |
                                           AV_BUFFERSRC_FLAG_PUSH);
        if (ret != AVERROR_EOF && ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Error while add the frame to buffer source(%s).\n",
                   av_err2str(ret));
    }
}

void sub2video_update(InputStream *ist, AVSubtitle *sub)
{
    AVFrame *frame = ist->sub2video.frame;
    int8_t *dst;
    int     dst_linesize;
    int num_rects, i;
    int64_t pts, end_pts;

    if (!frame)
        return;
    if (sub) {
        pts       = av_rescale_q(sub->pts + sub->start_display_time * 1000LL,
                                 AV_TIME_BASE_Q, ist->st->time_base);
        end_pts   = av_rescale_q(sub->pts + sub->end_display_time   * 1000LL,
                                 AV_TIME_BASE_Q, ist->st->time_base);
        num_rects = sub->num_rects;
    } else {
        pts       = ist->sub2video.end_pts;
        end_pts   = INT64_MAX;
        num_rects = 0;
    }
    if (sub2video_get_blank_frame(ist) < 0) {
        av_log(ist->dec_ctx, AV_LOG_ERROR,
               "Impossible to get a blank canvas.\n");
        return;
    }
    dst          = frame->data    [0];
    dst_linesize = frame->linesize[0];
    for (i = 0; i < num_rects; i++)
        sub2video_copy_rect(dst, dst_linesize, frame->width, frame->height, sub->rects[i]);
    sub2video_push_ref(ist, pts);
    ist->sub2video.end_pts = end_pts;
}

static void sub2video_heartbeat(InputStream *ist, int64_t pts)
{
    InputFile *infile = input_files[ist->file_index];
    int i, j, nb_reqs;
    int64_t pts2;

    /* When a frame is read from a file, examine all sub2video streams in
       the same file and send the sub2video frame again. Otherwise, decoded
       video frames could be accumulating in the filter graph while a filter
       (possibly overlay) is desperately waiting for a subtitle frame. */
    for (i = 0; i < infile->nb_streams; i++) {
        InputStream *ist2 = input_streams[infile->ist_index + i];
        if (!ist2->sub2video.frame)
            continue;
        /* subtitles seem to be usually muxed ahead of other streams;
           if not, subtracting a larger time here is necessary */
        pts2 = av_rescale_q(pts, ist->st->time_base, ist2->st->time_base) - 1;
        /* do not send the heartbeat frame if the subtitle is already ahead */
        if (pts2 <= ist2->sub2video.last_pts)
            continue;
        if (pts2 >= ist2->sub2video.end_pts ||
            (!ist2->sub2video.frame->data[0] && ist2->sub2video.end_pts < INT64_MAX))
            sub2video_update(ist2, NULL);
        for (j = 0, nb_reqs = 0; j < ist2->nb_filters; j++)
            nb_reqs += av_buffersrc_get_nb_failed_requests(ist2->filters[j]->filter);
        if (nb_reqs)
            sub2video_push_ref(ist2, pts2);
    }
}

static void sub2video_flush(InputStream *ist)
{
    int i;
    int ret;

    if (ist->sub2video.end_pts < INT64_MAX)
        sub2video_update(ist, NULL);
    for (i = 0; i < ist->nb_filters; i++) {
        ret = av_buffersrc_add_frame(ist->filters[i]->filter, NULL);
        if (ret != AVERROR_EOF && ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Flush the frame error.\n");
    }
}

/* end of sub2video hack */

static void term_exit_sigsafe(void)
{
#if HAVE_TERMIOS_H
    if(restore_tty)
        tcsetattr (0, TCSANOW, &oldtty);
#endif
}

void term_exit(void)
{
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    term_exit_sigsafe();
}

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static atomic_int transcode_init_done = ATOMIC_VAR_INIT(0);
static volatile int ffmpeg_exited = 0;
static int main_return_code = 0;

static void
sigterm_handler(int sig)
{
    int ret;
    received_sigterm = sig;
    received_nb_signals++;
    term_exit_sigsafe();
    if(received_nb_signals > 3) {
        ret = write(2/*STDERR_FILENO*/, "Received > 3 system signals, hard exiting\n",
                    strlen("Received > 3 system signals, hard exiting\n"));
        if (ret < 0) { /* Do nothing */ };
        exit(123);
    }
}

#if HAVE_SETCONSOLECTRLHANDLER
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    av_log(NULL, AV_LOG_DEBUG, "\nReceived windows signal %ld\n", fdwCtrlType);

    switch (fdwCtrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        sigterm_handler(SIGINT);
        return TRUE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sigterm_handler(SIGTERM);
        /* Basically, with these 3 events, when we return from this method the
           process is hard terminated, so stall as long as we need to
           to try and let the main thread(s) clean up and gracefully terminate
           (we have at most 5 seconds, but should be done far before that). */
        while (!ffmpeg_exited) {
            Sleep(0);
        }
        return TRUE;

    default:
        av_log(NULL, AV_LOG_ERROR, "Received unknown windows signal %ld\n", fdwCtrlType);
        return FALSE;
    }
}
#endif

void term_init(void)
{
#if HAVE_TERMIOS_H
    if (!run_as_daemon && stdin_interaction) {
        struct termios tty;
        if (tcgetattr (0, &tty) == 0) {
            oldtty = tty;
            restore_tty = 1;

            tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                             |INLCR|IGNCR|ICRNL|IXON);
            tty.c_oflag |= OPOST;
            tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
            tty.c_cflag &= ~(CSIZE|PARENB);
            tty.c_cflag |= CS8;
            tty.c_cc[VMIN] = 1;
            tty.c_cc[VTIME] = 0;

            tcsetattr (0, TCSANOW, &tty);
        }
        signal(SIGQUIT, sigterm_handler); /* Quit (POSIX).  */
    }
#endif

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
#ifdef SIGXCPU
    signal(SIGXCPU, sigterm_handler);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* Broken pipe (POSIX). */
#endif
#if HAVE_SETCONSOLECTRLHANDLER
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);
#endif
}

/* read a key without blocking */
static int read_key(void)
{
    unsigned char ch;
#if HAVE_TERMIOS_H
    int n = 1;
    struct timeval tv;
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(1, &rfds, NULL, NULL, &tv);
    if (n > 0) {
        n = read(0, &ch, 1);
        if (n == 1)
            return ch;

        return n;
    }
#elif HAVE_KBHIT
#    if HAVE_PEEKNAMEDPIPE
    static int is_pipe;
    static HANDLE input_handle;
    DWORD dw, nchars;
    // 1. 获取输入句柄，并且获取控制台模式(不过ffmpeg没有对控制台模式进一步处理).
    if(!input_handle){
        //获取输入设备的句柄.GetStdHandle see https://blog.csdn.net/dark_cy/article/details/89103875
        input_handle = GetStdHandle(STD_INPUT_HANDLE);
        /*GetConsoleMode() see https://docs.microsoft.com/en-us/windows/console/getconsolemode.
         返回0表示错误；非0成功.*/
        is_pipe = !GetConsoleMode(input_handle, &dw);//获取控制台模式 see https://blog.csdn.net/zanglengyu/article/details/125855938
    }

    // 2. 获取控制台模式失败的处理
    if (is_pipe) {
        /* When running under a GUI, you will end here.(当在GUI下运行时，您将在这里结束) */
        /* PeekNamedPipe()：从句柄的缓冲区读取数据，若不想读取，参2传NULL即可.管道不存在会报错返回0。
         * msdn：https://docs.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-peeknamedpipe
         * see http://www.manongjc.com/detail/19-xpsioohwzlyyvpp.html的例子*/
        if (!PeekNamedPipe(input_handle, NULL, 0, NULL, &nchars, NULL)) {//nchars是读到的字节数
            // input pipe may have been closed by the program that ran ffmpeg
            //(输入管道可能已被运行ffmpeg的程序关闭)
            return -1;
        }
        //Read it
        if(nchars != 0) {
            read(0, &ch, 1);
            return ch;
        }else{
            return -1;
        }
    }
#    endif

    if(kbhit())
        return(getch());
#endif
    return -1;
}

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > atomic_load(&transcode_init_done);
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void ffmpeg_cleanup(int ret)
{
    int i, j;

    if (do_benchmark) {
        int maxrss = getmaxrss() / 1024;
        av_log(NULL, AV_LOG_INFO, "bench: maxrss=%ikB\n", maxrss);
    }

    // 1. 释放fg数组相关内容
    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        // 1.1 释放滤波图
        avfilter_graph_free(&fg->graph);

        // 1.2 释放InputFilter数组
        for (j = 0; j < fg->nb_inputs; j++) {
            while (av_fifo_size(fg->inputs[j]->frame_queue)) {//释放帧队列剩余的帧
                AVFrame *frame;
                av_fifo_generic_read(fg->inputs[j]->frame_queue, &frame,
                                     sizeof(frame), NULL);
                av_frame_free(&frame);
            }
            av_fifo_freep(&fg->inputs[j]->frame_queue);
            if (fg->inputs[j]->ist->sub2video.sub_queue) {
                while (av_fifo_size(fg->inputs[j]->ist->sub2video.sub_queue)) {
                    AVSubtitle sub;
                    av_fifo_generic_read(fg->inputs[j]->ist->sub2video.sub_queue,
                                         &sub, sizeof(sub), NULL);
                    avsubtitle_free(&sub);
                }
                av_fifo_freep(&fg->inputs[j]->ist->sub2video.sub_queue);
            }
            av_buffer_unref(&fg->inputs[j]->hw_frames_ctx);
            av_freep(&fg->inputs[j]->name);
            av_freep(&fg->inputs[j]);
        }
        av_freep(&fg->inputs);

        // 1.3 释放OutputFilter数组
        for (j = 0; j < fg->nb_outputs; j++) {
            av_freep(&fg->outputs[j]->name);
            av_freep(&fg->outputs[j]->formats);
            av_freep(&fg->outputs[j]->channel_layouts);
            av_freep(&fg->outputs[j]->sample_rates);
            av_freep(&fg->outputs[j]);
        }
        av_freep(&fg->outputs);
        av_freep(&fg->graph_desc);

        // 1.4 释放fg元素
        av_freep(&filtergraphs[i]);
    }
    // 1.5 释放fg数组本身
    av_freep(&filtergraphs);

    av_freep(&subtitle_out);

    // 2. 关闭输出文件
    /* close files */
    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        AVFormatContext *s;
        if (!of)
            continue;
        s = of->ctx;
        if (s && s->oformat && !(s->oformat->flags & AVFMT_NOFILE))
            avio_closep(&s->pb);
        avformat_free_context(s);
        av_dict_free(&of->opts);

        av_freep(&output_files[i]);
    }

    // 3. 关闭输出流
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!ost)
            continue;

        for (j = 0; j < ost->nb_bitstream_filters; j++)
            av_bsf_free(&ost->bsf_ctx[j]);
        av_freep(&ost->bsf_ctx);

        av_frame_free(&ost->filtered_frame);
        av_frame_free(&ost->last_frame);
        av_dict_free(&ost->encoder_opts);

        av_freep(&ost->forced_keyframes);
        av_expr_free(ost->forced_keyframes_pexpr);
        av_freep(&ost->avfilter);
        av_freep(&ost->logfile_prefix);

        av_freep(&ost->audio_channels_map);
        ost->audio_channels_mapped = 0;

        av_dict_free(&ost->sws_dict);
        av_dict_free(&ost->swr_opts);

        avcodec_free_context(&ost->enc_ctx);// 释放编码器上下文
        avcodec_parameters_free(&ost->ref_par);

        if (ost->muxing_queue) {
            while (av_fifo_size(ost->muxing_queue)) {
                AVPacket pkt;
                av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
                av_packet_unref(&pkt);
            }
            av_fifo_freep(&ost->muxing_queue);
        }

        av_freep(&output_streams[i]);
    }

    // 4. 回收输入线程
#if HAVE_THREADS
    free_input_threads();
#endif

    // 5. 关闭输入文件
    for (i = 0; i < nb_input_files; i++) {
        avformat_close_input(&input_files[i]->ctx);
        av_freep(&input_files[i]);
    }

    // 6. 关闭输入流
    for (i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];

        av_frame_free(&ist->decoded_frame);
        av_frame_free(&ist->filter_frame);
        av_dict_free(&ist->decoder_opts);
        avsubtitle_free(&ist->prev_sub.subtitle);
        av_frame_free(&ist->sub2video.frame);
        av_freep(&ist->filters);
        av_freep(&ist->hwaccel_device);
        av_freep(&ist->dts_buffer);

        avcodec_free_context(&ist->dec_ctx);// 关闭解码器上下文

        av_freep(&input_streams[i]);
    }

    if (vstats_file) {
        if (fclose(vstats_file))
            av_log(NULL, AV_LOG_ERROR,
                   "Error closing vstats file, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
    }
    av_freep(&vstats_filename);

    // 7. 关闭输入输出文件、流数组本身.
    av_freep(&input_streams);
    av_freep(&input_files);
    av_freep(&output_streams);
    av_freep(&output_files);

    uninit_opts();

    avformat_network_deinit();

    if (received_sigterm) {
        av_log(NULL, AV_LOG_INFO, "Exiting normally, received signal %d.\n",
               (int) received_sigterm);
    } else if (ret && atomic_load(&transcode_init_done)) {
        av_log(NULL, AV_LOG_INFO, "Conversion failed!\n");
    }
    term_exit();
    ffmpeg_exited = 1;
}

/**
 * @brief 利用b中的key，将a中有同样key的value设置为NULL.获取时忽略后缀，设置时匹配大小写.
 * @param a 字典1
 * @param b 字典2
*/
void remove_avoptions(AVDictionary **a, AVDictionary *b)
{
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(b, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

/**
 * @brief 只有m中还有一个k-v键值对选项，那么程序就会退出.
*/
void assert_avoptions(AVDictionary *m)
{
    AVDictionaryEntry *t;
    if ((t = av_dict_get(m, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        exit_program(1);
    }
}

static void abort_codec_experimental(AVCodec *c, int encoder)
{
    exit_program(1);
}

static void update_benchmark(const char *fmt, ...)
{
    if (do_benchmark_all) {
        BenchmarkTimeStamps t = get_benchmark_time_stamps();
        va_list va;
        char buf[1024];

        if (fmt) {
            va_start(va, fmt);
            vsnprintf(buf, sizeof(buf), fmt, va);
            va_end(va);
            av_log(NULL, AV_LOG_INFO,
                   "bench: %8" PRIu64 " user %8" PRIu64 " sys %8" PRIu64 " real %s \n",
                   t.user_usec - current_time.user_usec,
                   t.sys_usec - current_time.sys_usec,
                   t.real_usec - current_time.real_usec, buf);
        }
        current_time = t;
    }
}

/**
 * @brief 给输出流退出时进行标记。
 * @param ost 输出流
 * @param this_stream ost的标记
 * @param others ost以外的其它输出流的标记
 * @note 并不是真正关闭输出流
*/
static void close_all_output_streams(OutputStream *ost, OSTFinished this_stream, OSTFinished others)
{
    int i;
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost2 = output_streams[i];
        ost2->finished |= ost == ost2 ? this_stream : others;
    }
}

/**
 * @brief 将pkt写到输出文件(可以是文件或者实时流地址)
 * @param of 输出文件
 * @param pkt pkt
 * @param ost 输出流
 * @param unqueue 是否排序？
*/
static void write_packet(OutputFile *of, AVPacket *pkt, OutputStream *ost, int unqueue)
{
    AVFormatContext *s = of->ctx;
    AVStream *st = ost->st;
    int ret;

    /*
     * Audio encoders may split the packets --  #frames in != #packets out.
     * But there is no reordering, so we can limit the number of output packets
     * by simply dropping them here.
     * Counting encoded video frames needs to be done separately because of
     * reordering, see do_video_out().
     * Do not count the packet when unqueued because it has been counted when queued.
     */
    /*
     * 音频编码器可能会分割数据包 --  #输入帧!= #输出包。
     * 但是没有重新排序，所以我们可以通过简单地将它们放在这里来限制输出数据包的数量。
     * 由于重新排序，编码的视频帧需要单独计数，参见do_video_out()。
     * 不要在未排队时计数数据包，因为它在排队时已被计数。
     */
    /* 1."视频需要编码"以外的类型，且unqueue=0时，会判断是否需要丢包. */
    if (!(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ost->encoding_needed) && !unqueue) {
        if (ost->frame_number >= ost->max_frames) {//帧数大于允许的最大帧数，不作处理，并将该包释放
            av_packet_unref(pkt);
            return;
        }
        ost->frame_number++;//视频流时，不会在这里计数
    }

    /* 2.没有写头或者写头失败，先将包缓存在复用队列，然后返回，不会调用到写帧函数 */
    if (!of->header_written) {
        AVPacket tmp_pkt = {0};
        /* the muxer is not initialized yet, buffer the packet(muxer尚未初始化，请缓冲该数据包) */
        //2.1 fifo没有空间，则给其扩容
        if (!av_fifo_space(ost->muxing_queue)) {//return f->end - f->buffer - av_fifo_size(f);
            /*新队列大小求法：若在最大队列大小内，那么以当前队列大小的两倍扩容.
            例如用户不指定大小，默认是给复用队列先开辟8个空间大小，若此时没空间，说明av_fifo_size()返回的值为8*sizeof(pkt)，
            那么乘以2后，new_size值就是16个pkt的大小了。*/
            int new_size = FFMIN(2 * av_fifo_size(ost->muxing_queue),
                                 ost->max_muxing_queue_size);
            /*若输出流的包缓存到达最大复用队列，那么ffmpeg会退出程序*/
            if (new_size <= av_fifo_size(ost->muxing_queue)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Too many packets buffered for output stream %d:%d.\n",
                       ost->file_index, ost->st->index);
                exit_program(1);
            }
            ret = av_fifo_realloc2(ost->muxing_queue, new_size);
            if (ret < 0)
                exit_program(1);
        }
        /*
         * av_packet_make_refcounted(): 确保给定数据包所描述的数据被引用计数。
         * @note 此函数不确保引用是可写的。 为此使用av_packet_make_writable。
         * @see av_packet_ref.
         * @see av_packet_make_writable.
         * @param pkt 计算需要参考的PKT数据包.
         * @return 成功为0，错误为负AVERROR。失败时，数据包不变。
         * 源码也不难，工作是：
         * 拷贝packet数据到pkt->buf->data中，再令pkt->data指向它.
         */
        ret = av_packet_make_refcounted(pkt);
        if (ret < 0)
            exit_program(1);
        av_packet_move_ref(&tmp_pkt, pkt);//所有权转移，源码很简单
        /*
         * av_fifo_generic_write(): 将数据从用户提供的回调提供给AVFifoBuffer。
         * @param f 要写入的AVFifoBuffer.
         * @param src 数据来源;非const，因为它可以被定义在func中的函数用作可修改的上下文.
         * @param size 要写入的字节数.
         * @param func 一般写函数;第一个参数是src，第二个参数是dest_buf，第三个参数是dest_buf_size.
         * Func必须返回写入dest_buf的字节数，或<= 0表示没有更多可写入的数据。如果func为NULL, src将被解释为源数据的简单字节数组。
         * @return 写入FIFO的字节数.
         * av_fifo_generic_write()的源码不算难.
         */
        av_fifo_generic_write(ost->muxing_queue, &tmp_pkt, sizeof(tmp_pkt), NULL);
        return;
    }

    if ((st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_sync_method == VSYNC_DROP) ||
        (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_sync_method < 0))
        pkt->pts = pkt->dts = AV_NOPTS_VALUE;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        int i;
        //关于下面获取q编码质量可参考https://blog.csdn.net/u012117034/article/details/123453863
        /*
         * av_packet_get_side_data():从packet获取side info.
         * @param type 期待side information的类型
         * @param size 用于存储side info大小的指针(可选)
         * @return 如果存在数据，则指向数据的指针，否则为NULL.
         * 源码很简单.
        */
        //获取编码质量状态的side data info
        uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS,
                                              NULL);
        //从sd中获取前32bit
        /*AV_RL32(sd)的大概调用过程：
         * 1)AV_RL(32, p)
         * 2）而AV_RL的定义：
         * #   define AV_RL(s, p)    AV_RN##s(p)
         * 3）所以此时调用变成(##表示将参数与前后拼接)：
         * AV_RN32(p)
         * 4）而AV_RN32的定义：
         * #   define AV_RN32(p) AV_RN(32, p)
         * 5）而AV_RN的定义：
         * #   define AV_RN(s, p) (*((const __unaligned uint##s##_t*)(p)))
         * 所以最终就是取sd前32bit的内容.(这部分是我使用vscode看源码得出的，qt貌似会跳转的共用体，不太准？)
         *
         * 至于为什么要获取32bit，看AV_PKT_DATA_QUALITY_STATS枚举注释，这个信息会由编码器填充，
         * 被放在side info的前32bit，然后第5个字节存放帧类型，第6个字节存放错误次数，
         * 第7、8两字节为保留位，
         * 后面剩余的8字节是sum of squared differences between encoder in and output.翻译是 编码器输入和输出之间差的平方和.
        */
        ost->quality = sd ? AV_RL32(sd) : -1;
        ost->pict_type = sd ? sd[4] : AV_PICTURE_TYPE_NONE;

        for (i = 0; i<FF_ARRAY_ELEMS(ost->error); i++) {//error[]数组大小是4，i可能是0-4的值
            if (sd && i < sd[5])//若错误次数大于0次以上，即表示有错误
                ost->error[i] = AV_RL64(sd + 8 + 8*i);//sd+8应该是跳过quality+pict_type+保留位，但8*i未理解，留个疑问
            else
                ost->error[i] = -1;//没有错误
        }

        //是否通过帧率对duration重写
        if (ost->frame_rate.num && ost->is_cfr) {
            //(通过帧速率覆盖数据包持续时间，这应该不会发生)
            //av_rescale_q的作用看init_output_stream_encode的注释.就是将参2转成参3的单位
            if (pkt->duration > 0)
                av_log(NULL, AV_LOG_WARNING, "Overriding packet duration by frame rate, this should not happen\n");
            pkt->duration = av_rescale_q(1, av_inv_q(ost->frame_rate),
                                         ost->mux_timebase);
        }
    }

    /*内部调用av_rescale_q，将pkt里面与时间戳相关的pts、dts、duration、convergence_duration转成以ost->st->time_base为单位*/
    av_packet_rescale_ts(pkt, ost->mux_timebase, ost->st->time_base);

    /*3.输出流需要有时间戳的处理*/
    /*AVFMT_NOTIMESTAMPS: 格式不需要/有任何时间戳.
     所以当没有该宏时，就说明需要时间戳*/
    if (!(s->oformat->flags & AVFMT_NOTIMESTAMPS)) {
        //3.1解码时间戳比显示时间戳大，重写它们的时间戳
        if (pkt->dts != AV_NOPTS_VALUE &&
            pkt->pts != AV_NOPTS_VALUE &&
            pkt->dts > pkt->pts) {
            av_log(s, AV_LOG_WARNING, "Invalid DTS: %"PRId64" PTS: %"PRId64" in output stream %d:%d, replacing by guess\n",
                   pkt->dts, pkt->pts,
                   ost->file_index, ost->st->index);
            pkt->pts =
            pkt->dts = pkt->pts + pkt->dts + ost->last_mux_dts + 1
                     - FFMIN3(pkt->pts, pkt->dts, ost->last_mux_dts + 1)
                     - FFMAX3(pkt->pts, pkt->dts, ost->last_mux_dts + 1);//减去最大最小值，最终得到的就是三者之中，处于中间的那个值
        }

        /*3.2这里后续再详细研究*/
        if ((st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) &&
            pkt->dts != AV_NOPTS_VALUE &&
            !(st->codecpar->codec_id == AV_CODEC_ID_VP9 && ost->stream_copy) && /*VP9且不转码以外的条件*/
            ost->last_mux_dts != AV_NOPTS_VALUE) {
            /*AVFMT_TS_NONSTRICT：格式不需要严格增加时间戳，但它们仍然必须是单调的*/
            int64_t max = ost->last_mux_dts + !(s->oformat->flags & AVFMT_TS_NONSTRICT);
            if (pkt->dts < max) {
                int loglevel = max - pkt->dts > 2 || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? AV_LOG_WARNING : AV_LOG_DEBUG;
                av_log(s, loglevel, "Non-monotonous DTS in output stream "
                       "%d:%d; previous: %"PRId64", current: %"PRId64"; ",
                       ost->file_index, ost->st->index, ost->last_mux_dts, pkt->dts);//dts没有单调递增
                if (exit_on_error) {//-xerror选项，默认是0
                    av_log(NULL, AV_LOG_FATAL, "aborting.\n");
                    exit_program(1);
                }
                av_log(s, loglevel, "changing to %"PRId64". This may result "
                       "in incorrect timestamps in the output file.\n",
                       max);//(这可能会导致输出文件中的时间戳不正确)
                if (pkt->pts >= pkt->dts)
                    pkt->pts = FFMAX(pkt->pts, max);
                pkt->dts = max;
            }
        }
    }
    ost->last_mux_dts = pkt->dts;   //保存最近一次有效的dts

    ost->data_size += pkt->size;    //统计所有写入的包的字节大小
    ost->packets_written++;         //统计所有写入的包的个数

    pkt->stream_index = ost->index;

    if (debug_ts) {//-debug_ts选项
        av_log(NULL, AV_LOG_INFO, "muxer <- type:%s "
                "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s size:%d\n",
                av_get_media_type_string(ost->enc_ctx->codec_type),
                av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &ost->st->time_base),
                av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &ost->st->time_base),
                pkt->size
              );
    }

    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        print_error("av_interleaved_write_frame()", ret);
        main_return_code = 1;
        close_all_output_streams(ost, MUXER_FINISHED | ENCODER_FINISHED, ENCODER_FINISHED);
    }
    av_packet_unref(pkt);
}

/**
 * @brief 给该输出流标记ENCODER_FINISHED.
 * @param ost 输出流
*/
static void close_output_stream(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];

    ost->finished |= ENCODER_FINISHED;
    if (of->shortest) {//重新设置录像时长.
        int64_t end = av_rescale_q(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, AV_TIME_BASE_Q);
        of->recording_time = FFMIN(of->recording_time, end);
    }
}

/*
 * Send a single packet to the output, applying any bitstream filters
 * associated with the output stream.  This may result in any number
 * of packets actually being written, depending on what bitstream
 * filters are applied.  The supplied packet is consumed and will be
 * blank (as if newly-allocated) when this function returns.
 *
 * If eof is set, instead indicate EOF to all bitstream filters and
 * therefore flush any delayed packets to the output.  A blank packet
 * must be supplied in this case.
 */
/*(向输出发送单个包，应用与输出流相关的任何位流过滤器。这可能会导致实际写入任意数量的数据包，具体取决于应用了什么位流过滤器。
 * 当此函数返回时，所提供的包将被消耗，并且为空(就像新分配的一样)。
 * 如果设置了eof，则将eof指示为所有位流过滤器，因此将任何延迟的数据包刷新到输出。
 * 在这种情况下，必须提供一个空白包)*/
/**
 * @brief write_packet.按照有位流和无位流的流程，不难.
 * @param of 输出文件
 * @param pkt 编码后的包
 * @param ost 输出流
 * @param eof eof
*/
static void output_packet(OutputFile *of, AVPacket *pkt,
                          OutputStream *ost, int eof)
{
    int ret = 0;

    /* apply the output bitstream filters, if any.(如果有的话，应用输出位流过滤器) */
    // 1. 有位流的写包流程
    if (ost->nb_bitstream_filters) {//推流没用到,暂未研究
        int idx;

        ret = av_bsf_send_packet(ost->bsf_ctx[0], eof ? NULL : pkt);
        if (ret < 0)
            goto finish;

        eof = 0;
        idx = 1;
        while (idx) {
            /* get a packet from the previous filter up the chain */
            ret = av_bsf_receive_packet(ost->bsf_ctx[idx - 1], pkt);
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                idx--;
                continue;
            } else if (ret == AVERROR_EOF) {
                eof = 1;
            } else if (ret < 0)
                goto finish;

            /* send it to the next filter down the chain or to the muxer */
            if (idx < ost->nb_bitstream_filters) {
                ret = av_bsf_send_packet(ost->bsf_ctx[idx], eof ? NULL : pkt);
                if (ret < 0)
                    goto finish;
                idx++;
                eof = 0;
            } else if (eof)
                goto finish;
            else
                write_packet(of, pkt, ost, 0);
        }
    } else if (!eof)//没位流的流程, 且eof=0. 没位流 且 eof=1时，该函数不处理任何东西
        write_packet(of, pkt, ost, 0);

finish:
    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(NULL, AV_LOG_ERROR, "Error applying bitstream filters to an output "
               "packet for stream #%d:%d.\n", ost->file_index, ost->index);
        if(exit_on_error)
            exit_program(1);
    }
}

static int check_recording_time(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];

    //当前的时长已经大于等于用户要录像的时长，则给该输出流标记ENCODER_FINISHED，翻返回0；否则返回1？
    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, of->recording_time,
                      AV_TIME_BASE_Q) >= 0) {
        close_output_stream(ost);
        return 0;
    }
    return 1;
}

/**
 * @brief 对frame进行编码，并进行写帧操作.
 * @param of 输出文件
 * @param ost 输出流
 * @param frame 要编码的帧
 * @return void.
 * @note 成功或者输出流完成编码不返回任何内容, 失败程序退出.
*/
static void do_audio_out(OutputFile *of, OutputStream *ost,
                         AVFrame *frame)
{
    AVCodecContext *enc = ost->enc_ctx;
    AVPacket pkt;
    int ret;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    //1. 检查该输出流是否完成编码.主要与录像时长有关.
    if (!check_recording_time(ost))
        return;

    //2. 输入帧为空，或者 audio_sync_method小于0，pts会参考ost->sync_opts
    if (frame->pts == AV_NOPTS_VALUE || audio_sync_method < 0)
        frame->pts = ost->sync_opts;

    //3. 根据当前帧的pts和nb_samples预估下一帧的pts。
    ost->sync_opts = frame->pts + frame->nb_samples;//音频时,frame->pts的单位是采样点个数,例如采样点数是1024,采样频率是44.1k，那么就是0.23s.
                                                    //可参考ffplay的decoder_decode_frame()注释

    //4. 统计已经编码的采样点个数和帧数.
    ost->samples_encoded += frame->nb_samples;
    ost->frames_encoded++;

    av_assert0(pkt.size || !pkt.data);
    update_benchmark(NULL);
    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "encoder <- type:audio "
               "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
               av_ts2str(frame->pts), av_ts2timestr(frame->pts, &enc->time_base),
               enc->time_base.num, enc->time_base.den);
    }

    //5. 发送输入帧到编码器进行编码
    ret = avcodec_send_frame(enc, frame);
    if (ret < 0)
        goto error;

    //6. 循环获取编码后的帧,并进行写帧
    while (1) {
        //6.1 获取编码后的帧
        ret = avcodec_receive_packet(enc, &pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            goto error;

        update_benchmark("encode_audio %d.%d", ost->file_index, ost->index);

        av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);//每次写帧前都会将pts转成复用的时基

        if (debug_ts) {
            av_log(NULL, AV_LOG_INFO, "encoder -> type:audio "
                   "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                   av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &enc->time_base),
                   av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &enc->time_base));
        }

        //6.2 写帧
        output_packet(of, &pkt, ost, 0);
    }

    return;
error:
    av_log(NULL, AV_LOG_FATAL, "Audio encoding failed\n");
    exit_program(1);
}

static void do_subtitle_out(OutputFile *of,
                            OutputStream *ost,
                            AVSubtitle *sub)
{
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i;
    AVCodecContext *enc;
    AVPacket pkt;
    int64_t pts;

    if (sub->pts == AV_NOPTS_VALUE) {
        av_log(NULL, AV_LOG_ERROR, "Subtitle packets must have a pts\n");
        if (exit_on_error)
            exit_program(1);
        return;
    }

    enc = ost->enc_ctx;

    if (!subtitle_out) {
        subtitle_out = av_malloc(subtitle_out_max_size);
        if (!subtitle_out) {
            av_log(NULL, AV_LOG_FATAL, "Failed to allocate subtitle_out\n");
            exit_program(1);
        }
    }

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else
        nb = 1;

    /* shift timestamp to honor -ss and make check_recording_time() work with -t */
    pts = sub->pts;
    if (output_files[ost->file_index]->start_time != AV_NOPTS_VALUE)
        pts -= output_files[ost->file_index]->start_time;
    for (i = 0; i < nb; i++) {
        unsigned save_num_rects = sub->num_rects;

        ost->sync_opts = av_rescale_q(pts, AV_TIME_BASE_Q, enc->time_base);
        if (!check_recording_time(ost))
            return;

        sub->pts = pts;
        // start_display_time is required to be 0
        sub->pts               += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
        sub->end_display_time  -= sub->start_display_time;
        sub->start_display_time = 0;
        if (i == 1)
            sub->num_rects = 0;

        ost->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, subtitle_out,
                                                    subtitle_out_max_size, sub);
        if (i == 1)
            sub->num_rects = save_num_rects;
        if (subtitle_out_size < 0) {
            av_log(NULL, AV_LOG_FATAL, "Subtitle encoding failed\n");
            exit_program(1);
        }

        av_init_packet(&pkt);
        pkt.data = subtitle_out;
        pkt.size = subtitle_out_size;
        pkt.pts  = av_rescale_q(sub->pts, AV_TIME_BASE_Q, ost->mux_timebase);
        pkt.duration = av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt.pts += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
            else
                pkt.pts += av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
        }
        pkt.dts = pkt.pts;
        output_packet(of, &pkt, ost, 0);
    }
}

/**
 * @brief 将next_picture编码成pkt，然后写帧
 * @param of 输出文件
 * @param ost 输出流
 * @param next_picture 要编码的帧
 * @param sync_ipts 要编码的帧的pts.即AVFrame->pts的值
*/
static void do_video_out(OutputFile *of,
                         OutputStream *ost,
                         AVFrame *next_picture,
                         double sync_ipts)
{
    int ret, format_video_sync;
    AVPacket pkt;
    AVCodecContext *enc = ost->enc_ctx;
    AVCodecParameters *mux_par = ost->st->codecpar;
    AVRational frame_rate;
    int nb_frames, nb0_frames, i;
    double delta, delta0;
    double duration = 0;
    int frame_size = 0;
    InputStream *ist = NULL;
    AVFilterContext *filter = ost->filter->filter;

    if (ost->source_index >= 0)
        ist = input_streams[ost->source_index];

    /*1.获取duration.其中获取duration会参考以下3种方法，duration是可能被重写的
     * 单位最终都被转成enc->time_base.
     */
    /*1.1通过过滤器里面的帧率得到duration*/
    frame_rate = av_buffersink_get_frame_rate(filter);
    if (frame_rate.num > 0 && frame_rate.den > 0)
        /*
         * 我们在init_output_stream_encode()调用init_encoder_time_base()看到，以视频为例，
         * AVCodecContext->time_base是由帧率的倒数赋值的，所以这里求出的duration基本是1*/
        duration = 1/(av_q2d(frame_rate) * av_q2d(enc->time_base));

    /*1.2若用户指定-r帧率选项，则通过用户的-r选项得到duration*/
    if(ist && ist->st->start_time != AV_NOPTS_VALUE && ist->st->first_dts != AV_NOPTS_VALUE && ost->frame_rate.num)
        duration = FFMIN(duration, 1/(av_q2d(ost->frame_rate) * av_q2d(enc->time_base)));

    /*1.3通过pkt中的pkt_duration得到duration*/
    if (!ost->filters_script &&                 /*过滤器脚本为空*/
        !ost->filters &&                        /*过滤器字符串为空*/
        (nb_filtergraphs == 0 || !filtergraphs[0]->graph_desc) && /*过滤器数为空或者第一个FilterGraph的graph_desc为空*/
        next_picture && /*帧已经开辟内存*/
        ist &&      /*输入流存在*/
        lrintf(next_picture->pkt_duration * av_q2d(ist->st->time_base) / av_q2d(enc->time_base)) > 0) /*求出的duration合法*/
    {
        /* lrintf()是四舍五入函数.
         * next_picture->pkt_duration：对应packet的持续时间，以AVStream->time_base单位表示，如果未知则为0。
         * 下面是利用两个比相等求出的：d1/t1=d2/t2，假设pkt_duration=40，ist->st->time_base={1,1000},enc->time_base={1,25}
         * 那么next_picture->pkt_duration * av_q2d(ist->st->time_base)得到的就是40/1000;也就是d1/t1，
         * 而除以1/25，那么就变成乘以25，最终得到式子d2=(d1*t2)/t1，即d2=(40*25)/1000。
         * 单位变成编码器的时基.
         */
        duration = lrintf(next_picture->pkt_duration * av_q2d(ist->st->time_base) / av_q2d(enc->time_base));
    }

    if (!next_picture) {
        //end, flushing
        /*一段汇编代码，取中值？一般不会进来.
         * 但在reap_filters的av_buffersink_get_frame_flags调用失败时，
         * flush=1且是文件末尾，并是视频类型时，会传next_picture=NULL进来.
         * 可以全局搜一下do_video_out函数，只在两处地方被调用.
         */
        nb0_frames = nb_frames = mid_pred(ost->last_nb0_frames[0],
                                          ost->last_nb0_frames[1],
                                          ost->last_nb0_frames[2]);
    } else {
        /*delta0是输入帧(next_picture)与输出帧之间的“漂移”*/
        delta0 = sync_ipts - ost->sync_opts; // delta0 is the "drift" between the input frame (next_picture) and where it would fall in the output.
        delta  = delta0 + duration;

        /* by default, we output a single frame(默认情况下，我们输出单个帧). */
        nb0_frames = 0; // tracks the number of times the PREVIOUS frame should be duplicated, mostly for variable framerate (VFR)
                        // (跟踪前一帧应该被复制的次数，主要是为了可变帧率(VFR))
        nb_frames = 1;

        /*选项视频同步格式*/
        format_video_sync = video_sync_method;//video_sync_method默认是-1(VSYNC_AUTO)
        //自动选择视频同步格式
        if (format_video_sync == VSYNC_AUTO) {
            if(!strcmp(of->ctx->oformat->name, "avi")) {
                format_video_sync = VSYNC_VFR;//输出格式是avi，使用可变帧率.
            } else
                /*1.输出文件格式允许可变帧率：
                    1）输出文件格式也允许没有时间戳，则视频同步格式为VSYNC_PASSTHROUGH。
                    2）输出文件格式不允许没有时间戳，则视频同步格式为VSYNC_VFR。(正常走这里)
                  2.输出文件格式不允许可变帧率，则为VSYNC_CFR。*/
                format_video_sync = (of->ctx->oformat->flags & AVFMT_VARIABLE_FPS) ?
                            ((of->ctx->oformat->flags & AVFMT_NOTIMESTAMPS) ? VSYNC_PASSTHROUGH : VSYNC_VFR) : VSYNC_CFR;

            //如果是VSYNC_CFR才会往下走
            if (   ist
                && format_video_sync == VSYNC_CFR
                && input_files[ist->file_index]->ctx->nb_streams == 1   //输入流只有1个
                && input_files[ist->file_index]->input_ts_offset == 0)  //留个疑问？
            {
                format_video_sync = VSYNC_VSCFR;
            }
            if (format_video_sync == VSYNC_CFR && copy_ts) {//-copyts选项
                format_video_sync = VSYNC_VSCFR;
            }
        }

        ost->is_cfr = (format_video_sync == VSYNC_CFR || format_video_sync == VSYNC_VSCFR);

        //控制sync_ipts、sync_opts的差距
        //sync_ipts、sync_opts的具体意义需要后续研究
        if (delta0 < 0 && /* sync_ipts落后于ost->sync_opts */
            delta > 0 &&  /* delta0此时为负，而根据上面delta的计算，此时delta0 + duration需要是一个正数 */
            format_video_sync != VSYNC_PASSTHROUGH &&
            format_video_sync != VSYNC_DROP) {
            if (delta0 < -0.6) {
                av_log(NULL, AV_LOG_VERBOSE, "Past duration %f too large\n", -delta0);
            } else
                av_log(NULL, AV_LOG_DEBUG, "Clipping frame in rate conversion by %f\n", -delta0);//裁剪帧的速率转换为-delta0
                //av_log(NULL, AV_LOG_INFO, "Clipping frame in rate conversion by %f\n", -delta0);//裁剪帧的速率转换为-delta0
            sync_ipts = ost->sync_opts;//使sync_ipts追上sync_opts
            duration += delta0;//那么此时，时长应该减去追上的那一段，即delta0，因为输入与输出的差就是delta0.
            delta0 = 0;
        }

        //后续研究
        switch (format_video_sync) {
        case VSYNC_VSCFR:
            if (ost->frame_number == 0 && delta0 >= 0.5) {
                av_log(NULL, AV_LOG_DEBUG, "Not duplicating %d initial frames\n", (int)lrintf(delta0));
                delta = duration;
                delta0 = 0;
                ost->sync_opts = lrint(sync_ipts);
            }
        case VSYNC_CFR:
            // FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
            if (frame_drop_threshold && delta < frame_drop_threshold && ost->frame_number) {
                nb_frames = 0;
            } else if (delta < -1.1)
                nb_frames = 0;
            else if (delta > 1.1) {
                nb_frames = lrintf(delta);
                if (delta0 > 1.1)
                    nb0_frames = lrintf(delta0 - 0.6);
            }
            break;
        case VSYNC_VFR://主要研究这里
            if (delta <= -0.6)//delta是个负数，不会进入上面的if控制语句，那么由上面语句代入delta计算得到：sync_opts - sync_ipts - duration >= 0.6
                              //意思是输出比输入超过一帧，且还有0.6s剩余？
                nb_frames = 0;
            else if (delta > 0.6)//需要考虑上面的if语句，sync_ipts - sync_opts + duration > 0.6
                ost->sync_opts = lrint(sync_ipts);//这里不太清楚，留个疑问
            break;
        case VSYNC_DROP:
        case VSYNC_PASSTHROUGH:
            ost->sync_opts = lrint(sync_ipts);
            break;
        default:
            av_assert0(0);
        }
    }//<== else end ==>

    nb_frames = FFMIN(nb_frames, ost->max_frames - ost->frame_number);//frame_number>=最大帧数，nb_frames将会是0或者负数
    nb0_frames = FFMIN(nb0_frames, nb_frames);

    //将数组的第一、第二个元素拷贝到第二、第三个元素的位置.
    //注，因为这里在拷贝第一个元素时，会将第二个元素的内容覆盖，
    //所以绝对不能使用memcpy，只能使用memmove，因为它能确保拷贝之前将重复的内存先拷贝到目的地址。
    //see https://www.runoob.com/cprogramming/c-function-memmove.html.
    memmove(ost->last_nb0_frames + 1,
            ost->last_nb0_frames,
            sizeof(ost->last_nb0_frames[0]) * (FF_ARRAY_ELEMS(ost->last_nb0_frames) - 1));//与memcpy一样，但更安全
    ost->last_nb0_frames[0] = nb0_frames;

    //暂不太清楚下面的意思,后续研究
    if (nb0_frames == 0 && ost->last_dropped) {
        nb_frames_drop++;
        av_log(NULL, AV_LOG_VERBOSE,
               "*** dropping frame %d from stream %d at ts %"PRId64"\n",
               ost->frame_number, ost->st->index, ost->last_frame->pts);
    }
    if (nb_frames > (nb0_frames && ost->last_dropped) + (nb_frames > nb0_frames)) {
        //帧太大会drop掉
        if (nb_frames > dts_error_threshold * 30) {
            av_log(NULL, AV_LOG_ERROR, "%d frame duplication too large, skipping\n", nb_frames - 1);
            nb_frames_drop++;
            return;
        }
        nb_frames_dup += nb_frames - (nb0_frames && ost->last_dropped) - (nb_frames > nb0_frames);
        av_log(NULL, AV_LOG_VERBOSE, "*** %d dup!\n", nb_frames - 1);
        if (nb_frames_dup > dup_warning) {
            av_log(NULL, AV_LOG_WARNING, "More than %d frames duplicated\n", dup_warning);
            dup_warning *= 10;//复制超过一定数量会提示，并且下一次提示是本次的10倍？
        }
    }
    ost->last_dropped = nb_frames == nb0_frames && next_picture;

    /* duplicates frame if needed */
    for (i = 0; i < nb_frames; i++) {
        AVFrame *in_picture;
        int forced_keyframe = 0;
        double pts_time;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;

        //in_picture第一次进来指向首帧，后续指向上一帧？
        if (i < nb0_frames && ost->last_frame) {
            in_picture = ost->last_frame;//这里正常不会进来
        } else
            in_picture = next_picture;

        if (!in_picture)
            return;

        in_picture->pts = ost->sync_opts;

        if (!check_recording_time(ost))
            return;

        //包含两个宏其中之一 并且 top_field_first>=0
        //AV_CODEC_FLAG_INTERLACED_DCT指隔行扫描？AV_CODEC_FLAG_INTERLACED_ME注释是：交错运动估计
        if (enc->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) &&
            ost->top_field_first >= 0)
            in_picture->top_field_first = !!ost->top_field_first;//如果内容是交错的，则首先显示顶部字段。

        //设置field_order。field_order: 交错视频中的场的顺序。
        if (in_picture->interlaced_frame) {//图片的内容是交错的
            if (enc->codec->id == AV_CODEC_ID_MJPEG)
                mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TT:AV_FIELD_BB;
            else
                mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TB:AV_FIELD_BT;
        } else
            mux_par->field_order = AV_FIELD_PROGRESSIVE;

        in_picture->quality = enc->global_quality;//编解码器的全局质量，无法按帧更改。这应该与MPEG-1/2/4 qscale成比例。
        in_picture->pict_type = 0;//Picture type of the frame.

        //利用AVFrame的pts给ost->forced_kf_ref_pts赋值
        if (ost->forced_kf_ref_pts == AV_NOPTS_VALUE &&
            in_picture->pts != AV_NOPTS_VALUE)
            ost->forced_kf_ref_pts = in_picture->pts;

        //单位转成秒？此时的in_picture->pts - ost->forced_kf_ref_pts单位都是enc->time_base？留个疑问
        pts_time = in_picture->pts != AV_NOPTS_VALUE ?
            (in_picture->pts - ost->forced_kf_ref_pts) * av_q2d(enc->time_base) : NAN;

        if (ost->forced_kf_index < ost->forced_kf_count &&
            in_picture->pts >= ost->forced_kf_pts[ost->forced_kf_index]) {//正常流程不会进来
            ost->forced_kf_index++;
            forced_keyframe = 1;
        } else if (ost->forced_keyframes_pexpr) {//正常流程不会进来
            double res;
            ost->forced_keyframes_expr_const_values[FKF_T] = pts_time;
            res = av_expr_eval(ost->forced_keyframes_pexpr,
                               ost->forced_keyframes_expr_const_values, NULL);
            ff_dlog(NULL, "force_key_frame: n:%f n_forced:%f prev_forced_n:%f t:%f prev_forced_t:%f -> res:%f\n",
                    ost->forced_keyframes_expr_const_values[FKF_N],
                    ost->forced_keyframes_expr_const_values[FKF_N_FORCED],
                    ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N],
                    ost->forced_keyframes_expr_const_values[FKF_T],
                    ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T],
                    res);
            if (res) {
                forced_keyframe = 1;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N] =
                    ost->forced_keyframes_expr_const_values[FKF_N];
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T] =
                    ost->forced_keyframes_expr_const_values[FKF_T];
                ost->forced_keyframes_expr_const_values[FKF_N_FORCED] += 1;
            }

            ost->forced_keyframes_expr_const_values[FKF_N] += 1;
        } else if (   ost->forced_keyframes
                   && !strncmp(ost->forced_keyframes, "source", 6)
                   && in_picture->key_frame==1) {//正常流程不会进来
            forced_keyframe = 1;
        }

        if (forced_keyframe) {//正常流程不会进来
            in_picture->pict_type = AV_PICTURE_TYPE_I;
            av_log(NULL, AV_LOG_DEBUG, "Forced keyframe at time %f\n", pts_time);
        }

        update_benchmark(NULL);//没有指定选项-benchmark_all，可忽略
        if (debug_ts) {
            av_log(NULL, AV_LOG_INFO, "encoder <- type:video "
                   "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
                   av_ts2str(in_picture->pts), av_ts2timestr(in_picture->pts, &enc->time_base),
                   enc->time_base.num, enc->time_base.den);
        }

        ost->frames_encoded++;//统计发送到编码器的帧个数

        //将帧发送到编码器
        ret = avcodec_send_frame(enc, in_picture);
        if (ret < 0)
            goto error;

        // Make sure Closed Captions will not be duplicated(确保不会重复关闭字幕)
        // av_frame_remove_side_data(): 如果frame中存在所提供类型的边数据，请将其释放并从frame中删除
        av_frame_remove_side_data(in_picture, AV_FRAME_DATA_A53_CC);

        //循环从编码器中读取编码后的帧
        while (1) {
            ret = avcodec_receive_packet(enc, &pkt);
            update_benchmark("encode_video %d.%d", ost->file_index, ost->index);
            if (ret == AVERROR(EAGAIN))
                break;//一般每次读完一个pkt会从这里退出while
            if (ret < 0)
                goto error;

            if (debug_ts) {
                av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
                       "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                       av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &enc->time_base),
                       av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &enc->time_base));
            }

            /* AV_CODEC_CAP_DELAY:
             * 编码器或解码器需要在末尾使用NULL输入进行刷新，以便提供完整和正确的输出。
             * NOTE: 如果没有设置此标志，则保证编解码器永远不会输入NULL数据。用户仍然可以向公共编码或解码函数发送NULL数据，
             * 但libavcodec不会将其传递给编解码器，除非设置了此标志。
             *
             * Decoders: 解码器有一个非零延迟，需要在最后用avpkt->data=NULL, avpkt->size=0来获得延迟的数据，直到解码器不再返回帧。
             * Encoders: 编码器需要在编码结束时提供NULL数据，直到编码器不再返回数据。
             *
             * NOTE: 对于实现AVCodec.encode2()函数的编码器，设置此标志还意味着编码器必须设置每个输出包的pts和持续时间。
             * 如果未设置此标志，则pts和持续时间将由libavcodec从输入帧中确定。
            */
            //编码后的pkt.pts为空 且 编码器能力集不包含AV_CODEC_CAP_DELAY
            if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & AV_CODEC_CAP_DELAY))
                pkt.pts = ost->sync_opts;

            //将编码后的时基转成复用时基,这里需要留意
            av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);

            if (debug_ts) {
                av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
                    "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                    av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ost->mux_timebase),
                    av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ost->mux_timebase));
            }

            frame_size = pkt.size;//记录当前帧大小
            output_packet(of, &pkt, ost, 0);//内部会写帧

            /* if two pass, output log */
            if (ost->logfile && enc->stats_out) {//忽略，推流没用到
                fprintf(ost->logfile, "%s", enc->stats_out);
            }
        }//<== while (1) end ==>

        ost->sync_opts++;
        /*
         * For video, number of frames in == number of packets out.
         * But there may be reordering, so we can't throw away frames on encoder
         * flush, we need to limit them here, before they go into encoder.
         */
        /* 对于视频，输入的帧数=输出的包数。但是可能会有重新排序，所以我们不能在编码器刷新时丢弃帧，
         * 我们需要在它们进入编码器之前，在这里限制它们. */
        ost->frame_number++;

        //打印视频相关信息，vstats_filename默认是NULL，默认不会进来
        if (vstats_filename && frame_size)
            do_video_stats(ost, frame_size);
    }//<== for (i = 0; i < nb_frames; i++) end ==>

    //将当前帧引用给last_frame
    if (!ost->last_frame)
        ost->last_frame = av_frame_alloc();
    av_frame_unref(ost->last_frame);
    if (next_picture && ost->last_frame)
        av_frame_ref(ost->last_frame, next_picture);
    else
        av_frame_free(&ost->last_frame);//next_picture为空会进入这里，因为last_frame在上面是av_frame_alloc的

    return;
error:
    av_log(NULL, AV_LOG_FATAL, "Video encoding failed\n");
    exit_program(1);
}


static double psnr(double d)
{
    return -10.0 * log10(d);
}

/**
 * @brief 往vstats_file文件写入视频流的相关信息
 * @param ost 输出流
 * @param frame_size 帧大小
 */
static void do_video_stats(OutputStream *ost, int frame_size)
{
    AVCodecContext *enc;
    int frame_number;
    double ti1, bitrate, avg_bitrate;

    /* this is executed just the first time do_video_stats is called */
    //这只在第一次调用do_video_stats时执行
    if (!vstats_file) {
        vstats_file = fopen(vstats_filename, "w");
        if (!vstats_file) {
            perror("fopen");
            exit_program(1);
        }
    }

    enc = ost->enc_ctx;
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
        frame_number = ost->st->nb_frames;//这个流的帧数
        if (vstats_version <= 1) {
            fprintf(vstats_file, "frame= %5d q= %2.1f ", frame_number,
                    ost->quality / (float)FF_QP2LAMBDA);
        } else  {
            fprintf(vstats_file, "out= %2d st= %2d frame= %5d q= %2.1f ", ost->file_index, ost->index, frame_number,
                    ost->quality / (float)FF_QP2LAMBDA);
        }

        if (ost->error[0]>=0 && (enc->flags & AV_CODEC_FLAG_PSNR))
            fprintf(vstats_file, "PSNR= %6.2f ", psnr(ost->error[0] / (enc->width * enc->height * 255.0 * 255.0)));

        fprintf(vstats_file,"f_size= %6d ", frame_size);
        /* compute pts value */
        ti1 = av_stream_get_end_pts(ost->st) * av_q2d(ost->st->time_base);
        if (ti1 < 0.01)
            ti1 = 0.01;

        bitrate     = (frame_size * 8) / av_q2d(enc->time_base) / 1000.0;
        avg_bitrate = (double)(ost->data_size * 8) / ti1 / 1000.0;
        fprintf(vstats_file, "s_size= %8.0fkB time= %0.3f br= %7.1fkbits/s avg_br= %7.1fkbits/s ",
               (double)ost->data_size / 1024, ti1, bitrate, avg_bitrate);
        fprintf(vstats_file, "type= %c\n", av_get_picture_type_char(ost->pict_type));
    }
}

static int init_output_stream(OutputStream *ost, char *error, int error_len);

/**
 * @brief 标记输出流完成编码和复用,若指定-shortest,所有输出流都会被标记.
 * @param ost 输出流
 */
static void finish_output_stream(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int i;

    // 1. 标记编码和复用完成.
    ost->finished = ENCODER_FINISHED | MUXER_FINISHED;

    // 2. 指定-shortest选项,还会把其它的输出流都标记完成.
    // 这里得出-shortest选项的意义:最短的流结束,其它的流也会结束.
    if (of->shortest) {
        for (i = 0; i < of->ctx->nb_streams; i++)
            output_streams[of->ost_index + i]->finished = ENCODER_FINISHED | MUXER_FINISHED;
    }
}

/**
 * Get and encode new output from any of the filtergraphs, without causing
 * activity.(在不引起活动的情况下，从任何过滤图获取并编码新的输出)
 *
 * @return  0 for success, <0 for severe errors
 */
/**
 * @brief 遍历每个输出流,从输出过滤器中获取一帧送去编码,然后进行写帧.
 * @param flush =1时,会为视频清空编码器?但是在do_video_out的for循环会直接return?
 * @return 成功=0, 失败返回负数或者程序退出.
*/
static int reap_filters(int flush)
{
    AVFrame *filtered_frame = NULL;
    int i;

    /* Reap all buffers present in the buffer sinks */
    /* 1.获取在buffer sinks中存在的所有缓冲区 */
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        OutputFile    *of = output_files[ost->file_index];
        AVFilterContext *filter;
        AVCodecContext *enc = ost->enc_ctx;
        int ret = 0;

        /* 1.1若ost->filter为空或者ost->filter->graph->graph为空则跳过。
         * ost->filter在init_simple_filtergraph()时被创建。
         * AVFilterGraph是什么时候？在configure_filtergraph().
         * configure_filtergraph()什么时候被调用?在该输入流的pkt被解码第一帧的时候.
         */
        if (!ost->filter || !ost->filter->graph->graph)
            continue;
        filter = ost->filter->filter;//输出过滤器ctx

        /* 1.2对还没调用init_output_stream()初始化的输出流，则调用。
         * 一般视频、音频都是在这里初始化.内部主要是打开编解码器和写头. */
        if (!ost->initialized) {
            char error[1024] = "";
            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }

        //若ost->filtered_frame为空则给其开辟内存.
        if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
        filtered_frame = ost->filtered_frame;

        while (1) {
            double float_pts = AV_NOPTS_VALUE; // this is identical to filtered_frame.pts but with higher precision
                                               //这与filtered_frame.pts相同，但精度更高
            // 6. 从输出过滤器读取一帧。
            // while一般从这里的 av_buffersink_get_frame_flags 退出，第二次再读时，因为输出过滤器没有帧可读会返回AVERROR(EAGAIN)。
            ret = av_buffersink_get_frame_flags(filter, filtered_frame,
                                               AV_BUFFERSINK_FLAG_NO_REQUEST);
            if (ret < 0) {
                /*6.1不是eagain也不是文件末尾*/
                if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                    av_log(NULL, AV_LOG_WARNING,
                           "Error in av_buffersink_get_frame_flags(): %s\n", av_err2str(ret));
                } else if (flush && ret == AVERROR_EOF) {/*6.2flush=1且是文件末尾*/
                    if (av_buffersink_get_type(filter) == AVMEDIA_TYPE_VIDEO)
                        do_video_out(of, ost, NULL, AV_NOPTS_VALUE);//帧传NULL，进行flush
                }
                break;
            }
            if (ost->finished) {
                av_frame_unref(filtered_frame);
                continue;
            }

            //计算float_pts、filtered_frame->pts的值.
            //思路也不难:
            //1)若用户指定start_time，则解码后的帧的显示时间戳需要减去start_time。
            //例如本来3s显示该帧，假设用户start_time=1s，那么就应该在3-1=2s就显示该帧.
            //2)若没指定，则值不会变.
            //这两者求法是一样的，区别是float_pts用的tb.den被修改过，
            //而filtered_frame->pts使用原来的enc->time_base去求，看ffmpeg的float_pts注释，区别是精度不一样。
            if (filtered_frame->pts != AV_NOPTS_VALUE) {
                int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;//用户是否指定起始时间
                AVRational filter_tb = av_buffersink_get_time_base(filter);
                AVRational tb = enc->time_base;
                // 1）av_clip(): 用来限制参1最终落在0~16的范围.
                // 2）av_log2()(参考ffplay音频相关): 应该是对齐成2的n次方吧。
                // 例如freq=8000，每秒30次，最终返回8.具体可以看源码是如何处理的。
                // 大概估计是8k/30=266.6，2*2^7<266.7<2*2^8.然后向上取整，所以返回8。表达式是：av_log2(8000/30)=8;
                // 44.1k/30=1470，2*2^9<1470<2*2^10.然后向上取整，所以返回10。表达式是：av_log2(44.1k/30)=10;
                int extra_bits = av_clip(29 - av_log2(tb.den), 0, 16);

                tb.den <<= extra_bits;//留个疑问
                float_pts =
                    av_rescale_q(filtered_frame->pts, filter_tb, tb) -
                    av_rescale_q(start_time, AV_TIME_BASE_Q, tb);//获取从开始到现在的时间差
                float_pts /= 1 << extra_bits;//留个疑问
                // avoid exact midoints to reduce the chance of rounding differences, this can be removed in case the fps code is changed to work with integers
                //(避免精确的中点以减少舍入差异的机会，这可以在FPS代码更改为使用整数时删除)
                float_pts += FFSIGN(float_pts) * 1.0 / (1<<17);//FFSIGN宏很简单:就是取正负,但除以1<<17是啥意思?留个疑问

                filtered_frame->pts =
                    av_rescale_q(filtered_frame->pts, filter_tb, enc->time_base) -
                    av_rescale_q(start_time, AV_TIME_BASE_Q, enc->time_base);//filtered_frame->pts单位变了，变成enc->time_base
            }

            //将读到的帧进行编码
            switch (av_buffersink_get_type(filter)) {
            case AVMEDIA_TYPE_VIDEO:
                //用户没指定输出流的宽高比，会使用帧的宽高比给编码器的宽高比赋值
                if (!ost->frame_aspect_ratio.num)
                    enc->sample_aspect_ratio = filtered_frame->sample_aspect_ratio;

                if (debug_ts) {
                    av_log(NULL, AV_LOG_INFO, "filter -> pts:%s pts_time:%s exact:%f time_base:%d/%d\n",
                            av_ts2str(filtered_frame->pts), av_ts2timestr(filtered_frame->pts, &enc->time_base),
                            float_pts,
                            enc->time_base.num, enc->time_base.den);
                }

                do_video_out(of, ost, filtered_frame, float_pts);
                break;
            case AVMEDIA_TYPE_AUDIO:
                //AV_CODEC_CAP_PARAM_CHANGE: 编解码器支持在任何点更改参数。
                if (!(enc->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                    enc->channels != filtered_frame->channels) {//编码器不支持更改参数且编码器与要编码的帧不一样，那么不编码该帧.
                    av_log(NULL, AV_LOG_ERROR,
                           "Audio filter graph output is not normalized and encoder does not support parameter changes\n");
                    break;
                }
                do_audio_out(of, ost, filtered_frame);
                break;
            default:
                // TODO support subtitle filters
                av_assert0(0);
            }//<== switch (av_buffersink_get_type(filter)) end ==>

            av_frame_unref(filtered_frame);
        }//<== while (1) end ==>
    }//<== for (i = 0; i < nb_output_streams; i++) end ==>

    return 0;
}

/**
 * @brief 打印完成输出后的状态.
 * @param total_size avio_size(oc->pb)得到的字节大小,我个人认为是ffmpeg写入到内存的大小.
 *                      它和写帧时的实际写入大小之差 / 写帧时的实际写入大小的值,是一个复用开销比.
 */
static void print_final_stats(int64_t total_size)
{
    uint64_t video_size = 0, audio_size = 0, extra_size = 0, other_size = 0;
    uint64_t subtitle_size = 0;
    uint64_t data_size = 0;
    float percent = -1.0;
    int i, j;
    int pass1_used = 1;

    // 1. 统计输出流已经输出到输出地址的信息.
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        // 记录单个流已写包的大小
        switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO: video_size += ost->data_size; break;
            case AVMEDIA_TYPE_AUDIO: audio_size += ost->data_size; break;
            case AVMEDIA_TYPE_SUBTITLE: subtitle_size += ost->data_size; break;
            default:                 other_size += ost->data_size; break;
        }
        extra_size += ost->enc_ctx->extradata_size; //统计所有输出流的sps,pps等信息头大小
        data_size  += ost->data_size;               //统计所有流已经发包的大小
        //flags与运算后的结果不等于AV_CODEC_FLAG_PASS1, pass1_used=0.
        if (   (ost->enc_ctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2))
            != AV_CODEC_FLAG_PASS1)
            pass1_used = 0;
    }

    // 2. 算出复用的开销.
    // ffmpeg写入的总大小和实际发送到输出地址的差值比? muxing overhead复用开销
    if (data_size && total_size>0 && total_size >= data_size)
        percent = 100.0 * (total_size - data_size) / data_size;

    // 3. 打印各个流输出到输出文件的大小.
    av_log(NULL, AV_LOG_INFO, "video:%1.0fkB audio:%1.0fkB subtitle:%1.0fkB other streams:%1.0fkB global headers:%1.0fkB muxing overhead: ",
           video_size / 1024.0,
           audio_size / 1024.0,
           subtitle_size / 1024.0,
           other_size / 1024.0,
           extra_size / 1024.0);
    if (percent >= 0.0)
        av_log(NULL, AV_LOG_INFO, "%f%%", percent);
    else
        av_log(NULL, AV_LOG_INFO, "unknown");
    av_log(NULL, AV_LOG_INFO, "\n");

    // 4. 打印输入流详细的信息.需要设置对应的日志等级才能被打印
    /* print verbose per-stream stats(打印详细的每个流统计) */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *f = input_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Input file #%d (%s):\n",
               i, f->ctx->url);

        for (j = 0; j < f->nb_streams; j++) {
            InputStream *ist = input_streams[f->ist_index + j];
            enum AVMediaType type = ist->dec_ctx->codec_type;

            total_size    += ist->data_size;//统计所有输入流 已经读取的总大小和总包数.
            total_packets += ist->nb_packets;

            av_log(NULL, AV_LOG_VERBOSE, "  Input stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets read (%"PRIu64" bytes); ",
                   ist->nb_packets, ist->data_size);

            // 输入流要转码的还会把已经解码的帧数、样本数打印
            if (ist->decoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames decoded",
                       ist->frames_decoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ist->samples_decoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) demuxed\n",
               total_packets, total_size);
    }

    // 5. 打印输出流详细的信息, 与上面的输入同理
    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Output file #%d (%s):\n",
               i, of->ctx->url);

        for (j = 0; j < of->ctx->nb_streams; j++) {
            OutputStream *ost = output_streams[of->ost_index + j];
            enum AVMediaType type = ost->enc_ctx->codec_type;

            total_size    += ost->data_size;
            total_packets += ost->packets_written;

            av_log(NULL, AV_LOG_VERBOSE, "  Output stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            if (ost->encoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames encoded",
                       ost->frames_encoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ost->samples_encoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets muxed (%"PRIu64" bytes); ",
                   ost->packets_written, ost->data_size);

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) muxed\n",
               total_packets, total_size);
    }

    // 6. 没有内容被输出过才会进来.
    if(video_size + data_size + audio_size + subtitle_size + extra_size == 0){
        av_log(NULL, AV_LOG_WARNING, "Output file is empty, nothing was encoded ");
        if (pass1_used) {
            av_log(NULL, AV_LOG_WARNING, "\n");
        } else {
            av_log(NULL, AV_LOG_WARNING, "(check -ss / -t / -frames parameters if used)\n");
        }
    }
}

/**
 * @brief 打印相关信息到控制台.我们平时看到的帧率、编码质量、码率都是在这个函数打印的.
 *
 * @param is_last_report 是否是最后一次打印.=1是最后一次打印
 * @param timer_start 程序开始时间
 * @param cur_time 当前时间
 */
static void print_report(int is_last_report, int64_t timer_start, int64_t cur_time)
{
    AVBPrint buf, buf_script;
    OutputStream *ost;
    AVFormatContext *oc;
    int64_t total_size;
    AVCodecContext *enc;
    int frame_number, vid, i;
    double bitrate;
    double speed;
    int64_t pts = INT64_MIN + 1;
    static int64_t last_time = -1;// 静态变量
    static int qp_histogram[52];  // qp元素范围数组,静态变量
    int hours, mins, secs, us;
    const char *hours_sign;
    int ret;
    float t;

    // 1. 不打印状态 且 is_last_report=0 且 progress_avio为空
    if (!print_stats && !is_last_report && !progress_avio)
        return;

    // 2. 不是最后的一次调用本函数报告,往下走
    if (!is_last_report) {
        // 2.1 第一次进来该函数会先保存cur_time, 然后直接返回
        if (last_time == -1) {
            last_time = cur_time;
            return;
        }
        // 2.2 起码0.5s及以上才会dump
        if ((cur_time - last_time) < 500000)
            return;

        // 2.3 更新last_time
        last_time = cur_time;
    }

    // 3. 获取当前与开始时间的差值
    t = (cur_time-timer_start) / 1000000.0;

    // 4. 默认获取第一个输出文件
    oc = output_files[0]->ctx;

    // 5. 获取已经写入的文件大小.待会用于求码率
    total_size = avio_size(oc->pb);
    if (total_size <= 0) // FIXME improve avio_size() so it works with non seekable output too(改进avio_size()，使它也能处理不可查找的输出)
        total_size = avio_tell(oc->pb);

    // 6. 打印视频的frame,fps,q
    vid = 0;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);// 初始化两个缓存
    av_bprint_init(&buf_script, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (i = 0; i < nb_output_streams; i++) {
        float q = -1;
        ost = output_streams[i];
        enc = ost->enc_ctx;

        // 转码时获取编码质量
        if (!ost->stream_copy)
            q = ost->quality / (float) FF_QP2LAMBDA;

        // vid=1且是视频时,往缓冲区追加相关字符串(存在多个视频流时才会进来)
        if (vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&buf, "q=%2.1f ", q);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file_index, ost->index, q);
        }

        // vid=0且是视频时,往缓冲区追加相关字符串
        if (!vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            float fps;

            frame_number = ost->frame_number;
            fps = t > 1 ? frame_number / t : 0;// 动态帧率求法,单位时间内写帧的数量,超过1s才会计算,否则帧率为0
            av_bprintf(&buf, "frame=%5d fps=%3.*f q=%3.1f ",
                     frame_number, fps < 9.95, fps, q);// %3.*f中的"*"可参考: https://zhidao.baidu.com/question/2269588931158396988.html
            av_bprintf(&buf_script, "frame=%d\n", frame_number);
            av_bprintf(&buf_script, "fps=%.2f\n", fps);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file_index, ost->index, q);
            if (is_last_report)
                av_bprintf(&buf, "L");

            if (qp_hist) {// -qphist选项
                int j;
                int qp = lrintf(q);//lrintf()是四舍五入函数.
                if (qp >= 0 && qp < FF_ARRAY_ELEMS(qp_histogram))
                    qp_histogram[qp]++;
                for (j = 0; j < 32; j++)
                    av_bprintf(&buf, "%X", av_log2(qp_histogram[j] + 1));
            }

            // 编码器上下文包含宏AV_CODEC_FLAG_PSNR 并且 (图片类型有效或者最后一次报告)
            // 推流命令不会进来
            if ((enc->flags & AV_CODEC_FLAG_PSNR) && (ost->pict_type != AV_PICTURE_TYPE_NONE || is_last_report)) {
                int j;
                double error, error_sum = 0;
                double scale, scale_sum = 0;
                double p;
                char type[3] = { 'Y','U','V' };
                av_bprintf(&buf, "PSNR=");
                for (j = 0; j < 3; j++) {
                    if (is_last_report) {
                        error = enc->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0 * frame_number;
                    } else {
                        error = ost->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0;
                    }
                    if (j)
                        scale /= 4;
                    error_sum += error;
                    scale_sum += scale;
                    p = psnr(error / scale);
                    av_bprintf(&buf, "%c:%2.2f ", type[j], p);
                    av_bprintf(&buf_script, "stream_%d_%d_psnr_%c=%2.2f\n",
                               ost->file_index, ost->index, type[j] | 32, p);
                }
                p = psnr(error_sum / scale_sum);
                av_bprintf(&buf, "*:%2.2f ", psnr(error_sum / scale_sum));
                av_bprintf(&buf_script, "stream_%d_%d_psnr_all=%2.2f\n",
                           ost->file_index, ost->index, p);
            }

            vid = 1;
        }//<== if (!vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) end ==>

        /* compute min output value(计算最小输出值) */
        /* av_stream_get_end_pts(): 返回最后一个muxed包的pts + 它的持续时间(可认为是当前时间,debug理解即可).
         * 当与demuxer一起使用时，返回值未定义. */
        int64_t tyycode = av_stream_get_end_pts(ost->st);
        if (av_stream_get_end_pts(ost->st) != AV_NOPTS_VALUE)
            pts = FFMAX(pts, av_rescale_q(av_stream_get_end_pts(ost->st),
                                          ost->st->time_base, AV_TIME_BASE_Q));// pts最终保存所有输出流的最大pts
        if (is_last_report)
            nb_frames_drop += ost->last_dropped;

    }//<== for (i = 0; i < nb_output_streams; i++) end ==>

    // 利用av_stream_get_end_pts得到的pts求出当前的时分秒
    secs = FFABS(pts) / AV_TIME_BASE;
    us = FFABS(pts) % AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
    hours_sign = (pts < 0) ? "-" : "";// 是否是负数

    // 求已经经过的时间的码率,算法: 已经写入的大小字节*8bit 除以 已经经过的时间
    bitrate = pts && total_size >= 0 ? total_size * 8 / (pts / 1000.0) : -1;

    // 求速率.pts / AV_TIME_BASE是转成单位秒; pts/t代表流写入的时间和实际时间的比.
    // 值越大,代表流写入得越快.
    speed = t != 0.0 ? (double)pts / AV_TIME_BASE / t : -1;

    // 7. 填充size和time
    if (total_size < 0) av_bprintf(&buf, "size=N/A time=");
    else                av_bprintf(&buf, "size=%8.0fkB time=", total_size / 1024.0);//除以1024是转成KB单位
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf, "N/A ");
    } else {
        av_bprintf(&buf, "%s%02d:%02d:%02d.%02d ",
                   hours_sign, hours, mins, secs, (100 * us) / AV_TIME_BASE);
                    // (100 * us) / AV_TIME_BASE中, 除以AV_TIME_BASE是转成秒,乘以100是为了显示转成秒后的前两位数字.
    }

    // 8. 填充码率
    if (bitrate < 0) {
        av_bprintf(&buf, "bitrate=N/A");
        av_bprintf(&buf_script, "bitrate=N/A\n");
    }else{
        av_bprintf(&buf, "bitrate=%6.1fkbits/s", bitrate);
        av_bprintf(&buf_script, "bitrate=%6.1fkbits/s\n", bitrate);
    }

    // 填充大小,注意是往buf_script填充
    if (total_size < 0) av_bprintf(&buf_script, "total_size=N/A\n");
    else                av_bprintf(&buf_script, "total_size=%"PRId64"\n", total_size);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf_script, "out_time_us=N/A\n");
        av_bprintf(&buf_script, "out_time_ms=N/A\n");
        av_bprintf(&buf_script, "out_time=N/A\n");
    } else {
        av_bprintf(&buf_script, "out_time_us=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time_ms=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time=%s%02d:%02d:%02d.%06d\n",
                   hours_sign, hours, mins, secs, us);
    }

    if (nb_frames_dup || nb_frames_drop)
        av_bprintf(&buf, " dup=%d drop=%d", nb_frames_dup, nb_frames_drop);
    av_bprintf(&buf_script, "dup_frames=%d\n", nb_frames_dup);
    av_bprintf(&buf_script, "drop_frames=%d\n", nb_frames_drop);

    // 9. 填充speed
    if (speed < 0) {
        av_bprintf(&buf, " speed=N/A");
        av_bprintf(&buf_script, "speed=N/A\n");
    } else {
        av_bprintf(&buf, " speed=%4.3gx", speed);
        av_bprintf(&buf_script, "speed=%4.3gx\n", speed);
    }

    // 10. 最终是这里打印.可以看到是打印buf这个缓冲区.
    if (print_stats || is_last_report) {
        const char end = is_last_report ? '\n' : '\r';
        if (print_stats==1 && AV_LOG_INFO > av_log_get_level()) {
            fprintf(stderr, "%s    %c", buf.str, end);
        } else
            av_log(NULL, AV_LOG_INFO, "%s    %c", buf.str, end);

        fflush(stderr);//每次打印完毕清空输出缓冲区.
    }
    av_bprint_finalize(&buf, NULL);//参2传NULL代表释放pan_buf内开辟过的内存.

    // 11. buf_script会写到avio
    if (progress_avio) {
        av_bprintf(&buf_script, "progress=%s\n",
                   is_last_report ? "end" : "continue");
        avio_write(progress_avio, buf_script.str,
                   FFMIN(buf_script.len, buf_script.size - 1));
        avio_flush(progress_avio);
        av_bprint_finalize(&buf_script, NULL);
        if (is_last_report) {
            if ((ret = avio_closep(&progress_avio)) < 0)
                av_log(NULL, AV_LOG_ERROR,
                       "Error closing progress log, loss of information possible: %s\n", av_err2str(ret));
        }
    }

    // 12. 若是最后一次打印,调用print_final_stats
    if (is_last_report)
        print_final_stats(total_size);
}

/**
 * @brief 从参数结构体拷贝相关参数到InputFilter.
 */
static void ifilter_parameters_from_codecpar(InputFilter *ifilter, AVCodecParameters *par)
{
    // We never got any input. Set a fake format, which will
    // come from libavformat.
    ifilter->format                 = par->format;
    ifilter->sample_rate            = par->sample_rate;
    ifilter->channels               = par->channels;
    ifilter->channel_layout         = par->channel_layout;
    ifilter->width                  = par->width;
    ifilter->height                 = par->height;
    ifilter->sample_aspect_ratio    = par->sample_aspect_ratio;
}

/**
 * @brief 将每个输出流的编码器剩余pkt清空.
 */
static void flush_encoders(void)
{
    int i, ret;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream   *ost = output_streams[i];
        AVCodecContext *enc = ost->enc_ctx;
        OutputFile      *of = output_files[ost->file_index];

        // 1. 没编码的流直接返回
        if (!ost->encoding_needed)
            continue;

        // Try to enable encoding with no input frames.(尝试启用无输入帧的编码)
        // Maybe we should just let encoding fail instead.(也许我们应该让编码失败)
        // 2. 没有调用init_output_stream初始化输出流, 重新初始化.
        if (!ost->initialized) {
            FilterGraph *fg = ost->filter->graph;
            char error[1024] = "";

            av_log(NULL, AV_LOG_WARNING,
                   "Finishing stream %d:%d without any data written to it.\n",
                   ost->file_index, ost->st->index);

            // 2.1 开辟了OutputFilter 并且 未配置滤波图
            if (ost->filter && !fg->graph) {
                int x;
                // 2.1.1 初始化每个输入过滤器的format
                for (x = 0; x < fg->nb_inputs; x++) {
                    InputFilter *ifilter = fg->inputs[x];
                    if (ifilter->format < 0)//这里看到ffmpeg每次配置滤波图都要初始化输入过滤器的参数
                        ifilter_parameters_from_codecpar(ifilter, ifilter->ist->st->codecpar);
                }

                // 2.1.2 若上面输入过滤器配置失败,处理下一个输出流.
                if (!ifilter_has_all_input_formats(fg))
                    continue;

                // 2.1.3 配置fg
                ret = configure_filtergraph(fg);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error configuring filter graph\n");
                    exit_program(1);
                }

                // 2.1.4 标记输出流完成编码和复用
                finish_output_stream(ost);
            }//<== if (ost->filter && !fg->graph) end ==>

            // 2.2 重新初始化输出流.
            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }//<== if (!ost->initialized) end ==>

        // 音频的编码器的frame_size <= 1, 不处理
        if (enc->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <= 1)
            continue;

        // 不是音视频不处理
        if (enc->codec_type != AVMEDIA_TYPE_VIDEO && enc->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        // 2. 清空编码器
        for (;;) {
            const char *desc = NULL;
            AVPacket pkt;
            int pkt_size;

            switch (enc->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                desc   = "audio";
                break;
            case AVMEDIA_TYPE_VIDEO:
                desc   = "video";
                break;
            default:
                av_assert0(0);
            }

            av_init_packet(&pkt);
            pkt.data = NULL;
            pkt.size = 0;

            update_benchmark(NULL);

            // flush 编码器的实际操作: 从编码器读取eagain, 那么一直往编码器发送空帧,直至遇到非eagain
            while ((ret = avcodec_receive_packet(enc, &pkt)) == AVERROR(EAGAIN)) {
                ret = avcodec_send_frame(enc, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
                           desc,
                           av_err2str(ret));
                    exit_program(1);
                }
            }

            update_benchmark("flush_%s %d.%d", desc, ost->file_index, ost->index);
            if (ret < 0 && ret != AVERROR_EOF) {
                av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
                       desc,
                       av_err2str(ret));
                exit_program(1);
            }
            if (ost->logfile && enc->stats_out) {
                fprintf(ost->logfile, "%s", enc->stats_out);
            }

            /* 这里对剩余编码器的包的处理:
             * 1)eof: 调用output_packet,参4传1,实际上没有使用位流的话,内部没处理任何内容.
             * 2)正常读到剩余的包: 若输出流已经完成,那么不要该包了,直接释放引用,然后返回处理下一个包;
             *                  若输出流没完成,那么继续 将该包输出到输出url. */
            if (ret == AVERROR_EOF) {
                output_packet(of, &pkt, ost, 1);
                break;//该输出流flush encoder完成,退出for处理下一个输出流
            }
            if (ost->finished & MUXER_FINISHED) {
                av_packet_unref(&pkt);
                continue;
            }

            //没有遇到eof或者输出流没完成,那么继续将该包输出到输出url
            av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);
            pkt_size = pkt.size;
            output_packet(of, &pkt, ost, 0);
            if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO && vstats_filename) {
                do_video_stats(ost, pkt_size);
            }
        }//<== for (;;) end ==>

    }//<== for (i = 0; i < nb_output_streams; i++) end ==>
}

/*
 * Check whether a packet from ist should be written into ost at this time
 * (检查来自ist的数据包此时是否应该被写入ost)
 */
/**
 * @brief 检查来自ist的数据包此时是否应该被写入ost
 * @param ist 输入流
 * @param ost 输出流
 * @return 1-此时可以输出 0-此时不可以输出
 */
static int check_output_constraints(InputStream *ist, OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int ist_index  = input_files[ist->file_index]->ist_index + ist->st->index;//获取输入流下标

    // 1. 若输出流保存的输入流下标 与 输入流保存的不一致,返回0
    if (ost->source_index != ist_index)
        return 0;

    // 2. 输出流完成
    if (ost->finished)
        return 0;

    // 3. 当前输入流的pts 还没到达 输出流指定的开始时间
    if (of->start_time != AV_NOPTS_VALUE && ist->pts < of->start_time)
        return 0;

    return 1;
}

/**
 * @brief 后续详细分析.比转码会简单很多.
 */
static void do_streamcopy(InputStream *ist, OutputStream *ost, const AVPacket *pkt)
{
    OutputFile *of = output_files[ost->file_index];
    InputFile   *f = input_files [ist->file_index];
    int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
    int64_t ost_tb_start_time = av_rescale_q(start_time, AV_TIME_BASE_Q, ost->mux_timebase);
    AVPacket opkt = { 0 };

    av_init_packet(&opkt);

    // 1. eof时,flush输出比特流过滤器.(后续研究)
    // EOF: flush output bitstream filters.(冲洗输出比特流过滤器)
    if (!pkt) {
        output_packet(of, &opkt, ost, 1);
        return;
    }

    // 2. (帧数=0 且 是非关键帧) 且 没指定复制最初的非关键帧
    if ((!ost->frame_number && !(pkt->flags & AV_PKT_FLAG_KEY)) &&
        !ost->copy_initial_nonkeyframes)
        return;

    // 3. 帧数=0 且 没指定-copypriorss选项
    if (!ost->frame_number && !ost->copy_prior_start) {
        int64_t comp_start = start_time;
        if (copy_ts && f->start_time != AV_NOPTS_VALUE)
            comp_start = FFMAX(start_time, f->start_time + f->ts_offset);
        if (pkt->pts == AV_NOPTS_VALUE ?
            ist->pts < comp_start :
            pkt->pts < av_rescale_q(comp_start, AV_TIME_BASE_Q, ist->st->time_base))
            return;
    }

    // 4. 当前时间 >= 输出文件要录像的时间,录像完毕
    if (of->recording_time != INT64_MAX &&
        ist->pts >= of->recording_time + start_time) {
        close_output_stream(ost);
        return;
    }

    // 5. 当前时间 >= 输入文件要录像的时间,录像完毕.
    if (f->recording_time != INT64_MAX) {
        start_time = f->ctx->start_time;//输入流的开始时间
        if (f->start_time != AV_NOPTS_VALUE && copy_ts)
            start_time += f->start_time;
        if (ist->pts >= f->recording_time + start_time) {
            close_output_stream(ost);
            return;
        }
    }

    // 6. copy下,当是视频时,sync_opts代表输出帧个数?
    /* force the input stream PTS */
    if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        ost->sync_opts++;

    // 7. 减去start_time是为啥?留个疑问
    if (pkt->pts != AV_NOPTS_VALUE)
        opkt.pts = av_rescale_q(pkt->pts, ist->st->time_base, ost->mux_timebase) - ost_tb_start_time;
    else
        opkt.pts = AV_NOPTS_VALUE;

    if (pkt->dts == AV_NOPTS_VALUE)
        opkt.dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ost->mux_timebase);
    else
        opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->mux_timebase);
    opkt.dts -= ost_tb_start_time;

    // 8. 音频类型 且 pkt->dts有效,会重置opkt.dts, opkt.pts
    if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && pkt->dts != AV_NOPTS_VALUE) {
        int duration = av_get_audio_frame_duration(ist->dec_ctx, pkt->size);
        if(!duration)
            duration = ist->dec_ctx->frame_size;
        opkt.dts = opkt.pts = av_rescale_delta(ist->st->time_base, pkt->dts,
                                               (AVRational){1, ist->dec_ctx->sample_rate}, duration, &ist->filter_in_rescale_delta_last,
                                               ost->mux_timebase) - ost_tb_start_time;
    }

    // 9. 时长单位转换
    opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->mux_timebase);

    opkt.flags    = pkt->flags;

    // 10. buf引用和data,size的赋值处理
    if (pkt->buf) {
        opkt.buf = av_buffer_ref(pkt->buf);
        if (!opkt.buf)
            exit_program(1);
    }
    opkt.data = pkt->data;
    opkt.size = pkt->size;

    // 11. 边数据拷贝
    av_copy_packet_side_data(&opkt, pkt);

    // 12. 写包
    output_packet(of, &opkt, ost, 0);
}

/**
 * @brief 这个猜测输入音频的通道布局思路很简单：
 * 1. 若输入流的解码器上下文的通道布局的值=0，则通过通道数去获取通道布局，
 *      若通道数也为0，看av_get_default_channel_layout源码知道，通道布局还是返回0，
 * 2. 若有，则直接返回1.
 * @return =1 获取输入流的通道布局成功； =0 获取输入流的通道布局失败
 */
int guess_input_channel_layout(InputStream *ist)
{
    AVCodecContext *dec = ist->dec_ctx;

    // 1. 若通道布局为0
    if (!dec->channel_layout) {
        char layout_name[256];
        // 1.1 判断通道channels是否越界，越界则返回0，因为guess_layout_max被赋值为INT_MAX
        if (dec->channels > ist->guess_layout_max)
            return 0;

        // 1.2 通过通道数获取通道布局
        dec->channel_layout = av_get_default_channel_layout(dec->channels);
        if (!dec->channel_layout)
            return 0;

        // 1.3 通过通道数获取通道布局成功后，获取该通道布局的字符串描述.
        // 这一步只是打印警告，注释掉实际上不会影响正常逻辑
        // Return a description of a channel layout.
        av_get_channel_layout_string(layout_name, sizeof(layout_name),
                                     dec->channels, dec->channel_layout);
        av_log(NULL, AV_LOG_WARNING, "Guessed Channel Layout for Input Stream "
               "#%d.%d : %s\n", ist->file_index, ist->st->index, layout_name);
    }
    return 1;
}

/**
 * @brief 对decode()的结果进行检测.与-xerror选项有关.
 * @param ist 输入流
 * @param got_output decode的传出参数.
 * @param ret 解码后的返回值
*/
static void check_decode_result(InputStream *ist, int *got_output, int ret)
{
    // 1. 保存成功解码一帧的次数,以及解码失败的次数.
    /*分析got_output、ret的情况:
     * 1)当got_output=1时:
     *  1.1)ret=0(大于0也一样),那么表示decode()成功解码一帧;次数保存在decode_error_stat[0];
     *  1.2)ret=负数,不存在.因为decode(),got_output=1时返回值必定是0.所以不考虑.
     * 2)当当got_output=0时:
     *  2.1)ret=0(大于0也一样),不会进入该if,所以不考虑.
     *  2.2)ret=负数,那么表示解码失败.次数保存在decode_error_stat[1];
    */
    if (*got_output || ret<0)
        decode_error_stat[ret<0] ++;

    // 2. 若解码失败 且 用户指定-xerror选项,那么程序退出.
    if (ret < 0 && exit_on_error)
        exit_program(1);

    // 3. 解码帧成功,但是该帧存在错误.用户指定-xerror选项,那么程序退出.没指定只会报警告.
    if (*got_output && ist) {
        //decode_error_flags:解码帧的错误标志，如果解码器产生帧，但在解码期间存在错误，则设置为FF_DECODE_ERROR_xxx标志的组合.
        //ist->decoded_frame->flags:帧标志，@ref lavu_frame_flags的组合.
        //AV_FRAME_FLAG_CORRUPT:帧数据可能被损坏，例如由于解码错误.
        if (ist->decoded_frame->decode_error_flags || (ist->decoded_frame->flags & AV_FRAME_FLAG_CORRUPT)) {
            av_log(NULL, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "%s: corrupt decoded frame in stream %d\n", input_files[ist->file_index]->ctx->url, ist->st->index);
            if (exit_on_error)
                exit_program(1);
        }
    }
}

// Filters can be configured only if the formats of all inputs are known.
//(只有当所有输入的格式都已知时，才能配置过滤器)
/**
 * @brief 遍历所有输入过滤器是否已经配置format.
 * @param fg 封装的系统过滤器
 *
 * @return 都已经配置format，返回1； 否则返回0
 */
static int ifilter_has_all_input_formats(FilterGraph *fg)
{
    int i;
    //遍历输入过滤器数组，若存在format没配置的元素，则返回0，只对视频、音频检测。
    for (i = 0; i < fg->nb_inputs; i++) {
        if (fg->inputs[i]->format < 0 && (fg->inputs[i]->type == AVMEDIA_TYPE_AUDIO ||
                                          fg->inputs[i]->type == AVMEDIA_TYPE_VIDEO))
            return 0;
    }
    return 1;
}

/**
 * @brief 主要工作是 配置FilterGraph 和 将解码后的一帧送去过滤器. 主要流程:
 * 1)判断InputFilter的相关参数是否被改变,若和解码后的帧参数不一致的话,need_reinit=1;
 *      特殊地,用户指定-reinit_filter=0时,即使参数不一样,need_reinit会重置为0;
 * 2)如果需要重新初始化 或者 fg->graph没有初始化的话, 则进入配置流程.
 * 3)发送该解码帧到fg->graph的中buffersrc.
 *
 * @param ifilter 输入过滤器
 * @param frame 解码帧
 * @return 成功=0, 失败返回负数或者程序退出.
 */
static int ifilter_send_frame(InputFilter *ifilter, AVFrame *frame)
{
    FilterGraph *fg = ifilter->graph;//获取该输入过滤器对应的fg, see init_simple_filtergraph
    int need_reinit, ret, i;

    /* determine if the parameters for this input changed(确定此输入的参数是否已更改) */
    need_reinit = ifilter->format != frame->format;//判断输入流与解码后的帧格式是否不一致

    // 1. 判断输入流的输入过滤器是否需要重新初始化.
    // 判断依据:输入流中的输入过滤器保存的参数 是否与 解码帧的参数 存在不一样.
    switch (ifilter->ist->st->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        need_reinit |= ifilter->sample_rate    != frame->sample_rate ||
                       ifilter->channels       != frame->channels ||
                       ifilter->channel_layout != frame->channel_layout;
        break;
    case AVMEDIA_TYPE_VIDEO:
        need_reinit |= ifilter->width  != frame->width ||
                       ifilter->height != frame->height;
        break;
    }

    // 2. 如果没指定-reinit_filter 且 fg->graph已经初始化,那么need_reinit会置为0.
    // 即输入流参数改变不重新初始化FilterGraph
    if (!ifilter->ist->reinit_filters && fg->graph)
        need_reinit = 0;

    // 3. 硬件输入过滤器的hw_frames_ctx 与 帧的hw_frames_ctx不相等 或者 其内部的data不相等(硬件相关,暂不深入研究)
    if (!!ifilter->hw_frames_ctx != !!frame->hw_frames_ctx ||
        (ifilter->hw_frames_ctx && ifilter->hw_frames_ctx->data != frame->hw_frames_ctx->data))
        need_reinit = 1;

    // 4. 如果需要重新初始化InputFilter,则从解码帧拷贝相关参数.
    if (need_reinit) {
        ret = ifilter_parameters_from_frame(ifilter, frame);
        if (ret < 0)
            return ret;
    }

    /* (re)init the graph if possible, otherwise buffer the frame and return */
    // ( (重新)初始化图形如果可能，否则缓冲帧并返回 )
    // 5. 如果需要重新初始化 或者 fg->graph没有初始化的话,往下执行.
    if (need_reinit || !fg->graph) {
        // 5.1 遍历fg的每个InputFilter的format是否已经初始化完成.
        // 正常推流命令,一般一个输入流对应一个fg,一个fg包含一个InputFilter(nb_inputs=1),一个OutputFilter(nb_outputs=1)
        for (i = 0; i < fg->nb_inputs; i++) {

            // 5.1.1 若fg中InputFilter数组,存在有未初始化的InputFilter->format,那么会把该解码帧先放到ifilter的帧队列.
            // 上面看到ifilter_parameters_from_frame()时会初始化format.
            if (!ifilter_has_all_input_formats(fg)) {
                /* av_frame_clone(): 创建一个引用与src相同数据的frame,源码很简单.
                 * 这是av_frame_alloc()+av_frame_ref()的快捷方式.
                 * 成功返回新创建的AVFrame，错误返回NULL. */
                AVFrame *tmp = av_frame_clone(frame);
                if (!tmp)
                    return AVERROR(ENOMEM);
                av_frame_unref(frame);

                //ifilter的帧队列空间不足,按两倍大小扩容.
                if (!av_fifo_space(ifilter->frame_queue)) {
                    ret = av_fifo_realloc2(ifilter->frame_queue, 2 * av_fifo_size(ifilter->frame_queue));
                    if (ret < 0) {
                        av_frame_free(&tmp);
                        return ret;
                    }
                }

                //av_fifo_generic_write(): 将数据从用户提供的回调提供给AVFifoBuffer。详细看write_packet()的注释.
                //这里会将数据先保存到ifilter的帧队列
                av_fifo_generic_write(ifilter->frame_queue, &tmp, sizeof(tmp), NULL);
                return 0;
            }
        }

        // 5.2 编码、写帧操作.
        // 一般不是这里调用去编码的.
        // 这里调用reap_filters(1)是为了处理need_reinit=1,fg->graph!=NULL时,输出旧过滤器的一帧解码帧到输出url.
        ret = reap_filters(1);
        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
            return ret;
        }

        // 5.3 配置FilterGraph(音视频流都是在这里调用去配置fg的).
        ret = configure_filtergraph(fg);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
            return ret;
        }
    }

    // 6. 将该解码帧送去滤波器,以便得到想要的输出帧格式.(最终会在reap_filters获取该输出帧格式和进行编码)
    ret = av_buffersrc_add_frame_flags(ifilter->filter, frame, AV_BUFFERSRC_FLAG_PUSH);
    if (ret < 0) {
        if (ret != AVERROR_EOF)
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}

/**
 * @brief 标记输入过滤器eof=1,并关闭输入过滤器
 * @param ifilter 输入过滤器
 * @param pts 当前的pts?
 * @return 成功-0 失败-负数
 */
static int ifilter_send_eof(InputFilter *ifilter, int64_t pts)
{
    int ret;

    // 1. 标记过滤器完成
    ifilter->eof = 1;

    // 2. 关闭过滤器源buffer(abuffer)
    if (ifilter->filter) {
        /* av_buffersrc_close(): EOF结束后关闭缓冲源.
         * 这类似于将NULL传递给av_buffersrc_add_frame_flags(),
         * 除了它接受EOF的时间戳，例如最后一帧结束的时间戳. */
        ret = av_buffersrc_close(ifilter->filter, pts, AV_BUFFERSRC_FLAG_PUSH);
        if (ret < 0)
            return ret;
    } else {
        // the filtergraph was never configured(从未配置过滤器)
        if (ifilter->format < 0)
            ifilter_parameters_from_codecpar(ifilter, ifilter->ist->st->codecpar);
        if (ifilter->format < 0 && (ifilter->type == AVMEDIA_TYPE_AUDIO || ifilter->type == AVMEDIA_TYPE_VIDEO)) {
            av_log(NULL, AV_LOG_ERROR, "Cannot determine format of input stream %d:%d after EOF\n", ifilter->ist->file_index, ifilter->ist->st->index);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

// This does not quite work like avcodec_decode_audio4/avcodec_decode_video2.
// There is the following difference: if you got a frame, you must call
// it again with pkt=NULL. pkt==NULL is treated differently from pkt->size==0
// (pkt==NULL means get more output, pkt->size==0 is a flush/drain packet)
/* 这并不像avcodec_decode_audio4/avcodec_decode_video2那样工作.
 * 有以下区别：如果您有一个frame，则必须使用pkt=NULL再次调用它。
 * pkt==NULL与pkt->size==0的处理方式不同（pkt==NULL意味着获得更多输出，pkt->size==0是一个刷新/漏包）
*/
/**
 * @brief 解码pkt.
 * @param avctx 解码器上下文
 * @param frame 用于存储解码后的一帧
 * @param got_frame =1:解码一帧成功; =0:解码一帧失败,可能发包或者接收帧错误,可能遇到eagain,使用返回值判断即可.
 * @param pkt 待解码的pkt
 *
 * @return 成功0,失败返回负数. 注意,当返回0成功时,是否成功解码一帧需要配合got_frame的值才能判断,因为此时接收包可能遇到eagain.
 *
 * @note avcodec_decode_video2是旧版本的解码方式,avcodec_send_packet + avcodec_receive_frame是新版本ffmpeg的解码方式.
*/
static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    // 1. 发送包去解码.
    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        // (特别是，我们不期望AVERROR(EAGAIN)，因为我们使用avcodec_receive_frame()读取所有已解码的帧，直到完成)
        if (ret < 0 && ret != AVERROR_EOF)// 除了eof,发包错误都会直接返回.
            return ret;
    }

    // 2. 获取解码后的一帧
    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN))//除了eagain,错误都会直接返回.
        return ret;

    // 3. 不是eagain,got_frame标记为1.
    if (ret >= 0)
        *got_frame = 1;

    //接收包遇到eagain时,返回值是0,got_frame=0
    return 0;
}

/**
 * @brief 将解码帧 送到 InputFilter数组的各个元素(ist->filters[i])处理, 具体看ifilter_send_frame().
 * @param ist 输入流
 * @param decoded_frame 解码帧
 * @return 成功=0, 失败返回负数或者程序退出.
 */
static int send_frame_to_filters(InputStream *ist, AVFrame *decoded_frame)
{
    int i, ret;
    AVFrame *f;

    av_assert1(ist->nb_filters > 0); /* ensure ret is initialized */
    // 1. 遍历输入流的每个过滤器(正常一个输入流只有一个InputFilter,即nb_filters=1).
    for (i = 0; i < ist->nb_filters; i++) {
        // 1.1 获取f的值.
        if (i < ist->nb_filters - 1) {//减1意义是: 只有当nb_filters>1时, 才会走这个if
            f = ist->filter_frame;
            ret = av_frame_ref(f, decoded_frame);//引用解码帧到过滤器帧
            if (ret < 0)
                break;
        } else
            f = decoded_frame;//推流正常走这里

        // 1.2 主要工作是 配置FilterGraph 和 将解码后的一帧送去过滤器.
        ret = ifilter_send_frame(ist->filters[i], f);
        if (ret == AVERROR_EOF)
            ret = 0; /* ignore */
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to inject frame into filter network: %s\n", av_err2str(ret));
            break;
        }
    }
    return ret;
}

/**
 * @brief 解码pkt,得到一帧解码帧,然后送去过滤器过滤.
 * @param ist 输入流
 * @param pkt 将要解码的pkt
 * @param got_output =1:解码一帧成功; =0:解码一帧失败,也可能遇到eagain,使用返回值判断即可.
 * @param decode_failed 标记decode()调用是否失败,=1表示解码失败.
 * @return 成功-0 失败-负数
 */
static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    AVCodecContext *avctx = ist->dec_ctx;
    int ret, err = 0;
    AVRational decoded_frame_tb;

    // 1. 给解码帧开辟内存
    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;

    // 2. 解码
    update_benchmark(NULL);
    ret = decode(avctx, decoded_frame, got_output, pkt);
    update_benchmark("decode_audio %d.%d", ist->file_index, ist->st->index);
    // 3. 返回值相关处理
    // 3.1 解码失败错误记录
    if (ret < 0)
        *decode_failed = 1;

    // 3.2 解码成功但采样率非法,将ret重新标记为解码失败
    if (ret >= 0 && avctx->sample_rate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Sample rate %d invalid\n", avctx->sample_rate);
        ret = AVERROR_INVALIDDATA;
    }

    // 3.3 check_decode_result处理.该函数里面若用户没指定-xerror选项,内部不会做太多处理,可以忽略它
    if (ret != AVERROR_EOF)
        check_decode_result(ist, got_output, ret);

    // 3.4 eagain/eof/或者真正的错误(没指定-xerror时),直接返回
    if (!*got_output || ret < 0)
        return ret;

    ist->samples_decoded += decoded_frame->nb_samples;//统计已经解码的音频样本数
    ist->frames_decoded++;//统计已经解码的音频数

    // 4. 使用样本数+采样率预测next_pts、next_dts
    /* increment next_dts to use for the case where the input stream does not
       have timestamps or there are multiple frames in the packet */
    //(增加next_dts以用于输入流没有时间戳或包中有多个帧的情况)
    ist->next_pts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;//例如样本数是1024,采样率是44.1k,那么该帧时长是0.023s.乘以AV_TIME_BASE是转成微秒
    ist->next_dts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;

    // 5. 获取解码帧的pts和tb
    if (decoded_frame->pts != AV_NOPTS_VALUE) {//解码帧pts有效在不处理
        decoded_frame_tb   = ist->st->time_base;
    } else if (pkt && pkt->pts != AV_NOPTS_VALUE) {//解码帧pts无效优先参考pkt
        decoded_frame->pts = pkt->pts;
        decoded_frame_tb   = ist->st->time_base;
    }else {//否则参考流的dts
        decoded_frame->pts = ist->dts;
        decoded_frame_tb   = AV_TIME_BASE_Q;
    }

    // 6. 将解码帧pts的单位转成采样率的单位
    /* av_rescale_delta():调整时间戳的大小，同时保留已知的持续时间.
     *
     * 此函数被设计为每个音频包调用，以将输入时间戳扩展到不同的时间基数.
     * 与简单的av_rescale_q()调用相比，该函数对于可能不一致的帧持续时间具有健壮性.
     *
     * 'last'形参是一个状态变量，必须为同一流的所有后续调用保留.
     * 对于第一次调用，' *last '应该初始化为#AV_NOPTS_VALUE.
     *
     * 参1: 输入时间基
     * 参2: 输入时间戳
     * 参3: duration时间基;这通常比' in_tb '和' out_tb '粒度更细(更大).
     * 参4: 到下一次调用此函数的持续时间(即当前包/帧的持续时间)
     * 参5: 指向用' fs_tb '表示的时间戳的指针，充当状态变量
     * 参6: 输出时间基
     * 返回值: 时间戳用' out_tb '表示
     * @note: 在这个函数的上下文中，“duration”以样本为单位，而不是以秒为单位. */
    if (decoded_frame->pts != AV_NOPTS_VALUE)
        decoded_frame->pts = av_rescale_delta(decoded_frame_tb, decoded_frame->pts,
                                              (AVRational){1, avctx->sample_rate}, decoded_frame->nb_samples, &ist->filter_in_rescale_delta_last,
                                              (AVRational){1, avctx->sample_rate});

    // 7. 将解码帧发送到过滤器处理
    ist->nb_samples = decoded_frame->nb_samples;// 保存最近一次解码帧的样本数.
    err = send_frame_to_filters(ist, decoded_frame);

    av_frame_unref(ist->filter_frame);//这里只是解引用filter_frame、decoded_frame,并未释放内存
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

/**
 * @brief 解码pkt,得到一帧解码帧,然后送去过滤器过滤.
 * @param ist 输入流
 * @param pkt 将要解码的pkt
 * @param got_output =1:解码一帧成功; =0:解码一帧失败,也可能遇到eagain,使用返回值判断即可.
 * @param duration_pts 传出参数,由解码后的帧成员pkt_duration得到,单位是AVStream->time_base units.
 * @param eof =0:未遇到eof; =1:eof到来
 * @param decode_failed 标记decode()调用是否失败,=1表示解码失败.
 * @return 成功-0 失败-负数
 */
static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output, int64_t *duration_pts, int eof,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    int i, ret = 0, err = 0;
    int64_t best_effort_timestamp;
    int64_t dts = AV_NOPTS_VALUE;
    AVPacket avpkt;

    // With fate-indeo3-2, we're getting 0-sized packets before EOF for some
    // reason. This seems like a semi-critical bug. Don't trigger EOF, and
    // skip the packet.
    //(使用fate-indeo3-2，我们在EOF之前得到了0大小的数据包。这似乎是一个半关键的bug。不要触发EOF，并跳过包)
    if (!eof && pkt && pkt->size == 0)
        return 0;

    // 1. 给解码帧开辟内存
    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;

    // 2. 重置pkt的dts
    if (ist->dts != AV_NOPTS_VALUE)
        dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);//转成ist->st->time_base单位的dts
    if (pkt) {
        avpkt = *pkt;
        //这里看到pkt->dts的值和单位是被改变的,值改成ist->dts, 单位改成ist->st->time_base
        avpkt.dts = dts; // ffmpeg.c probably shouldn't do this(FFmpeg.c可能不应该这样做)
    }

    // The old code used to set dts on the drain packet, which does not work
    // with the new API anymore.(旧的代码用于设置dts的漏包，这与新的API不再工作)
    // 3. 若是eof时,给dts_buffer数组增加一个元素.一般该数组只有一个元素,就是末尾时的dts
    if (eof) {
        void *new = av_realloc_array(ist->dts_buffer, ist->nb_dts_buffer + 1, sizeof(ist->dts_buffer[0]));
        if (!new)
            return AVERROR(ENOMEM);
        ist->dts_buffer = new;
        ist->dts_buffer[ist->nb_dts_buffer++] = dts;
    }

    // 4. 开始解码.
    update_benchmark(NULL);
    ret = decode(ist->dec_ctx, decoded_frame, got_output, pkt ? &avpkt : NULL);
    update_benchmark("decode_video %d.%d", ist->file_index, ist->st->index);
    if (ret < 0)
        *decode_failed = 1;

    // The following line may be required in some cases where there is no parser
    // or the parser does not has_b_frames correctly(在没有解析器或解析器没有正确地has_b_frames的情况下，可能需要使用下面这一行)
    // 5. 流参数的视频延迟帧数 < 解码器中帧重排序缓冲区的大小.即解码器的延迟比解复用延迟大的处理,可以理解为解码的速度跟不上解复用的速度.
    if (ist->st->codecpar->video_delay < ist->dec_ctx->has_b_frames) {
        //264解码器会使用has_b_frames给video_delay赋值,其余不做处理
        if (ist->dec_ctx->codec_id == AV_CODEC_ID_H264) {
            ist->st->codecpar->video_delay = ist->dec_ctx->has_b_frames;
        } else
            av_log(ist->dec_ctx, AV_LOG_WARNING,
                   "video_delay is larger in decoder than demuxer %d > %d.\n"
                   "If you want to help, upload a sample "
                   "of this file to ftp://upload.ffmpeg.org/incoming/ "
                   "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)\n",
                   ist->dec_ctx->has_b_frames,
                   ist->st->codecpar->video_delay);
    }

    // 6. 返回值相关处理
    // 6.1 check_decode_result处理.该函数里面若用户没指定-xerror选项,内部不会做太多处理,可以忽略它
    if (ret != AVERROR_EOF)
        check_decode_result(ist, got_output, ret);

    // 6.2 成功拿到解码一帧的判断,解码帧与解码器的参数是否一致
    if (*got_output && ret >= 0) {
        if (ist->dec_ctx->width  != decoded_frame->width ||
            ist->dec_ctx->height != decoded_frame->height ||
            ist->dec_ctx->pix_fmt != decoded_frame->format) {
            av_log(NULL, AV_LOG_DEBUG, "Frame parameters mismatch context %d,%d,%d != %d,%d,%d\n",
                decoded_frame->width,
                decoded_frame->height,
                decoded_frame->format,
                ist->dec_ctx->width,
                ist->dec_ctx->height,
                ist->dec_ctx->pix_fmt);
        }
    }

    // 6.3. eagain/eof/或者真正的错误(没指定-xerror时),直接返回
    if (!*got_output || ret < 0)
        return ret;

    if(ist->top_field_first>=0)
        decoded_frame->top_field_first = ist->top_field_first;

    ist->frames_decoded++;//统计解码器已经解码的帧数

    // 7. 硬件检索数据回调.一般为空.暂不深入研究
    if (ist->hwaccel_retrieve_data && decoded_frame->format == ist->hwaccel_pix_fmt) {
        err = ist->hwaccel_retrieve_data(ist->dec_ctx, decoded_frame);
        if (err < 0)
            goto fail;
    }
    ist->hwaccel_retrieved_pix_fmt = decoded_frame->format;

    // 8. best_effort_timestamp和pts的相关处理
    // 8.1 默认从解码后的一帧获取best_effort_timestamp(ffplay是走这种方式处理解码后的pts)
    best_effort_timestamp= decoded_frame->best_effort_timestamp;//使用各种启发式算法估计帧时间戳，单位为流的时基。
    *duration_pts = decoded_frame->pkt_duration;//传出参数,保存该帧的显示时长,

    // 8.2 若输入文件指定了-r选项,则从cfr_next_pts获取best_effort_timestamp
    if (ist->framerate.num)//输入文件的-r 25选项,注与输出文件的-r选项是不一样的.
        best_effort_timestamp = ist->cfr_next_pts++;//cfr_next_pts默认值是0

    // 8.3 若遇到eof 且 上面两步都没拿到值 且 dts_buffer有dts,则从dts_buffer数组获取best_effort_timestamp.
    //遇到eof 且 best_effort_timestamp没有值 且eof时在dts_buffer数组存有时间戳, 则取该数组首个元素给其赋值.
    if (eof && best_effort_timestamp == AV_NOPTS_VALUE && ist->nb_dts_buffer > 0) {
        best_effort_timestamp = ist->dts_buffer[0];

        //将数组元素往前移,覆盖首个元素.
        for (i = 0; i < ist->nb_dts_buffer - 1; i++)
            ist->dts_buffer[i] = ist->dts_buffer[i + 1];
        ist->nb_dts_buffer--;
    }

    // 8.4 保存decoded_frame->pts, ist->next_pts 以及 ist->pts.
    if(best_effort_timestamp != AV_NOPTS_VALUE) {
        //将best_effort_timestamp赋值给decoded_frame->pts,并转单位后赋值给ts变量
        int64_t ts = av_rescale_q(decoded_frame->pts = best_effort_timestamp, ist->st->time_base, AV_TIME_BASE_Q);

        if (ts != AV_NOPTS_VALUE)
            ist->next_pts = ist->pts = ts;//这里next_pts与pts都保存当前解码帧的pts. 猜想next_pts也保存当前pts原因应该是,
                                          //当出现解码帧的pts出错时,可以使用上一次正常解码的pts加上duration来估算出错的pts.后续可以验证一下.
    }

    //debug解码后的相关时间戳
    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "decoder -> ist_index:%d type:video "
               "frame_pts:%s frame_pts_time:%s best_effort_ts:%"PRId64" best_effort_ts_time:%s keyframe:%d frame_type:%d time_base:%d/%d\n",
               ist->st->index, av_ts2str(decoded_frame->pts),
               av_ts2timestr(decoded_frame->pts, &ist->st->time_base),
               best_effort_timestamp,
               av_ts2timestr(best_effort_timestamp, &ist->st->time_base),
               decoded_frame->key_frame, decoded_frame->pict_type,
               ist->st->time_base.num, ist->st->time_base.den);
    }

    if (ist->st->sample_aspect_ratio.num)//样本比例.正常不会进来
        decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;

    // 9. 发送视频解码帧到输入过滤器(视频的是: buffer).
    err = send_frame_to_filters(ist, decoded_frame);

fail:
    av_frame_unref(ist->filter_frame);//这里只是解引用filter_frame、decoded_frame,并未释放内存
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int transcode_subtitles(InputStream *ist, AVPacket *pkt, int *got_output,
                               int *decode_failed)
{
    AVSubtitle subtitle;
    int free_sub = 1;
    int i, ret = avcodec_decode_subtitle2(ist->dec_ctx,
                                          &subtitle, got_output, pkt);

    check_decode_result(NULL, got_output, ret);

    if (ret < 0 || !*got_output) {
        *decode_failed = 1;
        if (!pkt->size)
            sub2video_flush(ist);
        return ret;
    }

    if (ist->fix_sub_duration) {
        int end = 1;
        if (ist->prev_sub.got_output) {
            end = av_rescale(subtitle.pts - ist->prev_sub.subtitle.pts,
                             1000, AV_TIME_BASE);
            if (end < ist->prev_sub.subtitle.end_display_time) {
                av_log(ist->dec_ctx, AV_LOG_DEBUG,
                       "Subtitle duration reduced from %"PRId32" to %d%s\n",
                       ist->prev_sub.subtitle.end_display_time, end,
                       end <= 0 ? ", dropping it" : "");
                ist->prev_sub.subtitle.end_display_time = end;
            }
        }
        FFSWAP(int,        *got_output, ist->prev_sub.got_output);
        FFSWAP(int,        ret,         ist->prev_sub.ret);
        FFSWAP(AVSubtitle, subtitle,    ist->prev_sub.subtitle);
        if (end <= 0)
            goto out;
    }

    if (!*got_output)
        return ret;

    if (ist->sub2video.frame) {
        sub2video_update(ist, &subtitle);
    } else if (ist->nb_filters) {
        if (!ist->sub2video.sub_queue)
            ist->sub2video.sub_queue = av_fifo_alloc(8 * sizeof(AVSubtitle));
        if (!ist->sub2video.sub_queue)
            exit_program(1);
        if (!av_fifo_space(ist->sub2video.sub_queue)) {
            ret = av_fifo_realloc2(ist->sub2video.sub_queue, 2 * av_fifo_size(ist->sub2video.sub_queue));
            if (ret < 0)
                exit_program(1);
        }
        av_fifo_generic_write(ist->sub2video.sub_queue, &subtitle, sizeof(subtitle), NULL);
        free_sub = 0;
    }

    if (!subtitle.num_rects)
        goto out;

    ist->frames_decoded++;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!check_output_constraints(ist, ost) || !ost->encoding_needed
            || ost->enc->type != AVMEDIA_TYPE_SUBTITLE)
            continue;

        do_subtitle_out(output_files[ost->file_index], ost, &subtitle);
    }

out:
    if (free_sub)
        avsubtitle_free(&subtitle);
    return ret;
}

/**
 * @brief 将输入流的每一路输入过滤器关闭.
 * @param ist 输入流
 * @return 成功-0 失败-负数
 */
static int send_filter_eof(InputStream *ist)
{
    int i, ret;
    /* TODO keep pts also in stream time base to avoid converting back(保持PTS也在流时间基中，以避免转换回) */
    int64_t pts = av_rescale_q_rnd(ist->pts, AV_TIME_BASE_Q, ist->st->time_base,
                                   AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);//转为流的时基单位

    // 1. 将输入流的每一路输入过滤器关闭.
    for (i = 0; i < ist->nb_filters; i++) {
        ret = ifilter_send_eof(ist->filters[i], pts);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
/**
 * @brief 处理输入pkt.会调用解码函数进行处理.
 * @param ist 输入流
 * @param pkt 为空时,代表eof到来,需要刷空包冲刷解码器
 * @param no_eof eof到来,是由它决定是否发送eof给输入过滤器,0-发送,1-不发送.
 *
 * @return 0-eof到来; 1-eof未到来
 */
static int process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
    int ret = 0, i;
    int repeating = 0;
    int eof_reached = 0;

    AVPacket avpkt;
    // 1. 判断是否是第一次进来的时间戳，给InputStream的dts/pts赋值.
    //转码时,ist->dts参考has_b_frames; 不转码时优先参考pkt->pts,pkt->pts不存在则参考has_b_frames
    if (!ist->saw_first_ts) {
        /*求出has_b_frames缓存大小占用的时间.例如has_b_frames=2,avg_frame_rate={25,1},
         * 乘以AV_TIME_BASE是转成成微秒,除以帧率这假设是25,2*1000000/25=80,000μs.
         * has_b_frames: ffmpeg的注释是"解码器中帧重排序缓冲区的大小".
        */
        ist->dts = ist->st->avg_frame_rate.num ? - ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
        ist->pts = 0;
        //pkt存在 且 pkt->pts存在 且 不转码时,ist->dts由pkt->pts赋值,
        if (pkt && pkt->pts != AV_NOPTS_VALUE && !ist->decoding_needed) {
            ist->dts += av_rescale_q(pkt->pts, ist->st->time_base, AV_TIME_BASE_Q);
            ist->pts = ist->dts; //unused but better to set it to a value thats not totally wrong
        }
        ist->saw_first_ts = 1;
    }

    // 2. 如果next_dts/next_pts为空,使用ist->dts/ist->pts给其赋值.一般是首次时会进来.
    if (ist->next_dts == AV_NOPTS_VALUE)
        ist->next_dts = ist->dts;
    if (ist->next_pts == AV_NOPTS_VALUE)
        ist->next_pts = ist->pts;

    // 3. 按照是否空包来处理临时变量avpkt。
    if (!pkt) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
    } else {
        avpkt = *pkt;
    }

    // 4. 不是flush解码器时, 对ist->next_dts/ist->dts, ist->next_pts/ist->pts的处理.
    if (pkt && pkt->dts != AV_NOPTS_VALUE) {
        ist->next_dts = ist->dts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);
        // 非视频流 或者 视频流时不转码,ist->next_pts,ist->pts由ist->dts赋值;
        // 输入流是视频流且转码,不会进入if条件
        if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_VIDEO || !ist->decoding_needed)
            ist->next_pts = ist->pts = ist->dts;
    }

    // while we have more to decode or while the decoder did output something on EOF
    //(当我们有更多的东西要解码或者当解码器在EOF上输出一些东西的时候)
    // 5. 输入流要解码的流程.
    while (ist->decoding_needed) {
        int64_t duration_dts = 0;
        int64_t duration_pts = 0;
        int got_output = 0;
        int decode_failed = 0;

        ist->pts = ist->next_pts;
        ist->dts = ist->next_dts;

        // 5.1 解码包
        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            // 5.1.1 解码音频包
            ret = decode_audio    (ist, repeating ? NULL : &avpkt, &got_output,
                                   &decode_failed);
            break;
        case AVMEDIA_TYPE_VIDEO:
            // 5.1.2 解码视频包
            ret = decode_video    (ist, repeating ? NULL : &avpkt, &got_output, &duration_pts, !pkt,
                                   &decode_failed);
            // 预测next_dts
            if (!repeating || !pkt || got_output) {
                if (pkt && pkt->duration) {//统计eof+正常解码一帧的dts,即pkt不为空的情况
                    duration_dts = av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
                } else if(ist->dec_ctx->framerate.num != 0 && ist->dec_ctx->framerate.den != 0) {//统计pkt=NULL即刷空包时的dts
                    struct AVCodecParserContext *tyycode = av_stream_get_parser(ist->st);//一般是null
                    //这里实际思路就是: 使用帧率去获取dts.
                    //把下面ticks和ticks_per_frame去掉即可看出来.
                    int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict+1 : ist->dec_ctx->ticks_per_frame;
                    duration_dts = ((int64_t)AV_TIME_BASE * /* 转成微秒 */
                                    ist->dec_ctx->framerate.den * ticks) /
                                    ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
                                    //这里是连续除以num和ticks_per_frame，优先级从左到右.例如(1000000 * 1 * 2) / 25 / 2 = 80000/2=40000.
                }

                //next_dts指向下一个dts
                if(ist->dts != AV_NOPTS_VALUE && duration_dts) {
                    ist->next_dts += duration_dts;
                }else
                    ist->next_dts = AV_NOPTS_VALUE;
            }

            //成功解码一帧,预测next_pts
            if (got_output) {
                //可以看到,duration_pts非法时,参考duration_dts
                if (duration_pts > 0) {
                    ist->next_pts += av_rescale_q(duration_pts, ist->st->time_base, AV_TIME_BASE_Q);
                } else {
                    ist->next_pts += duration_dts;
                }
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            // 5.1.3 解码字幕包
            if (repeating)
                break;
            ret = transcode_subtitles(ist, &avpkt, &got_output, &decode_failed);
            if (!pkt && ret >= 0)
                ret = AVERROR_EOF;
            break;
        default:
            return -1;
        }//<== switch end ==>

        // 5.2 解码完成,标记并退出while
        if (ret == AVERROR_EOF) {
            eof_reached = 1;
            break;
        }

        // 5.3 失败处理
        if (ret < 0) {
            if (decode_failed) {//解码失败时会走这里
                av_log(NULL, AV_LOG_ERROR, "Error while decoding stream #%d:%d: %s\n",
                       ist->file_index, ist->st->index, av_err2str(ret));
            } else {//一般是发送解码帧到过滤器处理失败时会走这里
                av_log(NULL, AV_LOG_FATAL, "Error while processing the decoded "
                       "data for stream #%d:%d\n", ist->file_index, ist->st->index);
            }
            // 这里看到,当没指定-xerror选项,解码失败不会直接退出程序,只有decode_failed=0才会.
            if (!decode_failed || exit_on_error)
                exit_program(1);
            break;
        }

        // 5.4 成功解码一帧,标记输入流已经解码输出了.
        if (got_output)
            ist->got_output = 1;

        // 5.5 解码没报错但没有拿到解码帧,是eagain
        if (!got_output)
            break;

        // During draining, we might get multiple output frames in this loop.
        // ffmpeg.c does not drain the filter chain on configuration changes,
        // which means if we send multiple frames at once to the filters, and
        // one of those frames changes configuration, the buffered frames will
        // be lost. This can upset certain FATE tests.
        // Decode only 1 frame per call on EOF to appease these FATE tests.
        // The ideal solution would be to rewrite decoding to use the new
        // decoding API in a better way.
        /* 在引流过程中，我们可能会在这个循环中获得多个输出帧。ffmpeg.c不会在配置更改时耗尽过滤器链，
         * 这意味着如果我们一次向过滤器发送多个帧，而其中一个帧更改了配置，缓冲的帧将丢失。这可能会打乱某些FATE测试.
         * 在EOF上每次调用只解码1帧，以满足这些FATE测试。理想的解决方案是重写解码，以更好的方式使用新的解码API. */
        if (!pkt)
            break;

        repeating = 1;// 1)用于标记该pkt已经发送到解码器解码过, 下一次while循环应该传NULL进行解码;
                      // 2)当是视频时(字幕音频忽略),若传NULL还能拿到解码帧(got_output=1),说明该pkt可以输出多个帧,
                      // 所以视频时需要进入预测dts的逻辑,这就是if (!repeating || !pkt || got_output)需要或上got_output的意义.
    }//<== while (ist->decoding_needed) end ==>

    /* after flushing, send an EOF on all the filter inputs attached to the stream(冲洗后，对附加到流的所有过滤器输入发送EOF) */
    /* except when looping we need to flush but not to send an EOF(除非在循环时我们需要刷新但不发送EOF) */
    // 6. 刷空包 且 转码 且 eof到来 且 no_eof=0,那么发送eof给流对应的输入过滤器.
    // no_eof是关键,一般eof到来,是由它决定是否发送eof给过滤器.
    // 只有真正eof结束才会进来.若是eof但指定stream_loop循环,不会进来.
    if (!pkt && ist->decoding_needed && eof_reached && !no_eof) {
        int ret = send_filter_eof(ist);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "Error marking filters as finished\n");
            exit_program(1);
        }
    }

    // 7. copy的流程.(后续详细分析,简单看了一下,不难)
    /* handle stream copy */
    if (!ist->decoding_needed && pkt) {
        ist->dts = ist->next_dts;

        // 预测各种媒体流的next_dts
        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            av_assert1(pkt->duration >= 0);
            // 预测next_dts
            if (ist->dec_ctx->sample_rate) {
                ist->next_dts += ((int64_t)AV_TIME_BASE * ist->dec_ctx->frame_size) /
                                  ist->dec_ctx->sample_rate;//frame_size应该是样本数?
            } else {
                ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            // 若用户指定帧率,使用用户的帧率预测next_dts.
            if (ist->framerate.num) {
                // TODO: Remove work-around for c99-to-c89 issue 7
                AVRational time_base_q = AV_TIME_BASE_Q;
                int64_t next_dts = av_rescale_q(ist->next_dts, time_base_q, av_inv_q(ist->framerate));//next_dts转成帧率单位.
                // +1的意思就是预测下一帧的dts,因为帧率的间隔就是1.
                // 例如上面ist->next_dts=40000(40ms),单位是1000,000,帧率是{25,1},那么转成帧率单位后,next_dts就是1,那么+1在帧率单位中就是下一帧的dts.
                ist->next_dts = av_rescale_q(next_dts + 1, av_inv_q(ist->framerate), time_base_q);
            } else if (pkt->duration) {
                // 否则若pkt->duration存在则用其预测
                ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
            } else if(ist->dec_ctx->framerate.num != 0) {
                // 否则使用解码器上下文的帧率预测.
                // 这里实际思路就是: 使用帧率去获取dts.
                // 把下面ticks和ticks_per_frame去掉即可看出来.
                int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict + 1 : ist->dec_ctx->ticks_per_frame;
                ist->next_dts += ((int64_t)AV_TIME_BASE *
                                  ist->dec_ctx->framerate.den * ticks) /
                                  ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
            }
            break;
        }//<== switch end ==>

        ist->pts = ist->dts;// 可以看到copy时,pts由dts赋值,而dts由预测的next_dts得到.ffmpeg这里处理pts的方法我们可以进行参考
        ist->next_pts = ist->next_dts;
    }//<== if (!ist->decoding_needed && pkt) end ==>

    // 8. 判断是否进行copy 编码操作
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        // 输入流不可以输出到输出流 或者 输出流需要编码的话,不会执行copy.
        if (!check_output_constraints(ist, ost) || ost->encoding_needed)
            continue;

        // 这里的copy可认为是相当于的编码操作
        do_streamcopy(ist, ost, pkt);
    }

    return !eof_reached;
}

static void print_sdp(void)
{
    char sdp[16384];
    int i;
    int j;
    AVIOContext *sdp_pb;
    AVFormatContext **avc;

    for (i = 0; i < nb_output_files; i++) {
        if (!output_files[i]->header_written)
            return;
    }

    avc = av_malloc_array(nb_output_files, sizeof(*avc));
    if (!avc)
        exit_program(1);
    for (i = 0, j = 0; i < nb_output_files; i++) {
        if (!strcmp(output_files[i]->ctx->oformat->name, "rtp")) {
            avc[j] = output_files[i]->ctx;
            j++;
        }
    }

    if (!j)
        goto fail;

    av_sdp_create(avc, j, sdp, sizeof(sdp));

    if (!sdp_filename) {
        printf("SDP:\n%s\n", sdp);
        fflush(stdout);
    } else {
        if (avio_open2(&sdp_pb, sdp_filename, AVIO_FLAG_WRITE, &int_cb, NULL) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open sdp file '%s'\n", sdp_filename);
        } else {
            avio_printf(sdp_pb, "SDP:\n%s", sdp);
            avio_closep(&sdp_pb);
            av_freep(&sdp_filename);
        }
    }

fail:
    av_freep(&avc);
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const AVCodecHWConfig  *config = NULL;
        int i;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (ist->hwaccel_id == HWACCEL_GENERIC ||
            ist->hwaccel_id == HWACCEL_AUTO) {
            for (i = 0;; i++) {
                config = avcodec_get_hw_config(s->codec, i);
                if (!config)
                    break;
                if (!(config->methods &
                      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                if (config->pix_fmt == *p)
                    break;
            }
        }
        if (config) {
            if (config->device_type != ist->hwaccel_device_type) {
                // Different hwaccel offered, ignore.
                continue;
            }

            ret = hwaccel_decode_init(s);
            if (ret < 0) {
                if (ist->hwaccel_id == HWACCEL_GENERIC) {
                    av_log(NULL, AV_LOG_FATAL,
                           "%s hwaccel requested for input stream #%d:%d, "
                           "but cannot be initialized.\n",
                           av_hwdevice_get_type_name(config->device_type),
                           ist->file_index, ist->st->index);
                    return AV_PIX_FMT_NONE;
                }
                continue;
            }
        } else {
            const HWAccel *hwaccel = NULL;
            int i;
            for (i = 0; hwaccels[i].name; i++) {
                if (hwaccels[i].pix_fmt == *p) {
                    hwaccel = &hwaccels[i];
                    break;
                }
            }
            if (!hwaccel) {
                // No hwaccel supporting this pixfmt.
                continue;
            }
            if (hwaccel->id != ist->hwaccel_id) {
                // Does not match requested hwaccel.
                continue;
            }

            ret = hwaccel->init(s);
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL,
                       "%s hwaccel requested for input stream #%d:%d, "
                       "but cannot be initialized.\n", hwaccel->name,
                       ist->file_index, ist->st->index);
                return AV_PIX_FMT_NONE;
            }
        }

        if (ist->hw_frames_ctx) {
            s->hw_frames_ctx = av_buffer_ref(ist->hw_frames_ctx);
            if (!s->hw_frames_ctx)
                return AV_PIX_FMT_NONE;
        }

        ist->hwaccel_pix_fmt = *p;
        break;
    }

    return *p;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;

    if (ist->hwaccel_get_buffer && frame->format == ist->hwaccel_pix_fmt)
        return ist->hwaccel_get_buffer(s, frame, flags);

    return avcodec_default_get_buffer2(s, frame, flags);
}

/**
 * @brief 两个逻辑：
 * 1）输入流需要转码：对输入流ist做相关赋值，并调用avcodec_open2()
 * 2）输入流不需要转码：给pts、next_pts赋AV_NOPTS_VALUE()，这一步转码逻辑同样会执行
 *
 * @param ist_index 流下标
 * @param error 错误描述
 * @param error_len 错误描述长度
 *
 * @return 成功=0； 失败=负数
*/
static int init_input_stream(int ist_index, char *error, int error_len)
{
    int ret;
    InputStream *ist = input_streams[ist_index];

    /* 1.输入流需要转码的流程 */
    if (ist->decoding_needed) {
        /* 1.1获取输入流的编解码器.
         * 解码器在add_input_streams时已经赋值 */
        AVCodec *codec = ist->dec;
        if (!codec) {
            snprintf(error, error_len, "Decoder (codec %s) not found for input stream #%d:%d",
                    avcodec_get_name(ist->dec_ctx->codec_id), ist->file_index, ist->st->index);
            return AVERROR(EINVAL);
        }

        /* 1.2在add_input_streams对ist赋值的基础上，再对ist相关内容赋值 */
        ist->dec_ctx->opaque                = ist;
        ist->dec_ctx->get_format            = get_format;// 自定义获取像素格式,主要与硬件加速相关
        ist->dec_ctx->get_buffer2           = get_buffer;// 自定义获取buffer,主要与硬件加速相关
        ist->dec_ctx->thread_safe_callbacks = 1;         // 当用户自定义get_buffer,那么需要设置该值

        /*
         * 在AVCodecContext结构体中对refcounted_frames成员的解释：如果非零，则从avcodec_decode_video2()和avcodec_decode_audio4()返回的解码音
         * 频和视频帧是引用计数的，并且无限期有效。当它们不再需要时，调用者必须使用av_frame_unref()释放它们。否则，解码的帧一定不能被调用方释放，
         * 并且只有在下一次解码调用之前才有效。如果使用了avcodec_receive_frame()，这将总是自动启用。
         * -编码:未使用。
         * -解码:由调用者在avcodec_open2()之前设置。
        */
        av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);

        if (ist->dec_ctx->codec_id == AV_CODEC_ID_DVB_SUBTITLE && /* dvb字幕类型 */
           (ist->decoding_needed & DECODING_FOR_OST)) {/* 该输入流需要重新解码,open_output_file有：ist->decoding_needed |= DECODING_FOR_OST */
            /* 为dvb字幕设置compute_edt选项，该选项搜compute_edt源码看到，它作用是: 使用pts或timeout计算时间结束.
            AV_DICT_DONT_OVERWRITE：若存在该key，则不覆盖已有的value */
            av_dict_set(&ist->decoder_opts, "compute_edt", "1", AV_DICT_DONT_OVERWRITE);
            if (ist->decoding_needed & DECODING_FOR_FILTER)
                av_log(NULL, AV_LOG_WARNING, "Warning using DVB subtitles for filtering and output at the same time is not fully supported, also see -compute_edt [0|1]\n");
        }

        /* 默认输入文件的字幕类型为ass? */
        av_dict_set(&ist->decoder_opts, "sub_text_format", "ass", AV_DICT_DONT_OVERWRITE);

        /* Useful for subtitles retiming by lavf (FIXME), skipping samples in
         * audio, and video decoders such as cuvid or mediacodec.
         * (适用于由lavf (FIXME)重定时的字幕，跳过音频和视频解码器，如cuvid或mediacodec)
         * 例如1.mkv，ist->st->time_base={1,1000}
        */
        ist->dec_ctx->pkt_timebase = ist->st->time_base;

        /* 若用户没指定threads选项，则默认"auto" */
        if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
            av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
        /* Attached pics are sparse, therefore we would not want to delay their decoding till EOF.
         * (附件中的图片是稀少的，因此我们不想将它们的解码延迟到EOF)
         * 附属图片，则threads重设为1.
        */
        if (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            av_dict_set(&ist->decoder_opts, "threads", "1", 0);

        /* 1.3对输入流的硬件相关处理.推流没用到,后续再详细研究. */
        ret = hw_device_setup_for_decode(ist);
        if (ret < 0) {
            snprintf(error, error_len, "Device setup failed for "
                     "decoder on input stream #%d:%d : %s",
                     ist->file_index, ist->st->index, av_err2str(ret));
            return ret;
        }

        /* 1.4编解码器上下文与编解码器关联.
         * 这里看到，编解码器的相关选项是在这里被应用的 */
        if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 0);

            snprintf(error, error_len,
                     "Error while opening decoder for input stream "
                     "#%d:%d : %s",
                     ist->file_index, ist->st->index, av_err2str(ret));
            return ret;
        }

        /* 1.5检测是否含有多余的选项.
         * 当avcodec_open2成功打开后，若decoder_opts包含未知选项，会返回在decoder_opts中，
         * 所以此时decoder_opts只要有一个选项，就说明是非法的 */
        assert_avoptions(ist->decoder_opts);
    }

    /* 2.输入流不需要转码的流程 */
    ist->next_pts = AV_NOPTS_VALUE;
    ist->next_dts = AV_NOPTS_VALUE;

    return 0;
}

/**
 * @brief 根据输出流ost保存对应的输入流下标，获取输入流结构体.
 * @param ost 输出流
 * @return 成功=返回输出流对应的输入流; 失败=NULL
*/
static InputStream *get_input_stream(OutputStream *ost)
{
    if (ost->source_index >= 0)
        return input_streams[ost->source_index];
    return NULL;
}

static int compare_int64(const void *a, const void *b)
{
    return FFDIFFSIGN(*(const int64_t *)a, *(const int64_t *)b);
}

/* open the muxer when all the streams are initialized */
/**
 * @brief 若输出文件的所有输出流已经初始化完成。则给该输出文件进行写头，并且会清空每个输出流的复用队列。
 * 清空复用队列的操作：若队列没有包则不作处理；否则会将包读出来，然后调用write_packet()进行写帧操作，
 * 具体写帧流程看write_packet()，不难.
 *
 * @param of 输出文件
 * @param file_index 输出文件下标
*/
static int check_init_output_file(OutputFile *of, int file_index)
{
    int ret, i;

    /*1.检查每个输出流是否都已经被初始化.
     注，只有所有输出流完成初始化才会写头.*/
    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];//of->ost_index基本是0,暂时还没遇到过第一个流不是0的情况
        if (!ost->initialized)
            return 0;
    }

    of->ctx->interrupt_callback = int_cb;

    /*2.写头.
     * 这里看到，输出流的解复用选项在这里被使用。
     * 输入流的解复用选项在avformat_open_input()被使用.
    */
    ret = avformat_write_header(of->ctx, &of->opts);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not write header for output file #%d "
               "(incorrect codec parameters ?): %s\n",
               file_index, av_err2str(ret));
        return ret;
    }
    //assert_avoptions(of->opts);
    of->header_written = 1;//标记写头成功

    av_dump_format(of->ctx, file_index, of->ctx->url, 1);

    if (sdp_filename || want_sdp)
        print_sdp();

    /* flush the muxing queues */
    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];

        /* try to improve muxing time_base (only possible if nothing has been written yet) */
        /*尝试改进muxing time_base(只有在还没有写入任何内容的情况下才可能)*/
        /*av_fifo_size()：返回AVFifoBuffer中以字节为单位的数据量，也就是你可以从它读取的数据量。
         * 源码很简单：return (uint32_t)(f->wndx - f->rndx);
         *
         * 而该fifo队列没使用时，f->wndx=f->rndx=0;
         * 详细看AVFifoBuffer *frame_queue的注释.
        */
        if (!av_fifo_size(ost->muxing_queue))
            ost->mux_timebase = ost->st->time_base;//只有fifo队列没使用才会进来

        /*
         * av_fifo_generic_read():将数据从AVFifoBuffer提供 给 用户提供的回调.
         * 大概看了一下源码，数据会读完后，AVFifoBuffer的rptr会指向这片内存的起始地址.
         * 所以下面就是，将fifo的pkt出来，然后调用write_packet写包，最终while会把复用队列清空。
         * 若想更好邻居，需要理解av_fifo_generic_read()，而理解它需要理解rptr、wptr、rndx、wndx的含义。
         * 而理解这几个成员，则需要把libavutil/fifo.c里面的api看完，源码不多，该fifo队列设计类似ffplay的帧队列.
         * 不想花时间的，知道av_fifo_generic_read的作用：
         * 1）从fifo读出来出来，保存在&pkt；
         * 2）然后会把sizeof(pkt)大小的字节从fifo删掉即可。
        */
        while (av_fifo_size(ost->muxing_queue)) {
            AVPacket pkt;
            av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
            write_packet(of, &pkt, ost, 1);
        }
    }

    return 0;
}

/**
 * @brief 具体作用暂不分析.代码逻辑很简单.
 */
static int init_output_bsfs(OutputStream *ost)
{
    AVBSFContext *ctx;
    int i, ret;

    //如果nb_bitstream_filters为0，是不会覆盖输出流已有的参数ost->st->codecpar
    //没设置会直接返回0
    if (!ost->nb_bitstream_filters)
        return 0;

    /*1.遍历ost->bsf_ctx数组，给其赋值*/
    for (i = 0; i < ost->nb_bitstream_filters; i++) {
        ctx = ost->bsf_ctx[i];

        /*这里的赋值流程很简单，就是像链表一样赋值。
         * 1）当i=0时，使用ost->st->codecpar给ctx->par_in赋值；
         * 然后av_bsf_init()将ctx->par_in的值赋给ctx->par_out；
         * 2）当i!=0时，使用上一个元素的par_out给当前ctx->par_in赋值；
         * 然后av_bsf_init()继续将ctx->par_in的值赋给ctx->par_out；
         * 以此类推。画成赋值顺序的链表可以这样表示(注是赋值顺序，不是指针的指向)：
         * ost->st->codecpar ==> [0]ctx->par_in ==> [0]ctx->par_out
         * ==> [1]ctx->par_in ==> [1]ctx->par_out
         * ==> [2]ctx->par_in ==> [2]ctx->par_out 以此类推...
        */
        ret = avcodec_parameters_copy(ctx->par_in,
                                      i ? ost->bsf_ctx[i - 1]->par_out : ost->st->codecpar);
        if (ret < 0)
            return ret;

        //同理
        ctx->time_base_in = i ? ost->bsf_ctx[i - 1]->time_base_out : ost->st->time_base;

        /* av_bsf_init(): 初始化AVBSFContext.
         * 注，内部会调用avcodec_parameters_copy(ctx->par_out, ctx->par_in);
         * 给ctx->par_out赋值*/
        ret = av_bsf_init(ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing bitstream filter: %s\n",
                   ost->bsf_ctx[i]->filter->name);
            return ret;
        }
    }

    /*2.将ost->bsf_ctx尾元素的par_out，给输出流的参数ost->st->codecpar赋值.
     * 但注意，ctx->par_out实际上是由ost->st->codecpar赋值得到的，这里再使用
     * ctx->par_out给ost->st->codecpar赋值，有什么区别吗？
     * 笔者认为，唯一区别就是ctx->par_out经过了av_bsf_init的处理，与原来的参数
     * 比应该就是多了初始化的部分。
    .*/
    ctx = ost->bsf_ctx[ost->nb_bitstream_filters - 1];
    ret = avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
    if (ret < 0)
        return ret;

    ost->st->time_base = ctx->time_base_out;

    return 0;
}

/**
 * @brief 不转码时，给OutputStream类型初始化的流程，基本与转码的流程差不多.
 * @param ost 输出流
 * @return 成功=0； 失败=返回负数或者程序退出.
 */
static int init_output_stream_streamcopy(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    InputStream *ist = get_input_stream(ost);
    AVCodecParameters *par_dst = ost->st->codecpar;
    AVCodecParameters *par_src = ost->ref_par;
    AVRational sar;
    int i, ret;
    uint32_t codec_tag = par_dst->codec_tag;

    av_assert0(ist && !ost->filter);

    /*1.从输入流中拷贝参数到输出流的编码器上下文.
     该函数源码很简单，里面是深拷贝实现.*/
    ret = avcodec_parameters_to_context(ost->enc_ctx, ist->st->codecpar);
    if (ret >= 0)
        ret = av_opt_set_dict(ost->enc_ctx, &ost->encoder_opts);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Error setting up codec context options.\n");
        return ret;
    }

    /*2.从输出流的编码器拷贝参数到ost->ref_par.
     该函数源码很简单，里面是深拷贝实现.
    avcodec_parameters_to_context()/avcodec_parameters_from_context()基本一样的，只不过赋值的对象调换了*/
    ret = avcodec_parameters_from_context(par_src, ost->enc_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Error getting reference codec parameters.\n");
        return ret;
    }

    /*3.后续分析这个tag*/
    if (!codec_tag) {//ost->st->codecpar->codec_tag
        unsigned int codec_tag_tmp;
        if (!of->ctx->oformat->codec_tag ||
            av_codec_get_id (of->ctx->oformat->codec_tag, par_src->codec_tag) == par_src->codec_id ||
            !av_codec_get_tag2(of->ctx->oformat->codec_tag, par_src->codec_id, &codec_tag_tmp))
            codec_tag = par_src->codec_tag;
    }

    /*4. 从par_src拷贝编解码器信息到par_dst。
     * 看回上面，par_src是从输入流的参数得到的。参数传递的流程：
     * ist->st->codecpar ==> ost->enc_ctx ==> par_src(ost->ref_par) ==> par_dst(ost->st->codecpar)*/
    ret = avcodec_parameters_copy(par_dst, par_src);
    if (ret < 0)
        return ret;

    par_dst->codec_tag = codec_tag;

    if (!ost->frame_rate.num)
        ost->frame_rate = ist->framerate;
    ost->st->avg_frame_rate = ost->frame_rate;

    /*5.将内部时间信息从一个流传输到另一个流。*/
    /* avformat_transfer_internal_stream_timing_info():
     * @param ofmt ost的目标输出格式
     * @param ost 需要定时复制和调整的输出流
     * @param ist 引用输入流以从中复制计时
     * @param copy_tb 定义需要从何处导入流编解码器时基
     * 看该函数源码，大概就是对时基和帧率做处理.
    */
    ret = avformat_transfer_internal_stream_timing_info(of->ctx->oformat, ost->st, ist->st, copy_tb);
    if (ret < 0)
        return ret;

    // copy timebase while removing common factors(在删除公共因子的同时复制时基)
    /*6.初始化 流的时基, 将编码器的时基拷贝给流的时基.*/
    if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
        ost->st->time_base = av_add_q(av_stream_get_codec_timebase(ost->st), (AVRational){0, 1});

    // copy estimated duration as a hint to the muxer((复制估计持续时间作为对muxer的提示))
    /*7.若输出流的duration不存在，则将输入流的duration转换单位后，赋值给输出流的duration*/
    if (ost->st->duration <= 0 && ist->st->duration > 0)
        ost->st->duration = av_rescale_q(ist->st->duration, ist->st->time_base, ost->st->time_base);

    // copy disposition
    ost->st->disposition = ist->st->disposition;

    /*8.若输入流的side_data存在，则使用输入流的size_data给输出流赋值.不懂可参考转码的注释.*/
    if (ist->st->nb_side_data) {
        for (i = 0; i < ist->st->nb_side_data; i++) {
            const AVPacketSideData *sd_src = &ist->st->side_data[i];
            uint8_t *dst_data;

            dst_data = av_stream_new_side_data(ost->st, sd_src->type, sd_src->size);
            if (!dst_data)
                return AVERROR(ENOMEM);
            memcpy(dst_data, sd_src->data, sd_src->size);
        }
    }

    /*是否对side_data数据类型为AV_PKT_DATA_DISPLAYMATRIX的内容重写.*/
    if (ost->rotate_overridden) {
        uint8_t *sd = av_stream_new_side_data(ost->st, AV_PKT_DATA_DISPLAYMATRIX,
                                              sizeof(int32_t) * 9);
        if (sd)
            av_display_rotation_set((int32_t *)sd, -ost->rotate_override_value);//转码时，这里旋转重写的值(rotate_override_value)是0.
    }

    /*9.根据不同媒体类型给成员赋值*/
    switch (par_dst->codec_type) {//par_dst = ost->st->codecpar;
    case AVMEDIA_TYPE_AUDIO:
        /*-acodec copy和-vol不能同时使用.因为我们看到audio_volume默认是256,
         当不是256说明用户添加-vol，就需要解码，而用户又指定了copy，所以ffmpeg会报错.*/
        if (audio_volume != 256) {
            av_log(NULL, AV_LOG_FATAL, "-acodec copy and -vol are incompatible (frames are not decoded)\n");
            exit_program(1);
        }
        /* block_align: 仅音频。某些格式所需的每个编码音频帧的字节数。
         * 对应于WAVEFORMATEX中的nBlockAlign。*/
        if((par_dst->block_align == 1 || par_dst->block_align == 1152 || par_dst->block_align == 576) && par_dst->codec_id == AV_CODEC_ID_MP3)
            par_dst->block_align= 0;
        if(par_dst->codec_id == AV_CODEC_ID_AC3)
            par_dst->block_align= 0;
        break;
    case AVMEDIA_TYPE_VIDEO:
        //获取宽高比，优先获取用户输入的frame_aspect_ratio，再到ist->st->sample_aspect_ratio、par_src->sample_aspect_ratio
        if (ost->frame_aspect_ratio.num) { // overridden by the -aspect cli option(被-aspect命令行选项覆盖)
            sar =
                av_mul_q(ost->frame_aspect_ratio,
                         (AVRational){ par_dst->height, par_dst->width });
            av_log(NULL, AV_LOG_WARNING, "Overriding aspect ratio "
                   "with stream copy may produce invalid files\n");//(用流复制重写纵横比可能会产生无效文件)
            }
        else if (ist->st->sample_aspect_ratio.num)
            sar = ist->st->sample_aspect_ratio;
        else
            sar = par_src->sample_aspect_ratio;
        ost->st->sample_aspect_ratio = par_dst->sample_aspect_ratio = sar;
        ost->st->avg_frame_rate = ist->st->avg_frame_rate;
        ost->st->r_frame_rate = ist->st->r_frame_rate;
        break;
    }

    ost->mux_timebase = ist->st->time_base;

    return 0;
}

/**
 * @brief 这里会给每一个流增加一个encoder元数据选项。
 * @param of 输出文件封装结构体
 * @param ost 输出流
 */
static void set_encoder_id(OutputFile *of, OutputStream *ost)
{
    AVDictionaryEntry *e;

    uint8_t *encoder_string;
    int encoder_string_len;
    int format_flags = 0;
    int codec_flags = ost->enc_ctx->flags;

    /*1.若流的metadata的encoder选项不是空，则返回.
    因为在open_output_file()会将每一个流的元数据的encoder置为NULL*/
    if (av_dict_get(ost->st->metadata, "encoder",  NULL, 0))
        return;

    {
        //tyycode
        /* 测试av_opt_eval_flags():可以看到，从of-ctx找到fflags后，就是libavformat/options_table.h文件里的变量
         * static const AVOption avformat_options[]的fflags选项。*/
        const AVOption *o = av_opt_find(of->ctx, "fflags", NULL, 0, 0);
        if (!o)
            return;
        /*假设解复用选项of->opts的fflags的value值被设为"32"，那么通过av_opt_eval_flags后，将里面的数字会设置到format_flags中，
           所以看到，format_flags的值 从0变成32.注，因为fflags是int64(具体类型需要看源码)，所以必须是该类型是字符串，例下面"32"写成"32.2"是无法读到数字的*/
        av_opt_eval_flags(of->ctx, o, "32", &format_flags);
    }
    /*2.若用户设置了解复用的fflags选项，则进行获取，然后保存在临时变量format_flags中*/
    /*关于AVOption模块，可参考: https://blog.csdn.net/ericbar/article/details/79872779*/
    e = av_dict_get(of->opts, "fflags", NULL, 0);
    if (e) {
        //若解复用不支持fflags选项，则返回
        const AVOption *o = av_opt_find(of->ctx, "fflags", NULL, 0, 0);
        if (!o)
            return;
        /*opt_eval_funcs: 计算选项字符串。这组函数可用于计算选项字符串并从中获取数字。它们所做的事情与av_opt_set()相同，只是结果被写入调用方提供的指针*/
        av_opt_eval_flags(of->ctx, o, e->value, &format_flags);
    }
    /*3.同理，若用户设置了编码器的flags选项，则进行获取，然后保存在临时变量codec_flags中*/
    e = av_dict_get(ost->encoder_opts, "flags", NULL, 0);
    if (e) {
        const AVOption *o = av_opt_find(ost->enc_ctx, "flags", NULL, 0, 0);
        if (!o)
            return;
        av_opt_eval_flags(ost->enc_ctx, o, e->value, &codec_flags);
    }

    /*4.获取编码器长度.LIBAVCODEC_IDENT宏里面的 ## 是拼接前后两个宏参数的意思.*/
    encoder_string_len = sizeof(LIBAVCODEC_IDENT) + strlen(ost->enc->name) + 2;
    encoder_string     = av_mallocz(encoder_string_len);
    if (!encoder_string)
        exit_program(1);

    /*5.若format_flags和codec_flags都不包含宏AVFMT_FLAG_BITEXACT，则使用LIBAVCODEC_IDENT字符串,
        否则直接使用"Lavc ".*/
    if (!(format_flags & AVFMT_FLAG_BITEXACT) && !(codec_flags & AV_CODEC_FLAG_BITEXACT)){
        char *t1 = AV_STRINGIFY(AV_VERSION_DOT(58,100,54));//将宏转成字符串
        char *tyycode1 = LIBAVCODEC_IDENT;
        char *tyycode2 = LIBAVCODEC_IDENT " ";//这里只是在末尾加个空格
        av_strlcpy(encoder_string, LIBAVCODEC_IDENT " ", encoder_string_len);
    }
    else{
        av_strlcpy(encoder_string, "Lavc ", encoder_string_len);
    }
    av_strlcat(encoder_string, ost->enc->name, encoder_string_len);//字符串拼接
    av_dict_set(&ost->st->metadata, "encoder",  encoder_string,
                AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);//最终设置到流的元数据

    {
        //tyycode
        AVDictionaryEntry *t = NULL;
        while((t = av_dict_get(ost->st->metadata, "", t, AV_DICT_IGNORE_SUFFIX))){
            printf("tyy_print_AVDirnary, t->key: %s, t->value: %s\n", t->key, t->value);
        }
    }
}

/**
 * @brief 后续遇到再分析，因为比较少用
*/
static void parse_forced_key_frames(char *kf, OutputStream *ost,
                                    AVCodecContext *avctx)
{
    char *p;
    int n = 1, i, size, index = 0;
    int64_t t, *pts;

    for (p = kf; *p; p++)
        if (*p == ',')
            n++;
    size = n;
    pts = av_malloc_array(size, sizeof(*pts));
    if (!pts) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate forced key frames array.\n");
        exit_program(1);
    }

    p = kf;
    for (i = 0; i < n; i++) {
        char *next = strchr(p, ',');

        if (next)
            *next++ = 0;

        if (!memcmp(p, "chapters", 8)) {

            AVFormatContext *avf = output_files[ost->file_index]->ctx;
            int j;

            if (avf->nb_chapters > INT_MAX - size ||
                !(pts = av_realloc_f(pts, size += avf->nb_chapters - 1,
                                     sizeof(*pts)))) {
                av_log(NULL, AV_LOG_FATAL,
                       "Could not allocate forced key frames array.\n");
                exit_program(1);
            }
            t = p[8] ? parse_time_or_die("force_key_frames", p + 8, 1) : 0;
            t = av_rescale_q(t, AV_TIME_BASE_Q, avctx->time_base);

            for (j = 0; j < avf->nb_chapters; j++) {
                AVChapter *c = avf->chapters[j];
                av_assert1(index < size);
                pts[index++] = av_rescale_q(c->start, c->time_base,
                                            avctx->time_base) + t;
            }

        } else {

            t = parse_time_or_die("force_key_frames", p, 1);
            av_assert1(index < size);
            pts[index++] = av_rescale_q(t, AV_TIME_BASE_Q, avctx->time_base);

        }

        p = next;
    }

    av_assert0(index == size);
    qsort(pts, size, sizeof(*pts), compare_int64);
    ost->forced_kf_count = size;
    ost->forced_kf_pts   = pts;
}

/**
 * @brief 初始化编码器时间基。
 * @param ost 输出流
 * @param default_time_base 编码的时间基
 *
 * @note 转时基只涉及输入容器与输出容器的时间基。解复用以及复用的时间基(即输入输出容器的时间基，对应字段是AVFormatCtx->stream->time_base)。
 * 而这里的编码时间基，可认为是中间的时间基，用于运算。
 * 可参考这三篇(不一定百分百准确)：
 * https://blog.csdn.net/weixin_44517656/article/details/110452852
 * https://blog.csdn.net/weixin_44517656/article/details/110494609
 * https://blog.csdn.net/weixin_44517656/article/details/110559611
 * @return void
*/
static void init_encoder_time_base(OutputStream *ost, AVRational default_time_base)
{
    InputStream *ist = get_input_stream(ost);
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVFormatContext *oc;

    //1.用户指定且有效，则使用用户的参数作为编码时间基？
    if (ost->enc_timebase.num > 0) {
        enc_ctx->time_base = ost->enc_timebase;
        return;
    }

    //2.用户指定且无效，那么判断输入流的流时间基(解码时间基)，若有效则使用输入容器的时间基，否则使用参数传进的默认时间基
    if (ost->enc_timebase.num < 0) {
        if (ist) {
            enc_ctx->time_base = ist->st->time_base;
            return;
        }

        //用户传入的编码时间基非法，使用参数传进的默认时间基.
        oc = output_files[ost->file_index]->ctx;
        av_log(oc, AV_LOG_WARNING, "Input stream data not available, using default time base\n");
    }

    //3.用户没指定，则使用默认时基
    enc_ctx->time_base = default_time_base;
}

/**
 * @brief 对封装的输出流结构体OutputStream进行初始化。
 *  主要是OutputStream内部相关成员以及OutputStream->enc_ctx的初始化。
 * @param ost 输出流
 * @return =0表示成功，否则为一个与AVERROR代码对应的负值 或者 程序退出
*/
static int init_output_stream_encode(OutputStream *ost)
{
    InputStream *ist = get_input_stream(ost);
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVCodecContext *dec_ctx = NULL;
    AVFormatContext *oc = output_files[ost->file_index]->ctx;//获取输出流对应输出文件的oc
    int j, ret;

    /* 1.给输出流的元数据添加encoder选项 */
    set_encoder_id(output_files[ost->file_index], ost);

    // Muxers use AV_PKT_DATA_DISPLAYMATRIX to signal rotation. On the other
    // hand, the legacy API makes demuxers set "rotate" metadata entries,
    // which have to be filtered out to prevent leaking them to output files.
    /*(muxer使用AV_PKT_DATA_DISPLAYMATRIX来表示旋转。另一方面，遗留API使解复用器设置“旋转”元数据条目，这些条目必须被过滤掉，以防止泄漏到输出文件中。)*/
    /* 2.若输出流的元数据存在rotate选项，则将其删除 */
    av_dict_set(&ost->st->metadata, "rotate", NULL, 0);

    /* 3.主要初始化流的disposition */
    if (ist) {
        ost->st->disposition          = ist->st->disposition;/*这个流中的disposition成员好像与附属图片相关*/

        dec_ctx = ist->dec_ctx;

        enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;/*这定义了色度样本的位置*/
    } else {
        /*可后续研究*/
        for (j = 0; j < oc->nb_streams; j++) {
            AVStream *st = oc->streams[j];
            /*这个if条件当某个媒体类型存在多个时，才会满足.例如ost是视频流，且输出视频流有两个，那么就会满足*/
            if (st != ost->st && st->codecpar->codec_type == ost->st->codecpar->codec_type)
                break;
        }
        if (j == oc->nb_streams)
            if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
                ost->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                ost->st->disposition = AV_DISPOSITION_DEFAULT;
    }

    /* 4.获取视频帧率 */
    if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        /* 若用户没有指定帧率，优先从过滤器中获取，if条件越在上面，优先级越高 */
        if (!ost->frame_rate.num)
            ost->frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->framerate;
        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->st->r_frame_rate;
        if (ist && !ost->frame_rate.num) {
            ost->frame_rate = (AVRational){25, 1};
            av_log(NULL, AV_LOG_WARNING,
                   "No information "
                   "about the input framerate is available. Falling "
                   "back to a default value of 25fps for output stream #%d:%d. Use the -r option "
                   "if you want a different framerate.\n",
                   ost->file_index, ost->index);
        }

        /* 若编码器有支持的帧率数组，且没有强制帧率，那么会根据上面存储的帧率，在ffmpeg的帧率数组中找到一个最近的帧率 */
        if (ost->enc->supported_framerates && !ost->force_fps) {
            int idx = av_find_nearest_q_idx(ost->frame_rate, ost->enc->supported_framerates);
            ost->frame_rate = ost->enc->supported_framerates[idx];
        }
        // reduce frame rate for mpeg4 to be within the spec limits
        // (降低mpeg4的帧率，使其在规格限制内)
        // 怎么降低，有兴趣的自行看源码，这里不深入研究
        if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
            av_reduce(&ost->frame_rate.num, &ost->frame_rate.den,
                      ost->frame_rate.num, ost->frame_rate.den, 65535);
        }
    }

    /* 5.设置不同媒体类型的编码器中的相关参数 */
    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        /*通过过滤器获取音频相关参数*/
        enc_ctx->sample_fmt     = av_buffersink_get_format(ost->filter->filter);
        if (dec_ctx)
            enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
                                                 av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
        enc_ctx->sample_rate    = av_buffersink_get_sample_rate(ost->filter->filter);
        enc_ctx->channel_layout = av_buffersink_get_channel_layout(ost->filter->filter);
        enc_ctx->channels       = av_buffersink_get_channels(ost->filter->filter);

        init_encoder_time_base(ost, av_make_q(1, enc_ctx->sample_rate));//通过采样率设置音频编码器的时间基
        break;

    case AVMEDIA_TYPE_VIDEO:
        //通过帧率设置视频编码器的时间基.注意av_inv_q是分子分母调转然后返回.例如帧率={25,1}，那么返回时间基就是{1,25}
        //视频编码器的时间基一般都是设为帧率的倒数,see https://blog.csdn.net/weixin_44517656/article/details/110355462
        init_encoder_time_base(ost, av_inv_q(ost->frame_rate));

        //若编码器的时间基存在一个为0，则从过滤器中获取时间基
        if (!(enc_ctx->time_base.num && enc_ctx->time_base.den))
            enc_ctx->time_base = av_buffersink_get_time_base(ost->filter->filter);

        /*检测帧率是否很大，因为上面讲过，编码器的时基就是帧率的倒数，若时基很小，说明帧率很大。
        若帧率很大，会判断视频同步方法：若不是PASSTHROUGH，则会优先判断是否是auto，然后再判断是否是vscfr、cfr.
        理解这里需要知道逻辑运算符的优先级，或运算是比与运算低的，具体看open_output_file()中-map=0的字幕流逻辑.*/
        if (   av_q2d(enc_ctx->time_base) < 0.001 && video_sync_method != VSYNC_PASSTHROUGH/*1)时间基小于千分之一且video_sync_method!=0*/
           && (video_sync_method == VSYNC_CFR /*4)视频同步方法是cfr*/
               || video_sync_method == VSYNC_VSCFR || /*3)视频同步方法是vscfr*/
               (video_sync_method == VSYNC_AUTO && !(oc->oformat->flags & AVFMT_VARIABLE_FPS))))/*2)视频同步方法是自动且不包含该宏*/
        {
            av_log(oc, AV_LOG_WARNING, "Frame rate very high for a muxer not efficiently supporting it.\n"
                                       "Please consider specifying a lower framerate, a different muxer or -vsync 2\n");
        }

        /* 将forced_kf_pts由单位AV_TIME_BASE_Q转成enc_ctx->time_base的时基单位.暂未深入研究
         * av_rescale_q()的原理：假设要转的pts=a，时基1{x1,y1}，时基2{x2,y2};
         * 因为转化前后的比是一样的，设转换后的pts=b，那么有a*x1/y1=b*x2/y2; 化简：
         * b=(a*x1*y2)/(y1*x2).
         * 对比注释，a * bq / cq这句话实际上看不出来，我们看源码验证上面：
         * 在av_rescale_q_rnd内部会将两个时基换算，然后调用av_rescale_rnd()，参数最终变成：
         * av_rescale_rnd(a, x1*y2, y1*x2, AV_ROUND_NEAR_INF);
         * 在av_rescale_rnd()内部，正常逻辑走：return (a * b + r) / c;
         * 那么去掉加上r，就验证了我们上面的原理.
         * av_rescale_q() see https://blog.csdn.net/weixin_44517656/article/details/110559611 */
        for (j = 0; j < ost->forced_kf_count; j++)
            ost->forced_kf_pts[j] = av_rescale_q(ost->forced_kf_pts[j],
                                                 AV_TIME_BASE_Q,
                                                 enc_ctx->time_base);

        /* 获取样品宽高比。获取方法：用户指定，则会使用av_mul_q获取；没指定，则从过滤器获取宽高比.
         * av_mul_q:两个有理数相乘，结果通过参数b传出,内部调用了av_reduce().
        */
        enc_ctx->width  = av_buffersink_get_w(ost->filter->filter);
        enc_ctx->height = av_buffersink_get_h(ost->filter->filter);
        enc_ctx->sample_aspect_ratio = ost->st->sample_aspect_ratio =
            ost->frame_aspect_ratio.num ? // overridden by the -aspect cli option(被-aspect命令行选项覆盖)
            av_mul_q(ost->frame_aspect_ratio, (AVRational){ enc_ctx->height, enc_ctx->width }) :
            av_buffersink_get_sample_aspect_ratio(ost->filter->filter);

        /*从过滤器获取像素格式*/
        enc_ctx->pix_fmt = av_buffersink_get_format(ost->filter->filter);
        /*内部libavcodec像素/样本格式的每个样本/像素位*/
        if (dec_ctx)
            enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
                                                 av_pix_fmt_desc_get(enc_ctx->pix_fmt)->comp[0].depth);

        /*设置帧率到编码器上下文*/
        enc_ctx->framerate = ost->frame_rate;
        /*设置帧率到输出流的平均帧率*/
        ost->st->avg_frame_rate = ost->frame_rate;

        if (!dec_ctx ||
            enc_ctx->width   != dec_ctx->width  ||
            enc_ctx->height  != dec_ctx->height ||
            enc_ctx->pix_fmt != dec_ctx->pix_fmt) {
            enc_ctx->bits_per_raw_sample = frame_bits_per_raw_sample;
        }

        if (ost->top_field_first == 0) {
            enc_ctx->field_order = AV_FIELD_BB;
        } else if (ost->top_field_first == 1) {
            enc_ctx->field_order = AV_FIELD_TT;
        }

        /*
         * 强制关键帧表达式解析.
         * 两种逻辑解析：
         * 1）forced_keyframes前5个字符面是"expr:"；
         * 2）forced_keyframes前6个字符面是"source"；
         * 暂不深入研究，比较少遇到，二次封装时可注释掉.
        */
        if (ost->forced_keyframes) {
            /*
             * av_expr_parse(): 解析表达式。
             * @param expr 是一个指针，在成功解析的情况下放置一个包含解析值的AVExpr，否则为NULL。
             *              当用户不再需要AVExpr时，必须使用av_expr_free()释放指向AVExpr的对象。
             * @param s 表达式作为一个以0结尾的字符串，例如“1+2^3+5*5+sin(2/3)”。就理解成是一个字符串即可.
             *
             * @param const_names NULL终止数组，零终止字符串的常量标识符，例如{"PI"， "E"， 0}
             * @param func1_names NULL终止数组，包含0终止的funcs1标识符字符串
             * @param funcs1 NULL结束的函数指针数组，用于带有1个参数的函数
             * @param func2_names NULL终止数组，包含0终止的funcs2标识符字符串
             * @param funcs2带两个参数的函数指针数组，以NULL结尾
             *
             * @param log_ctx父日志上下文
             * @return >= 0表示成功，否则为一个与AVERROR代码对应的负值
            */
            if (!strncmp(ost->forced_keyframes, "expr:", 5)) {
                ret = av_expr_parse(&ost->forced_keyframes_pexpr, ost->forced_keyframes+5,
                                    forced_keyframes_const_names, NULL, NULL, NULL, NULL, 0, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Invalid force_key_frames expression '%s'\n", ost->forced_keyframes+5);
                    return ret;
                }
                /*forced_keyframes_expr_const_values[]数组初始化*/
                ost->forced_keyframes_expr_const_values[FKF_N] = 0;
                ost->forced_keyframes_expr_const_values[FKF_N_FORCED] = 0;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N] = NAN;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T] = NAN;

                // Don't parse the 'forced_keyframes' in case of 'keep-source-keyframes',
                // parse it only for static kf timings
                //(不要在'keep-source-keyframes'的情况下解析' force_keyframes '，只在静态kf计时时解析它)
            } else if(strncmp(ost->forced_keyframes, "source", 6)) {
                parse_forced_key_frames(ost->forced_keyframes, ost, ost->enc_ctx);
            }
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        enc_ctx->time_base = AV_TIME_BASE_Q;//字幕编码上下文的时基单位是微秒.
        //没有分辨率，默认使用输入流中保存的分辨率
        if (!enc_ctx->width) {
            enc_ctx->width     = input_streams[ost->source_index]->st->codecpar->width;
            enc_ctx->height    = input_streams[ost->source_index]->st->codecpar->height;
        }
        break;
    case AVMEDIA_TYPE_DATA:
        break;
    default:
        abort();
        break;
    }

    /* 6.以编码器上下文的时基作为复用的时基 */
    ost->mux_timebase = enc_ctx->time_base;

    return 0;
}

//tyycode 用于测试av_add_q()等函数
#if 1
#if HAVE_FAST_CLZ
#if AV_GCC_VERSION_AT_LEAST(3,4)
#ifndef ff_ctz
#define ff_ctz(v) __builtin_ctz(v)
#endif
#ifndef ff_ctzll
#define ff_ctzll(v) __builtin_ctzll(v)
#endif
#ifndef ff_clz
#define ff_clz(v) __builtin_clz(v)
#endif
#endif
#endif
/* Stein's binary GCD algorithm:
 * https://en.wikipedia.org/wiki/Binary_GCD_algorithm */
int64_t av_gcd(int64_t a, int64_t b) {
    int za, zb, k;
    int64_t u, v;
    if (a == 0)
        return b;
    if (b == 0)
        return a;
    za = ff_ctzll(a);//求次方数，例如1000,那么就是za=3，1000000，那么za=6.不能求出10的次方数会返回0？例如我测试99,2345都是返回0
    zb = ff_ctzll(b);
    k  = FFMIN(za, zb);
    u = llabs(a >> za);//获取给定值的绝对值，返回值是long long，与abs，labs类似，只不过精度不一样.
    v = llabs(b >> zb);//例如100 0000，二进制=11110100001001000000，右移6位是15625
    while (u != v) {
        if (u > v)
            FFSWAP(int64_t, v, u);
        v -= u;
        v >>= ff_ctzll(v);
    }
    return (uint64_t)u << k;
}

//不深入研究
int av_reduce(int *dst_num, int *dst_den,
              int64_t num, int64_t den, int64_t max)
{
    AVRational a0 = { 0, 1 }, a1 = { 1, 0 };
    /* ^: 异或，相同为0，不同为1.
     * 将num den为正负的四种可能列出来，正正 正负 负正 负负，
     * 结果对应0 1 1 0。故sign为1，就说明num或者den存在一正一负.
    */
    int sign = (num < 0) ^ (den < 0);
    int64_t gcd = av_gcd(FFABS(num), FFABS(den));//gcd不深入研究

    if (gcd) {
        num = FFABS(num) / gcd;
        den = FFABS(den) / gcd;
    }
    if (num <= max && den <= max) {
        a1 = (AVRational) { num, den };
        den = 0;
    }

    /*正常逻辑不会进入*/
    while (den) {
        uint64_t x        = num / den;
        int64_t next_den  = num - den * x;
        int64_t a2n       = x * a1.num + a0.num;
        int64_t a2d       = x * a1.den + a0.den;

        if (a2n > max || a2d > max) {
            if (a1.num) x =          (max - a0.num) / a1.num;
            if (a1.den) x = FFMIN(x, (max - a0.den) / a1.den);

            if (den * (2 * x * a1.den + a0.den) > num * a1.den)
                a1 = (AVRational) { x * a1.num + a0.num, x * a1.den + a0.den };
            break;
        }

        a0  = a1;
        a1  = (AVRational) { a2n, a2d };
        num = den;
        den = next_den;
    }
    av_assert2(av_gcd(a1.num, a1.den) <= 1U);
    av_assert2(a1.num <= max && a1.den <= max);

    *dst_num = sign ? -a1.num : a1.num;
    *dst_den = a1.den;

    return den == 0;
}

/**
 * @brief 两个有理数相加.
 * 例如，以{1,2},{1,4}为例，
 * 那么tyycodeNum的算法就是得出分子；
 * tyycodeDen就是求出分母.
 * 这样这得出了两个有理数相加后(同分母)的有理数.
*/
AVRational av_add_q(AVRational b, AVRational c) {
    int64_t tyycodeNum = b.num * (int64_t) c.den + c.num * (int64_t) b.den;
    int64_t tyycodeDen = b.den * (int64_t) c.den;
    av_reduce(&b.num, &b.den, tyycodeNum, tyycodeDen, INT_MAX);
    return b;
}
#endif

/**
 * @brief 初始化输出流，流程分为初始化分转码和不转码。
 * 1.转码的流程：
 * 1）打开avcodec_open2前对ost、ost->enc_ctx初始化；
 * 2）然后调用avcodec_open2；
 * 3）将ost->enc_ctx的参数拷贝到ost->st->codecpar、ost->st->codec;
 * 4）调用init_output_bsfs()处理bsfs。
 * 5）调用check_init_output_file()，判断是否全部初始化完成，完成则写头avformat_write_header()。
 * 2.不转码的流程：
 * 1）调用init_output_stream_streamcopy初始化。
 *      注意，该函数内部并未调用avcodec_open2，也就说不转码时不需要调用该函数.
 * 2）调用init_output_bsfs()处理bsfs。
 * 3）调用check_init_output_file()，判断是否全部初始化完成，完成则写头avformat_write_header()。
 *
 * @param ost 输出流
 * @param error 指针，出错时将错误字符串进行传出
 * @param error_len error的长度
 */
static int init_output_stream(OutputStream *ost, char *error, int error_len)
{
    int ret = 0;

    /* 1.输出流需要编码的流程 */
    if (ost->encoding_needed) {
        AVCodec      *codec = ost->enc;//在new_xxx_stream()时初始化了
        AVCodecContext *dec = NULL;
        InputStream *ist;

        /* 1.1给输出流结构体OutputStream相关成员赋值 */
        ret = init_output_stream_encode(ost);
        if (ret < 0)
            return ret;

        /*1.2若输入文件存在subtitle_header(相当于存在字幕流)，输出文件也应该在输出流拷贝一份subtitle_header.
         * 包含文本字幕样式信息的头。
         * 对于SUBTITLE_ASS副标题类型，它应该包含整个ASS[脚本信息]和[V4+样式]部分，
         * 加上[事件]行和下面的格式行。它不应该包括任何对白(对话)线。
         * dec->subtitle_header应该指的是字幕？而不是指字幕头部相关的内容？笔者对字幕不是太熟悉，有兴趣可自行研究.
        */
        if ((ist = get_input_stream(ost)))
            dec = ist->dec_ctx;
        if (dec && dec->subtitle_header) {
            /* ASS code assumes this buffer is null terminated so add extra byte.
            (ASS代码假设这个缓冲区是空的，因此添加额外的字节)*/
            ost->enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
            if (!ost->enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(ost->enc_ctx->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
            ost->enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }
        /*1.3编码时线程数没设置，将设为自动*/
        if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0))
            av_dict_set(&ost->encoder_opts, "threads", "auto", 0);
        /*1.4音频时，若没设置音频码率，默认是128k*/
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !codec->defaults &&
            !av_dict_get(ost->encoder_opts, "b", NULL, 0) &&
            !av_dict_get(ost->encoder_opts, "ab", NULL, 0))
            av_dict_set(&ost->encoder_opts, "b", "128000", 0);

        /*1.5若AVHWFramesContext中的像素格式与av_buffersink_get_format获取的像素格式一样，
         * 则对av_buffersink_get_hw_frames_ctx得到的AVHWFramesContext引用数加1,
         * 否则调用hw_device_setup_for_encode，对dev->device_ref引用数加1*/
        //简单来说，这里就是设置enc_ctx->hw_frames_ctx或者enc_ctx->hw_device_ctx
        if (ost->filter && av_buffersink_get_hw_frames_ctx(ost->filter->filter) &&
            ((AVHWFramesContext*)av_buffersink_get_hw_frames_ctx(ost->filter->filter)->data)->format ==
            av_buffersink_get_format(ost->filter->filter)) {
            /*
             * ost->enc_ctx->hw_frames_ctx的注释：
             * 对AVHWFramesContext的引用，描述了输入(编码)或输出(解码)帧。引用由调用者设置，
             * 然后由libavcodec拥有(和释放)——设置后调用者永远不应该读取它。
             *
             * - decoding:该字段应该由调用者从get_format()回调中设置。之前的引用(如果有的话)在get_format()调用之前总是会被libavcodec取消。
             * 如果默认的get_buffer2()是与hwaccel像素格式一起使用的，那么这个AVHWFramesContext将用于分配帧缓冲区。
             * - encoding:对于配置为hwaccel像素格式的硬件编码器，该字段应该由调用者设置为描述输入帧的AVHWFramesContext的引用。
             * AVHWFramesContext.format必须等于AVCodecContext.pix_fmt。
             * 这个字段应该在调用avcodec_open2()之前设置。
            */
            /*看av_buffer_ref()源码：就是开辟一个AVBufferRef结构体后，浅拷贝指向传入参数，即就是引用数+1*/
            ost->enc_ctx->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(ost->filter->filter));
            if (!ost->enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
        } else {
            ret = hw_device_setup_for_encode(ost);
            if (ret < 0) {
                snprintf(error, error_len, "Device setup failed for "
                         "encoder on output stream #%d:%d : %s",
                     ost->file_index, ost->index, av_err2str(ret));
                return ret;
            }
        }
        /*1.6判断输入输出的字幕类型是否是非法转换.
          这里与open_output_file的new字幕流的相关判断类似*/
        if (ist && ist->dec->type == AVMEDIA_TYPE_SUBTITLE && ost->enc->type == AVMEDIA_TYPE_SUBTITLE) {
            int input_props = 0, output_props = 0;
            AVCodecDescriptor const *input_descriptor =
                avcodec_descriptor_get(dec->codec_id);
            AVCodecDescriptor const *output_descriptor =
                avcodec_descriptor_get(ost->enc_ctx->codec_id);
            //例如输入描述符的字幕类型是ass(name=ass)，input_descriptor->props=131072，这个值就是AV_CODEC_PROP_TEXT_SUB的值
            //以推流命令的-scodec text，那么通过enc_ctx->codec_id得到的output_descriptor->props也是131072，那么认为合法转换
            if (input_descriptor)
                input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (output_descriptor)
                output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (input_props && output_props && input_props != output_props) {
                snprintf(error, error_len,
                         "Subtitle encoding currently only possible from text to text "
                         "or bitmap to bitmap");//(字幕编码目前只能从文本到文本 或 位图到位图)
                return AVERROR_INVALIDDATA;
            }
        }

        /*1.7编解码器上下文与编解码器关联.
        这里看到，编解码器的相关选项是在这里被应用的*/
        if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 1);
            snprintf(error, error_len,
                     "Error while opening encoder for output stream #%d:%d - "
                     "maybe incorrect parameters such as bit_rate, rate, width or height",
                    ost->file_index, ost->index);
            return ret;
        }
        /*1.8若是音频且音频编码器不支持在每次调用中接收不同数量的样本，那么设置样本大小*/
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(ost->filter->filter,
                                            ost->enc_ctx->frame_size);
        /*1.9检测是否有未知的选项，因为avcodec_open2应用后，未知的选项会返回ost->encoder_opts本身*/
        assert_avoptions(ost->encoder_opts);

        //比特率设置过低。处理步骤它接受bits/s作为参数，而不是kbits/s
        if (ost->enc_ctx->bit_rate && ost->enc_ctx->bit_rate < 1000 &&
            ost->enc_ctx->codec_id != AV_CODEC_ID_CODEC2 /* don't complain about 700 bit/s modes */)
            av_log(NULL, AV_LOG_WARNING, "The bitrate parameter is set too low."
                                         " It takes bits/s as argument, not kbits/s\n");

        /*1.10从编码器拷贝参数到流中。
         因为上面我们对编码器部分成员做了赋值*/
        ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL,
                   "Error initializing the output stream codec context.\n");
            exit_program(1);
        }

        /*1.11将ost->enc_ctx拷贝到ost->st->codec，这一步可以不需要？
         * 这一步st->codec里的参数被新版本的st->codecpar代替了，
         * ffmpeg注释说st->codec后面可能废弃，这里笔者感觉可以不写，不过写一下问题也不大.*/
        /*
         * FIXME: ost->st->codec should't be needed here anymore.
         * (修复:ost->st->codec在这里不需要了)
         */
        /*
         * avcodec_copy_context()注释：
         * 将源AVCodecContext的设置复制到目标AVCodecContext中。
         * 最终的目的地编解码器上下文将是未打开的，即你需要调用avcodec_open2()，然
         * 后才能使用这个AVCodecContext解码/编码, 视频/音频数据。
         * 弃用:
         * 此函数的语义定义不明确，不应使用。如果需要将流参数从一个编解码器上下文传输到另一个，
         * 请使用中间的AVCodecParameters实例和avcodec_parameters_from_context()/avcodec_parameters_to_context()功能。
        */
        ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
        if (ret < 0)
            return ret;

        /*1.12将ost->enc_ctx->coded_side_data[i]的数据拷贝到st->side_data[type]，
        而这里的st是输出流的st.*/
        /*与整个编码流相关联的附加数据*/
        if (ost->enc_ctx->nb_coded_side_data) {
            int i;

            for (i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
                const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
                uint8_t *dst_data;

                /*av_stream_new_side_data()注释:
                 * 从流分配新信息。
                 * 返回值：指针指向新分配的数据，否则为NULL.
                 *
                 * 但是注意，av_stream_new_side_data()内部是没有拷贝数据的，只是单纯的开辟内存，
                 * 源码很简单：
                 * 1）若传入的sd_src->type在st->side_data数组找到，则st->side_data[type]指向新new的data，
                 * 并返回data；
                 * 2）若传入的sd_src->type在st->side_data数组找不到，则st->side_data数组元素加1，然后该元素同样指向新new的data，
                 * 并返回data；
                */
                dst_data = av_stream_new_side_data(ost->st, sd_src->type, sd_src->size);
                if (!dst_data)
                    return AVERROR(ENOMEM);

                /*拷贝数据.
                 * 看源码知道dst_data和st->side_data[type]指向一样，
                 * 所以此时相当于将ost->enc_ctx->coded_side_data[i]的数据拷贝到st->side_data[type]*/
                memcpy(dst_data, sd_src->data, sd_src->size);
            }
        }

        /*
         * Add global input side data. For now this is naive, and copies it
         * from the input stream's global side data. All side data should
         * really be funneled over AVFrame and libavfilter, then added back to
         * packet side data, and then potentially using the first packet for
         * global side data.
         * (添加全局输入side data。现在，这是幼稚的，从输入流的全局side data复制它。
         * 所有side data都应该通过AVFrame和libavfilter过滤，然后添加回包side data，然后可能使用第一个包用于全局side data)
         */
        /*1.13若输入流的side_data存在，则使用输入流的size_data给输出流赋值.
          所以这里看到，上面依赖coded_side_data填了输出流的side data，是可能被ist->st->side_data重写的.*/
        if (ist) {
            int i;
            for (i = 0; i < ist->st->nb_side_data; i++) {
                AVPacketSideData *sd = &ist->st->side_data[i];
                uint8_t *dst = av_stream_new_side_data(ost->st, sd->type, sd->size);
                if (!dst)
                    return AVERROR(ENOMEM);
                memcpy(dst, sd->data, sd->size);

                /*AV_PKT_DATA_DISPLAYMATRIX: 这个side data包含一个3x3的变换矩阵，
                 * 它描述了一个仿射变换，需要应用到解码的视频帧中以获得正确的表示。*/
                /*av_display_rotation_set(): 初始化一个描述按指定角度(以度为单位)纯逆时针旋转的变换矩阵。
                 * @param matri 分配的转换矩阵(将被此函数完全覆盖).
                 * @param angle rotation angle in degrees(单位为度).
                 * 源码就不深入研究了.
                */
                if (ist->autorotate && sd->type == AV_PKT_DATA_DISPLAYMATRIX)
                    av_display_rotation_set((uint32_t *)dst, 0);
            }
        }

        // copy timebase while removing common factors(在删除公共因子的同时复制时基)
        /*1.14初始化 流的时基, 将编码器的时基拷贝给流的时基.
         * 传{0,1}在两个有理数中加上相当于加上0，故等价于拷贝.
         * (注上面init_output_stream_encode()是给编码器的enc_ctx->time_base赋值)*/
        //例如enc_ctx->time_base={1,1000000},cp后，st->time_base={1,1000000}
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational){0, 1});

        // copy estimated duration as a hint to the muxer(复制估计持续时间作为对muxer的提示)
        /*1.15若输出流的duration不存在，则将输入流的duration转换单位后，赋值给输出流的duration.
        单位从ist->st->time_base转成ost->st->time_base*/
        if (ost->st->duration <= 0 && ist && ist->st->duration > 0)
            ost->st->duration = av_rescale_q(ist->st->duration, ist->st->time_base, ost->st->time_base);

        ost->st->codec->codec= ost->enc_ctx->codec;
    } else if (ost->stream_copy) {/*2.输出流不需要编码的流程*/
        ret = init_output_stream_streamcopy(ost);
        if (ret < 0)
            return ret;
    }

    // parse user provided disposition, and update stream values(解析用户提供的处理，并更新流值)
    /*3.若用户设置disposition，则将其设置到ost->st->disposition。推流没用到*/
    if (ost->disposition) {
        /*关于AVOption模块，可参考: https://blog.csdn.net/ericbar/article/details/79872779*/
        static const AVOption opts[] = {
            { "disposition"         , NULL, 0, AV_OPT_TYPE_FLAGS, { .i64 = 0 }, INT64_MIN, INT64_MAX, .unit = "flags" },
            { "default"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DEFAULT           },    .unit = "flags" },
            { "dub"                 , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DUB               },    .unit = "flags" },
            { "original"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_ORIGINAL          },    .unit = "flags" },
            { "comment"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_COMMENT           },    .unit = "flags" },
            { "lyrics"              , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_LYRICS            },    .unit = "flags" },
            { "karaoke"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_KARAOKE           },    .unit = "flags" },
            { "forced"              , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_FORCED            },    .unit = "flags" },
            { "hearing_impaired"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_HEARING_IMPAIRED  },    .unit = "flags" },
            { "visual_impaired"     , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_VISUAL_IMPAIRED   },    .unit = "flags" },
            { "clean_effects"       , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_CLEAN_EFFECTS     },    .unit = "flags" },
            { "attached_pic"        , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_ATTACHED_PIC      },    .unit = "flags" },
            { "captions"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_CAPTIONS          },    .unit = "flags" },
            { "descriptions"        , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DESCRIPTIONS      },    .unit = "flags" },
            { "dependent"           , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DEPENDENT         },    .unit = "flags" },
            { "metadata"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_METADATA          },    .unit = "flags" },
            { NULL },
        };
        static const AVClass class = {
            .class_name = "",
            .item_name  = av_default_item_name,
            .option     = opts,
            .version    = LIBAVUTIL_VERSION_INT,
        };
        const AVClass *pclass = &class;

        /*av_opt_eval_flags(): 利用参2给参4设置值，值为参3(可参考set_encoder_id()里的注释).
        传参2的作用是利用该AVOption里面的一些属性，例如disposition的类型是AV_OPT_TYPE_FLAGS，共用体default_val取的是i64*/
        ret = av_opt_eval_flags(&pclass, &opts[0], ost->disposition, &ost->st->disposition);
        if (ret < 0)
            return ret;
    }

    /* initialize bitstream filters for the output stream
     * needs to be done here, because the codec id for streamcopy is not
     * known until now
     * (输出流的初始化位流过滤器需要在这里完成，因为流复制的编解码器id到现在还不知道)*/
    /*4.根据是否设置bitstream_filters，来给ost->bsf_ctx数组赋值。
     * 内部会给ost->st->codecpar重新拷贝处理.*/
    ret = init_output_bsfs(ost);
    if (ret < 0)
        return ret;

    ost->initialized = 1;

    /*5.判断输出文件的所有输出流是否初始化完成，若完成，则会对该输出文件进行写头*/
    ret = check_init_output_file(output_files[ost->file_index], ost->file_index);
    if (ret < 0)
        return ret;

    return ret;
}

/**
 * @brief 报告有新的流.简单看一下即可.
 * @param input_index 输入(文件)下标
 * @param pkt pkt
 */
static void report_new_stream(int input_index, AVPacket *pkt)
{
    InputFile *file = input_files[input_index];
    AVStream *st = file->ctx->streams[pkt->stream_index];

    // 1. 包的流下标 < 流警告数,不管它.
    if (pkt->stream_index < file->nb_streams_warn)
        return;

    av_log(file->ctx, AV_LOG_WARNING,
           "New %s stream %d:%d at pos:%"PRId64" and DTS:%ss\n",
           av_get_media_type_string(st->codecpar->codec_type),
           input_index, pkt->stream_index,
           pkt->pos, av_ts2timestr(pkt->dts, &st->time_base));
    // 2. 保存
    file->nb_streams_warn = pkt->stream_index + 1;
}

/**
 * @brief 打开输入流的解码器, 输出流满足一定条件也会打开编码器,
 *          但音视频一般不会在transcode_init打开.
 *
 * @return 0-成功 负数-失败
 */
static int transcode_init(void)
{
    int ret = 0, i, j, k;
    AVFormatContext *oc;
    OutputStream *ost;
    InputStream *ist;
    char error[1024] = {0};

    /* 1.暂时没看懂,推流命令没用到 */
    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        for (j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *ofilter = fg->outputs[j];
            if (!ofilter->ost || ofilter->ost->source_index >= 0)//笔者没看懂这里if条件代表的含义
                continue;//推流命令走这里
            if (fg->nb_inputs != 1)
                continue;
            for (k = nb_input_streams-1; k >= 0 ; k--)
                if (fg->inputs[0]->ist == input_streams[k])
                    break;
            ofilter->ost->source_index = k;
        }
    }

    /* init framerate emulation(init帧速率仿真) */
    /* 2.用户指定-re选项，则给每个输入文件的每个输入流保存一个开始时间 */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        if (ifile->rate_emu){
            for (j = 0; j < ifile->nb_streams; j++){
                //这里看到ist_index的作用，估计是为了防止一些输入文件中, 第一个流下标不是0的情况
                input_streams[j + ifile->ist_index]->start = av_gettime_relative();
            }
        }
    }

    /* init input streams */
    /* 3.进一步初始化输入流(add_input_streams初始化了一部分),内部会对硬件相关处理.
     * 转码时实际上主要是打开解码器(avcodec_open2()),不转码则只处理一些参数 */
    for (i = 0; i < nb_input_streams; i++)
        if ((ret = init_input_stream(i, error, sizeof(error))) < 0) {
            /* 只要有一个输入流初始化失败，则将所有输出流编解码器上下文全部关闭 */
            for (i = 0; i < nb_output_streams; i++) {
                ost = output_streams[i];
                /* 这个函数释放enc_ctx内部相关内容, 但不会释放enc_ctx本身，需要avcodec_free_context()去做.
                 * 参考ffmpeg/tests/api/api-h264-test.c/video_decode_example(),
                 * 实际上ffmpeg注释也说了，avcodec_alloc_context3()开辟的上下文应该使用avcodec_free_context()释放 */
                avcodec_close(ost->enc_ctx);
            }
            goto dump_format;
        }

    /* open each encoder */
    /* 4.进一步初始化输出流(new_video(audio,subtitle,data)_stream初始化了一部分),内部会对硬件相关处理.
     * 转码时实际上主要是打开编码器(avcodec_open2()),不转码则只处理一些参数. */
    for (i = 0; i < nb_output_streams; i++) {
        // skip streams fed from filtergraphs until we have a frame for them(跳过过滤器的数据流，直到我们找到合适的帧)
        /* 因为我们在open_output_file()给需要转码的流创建了filter，所以音视频流会执行continue */
        if (output_streams[i]->filter)
            continue;

        /* 字幕一般会走这里，因为在open_output_files()创建filter时，字幕不会创建 */
        ret = init_output_stream(output_streams[i], error, sizeof(error));
        if (ret < 0)
            goto dump_format;
    }

    /* discard unused programs */
    /* 5. 暂不研究，推流没用到 */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        for (j = 0; j < ifile->ctx->nb_programs; j++) {
            AVProgram *p = ifile->ctx->programs[j];
            int discard  = AVDISCARD_ALL;

            for (k = 0; k < p->nb_stream_indexes; k++)
                if (!input_streams[ifile->ist_index + p->stream_index[k]]->discard) {
                    discard = AVDISCARD_DEFAULT;
                    break;
                }
            p->discard = discard;
        }
    }

    /* write headers for files with no streams */
    /* 写没有流的文件头.输出文件是/dev/null这种的处理？
     * 6. 暂不研究，推流没用到
     */
    for (i = 0; i < nb_output_files; i++) {
        oc = output_files[i]->ctx;
        if (oc->oformat->flags & AVFMT_NOSTREAMS && oc->nb_streams == 0) {
            ret = check_init_output_file(output_files[i], i);
            if (ret < 0)
                goto dump_format;
        }
    }

    // 下面都是dump打印到控制台的相关内容,可以不看.
 dump_format:
    /* dump the stream mapping */
    /*dump输入输出流的信息，比较简单，可以不看*/
    av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");

    /* 7. 遍历每个输入流的每个输入过滤器的graph_desc是否为空，不为空则打印相关信息；为空则不打印. */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];

        //遍历输入流的每个输入过滤器
        for (j = 0; j < ist->nb_filters; j++) {
            if (!filtergraph_is_simple(ist->filters[j]->graph)) {//推流命令不会进来
                av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
                       ist->file_index, ist->st->index, ist->dec ? ist->dec->name : "?",
                       ist->filters[j]->name);
                if (nb_filtergraphs > 1)
                    av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
                av_log(NULL, AV_LOG_INFO, "\n");
            }
        }
    }

    /* 8. 遍历输出流 */
    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];

        /*附属图片的处理*/
        if (ost->attachment_filename) {
            /* an attached file */
            av_log(NULL, AV_LOG_INFO, "  File %s -> Stream #%d:%d\n",
                   ost->attachment_filename, ost->file_index, ost->index);
            continue;
        }

        /* 判断输出流的输出过滤器的graph_desc是否为空，
         * 不为空则打印，然后continue；为空则不打印.
         * 这里一般是用到过滤器的处理.*/
        if (ost->filter && !filtergraph_is_simple(ost->filter->graph)) {
            /* output from a complex graph */
            av_log(NULL, AV_LOG_INFO, "  %s", ost->filter->name);
            if (nb_filtergraphs > 1)
                av_log(NULL, AV_LOG_INFO, " (graph %d)", ost->filter->graph->index);

            av_log(NULL, AV_LOG_INFO, " -> Stream #%d:%d (%s)\n", ost->file_index,
                   ost->index, ost->enc ? ost->enc->name : "?");
            continue;
        }

        /* 下面是正常流程 */
        av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d -> #%d:%d",
               input_streams[ost->source_index]->file_index,
               input_streams[ost->source_index]->st->index,
               ost->file_index,
               ost->index);//打印第一个输出流的map信息
        if (ost->sync_ist != input_streams[ost->source_index])
            av_log(NULL, AV_LOG_INFO, " [sync #%d:%d]",
                   ost->sync_ist->file_index,
                   ost->sync_ist->st->index);

        if (ost->stream_copy)
            av_log(NULL, AV_LOG_INFO, " (copy)");
        else {
            const AVCodec *in_codec    = input_streams[ost->source_index]->dec;//获取输入流的解码器
            const AVCodec *out_codec   = ost->enc;//获取输出流的编码器
            const char *decoder_name   = "?";
            const char *in_codec_name  = "?";
            const char *encoder_name   = "?";
            const char *out_codec_name = "?";
            const AVCodecDescriptor *desc;

            // 判断解码器是属于本地(native)还是属于第三方库
            if (in_codec) {
                decoder_name  = in_codec->name;//获取第三方库解码器名字
                desc = avcodec_descriptor_get(in_codec->id);//获取本地ffmepg的描述
                if (desc)
                    in_codec_name = desc->name;//描述存在，则从描述获取ffmpeg的解码器名字
                if (!strcmp(decoder_name, in_codec_name))//若第三方库解码器名字和ffmpeg的解码器名字相等，则认为是本地的解码器
                    decoder_name = "native";
            }

            // 判断编码器是属于本地(native)还是属于第三方库.
            if (out_codec) {
                encoder_name   = out_codec->name;//第三方库编码器名字。例如libx264
                desc = avcodec_descriptor_get(out_codec->id);//
                if (desc)
                    out_codec_name = desc->name;//ffmpeg的编码器名字，例如h264
                if (!strcmp(encoder_name, out_codec_name))//例如上面例子，因为不相等，所以encoder_name="libx264"
                    encoder_name = "native";
            }

            av_log(NULL, AV_LOG_INFO, " (%s (%s) -> %s (%s))",
                   in_codec_name, decoder_name,
                   out_codec_name, encoder_name);
        }
        av_log(NULL, AV_LOG_INFO, "\n");
    }//<== for (i = 0; i < nb_output_streams; i++) end ==>

    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "%s\n", error);
        return ret;
    }

    /* 9. 标记transcode_init()完成.
     atomic_store是一个原子操作，理解成C++的std::atomic<>模板即可*/
    atomic_store(&transcode_init_done, 1);

    return 0;
}

/* Return 1 if there remain streams where more output is wanted, 0 otherwise.(如果还有需要更多输出的流，则返回1，否则返回0) */
/**
 * @brief 如果还有需要更多输出的流，则返回1，否则返回0
*/
static int need_output(void)
{
    int i;

    /*1.遍历所有输出流：
     * 1）若某个流已经完成 或者 该流>=用户指定的输出文件大小限制，那么不处理该流；
     * 2）否则，则去判断 该输出流的帧数 是否>= 用户指定的最大帧数，若>=，则会把 包含该输出流的文件的所有流 都标记为完成.*/
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost    = output_streams[i];
        OutputFile *of       = output_files[ost->file_index];
        AVFormatContext *os  = output_files[ost->file_index]->ctx;

        if (ost->finished ||    /* 该输出流已经完成 */
            (os->pb && avio_tell(os->pb) >= of->limit_filesize))/* 当前输出io的字节大小 >= 用户指定的输出文件大小限制 */
            continue;

        if (ost->frame_number >= ost->max_frames) {//该输出流的帧数 >= 用户指定的最大帧数
            int j;
            for (j = 0; j < of->ctx->nb_streams; j++)
                close_output_stream(output_streams[of->ost_index + j]);
            continue;//不会往下执行返回1，所以当进入该if，就会返回0.
        }

        return 1;
    }

    return 0;
}

/**
 * Select the output stream to process.
 *
 * @return  selected output stream, or NULL if none available
 */
/**
 * @brief 选择一个输出流去处理。选择原理：
 * 1）若输出流没有初始化 且 输入没完成，那么优先返回该输出流;
 * 2）若已完成，则依赖输出流的st->cur_dts去选，遍历所有输出流，每次只
 *      选ost->st->cur_dts值最小的输出流进行返回。
 *
 * @return 成功=返回选择的输出流； 失败=返回NULL，表示流不可用
 */
static OutputStream *choose_output(void)
{
    int i;
    int64_t opts_min = INT64_MAX;// 记录所有输出流中dts最小的值，首次会设为最大值，是为了满足第一次if条件
    OutputStream *ost_min = NULL;// 对应dts为最小时的流

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        int64_t opts = ost->st->cur_dts == AV_NOPTS_VALUE ? INT64_MIN :
                       av_rescale_q(ost->st->cur_dts, ost->st->time_base,
                                    AV_TIME_BASE_Q);// 为空则设为INT64_MIN，否则将其转成微秒单位

        if (ost->st->cur_dts == AV_NOPTS_VALUE)
            av_log(NULL, AV_LOG_DEBUG,
                "cur_dts is invalid st:%d (%d) [init:%d i_done:%d finish:%d] (this is harmless if it occurs once at the start per stream)\n",
                ost->st->index, ost->st->id, ost->initialized, ost->inputs_done, ost->finished);//(如果在每个流开始时只发生一次，那么这是无害的)

        // 1.若输出流没有初始化 且 输入没完成，那么优先返回该输出流
        if (!ost->initialized && !ost->inputs_done)
            return ost;

        /* 2.依赖输出流的st->cur_dts(opts)去选，遍历所有输出流，每次只
         * 选ost->st->cur_dts值最小的输出流进行返回*/
        if (!ost->finished && opts < opts_min) {
            opts_min = opts;
            ost_min  = ost->unavailable ? NULL : ost;
        }
    }
    return ost_min;
}

static void set_tty_echo(int on)
{
#if HAVE_TERMIOS_H
    struct termios tty;
    if (tcgetattr(0, &tty) == 0) {
        if (on) tty.c_lflag |= ECHO;
        else    tty.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &tty);
    }
#endif
}

/**
 * @brief 处理一些键盘事件.不详细研究了.
*/
static int check_keyboard_interaction(int64_t cur_time)
{
    int i, ret, key;
    static int64_t last_time;
    if (received_nb_signals){
        printf("tyycode, received_nb_signals: %d\n", received_nb_signals);
        return AVERROR_EXIT;
    }

    /* read_key() returns 0 on EOF */
    /*1.本次距离上一次时间有0.1s及以上，且是前台运行，则从键盘读取一个字符*/
    if(cur_time - last_time >= 100000 && !run_as_daemon){
        key =  read_key();
        last_time = cur_time;
    }else
        key = -1;
    if (key == 'q')
        return AVERROR_EXIT;
    if (key == '+') av_log_set_level(av_log_get_level()+10);
    if (key == '-') av_log_set_level(av_log_get_level()-10);
    if (key == 's') qp_hist     ^= 1;
    if (key == 'h'){
        if (do_hex_dump){
            do_hex_dump = do_pkt_dump = 0;
        } else if(do_pkt_dump){
            do_hex_dump = 1;
        } else
            do_pkt_dump = 1;
        av_log_set_level(AV_LOG_DEBUG);
    }
    if (key == 'c' || key == 'C'){
        char buf[4096], target[64], command[256], arg[256] = {0};
        double time;
        int k, n = 0;
        fprintf(stderr, "\nEnter command: <target>|all <time>|-1 <command>[ <argument>]\n");
        i = 0;
        set_tty_echo(1);
        while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
            if (k > 0)
                buf[i++] = k;
        buf[i] = 0;
        set_tty_echo(0);
        fprintf(stderr, "\n");
        if (k > 0 &&
            (n = sscanf(buf, "%63[^ ] %lf %255[^ ] %255[^\n]", target, &time, command, arg)) >= 3) {
            av_log(NULL, AV_LOG_DEBUG, "Processing command target:%s time:%f command:%s arg:%s",
                   target, time, command, arg);
            for (i = 0; i < nb_filtergraphs; i++) {
                FilterGraph *fg = filtergraphs[i];
                if (fg->graph) {
                    if (time < 0) {
                        ret = avfilter_graph_send_command(fg->graph, target, command, arg, buf, sizeof(buf),
                                                          key == 'c' ? AVFILTER_CMD_FLAG_ONE : 0);
                        fprintf(stderr, "Command reply for stream %d: ret:%d res:\n%s", i, ret, buf);
                    } else if (key == 'c') {
                        fprintf(stderr, "Queuing commands only on filters supporting the specific command is unsupported\n");
                        ret = AVERROR_PATCHWELCOME;
                    } else {
                        ret = avfilter_graph_queue_command(fg->graph, target, command, arg, 0, time);
                        if (ret < 0)
                            fprintf(stderr, "Queuing command failed with error %s\n", av_err2str(ret));
                    }
                }
            }
        } else {
            av_log(NULL, AV_LOG_ERROR,
                   "Parse error, at least 3 arguments were expected, "
                   "only %d given in string '%s'\n", n, buf);
        }
    }
    if (key == 'd' || key == 'D'){
        int debug=0;
        if(key == 'D') {
            debug = input_streams[0]->st->codec->debug<<1;
            if(!debug) debug = 1;
            while(debug & (FF_DEBUG_DCT_COEFF
#if FF_API_DEBUG_MV
                                             |FF_DEBUG_VIS_QP|FF_DEBUG_VIS_MB_TYPE
#endif
                                                                                  )) //unsupported, would just crash
                debug += debug;
        }else{
            char buf[32];
            int k = 0;
            i = 0;
            set_tty_echo(1);
            while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
                if (k > 0)
                    buf[i++] = k;
            buf[i] = 0;
            set_tty_echo(0);
            fprintf(stderr, "\n");
            if (k <= 0 || sscanf(buf, "%d", &debug)!=1)
                fprintf(stderr,"error parsing debug value\n");
        }
        for(i=0;i<nb_input_streams;i++) {
            input_streams[i]->st->codec->debug = debug;
        }
        for(i=0;i<nb_output_streams;i++) {
            OutputStream *ost = output_streams[i];
            ost->enc_ctx->debug = debug;
        }
        if(debug) av_log_set_level(AV_LOG_DEBUG);
        fprintf(stderr,"debug=%d\n", debug);
    }
    if (key == '?'){
        fprintf(stderr, "key    function\n"
                        "?      show this help\n"
                        "+      increase verbosity\n"
                        "-      decrease verbosity\n"
                        "c      Send command to first matching filter supporting it\n"
                        "C      Send/Queue command to all matching filters\n"
                        "D      cycle through available debug modes\n"
                        "h      dump packets/hex press to cycle through the 3 states\n"
                        "q      quit\n"
                        "s      Show QP histogram\n"
        );
    }
    return 0;
}

/**
 * @brief 读取包线程.多个输入文件时,一个输入文件对应一个输入线程.
 *          该函数的处理还是比较简单的.
 * @param arg
 */
#if HAVE_THREADS
static void *input_thread(void *arg)
{
    InputFile *f = arg;
    unsigned flags = f->non_blocking ? AV_THREAD_MESSAGE_NONBLOCK : 0;
    int ret = 0;

    while (1) {
        AVPacket pkt;
        // 1. 读pkt
        ret = av_read_frame(f->ctx, &pkt);

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (ret < 0) {
            av_thread_message_queue_set_err_recv(f->in_thread_queue, ret);
            break;
        }

        // 2. 发送pkt到消息队列
        ret = av_thread_message_queue_send(f->in_thread_queue, &pkt, flags);
        if (flags && ret == AVERROR(EAGAIN)) {//非阻塞且eagain时
            flags = 0;
            ret = av_thread_message_queue_send(f->in_thread_queue, &pkt, flags);
            av_log(f->ctx, AV_LOG_WARNING,
                   "Thread message queue blocking; consider raising the "
                   "thread_queue_size option (current value: %d)\n",
                   f->thread_queue_size);//队列大小不够
        }
        // 3. 真正发送错误(一般是调用者设置了停止发送导致该错误),会把该pkt内部引用释放掉,并设置错误接收.
        // 看源码知道,设置错误接收是为了:当有调用者阻塞在接收包函数时, 让其返回.
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(f->ctx, AV_LOG_ERROR,
                       "Unable to send packet to main thread: %s\n",
                       av_err2str(ret));

            av_packet_unref(&pkt);
            av_thread_message_queue_set_err_recv(f->in_thread_queue, ret);
            break;
        }
    }

    return NULL;
}

/**
 * @brief 回收一个输入文件线程.
 * @param i 输入文件下标
 */
static void free_input_thread(int i)
{
    InputFile *f = input_files[i];
    AVPacket pkt;

    if (!f || !f->in_thread_queue)
        return;

    // 1. 往线程队列发送eof. 这样做能保证生产者无法再往该队列send pkt.
    av_thread_message_queue_set_err_send(f->in_thread_queue, AVERROR_EOF);

    // 2. 将线程队列剩余的pkt释放引用
    while (av_thread_message_queue_recv(f->in_thread_queue, &pkt, 0) >= 0)
        av_packet_unref(&pkt);

    // 3. 回收该线程.
    pthread_join(f->thread, NULL);
    f->joined = 1;

    // 4. 回收队列
    av_thread_message_queue_free(&f->in_thread_queue);
}

/**
 * @brief 回收所有输入文件线程.
 */
static void free_input_threads(void)
{
    int i;

    for (i = 0; i < nb_input_files; i++)
        free_input_thread(i);
}

/**
 * @brief 为输入文件创建线程，当存在多个输入文件时，一个输入文件对应一个线程。
 * @param i 输入文件的下标.
 * @return 0=成功； 负数=失败
*/
static int init_input_thread(int i)
{
    int ret;
    InputFile *f = input_files[i];

    /* 1.若输入文件数为1时，不需要开辟线程 */
    if (nb_input_files == 1)
        return 0;

    //lavfi指虚拟设备？新版本不支持该选项,可看https://www.jianshu.com/p/7f675764704b
    /*2.是否设置为非阻塞，设置条件：
     1）pb存在，则判断seekable是否为0，为0则说明不可以seek，则设置为非阻塞，否则为非0时，不会设置；
     2）pb不存在，则判断输入格式是否是lavfi，是则不设置非阻塞，否则设置为非阻塞*/
    if (f->ctx->pb ? !f->ctx->pb->seekable :
        strcmp(f->ctx->iformat->name, "lavfi"))
        f->non_blocking = 1;

    /* 3.开辟消息队列。
     内部消息队列最终是依赖AVFifoBuffer、pthread_mutex_t、pthread_cond_t去实现的. */
    ret = av_thread_message_queue_alloc(&f->in_thread_queue,
                                        f->thread_queue_size, sizeof(AVPacket));
    if (ret < 0)
        return ret;

    /* 4.创建线程 */
    if ((ret = pthread_create(&f->thread, NULL, input_thread, f))) {
        av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));
        av_thread_message_queue_free(&f->in_thread_queue);
        return AVERROR(ret);
    }

    return 0;
}

/**
 * @brief 为每个输入文件创建线程.
 * @return 0=成功； 负数=失败
*/
static int init_input_threads(void)
{
    int i, ret;

    for (i = 0; i < nb_input_files; i++) {
        ret = init_input_thread(i);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/**
 * @brief 多线程读取时,会从消息队列中读取pkt.
 * av_thread_message_queue_recv的实现不难.
 * @return 成功=0 失败返回负数(EAGAIN或者err_recv)
*/
static int get_input_packet_mt(InputFile *f, AVPacket *pkt)
{
    return av_thread_message_queue_recv(f->in_thread_queue, pkt,
                                        f->non_blocking ?
                                        AV_THREAD_MESSAGE_NONBLOCK : 0);
}
#endif

/**
 * @brief 从输入文件读取一个pkt.
 * @return 成功=0 失败返回负数
*/
static int get_input_packet(InputFile *f, AVPacket *pkt)
{
    // 1. 判断是否指定-re选项稳定读取pkt.
    // 实现稳定读取的思路很简单,就是对比当前读输入流当前的dts与该输入流转码经过的时间的大小.
    if (f->rate_emu) {
        int i;
        for (i = 0; i < f->nb_streams; i++) {
            InputStream *ist = input_streams[f->ist_index + i];
            // 很简单,就是:dts*1000000/1000000.内部会进行四舍五入的操作
            int64_t pts = av_rescale(ist->dts, 1000000, AV_TIME_BASE);
            // 该输入流转码经过的时间
            int64_t now = av_gettime_relative() - ist->start;
            // 输入流的读取速度比实际时间差还要大,说明太快了,需要暂停一下,那么返回EAGAIN.
            if (pts > now)
                return AVERROR(EAGAIN);
        }
    }

    // 2. 从输入文件读取一个pkt.
#if HAVE_THREADS
    //多个输入文件的流程
    if (nb_input_files > 1)
        return get_input_packet_mt(f, pkt);
#endif
    //单个文件的正常流程
    return av_read_frame(f->ctx, pkt);
}

/**
 * @brief 获取一个不可用的流，若没有则返回0.
 * @return 1=该流不可用；0=可用
*/
static int got_eagain(void)
{
    int i;
    for (i = 0; i < nb_output_streams; i++)
        if (output_streams[i]->unavailable)
            return 1;
    return 0;
}

/**
 * @brief 将所有输入文件的eagain置为0，再将所有输出流的unavailable置为0
*/
static void reset_eagain(void)
{
    int i;
    for (i = 0; i < nb_input_files; i++)
        input_files[i]->eagain = 0;
    for (i = 0; i < nb_output_streams; i++)
        output_streams[i]->unavailable = 0;
}

// set duration to max(tmp, duration) in a proper time base and return duration's time_base
/**
 * @brief 比较不同单位下的两个时长,最终保存最大的时长,并返回最大时长的时基.
 *
 * @param tmp 时长1
 * @param duration 时长2,传出参数,用于保存最大的时长.
 * @param tmp_time_base tmp的时基单位
 * @param time_base duration的时基单位
 *
 * @return 返回最大时长的时基
 */
static AVRational duration_max(int64_t tmp, int64_t *duration, AVRational tmp_time_base,
                               AVRational time_base)
{
    int ret;

    // 1. 为空,直接将临时的时长和时基赋值后返回.
    if (!*duration) {
        *duration = tmp;
        return tmp_time_base;
    }

    // 2. 比较大小.
    ret = av_compare_ts(*duration, time_base, tmp, tmp_time_base);
    if (ret < 0) {
        *duration = tmp;
        return tmp_time_base;
    }

    return time_base;
}

/**
 * @brief seek到文件开始位置, 并获取该输入文件中时长为最长的流的duration,以及对应的时基.
 * @param ifile 输入文件
 * @param is 输入文件上下文
 * @return >=0-成功 负数-失败
 */
static int seek_to_start(InputFile *ifile, AVFormatContext *is)
{
    InputStream *ist;
    AVCodecContext *avctx;
    int i, ret, has_audio = 0;
    int64_t duration = 0;

    // 1. 该函数用于移动到指定时间戳的关键帧位置.
    /* 对比ffplay使用的avformat_seek_file(): 该函数是新版本的seek api,
     * 它用于移动到时间戳最近邻的位置(在min_ts与max_ts范围内),内部会调用av_seek_frame*/
    ret = av_seek_frame(is, -1, is->start_time, 0);
    if (ret < 0)
        return ret;

    // 2. 判断seek前 是否存在 音频流解码成功过.
    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        /* duration is the length of the last frame in a stream
         * when audio stream is present we don't care about
         * last video frame length because it's not defined exactly */
        //(持续时间是流中有音频流时的最后一帧的长度，我们不关心最后一个视频帧的长度，因为它没有确切的定义)
        // 音频流 且 最近一次音频解码样本数不为0
        if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples)
            has_audio = 1;
    }

    // 3. 获取该输入文件中时长为最长的流的duration,以及对应的时基
    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        // 3.1 获取一帧的时长,有音频的情况下不会考虑视频.
        if (has_audio) {
            if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples) {
                // 音频以采样点数/采样率作为时长
                AVRational sample_rate = {1, avctx->sample_rate};

                duration = av_rescale_q(ist->nb_samples, sample_rate, ist->st->time_base);
            } else {
                continue;//不是音频流则跳过,因为上面has_audio=1,所以当有音频流时,非音频流都会走这里
            }
        } else {
            // 视频以 1/帧率 作为时长
            if (ist->framerate.num) {
                //以用户指定的帧率作为时长单位,刻度是1,例如帧率是25,那么刻度1在25单位下就是0.04
                duration = av_rescale_q(1, av_inv_q(ist->framerate), ist->st->time_base);
            } else if (ist->st->avg_frame_rate.num) {
                duration = av_rescale_q(1, av_inv_q(ist->st->avg_frame_rate), ist->st->time_base);
            } else {
                duration = 1;
            }
        }

        // 为空先保存ifile->duration的时基
        if (!ifile->duration)
            ifile->time_base = ist->st->time_base;

        // 3.2 获取该流的总时长.
        /* the total duration of the stream, max_pts - min_pts is
         * the duration of the stream without the last frame.(流的总持续时间，max_pts - min_pts是没有最后一帧的流的持续时间) */
        // 1)ist->max_pts > ist->min_pts是验证是否正常.
        // 2)ist->max_pts - (uint64_t)ist->min_pts求出来的结果是不包含最后一帧的流的时长,
        // INT64_MAX - duration中的duration是一帧的时长,那么INT64_MAX - duration就是代表ist->max_pts - (uint64_t)ist->min_pts的最大值,
        // 所以ist->max_pts - (uint64_t)ist->min_pts < INT64_MAX - duration就是判断是否溢出.
        if (ist->max_pts > ist->min_pts && ist->max_pts - (uint64_t)ist->min_pts < INT64_MAX - duration)
            duration += ist->max_pts - ist->min_pts;//得到该流的总时长

        // 3.3 获取该输入文件中时长为最长的流,以及对应的时基
        ifile->time_base = duration_max(duration, &ifile->duration, ist->st->time_base,
                                        ifile->time_base);
    }

    // 4. 循环次数完成减1
    if (ifile->loop > 0)
        ifile->loop--;

    return ret;
}

/*
 * Return
 * - 0 -- one packet was read and processed
 * - AVERROR(EAGAIN) -- no packets were available for selected file,
 *   this function should be called again
 * - AVERROR_EOF -- this function should not be called again
 */
/**
 * @brief
 */
static int process_input(int file_index)
{
    InputFile *ifile = input_files[file_index];
    AVFormatContext *is;
    InputStream *ist;
    AVPacket pkt;
    int ret, thread_ret, i, j;
    int64_t duration;
    int64_t pkt_dts;

    is  = ifile->ctx;
    // 1. 从输入文件读取一个pkt
    ret = get_input_packet(ifile, &pkt);
    // 1.1 eagain,直接返回
    if (ret == AVERROR(EAGAIN)) {
        ifile->eagain = 1;
        return ret;
    }

    // 1.2 eof或者错误的情况,如果设置了循环读取,那么会直接进入循环读取的逻辑.
    if (ret < 0 && ifile->loop) {
        AVCodecContext *avctx;
        // 1.2.1 遍历每一个输入流, 若该输入流:
        // 1)是copy,那么for循环不需要处理任何内容,因为copy时根本不会用到编解码器;
        // 2)是需要解码,那么会不断往解码器刷空包,直至解码器返回eof; 注意,并不是发送一次空包后,
        //      解码器就立马返回eof,我debug这里,一般发送5-8次空包左右,解码器会返回eof.  这里debug不难.
        for (i = 0; i < ifile->nb_streams; i++) {
            ist = input_streams[ifile->ist_index + i];
            avctx = ist->dec_ctx;
            if (ist->decoding_needed) {
                ret = process_input_packet(ist, NULL, 1);//传NULL代表刷空包,参3传1代表不想输入过滤器发送eof,因为这里是循环
                if (ret>0)//1代表eof未到来,那么就先返回, 只有解码器的eof到来才会往下执行重置解码器
                    return 0;

                /* 重置内部解码器状态/刷新内部缓冲区。应该在寻找或切换到其他流时调用.
                 * @note 当被引用的帧没有被使用时(例如avctx->refcounted_frames = 0)，这会使之前从解码器返回的帧无效.
                 * 当使用被引用的帧时，解码器只是释放它可能保留在内部的任何引用，但调用方的引用仍然有效. */
                avcodec_flush_buffers(avctx);
            }
        }

        // 1.2.2 释放掉对应该输入文件的线程
#if HAVE_THREADS
        free_input_thread(file_index);
#endif

        // 1.2.3 seek到文件开始
        ret = seek_to_start(ifile, is);
#if HAVE_THREADS
        // 1.2.4 重新为该输入文件创建线程
        thread_ret = init_input_thread(file_index);
        if (thread_ret < 0)
            return thread_ret;
#endif
        if (ret < 0)//seek的失败是放在下面的错误统一处理
            av_log(NULL, AV_LOG_WARNING, "Seek to start failed.\n");
        else
            ret = get_input_packet(ifile, &pkt);// 1.2.5 seek成功,会重新再读取一个pkt
        if (ret == AVERROR(EAGAIN)) {
            ifile->eagain = 1;
            return ret;
        }
    }//<== if (ret < 0 && ifile->loop) end ==>

    // 1.3 读取包错误或者是eof
    if (ret < 0) {
        // 1.3.1 遇到真正错误并指定-xerror选项,直接退出
        if (ret != AVERROR_EOF) {
            print_error(is->url, ret);
            if (exit_on_error)
                exit_program(1);
        }

        // 1.3.2 遍历所有输入流做清理工作
        for (i = 0; i < ifile->nb_streams; i++) {
            // 若输入流是需要解码的,则需要刷空包,以清除解码器剩余的包.
            ist = input_streams[ifile->ist_index + i];
            if (ist->decoding_needed) {
                ret = process_input_packet(ist, NULL, 0);//参3传0代表向过滤器发送eof,因为这里是真正结束了
                if (ret>0)
                    return 0;
            }

            // 将输入流对应的输出流标记完成编码和复用.
            /* mark all outputs that don't go through lavfi as finished(将所有未经过lavfi(指filter?)的输出标记为已完成) */
            for (j = 0; j < nb_output_streams; j++) {
                OutputStream *ost = output_streams[j];

                if (ost->source_index == ifile->ist_index + i && /* 输出流保存的输入流下标 与 输入流下标本身一致 */
                    (ost->stream_copy || ost->enc->type == AVMEDIA_TYPE_SUBTITLE))/* 拷贝或者是字幕 */
                    finish_output_stream(ost);
            }
        }

        ifile->eof_reached = 1;
        return AVERROR(EAGAIN);//为啥返回eagain?不是应该返回eof好点?留个疑问
    }

    // 2 重置input_files->eagain和output_streams->unavailable
    reset_eagain();

    // -dump选项
    if (do_pkt_dump) {
        av_pkt_dump_log2(NULL, AV_LOG_INFO, &pkt, do_hex_dump,
                         is->streams[pkt.stream_index]);
    }

    // 3. 出现新的流的包,报告并丢弃
    /* the following test is needed in case new streams appear
       dynamically in stream : we ignore them(如果新流在流中动态出现，则需要进行以下测试:忽略它们) */
    if (pkt.stream_index >= ifile->nb_streams) {
        report_new_stream(file_index, &pkt);
        goto discard_packet;
    }

    ist = input_streams[ifile->ist_index + pkt.stream_index];// 根据pkt保存的流下标 获取 对应输入流

    ist->data_size += pkt.size;// 统计该输入流已经读取的包大小和数目
    ist->nb_packets++;

    // 4. =1,该流读到的包都会被丢弃
    if (ist->discard)
        goto discard_packet;

    // 5. 若读到的pkt内容损坏 且 指定-xerror, 程序退出
    if (pkt.flags & AV_PKT_FLAG_CORRUPT) {
        av_log(NULL, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
               "%s: corrupt input packet in stream %d\n", is->url, pkt.stream_index);
        if (exit_on_error)
            exit_program(1);
    }

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "demuxer -> ist_index:%d type:%s "
               "next_dts:%s next_dts_time:%s next_pts:%s next_pts_time:%s pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s off:%s off_time:%s\n",
               ifile->ist_index + pkt.stream_index, av_get_media_type_string(ist->dec_ctx->codec_type),
               av_ts2str(ist->next_dts), av_ts2timestr(ist->next_dts, &AV_TIME_BASE_Q),
               av_ts2str(ist->next_pts), av_ts2timestr(ist->next_pts, &AV_TIME_BASE_Q),
               av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ist->st->time_base),
               av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ist->st->time_base),
               av_ts2str(input_files[ist->file_index]->ts_offset),
               av_ts2timestr(input_files[ist->file_index]->ts_offset, &AV_TIME_BASE_Q));
    }

    // 6. 留个疑问
    // 能否进来由pts_wrap_bits决定,pts_wrap_bits(PTS中的位数(用于封装控制))一般值为64.
    if(!ist->wrap_correction_done && is->start_time != AV_NOPTS_VALUE && ist->st->pts_wrap_bits < 64){
        int64_t stime, stime2;
        // Correcting starttime based on the enabled streams
        // FIXME this ideally should be done before the first use of starttime but we do not know which are the enabled streams at that point.
        //       so we instead do it here as part of discontinuity handling
        /* 根据启用的流更正开始时间在理想情况下，这应该在第一次使用starttime之前完成,
         * 但是我们不知道在那个时候哪些是启用的流。所以我们在这里做它作为不连续处理的一部分. */
        if (   ist->next_dts == AV_NOPTS_VALUE
            && ifile->ts_offset == -is->start_time
            && (is->iformat->flags & AVFMT_TS_DISCONT)) {//格式允许时间戳不连续
            int64_t new_start_time = INT64_MAX;
            // 获取所有输入流中, 开始时间最小的值.
            for (i=0; i<is->nb_streams; i++) {
                AVStream *st = is->streams[i];
                if(st->discard == AVDISCARD_ALL || st->start_time == AV_NOPTS_VALUE)
                    continue;
                new_start_time = FFMIN(new_start_time, av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q));
            }
            // 如果所有输入流的最小的开始时间 都比 AVFormatContext的开始时间大,那么需要调整
            if (new_start_time > is->start_time) {
                av_log(is, AV_LOG_VERBOSE, "Correcting start time by %"PRId64"\n", new_start_time - is->start_time);
                ifile->ts_offset = -new_start_time;//为啥直接加和负号?留个疑问
            }
        }

        stime = av_rescale_q(is->start_time, AV_TIME_BASE_Q, ist->st->time_base);
        stime2= stime + (1ULL<<ist->st->pts_wrap_bits);//1ULL特指64无符号bit,1左移pts_wrap_bits位
        ist->wrap_correction_done = 1;

        //留个疑问
        if(stime2 > stime && pkt.dts != AV_NOPTS_VALUE && pkt.dts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt.dts -= 1ULL<<ist->st->pts_wrap_bits;
            ist->wrap_correction_done = 0;
        }
        if(stime2 > stime && pkt.pts != AV_NOPTS_VALUE && pkt.pts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt.pts -= 1ULL<<ist->st->pts_wrap_bits;
            ist->wrap_correction_done = 0;
        }
    }

    // 7. 利用输入流的side数据 添加到 第一个pkt中
    /* add the stream-global side data to the first packet(将流全局side数据添加到第一个包中) */
    if (ist->nb_packets == 1) {
        for (i = 0; i < ist->st->nb_side_data; i++) {
            AVPacketSideData *src_sd = &ist->st->side_data[i];
            uint8_t *dst_data;

            // 不处理这个类型的边数据
            if (src_sd->type == AV_PKT_DATA_DISPLAYMATRIX)
                continue;

            // 从packet获取side info, 如果该类型在pkt已经存在数据,则不处理
            if (av_packet_get_side_data(&pkt, src_sd->type, NULL))//在wirte_packet()有对该函数说明
                continue;

            /* av_packet_new_side_data(): 分配数据包的新信息.
             * 参1: pkt; 参2: 要新开辟的边数据类型; 参3: 新开辟的边数据大小.
             * 返回值: 指向pkt中新开辟的边数据的地址. */
            // 在pkt没有该类型的边数据的,则为该类型重新开辟空间,并从ist->st->side_data拷贝数据.
            dst_data = av_packet_new_side_data(&pkt, src_sd->type, src_sd->size);
            if (!dst_data)
                exit_program(1);

            memcpy(dst_data, src_sd->data, src_sd->size);// 从ist->st->side_data拷贝数据到pkt的side data
        }
    }

    // 加上ts_offset,ts_offset一般是0,ts_offset是什么时间戳偏移?留个疑问
    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);

    // -itsscale选项,默认是1.0,设置输入ts的刻度,
    // 乘以ts_scale就是将pts,dts转成该刻度单位
    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts *= ist->ts_scale;
    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts *= ist->ts_scale;

    // 9. 留个疑问
    pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    if ((ist->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) && /* 视频或者音频 */
        pkt_dts != AV_NOPTS_VALUE && ist->next_dts == AV_NOPTS_VALUE && !copy_ts /* dts有效 且 next_dts无效 且 没指定-copyts */
        && (is->iformat->flags & AVFMT_TS_DISCONT) && ifile->last_ts != AV_NOPTS_VALUE) {// 允许时间戳不连续 且 last_ts有效
        int64_t delta   = pkt_dts - ifile->last_ts;//两次dts差值
        if (delta < -1LL*dts_delta_threshold*AV_TIME_BASE ||
            delta >  1LL*dts_delta_threshold*AV_TIME_BASE){//时间戳改变不在阈值范围[-dts_delta_threshold, dts_delta_threshold]
            ifile->ts_offset -= delta;
            av_log(NULL, AV_LOG_DEBUG,
                   "Inter stream timestamp discontinuity %"PRId64", new offset= %"PRId64"\n",
                   delta, ifile->ts_offset);
            pkt.dts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            if (pkt.pts != AV_NOPTS_VALUE)
                pkt.pts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
        }
    }

    // 10. 使用duration给pkt.pts算出显示时间戳,以及保存最大的pts和最小的pts,这样后面可以根据两者相减算出文件时长.
    duration = av_rescale_q(ifile->duration, ifile->time_base, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE) {
        pkt.pts += duration;
        ist->max_pts = FFMAX(pkt.pts, ist->max_pts);
        ist->min_pts = FFMIN(pkt.pts, ist->min_pts);
    }

    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += duration;

    // 11. 算下一帧的差值.和第9步的条件基本一样.留个疑问
    pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    if ((ist->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) &&
         pkt_dts != AV_NOPTS_VALUE && ist->next_dts != AV_NOPTS_VALUE &&
        !copy_ts) {
        int64_t delta   = pkt_dts - ist->next_dts;
        if (is->iformat->flags & AVFMT_TS_DISCONT) {
            if (delta < -1LL*dts_delta_threshold*AV_TIME_BASE ||
                delta >  1LL*dts_delta_threshold*AV_TIME_BASE ||
                pkt_dts + AV_TIME_BASE/10 < FFMAX(ist->pts, ist->dts)) {
                ifile->ts_offset -= delta;
                av_log(NULL, AV_LOG_DEBUG,
                       "timestamp discontinuity for stream #%d:%d "
                       "(id=%d, type=%s): %"PRId64", new offset= %"PRId64"\n",
                       ist->file_index, ist->st->index, ist->st->id,
                       av_get_media_type_string(ist->dec_ctx->codec_type),
                       delta, ifile->ts_offset);
                pkt.dts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
                if (pkt.pts != AV_NOPTS_VALUE)
                    pkt.pts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            }
        } else {
            if ( delta < -1LL*dts_error_threshold*AV_TIME_BASE ||
                 delta >  1LL*dts_error_threshold*AV_TIME_BASE) {
                av_log(NULL, AV_LOG_WARNING, "DTS %"PRId64", next:%"PRId64" st:%d invalid dropping\n", pkt.dts, ist->next_dts, pkt.stream_index);
                pkt.dts = AV_NOPTS_VALUE;
            }
            if (pkt.pts != AV_NOPTS_VALUE){
                int64_t pkt_pts = av_rescale_q(pkt.pts, ist->st->time_base, AV_TIME_BASE_Q);
                delta   = pkt_pts - ist->next_dts;
                if ( delta < -1LL*dts_error_threshold*AV_TIME_BASE ||
                     delta >  1LL*dts_error_threshold*AV_TIME_BASE) {
                    av_log(NULL, AV_LOG_WARNING, "PTS %"PRId64", next:%"PRId64" invalid dropping st:%d\n", pkt.pts, ist->next_dts, pkt.stream_index);
                    pkt.pts = AV_NOPTS_VALUE;
                }
            }
        }
    }

    // 保存最近一个pkt的dts
    if (pkt.dts != AV_NOPTS_VALUE)
        ifile->last_ts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "demuxer+ffmpeg -> ist_index:%d type:%s pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s off:%s off_time:%s\n",
               ifile->ist_index + pkt.stream_index, av_get_media_type_string(ist->dec_ctx->codec_type),
               av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ist->st->time_base),
               av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ist->st->time_base),
               av_ts2str(input_files[ist->file_index]->ts_offset),
               av_ts2timestr(input_files[ist->file_index]->ts_offset, &AV_TIME_BASE_Q));
    }

    // 略,内部一般会因ist2->sub2video.frame为空返回.
    sub2video_heartbeat(ist, pkt.pts);

    // 正常处理pkt的流程.可以看到正常处理是不关心返回值
    process_input_packet(ist, &pkt, 0);

discard_packet:
    av_packet_unref(&pkt);

    return 0;
}

/**
 * Perform a step of transcoding for the specified filter graph.(执行指定滤波图的转码步骤)
 *
 * @param[in]  graph     filter graph to consider(要考虑的滤波图)
 * @param[out] best_ist  input stream where a frame would allow to continue(输入流中的帧允许继续，传出参数)
 * @return  0 for success, <0 for error
 */
/**
 * @brief 从buffersink读取帧:
 * 1)如果有帧,会调reap_filters进行编码写帧;
 * 2)如果没有帧可读,eof或者读帧失败会直接返回;否则会进行eagain的判断:
 *      2.1)会选择输入文件中读帧失败次数最多的输入流,作为best_ist进行传出.
 * @param gragh 滤波图
 * @param best_ist eagain时,失败次数最多的输入流.
 * @return 成功=0,注意best_ist=NULL时也会返回0,认为转码结束; 失败返回负数或者程序退出.
*/
static int transcode_from_filter(FilterGraph *graph, InputStream **best_ist)
{
    int i, ret;
    int nb_requests, nb_requests_max = 0;
    InputFilter *ifilter;
    InputStream *ist;

    *best_ist = NULL;

//    {
//        // 从ffmpeg源码提取的avfilter_graph_request_oldest的部分代码,
//        // 用于测试avfilter_graph_request_oldest的作用.不测试时必须注释掉.
//        int r = -1;
//        AVFilterLink *oldest = graph->graph->sink_links[0];
//        oldest = graph->graph->sink_links[0];
//        if (oldest->dst->filter->activate) {
//            /* For now, buffersink is the only filter implementing activate. */
//            r = av_buffersink_get_frame_flags(oldest->dst, NULL,
//                                              AV_BUFFERSINK_FLAG_PEEK);
//            if (r != AVERROR_EOF)
//                return r;
//        } else {
//            //r = ff_request_frame(oldest);
//        }
//    }

    /*
     * avfilter_graph_request_oldest(): 在最老的sink link中请求帧。
     * 如果请求返回AVERROR_EOF，则尝试下一个。
     * 请注意，这个函数并不是过滤器图的唯一调度机制，它只是一个方便的函数，在正常情况下以平衡的方式帮助排出过滤器图。
     * 还要注意，AVERROR_EOF并不意味着帧在进程期间没有到达某些sinks。
     * 当有多个sink links时，如果请求的link返回一个EOF，这可能会导致过滤器刷新发送到另一个sink link的挂起的帧，尽管这些帧没有被请求。
     * @return ff_request_frame()的返回值，或者如果所有的links都返回AVERROR_EOF，则返回AVERROR_EOF。
     * ff_request_frame()的返回值：
     * >=0： 成功；
     * AVERROR_EOF：文件结尾；
     * AVERROR(EAGAIN)：如果之前的过滤器当前不能输出一帧，也不能保证EOF已经到达。
    */
    /* 1.获取graph有多少frame 可以从buffersink读取.正常都是返回eagain(-11) */
    ret = avfilter_graph_request_oldest(graph->graph);
    if (ret >= 0)
        return reap_filters(0);

    if (ret == AVERROR_EOF) {
        ret = reap_filters(1);
        // 为每个输出流标记编码结束
        for (i = 0; i < graph->nb_outputs; i++)
            close_output_stream(graph->outputs[i]->ost);
        return ret;
    }
    if (ret != AVERROR(EAGAIN))
        return ret;

    // 处理EAGAIN的情况
    /* 2. 判断输入文件是否处理完成. */
    for (i = 0; i < graph->nb_inputs; i++) {
        ifilter = graph->inputs[i];
        // 2.1 输入文件遇到eagain或者eof(上面的是输出流的EAGAIN,eof处理),执行continue
        ist = ifilter->ist;
        if (input_files[ist->file_index]->eagain ||
            input_files[ist->file_index]->eof_reached)
            continue;

        /* av_buffersrc_get_nb_failed_requests():
         * 获取失败请求的数量。当调用request_frame方法而缓冲区中没有帧时，请求失败。
         * 当添加一个帧时，该数字被重置*/
        // 2.2 记录遇到失败次数最多的输入流.
        // 注意是从输入filter请求.
        nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
        if (nb_requests > nb_requests_max) {
            nb_requests_max = nb_requests;
            *best_ist = ist;
        }
    }

    // 3. 找不到best_ist,说明输入文件遇到eagain或者eof,或者没有失败次数.那么会标记对应的输出流为不可用
    if (!*best_ist)
        for (i = 0; i < graph->nb_outputs; i++)
            graph->outputs[i]->ost->unavailable = 1;

    // 4. 找不到best_ist(即*best_ist=NULL),也会返回0.
    return 0;
}

/**
 * Run a single step of transcoding.(运行转码的单个步骤)
 *
 * @return  0 for success, <0 for error
 */
static int transcode_step(void)
{
    OutputStream *ost;
    InputStream  *ist = NULL;
    int ret;

    /* 1. 选择一个输出流 */
    ost = choose_output();
    if (!ost) {
        // 1.1 若拿到不可用的流时,且output_streams[i]->unavailable=1,会清空unavailable=0,然后返回.相当于再尝试获取输出流
        if (got_eagain()) {
            reset_eagain();
            av_usleep(10000);
            return 0;
        }

        // 1.2 没有更多的输入来读取，完成
        av_log(NULL, AV_LOG_VERBOSE, "No more inputs to read from, finishing.\n");
        return AVERROR_EOF;
    }

    /* 2. 过滤器相关，推流没用到，暂未研究 */
    if (ost->filter && !ost->filter->graph->graph) {
        if (ifilter_has_all_input_formats(ost->filter->graph)) {
            ret = configure_filtergraph(ost->filter->graph);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
                return ret;
            }
        }
    }

    /* 3. (基于输出流)选择一个输入流. */
    // 3.1 ost->filter 和 ost->filter->graph->graph都已经初始化的选择流程.
    if (ost->filter && ost->filter->graph->graph) {
        /* 3.1.1 对还没调用init_output_stream()初始化的流，则调用。
         * 注，因为在init_simple_filtergraph()是没有对
         * FilterGraph(即fg变量)里面的AVFilterGraph *graph赋值的，
         * 而ost->filter=fg->outputs[0]()即指向OutputFilter，
         * OutputFilter内部的FilterGraph *graph(这里的graph与前者graph的类型不一样)是指向fg变量，
         * 也就是说，ost->filter->graph的指向就是指向fg变量。那么由于fg->graph没赋值，
         * 所以一般不是这里调用init_output_stream()对音视频的输出流进行初始化。
        */
        if (!ost->initialized) {
            char error[1024] = {0};
            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }
        if ((ret = transcode_from_filter(ost->filter->graph, &ist)) < 0)
            return ret;
        if (!ist)// 这里看到,transcode_from_filter找不到best_ist且成功,会直接返回0
            return 0;
    } else if (ost->filter) {
        // 3.2 只有ost->filter被初始化的选择流程. 推流时,ost->filter对应fg->outputs[0](see init_simple_filtergraph())
        // 3.2.1 遍历 该输出流 对应的输入流的输入过滤器个数, 通过输入过滤器获取对应的输入流,判断是否可以作为输入
        // 正常输入流与输入过滤器个数都是1比1的(see init_simple_filtergraph()).
        int i;
        for (i = 0; i < ost->filter->graph->nb_inputs; i++) {
            InputFilter *ifilter = ost->filter->graph->inputs[i];
            if (!ifilter->ist->got_output && !input_files[ifilter->ist->file_index]->eof_reached) {//优先选择输入流还没解码成功过的 且 输入文件还没到达eof
                ist = ifilter->ist;
                break;
            }
        }
        // 3.2.2 没有找到输入流,认为处理所有输入流完成,标记inputs_done=1,函数返回.
        if (!ist) {
            ost->inputs_done = 1;
            return 0;
        }
    } else {
        //3.3 这里只要输入流下标>=0,就会选择该输入流
        av_assert0(ost->source_index >= 0);
        ist = input_streams[ost->source_index];
    }

    // 4. 读取一个pkt并解码发送到过滤器
    ret = process_input(ist->file_index);
    if (ret == AVERROR(EAGAIN)) {
        if (input_files[ist->file_index]->eagain)
            ost->unavailable = 1;
        return 0;
    }

    if (ret < 0)
        return ret == AVERROR_EOF ? 0 : ret;

    // 5. 从过滤器中读取一帧,并进行编码写帧.
    // 如果没有任何一个输入流被成功解码,会直接返回0.
    return reap_filters(0);
}

/**
 * @brief The following code is the main loop of the file converter
 * (下面的代码是文件转换器的主循环)
 * @return 成功-0 失败-负数或者程序直接退出
 */
static int transcode(void)
{
    int ret, i;
    AVFormatContext *os;
    OutputStream *ost;
    InputStream *ist;
    int64_t timer_start;
    int64_t total_packets_written = 0;

    /* 1.转码前的初始化 */
    ret = transcode_init();
    if (ret < 0)
        goto fail;

    if (stdin_interaction) {
        av_log(NULL, AV_LOG_INFO, "Press [q] to stop, [?] for help\n");
    }

    timer_start = av_gettime_relative();

#if HAVE_THREADS
    /* 2.存在多个输入文件时，给每个输入文件对应的开辟一个线程. */
    if ((ret = init_input_threads()) < 0)
        goto fail;
#endif

    // 3. 循环转码
    while (!received_sigterm) {
        int64_t cur_time= av_gettime_relative();

        /* if 'q' pressed, exits */
        if (stdin_interaction)
            if (check_keyboard_interaction(cur_time) < 0)//键盘事件相关处理，这里不详细研究
                break;

        /* check if there's any stream where output is still needed */
        if (!need_output()) {
            av_log(NULL, AV_LOG_VERBOSE, "No more output streams to write to, finishing.\n");
            break;
        }

        ret = transcode_step();
        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
            break;
        }

        /* dump report by using the output first video and audio streams */
        print_report(0, timer_start, cur_time);
    }//<== while (!received_sigterm) end ==>

    // 4. 多输入文件时.回收所有输入文件线程.
#if HAVE_THREADS
    free_input_threads();
#endif

    // 5. 确认所有输入流的解码器已经使用空包刷新缓冲区.一般在process_input()里面完成刷空包.
    /* at the end of stream, we must flush the decoder buffers(在流结束时,必须刷新解码器缓冲区) */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (!input_files[ist->file_index]->eof_reached) {// 文件没遇到eof.一般在process_input()被置1
            process_input_packet(ist, NULL, 0);
        }
    }

    // 6. 清空编码器
    flush_encoders();

    term_exit();

    /* write the trailer if needed and close file */
    // 7. 为每个输出文件调用av_write_trailer().
    for (i = 0; i < nb_output_files; i++) {
        os = output_files[i]->ctx;
        if (!output_files[i]->header_written) {
            av_log(NULL, AV_LOG_ERROR,
                   "Nothing was written into output file %d (%s), because "
                   "at least one of its streams received no packets.\n",
                   i, os->url);
            continue;// 没写头的文件不写尾
        }
        if ((ret = av_write_trailer(os)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error writing trailer of %s: %s\n", os->url, av_err2str(ret));
            if (exit_on_error)
                exit_program(1);
        }
    }

    /* dump report by using the first video and audio streams */
    print_report(1, timer_start, av_gettime_relative());

    // 8. 关闭编码器
    /* close each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];
        if (ost->encoding_needed) {
            av_freep(&ost->enc_ctx->stats_in);// 编码器这样关闭就可以了吗?
        }
        total_packets_written += ost->packets_written;//统计所以输出流已经写帧的包数.
    }

    // 写入的包数为0 且 abort_on_flags 与上 1 的结果不为0, 程序退出.
    if (!total_packets_written && (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT)) {
        av_log(NULL, AV_LOG_FATAL, "Empty output\n");
        exit_program(1);
    }

    // 9. 关闭解码器
    /* close each decoder */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->dec_ctx);// 关闭解码器
            if (ist->hwaccel_uninit)
                ist->hwaccel_uninit(ist->dec_ctx);
        }
    }

    // 10. 释放硬件相关
    av_buffer_unref(&hw_device_ctx);
    hw_device_free_all();

    /* finished ! */
    ret = 0;

 fail:
#if HAVE_THREADS
    free_input_threads();
#endif

    if (output_streams) {
        for (i = 0; i < nb_output_streams; i++) {
            ost = output_streams[i];
            if (ost) {
                if (ost->logfile) {
                    if (fclose(ost->logfile))
                        av_log(NULL, AV_LOG_ERROR,
                               "Error closing logfile, loss of information possible: %s\n",
                               av_err2str(AVERROR(errno)));
                    ost->logfile = NULL;
                }
                av_freep(&ost->forced_kf_pts);
                av_freep(&ost->apad);
                av_freep(&ost->disposition);
                av_dict_free(&ost->encoder_opts);
                av_dict_free(&ost->sws_dict);
                av_dict_free(&ost->swr_opts);
                av_dict_free(&ost->resample_opts);
            }
        }
    }
    return ret;
}

/**
 * @brief 获取进程的实时时间戳、该进程在内核层使用的时间、该进程在用户层使用的时间.
*/
static BenchmarkTimeStamps get_benchmark_time_stamps(void)
{
    // 1. 获取实时时间
    BenchmarkTimeStamps time_stamps = { av_gettime_relative() };
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    time_stamps.user_usec =
        (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
    time_stamps.sys_usec =
        (rusage.ru_stime.tv_sec * 1000000LL) + rusage.ru_stime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    /*
     * 获取当前进程的句柄，它是一个伪句柄，不需要回收，因为是伪句柄，不是真正的句柄
     * GetCurrentProcess() see: https://blog.csdn.net/zhangjinqing1234/article/details/6449844
    */
    proc = GetCurrentProcess();
    /*
     * GetProcessTimes() : 获取进程的创建时间、结束时间、内核时间、用户时间,
     * see: http://www.vbgood.com/api-getprocesstimes.html
     * FILETIME结构 :FILETIME 结构表示自 1601 年 1 月 1 日以来的 100 纳秒为间隔数(单位为100纳秒),
     *                  结构包含组合在一起形成一个 64 位值的两个 32 位值。
     * see: https://blog.csdn.net/vincen1989/article/details/7868879
     *
     * 除以10是因为要将100ns(纳秒)转成μs(微秒).具体转换：
     * 1s=10^6μs;
     * 1s=10^9ns;
     * 那么，10^6μs=10^9ns;  得出，1μs=1000ns.
     * 设100ns的单位是hs，那么有：
     * 1μs=10hs;
     * 故，从获取到的时间即(int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)设为x，单位100ns即hs，
     * 转化后的结果是y，单位是微秒，那么由它们的比是一样的得到表达式：
     * 1μs=10hs;yμs=xhs; ==> 1/y=10/x; ==> x=10y; ==> y=x/10
     * 这就是下面除以10的原因.
    */
    GetProcessTimes(proc, &c, &e, &k, &u);
    time_stamps.user_usec =
        ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    time_stamps.sys_usec =
        ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
#else
    time_stamps.user_usec = time_stamps.sys_usec = 0;
#endif
    return time_stamps;
}

static int64_t getmaxrss(void)
{
#if HAVE_GETRUSAGE && HAVE_STRUCT_RUSAGE_RU_MAXRSS
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    return (int64_t)rusage.ru_maxrss * 1024;
#elif HAVE_GETPROCESSMEMORYINFO
    HANDLE proc;
    PROCESS_MEMORY_COUNTERS memcounters;
    proc = GetCurrentProcess();
    memcounters.cb = sizeof(memcounters);
    GetProcessMemoryInfo(proc, &memcounters, sizeof(memcounters));
    return memcounters.PeakPagefileUsage;
#else
    return 0;
#endif
}

static void log_callback_null(void *ptr, int level, const char *fmt, va_list vl)
{
}

// tyycode我们加上这个结构体，会真的非常方便我们debug!!!!!!
struct AVDictionary {
    int count;
    AVDictionaryEntry *elems;
};

// tyycode
AVDictionaryEntry *av_dict_get1(const AVDictionary *m, const char *key,
                               const AVDictionaryEntry *prev, int flags)
{
    unsigned int i, j;

    if (!m)
        return NULL;

    if (prev)
        i = prev - m->elems + 1;
    else
        i = 0;

    for (; i < m->count; i++) {
        const char *s = m->elems[i].key;
        if (flags & AV_DICT_MATCH_CASE)
            for (j = 0; s[j] == key[j] && key[j]; j++)
                ;
        else
            for (j = 0; av_toupper(s[j]) == av_toupper(key[j]) && key[j]; j++)
                ;
        if (key[j])
            continue;
        if (s[j] && !(flags & AV_DICT_IGNORE_SUFFIX))
            continue;
        return &m->elems[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int i, ret;
    BenchmarkTimeStamps ti;

    init_dynload();

    register_exit(ffmpeg_cleanup);

    setvbuf(stderr,NULL,_IONBF,0); /* win32 runtime needs this */

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    if(argc>1 && !strcmp(argv[1], "-d")){
        run_as_daemon=1;
        av_log_set_callback(log_callback_null);
        argc--;
        argv++;
    }

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    show_banner(argc, argv, options);

//    struct TestSpecifierOpt {
//        const char* c;
//    };
//    struct TestOptionsContext {
//        struct TestSpecifierOpt *t;
//        int        nb_t;
//    };

//    struct TestOptionsContext o;
//    void *dst = o.t;// 获取结构体成员偏移地址错误写法，获取一个没问题，若通过dst再获取其它成员就是错误写法，
//    int *dstcount;
//    {
//        struct TestSpecifierOpt **so = dst;
//        dstcount = (int*)(so + 1);
//        *dstcount=1000;
//        printf("dstcount: %d, o.nv_t: %d\n", *dstcount, (int)o.nb_t);
//    }
    OptionsContext o;
    {
        memset(&o, 0, sizeof(o));

        o.stop_time = INT64_MAX;
        o.mux_max_delay  = 0.7;
        o.start_time     = AV_NOPTS_VALUE;
        o.start_time_eof = AV_NOPTS_VALUE;
        o.recording_time = INT64_MAX;
        o.limit_filesize = UINT64_MAX;
        o.chapters_input_file = INT_MAX;
        o.accurate_seek  = 1;
    }
    void *dst = (uint8_t *)&o + offsetof(OptionsContext, codec_names);// 正确写法
    printf("dst: %#X, o.codec_names: %#X, &o.codec_names: %#X",
           dst, o.codec_names, &o.codec_names);//验证得到的dst是该成员指针的地址还是该成员指针的值.
    // 结果：dst: 0X6CFA58, o.codec_names: 0, &o.codec_names: 0X6CFA58，说明通过偏移获取结构体成员，得到的是该成员指针的地址
    int *dstcount;
    {
        SpecifierOpt **so = dst;
        dstcount = (int*)(so + 1);// 指针加1等价于该地址加上sizeof该类型的字节数，因为指针加1时仅由本身类型决定
        // 所以上面的语句等价于：dstcount = (uint8_t *)dst + sizeof (SpecifierOpt *);
        // 具体看https://blog.csdn.net/qq_39505298/article/details/76248679

        //*so = grow_array(*so, sizeof (**so), dstcount, *dstcount+1);
        *dstcount=1000;
        printf("dstcount: %d, o.nb_codec_names: %d\n", *dstcount, o.nb_codec_names);
    }

    /* parse options and open all input/output files */
    ret = ffmpeg_parse_options(argc, argv);
    if (ret < 0)
        exit_program(1);

    if (nb_output_files <= 0 && nb_input_files == 0) {
        show_usage();
        av_log(NULL, AV_LOG_WARNING, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit_program(1);
    }

    /* file converter / grab */
    // 这里还是要判断nb_output_files <= 0，因为上面nb_output_files=-1时，不会退出程序
    if (nb_output_files <= 0) {
        av_log(NULL, AV_LOG_FATAL, "At least one output file must be specified\n");
        exit_program(1);
    }

    /*若输出格式不是rtp协议，那么want_sdp=0*/
    for (i = 0; i < nb_output_files; i++) {
        //例如flv与rtp比较
        if (strcmp(output_files[i]->ctx->oformat->name, "rtp"))
            want_sdp = 0;
    }

    current_time = ti = get_benchmark_time_stamps();
    if (transcode() < 0)
        exit_program(1);
    if (do_benchmark) {
        int64_t utime, stime, rtime;
        current_time = get_benchmark_time_stamps();
        utime = current_time.user_usec - ti.user_usec;
        stime = current_time.sys_usec  - ti.sys_usec;
        rtime = current_time.real_usec - ti.real_usec;
        av_log(NULL, AV_LOG_INFO,
               "bench: utime=%0.3fs stime=%0.3fs rtime=%0.3fs\n",
               utime / 1000000.0, stime / 1000000.0, rtime / 1000000.0);
    }
    av_log(NULL, AV_LOG_DEBUG, "%"PRIu64" frames successfully decoded, %"PRIu64" decoding errors\n",
           decode_error_stat[0], decode_error_stat[1]);
    if ((decode_error_stat[0] + decode_error_stat[1]) * max_error_rate < decode_error_stat[1])
        exit_program(69);

    // 调用ffmpeg_cleanup. 因为上面注册回调是使用ffmpeg_cleanup注册的.
    exit_program(received_nb_signals ? 255 : main_return_code);
    return main_return_code;
}
