/*
 * ffmpeg filter configuration
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

#include <stdint.h>

#include "ffmpeg.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include "libavresample/avresample.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "libavutil/samplefmt.h"

static const enum AVPixelFormat *get_compliance_unofficial_pix_fmts(enum AVCodecID codec_id, const enum AVPixelFormat default_formats[])
{
    static const enum AVPixelFormat mjpeg_formats[] =
        { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
          AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV444P,
          AV_PIX_FMT_NONE };
    static const enum AVPixelFormat ljpeg_formats[] =
        { AV_PIX_FMT_BGR24   , AV_PIX_FMT_BGRA    , AV_PIX_FMT_BGR0,
          AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ422P,
          AV_PIX_FMT_YUV420P , AV_PIX_FMT_YUV444P , AV_PIX_FMT_YUV422P,
          AV_PIX_FMT_NONE};

    if (codec_id == AV_CODEC_ID_MJPEG) {
        return mjpeg_formats;
    } else if (codec_id == AV_CODEC_ID_LJPEG) {
        return ljpeg_formats;
    } else {
        return default_formats;
    }
}

enum AVPixelFormat choose_pixel_fmt(AVStream *st, AVCodecContext *enc_ctx, AVCodec *codec, enum AVPixelFormat target)
{
    if (codec && codec->pix_fmts) {
        const enum AVPixelFormat *p = codec->pix_fmts;
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(target);
        //FIXME: This should check for AV_PIX_FMT_FLAG_ALPHA after PAL8 pixel format without alpha is implemented
        int has_alpha = desc ? desc->nb_components % 2 == 0 : 0;
        enum AVPixelFormat best= AV_PIX_FMT_NONE;

        if (enc_ctx->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL) {
            p = get_compliance_unofficial_pix_fmts(enc_ctx->codec_id, p);
        }
        for (; *p != AV_PIX_FMT_NONE; p++) {
            best= avcodec_find_best_pix_fmt_of_2(best, *p, target, has_alpha, NULL);
            if (*p == target)
                break;
        }
        if (*p == AV_PIX_FMT_NONE) {
            if (target != AV_PIX_FMT_NONE)
                av_log(NULL, AV_LOG_WARNING,
                       "Incompatible pixel format '%s' for codec '%s', auto-selecting format '%s'\n",
                       av_get_pix_fmt_name(target),
                       codec->name,
                       av_get_pix_fmt_name(best));
            return best;
        }
    }
    return target;
}

void choose_sample_fmt(AVStream *st, AVCodec *codec)
{
    if (codec && codec->sample_fmts) {
        const enum AVSampleFormat *p = codec->sample_fmts;
        for (; *p != -1; p++) {
            if (*p == st->codecpar->format)
                break;
        }
        if (*p == -1) {
            if((codec->capabilities & AV_CODEC_CAP_LOSSLESS) && av_get_sample_fmt_name(st->codecpar->format) > av_get_sample_fmt_name(codec->sample_fmts[0]))
                av_log(NULL, AV_LOG_ERROR, "Conversion will not be lossless.\n");
            if(av_get_sample_fmt_name(st->codecpar->format))
            av_log(NULL, AV_LOG_WARNING,
                   "Incompatible sample format '%s' for codec '%s', auto-selecting format '%s'\n",
                   av_get_sample_fmt_name(st->codecpar->format),
                   codec->name,
                   av_get_sample_fmt_name(codec->sample_fmts[0]));
            st->codecpar->format = codec->sample_fmts[0];
        }
    }
}

static char *choose_pix_fmts(OutputFilter *ofilter)
{
    OutputStream *ost = ofilter->ost;
    AVDictionaryEntry *strict_dict = av_dict_get(ost->encoder_opts, "strict", NULL, 0);
    if (strict_dict)
        // used by choose_pixel_fmt() and below
        av_opt_set(ost->enc_ctx, "strict", strict_dict->value, 0);

     if (ost->keep_pix_fmt) {
        avfilter_graph_set_auto_convert(ofilter->graph->graph,
                                            AVFILTER_AUTO_CONVERT_NONE);
        if (ost->enc_ctx->pix_fmt == AV_PIX_FMT_NONE)
            return NULL;
        return av_strdup(av_get_pix_fmt_name(ost->enc_ctx->pix_fmt));
    }
    if (ost->enc_ctx->pix_fmt != AV_PIX_FMT_NONE) {
        return av_strdup(av_get_pix_fmt_name(choose_pixel_fmt(ost->st, ost->enc_ctx, ost->enc, ost->enc_ctx->pix_fmt)));
    } else if (ost->enc && ost->enc->pix_fmts) {
        const enum AVPixelFormat *p;
        AVIOContext *s = NULL;
        uint8_t *ret;
        int len;

        if (avio_open_dyn_buf(&s) < 0)
            exit_program(1);

        p = ost->enc->pix_fmts;
        if (ost->enc_ctx->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL) {
            p = get_compliance_unofficial_pix_fmts(ost->enc_ctx->codec_id, p);
        }

        for (; *p != AV_PIX_FMT_NONE; p++) {
            const char *name = av_get_pix_fmt_name(*p);
            avio_printf(s, "%s|", name);
        }
        len = avio_close_dyn_buf(s, &ret);
        ret[len - 1] = 0;
        return ret;
    } else
        return NULL;
}

/* Define a function for building a string containing a list of
 * allowed formats. */
#define DEF_CHOOSE_FORMAT(suffix, type, var, supported_list, none, get_name)   \
static char *choose_ ## suffix (OutputFilter *ofilter)                         \
{                                                                              \
    if (ofilter->var != none) {                                                \
        get_name(ofilter->var);                                                \
        return av_strdup(name);                                                \
    } else if (ofilter->supported_list) {                                      \
        const type *p;                                                         \
        AVIOContext *s = NULL;                                                 \
        uint8_t *ret;                                                          \
        int len;                                                               \
                                                                               \
        if (avio_open_dyn_buf(&s) < 0)                                         \
            exit_program(1);                                                           \
                                                                               \
        for (p = ofilter->supported_list; *p != none; p++) {                   \
            get_name(*p);                                                      \
            avio_printf(s, "%s|", name);                                       \
        }                                                                      \
        len = avio_close_dyn_buf(s, &ret);                                     \
        ret[len - 1] = 0;                                                      \
        return ret;                                                            \
    } else                                                                     \
        return NULL;                                                           \
}

//DEF_CHOOSE_FORMAT(pix_fmts, enum AVPixelFormat, format, formats, AV_PIX_FMT_NONE,
//                  GET_PIX_FMT_NAME)

DEF_CHOOSE_FORMAT(sample_fmts, enum AVSampleFormat, format, formats,
                  AV_SAMPLE_FMT_NONE, GET_SAMPLE_FMT_NAME)

DEF_CHOOSE_FORMAT(sample_rates, int, sample_rate, sample_rates, 0,
                  GET_SAMPLE_RATE_NAME)

DEF_CHOOSE_FORMAT(channel_layouts, uint64_t, channel_layout, channel_layouts, 0,
                  GET_CH_LAYOUT_NAME)

/**
 * @brief 输出流会创建一个OutputFilter过滤器，输入流会创建一个InputFilter过滤器，
 *          它们最终保存在新创建的FilterGraph系统过滤器中。
 * @param ist 输入流
 * @param ost 与输入流对应的输出流
 * @return 成功=0 失败=程序退出
 *
 * @note 这个函数看到，虽然在FilterGraph的成员outputs、inputs都是二级指针，但都是只使用第一个元素，
 * 即下面看到fg->outputs[0]、fg->inputs[0]都是固定下标0，所以这个函数只被调用一次(for循环为每个输出流调用一次)，实际上ffmpeg
 * 也是这样调的，读者可以自行搜索，看到该函数只会被调用一次.
*/
int init_simple_filtergraph(InputStream *ist, OutputStream *ost)
{
    /*1.给封装好的系统过滤器FilterGraph开辟内存*/
    FilterGraph *fg = av_mallocz(sizeof(*fg));

    if (!fg)
        exit_program(1);
    fg->index = nb_filtergraphs;

    /*2.开辟一个OutputFilter *指针和开辟一个OutputFilter结构体，
     并让该指针指向该结构体，然后对该结构体进行相关赋值*/
    GROW_ARRAY(fg->outputs, fg->nb_outputs);//开辟OutputFilter *指针
    if (!(fg->outputs[0] = av_mallocz(sizeof(*fg->outputs[0]))))// 开辟一个OutputFilter结构体
        exit_program(1);
    fg->outputs[0]->ost   = ost;//保存输出流
    fg->outputs[0]->graph = fg;//保存该系统过滤器
    fg->outputs[0]->format = -1;

    ost->filter = fg->outputs[0];//同样ost中也会保存该OutputFilter.(建议画图容易理解)

    /*3.开辟一个InputFilter *指针和开辟一个InputFilter结构体，
     并让该指针指向该结构体，然后对该结构体进行相关赋值*/
    GROW_ARRAY(fg->inputs, fg->nb_inputs);
    if (!(fg->inputs[0] = av_mallocz(sizeof(*fg->inputs[0]))))
        exit_program(1);
    /*与输出过滤器赋值同理*/
    fg->inputs[0]->ist   = ist;
    fg->inputs[0]->graph = fg;
    fg->inputs[0]->format = -1;
    /*给输入过滤器中的帧队列开辟内存，注意开辟的是指针大小的内存*/
    fg->inputs[0]->frame_queue = av_fifo_alloc(8 * sizeof(AVFrame*));/*开辟内存，只不过是使用结构体AVFifoBuffer进行返回，
                                                                        av_fifo_alloc源码很简单，可以看看*/
    if (!fg->inputs[0]->frame_queue)
        exit_program(1);

    GROW_ARRAY(ist->filters, ist->nb_filters);// 给输入流的过滤器开辟一个指针
    ist->filters[ist->nb_filters - 1] = fg->inputs[0];// 给输入流的过滤器赋值，对比输出流OutputStream可以看到，输入流可以有多个输入过滤器

    /*4.保存FilterGraph fg，其中该fg保存了新开辟的输出过滤器OutputFilter 以及 新开辟的输入过滤器InputFilter*/
    GROW_ARRAY(filtergraphs, nb_filtergraphs);
    filtergraphs[nb_filtergraphs - 1] = fg;

    return 0;
}

static char *describe_filter_link(FilterGraph *fg, AVFilterInOut *inout, int in)
{
    AVFilterContext *ctx = inout->filter_ctx;
    AVFilterPad *pads = in ? ctx->input_pads  : ctx->output_pads;
    int       nb_pads = in ? ctx->nb_inputs   : ctx->nb_outputs;
    AVIOContext *pb;
    uint8_t *res = NULL;

    if (avio_open_dyn_buf(&pb) < 0)
        exit_program(1);

    avio_printf(pb, "%s", ctx->filter->name);
    if (nb_pads > 1)
        avio_printf(pb, ":%s", avfilter_pad_get_name(pads, inout->pad_idx));
    avio_w8(pb, 0);
    avio_close_dyn_buf(pb, &res);
    return res;
}

static void init_input_filter(FilterGraph *fg, AVFilterInOut *in)
{
    InputStream *ist = NULL;
    enum AVMediaType type = avfilter_pad_get_type(in->filter_ctx->input_pads, in->pad_idx);
    int i;

    // TODO: support other filter types
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_FATAL, "Only video and audio filters supported "
               "currently.\n");
        exit_program(1);
    }

    if (in->name) {
        AVFormatContext *s;
        AVStream       *st = NULL;
        char *p;
        int file_idx = strtol(in->name, &p, 0);

        if (file_idx < 0 || file_idx >= nb_input_files) {
            av_log(NULL, AV_LOG_FATAL, "Invalid file index %d in filtergraph description %s.\n",
                   file_idx, fg->graph_desc);
            exit_program(1);
        }
        s = input_files[file_idx]->ctx;

        for (i = 0; i < s->nb_streams; i++) {
            enum AVMediaType stream_type = s->streams[i]->codecpar->codec_type;
            if (stream_type != type &&
                !(stream_type == AVMEDIA_TYPE_SUBTITLE &&
                  type == AVMEDIA_TYPE_VIDEO /* sub2video hack */))
                continue;
            if (check_stream_specifier(s, s->streams[i], *p == ':' ? p + 1 : p) == 1) {
                st = s->streams[i];
                break;
            }
        }
        if (!st) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier '%s' in filtergraph description %s "
                   "matches no streams.\n", p, fg->graph_desc);
            exit_program(1);
        }
        ist = input_streams[input_files[file_idx]->ist_index + st->index];
        if (ist->user_set_discard == AVDISCARD_ALL) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier '%s' in filtergraph description %s "
                   "matches a disabled input stream.\n", p, fg->graph_desc);
            exit_program(1);
        }
    } else {
        /* find the first unused stream of corresponding type */
        for (i = 0; i < nb_input_streams; i++) {
            ist = input_streams[i];
            if (ist->user_set_discard == AVDISCARD_ALL)
                continue;
            if (ist->dec_ctx->codec_type == type && ist->discard)
                break;
        }
        if (i == nb_input_streams) {
            av_log(NULL, AV_LOG_FATAL, "Cannot find a matching stream for "
                   "unlabeled input pad %d on filter %s\n", in->pad_idx,
                   in->filter_ctx->name);
            exit_program(1);
        }
    }
    av_assert0(ist);

    ist->discard         = 0;
    ist->decoding_needed |= DECODING_FOR_FILTER;
    ist->st->discard = AVDISCARD_NONE;

    GROW_ARRAY(fg->inputs, fg->nb_inputs);
    if (!(fg->inputs[fg->nb_inputs - 1] = av_mallocz(sizeof(*fg->inputs[0]))))
        exit_program(1);
    fg->inputs[fg->nb_inputs - 1]->ist   = ist;
    fg->inputs[fg->nb_inputs - 1]->graph = fg;
    fg->inputs[fg->nb_inputs - 1]->format = -1;
    fg->inputs[fg->nb_inputs - 1]->type = ist->st->codecpar->codec_type;
    fg->inputs[fg->nb_inputs - 1]->name = describe_filter_link(fg, in, 1);

    fg->inputs[fg->nb_inputs - 1]->frame_queue = av_fifo_alloc(8 * sizeof(AVFrame*));
    if (!fg->inputs[fg->nb_inputs - 1]->frame_queue)
        exit_program(1);

    GROW_ARRAY(ist->filters, ist->nb_filters);
    ist->filters[ist->nb_filters - 1] = fg->inputs[fg->nb_inputs - 1];
}

int init_complex_filtergraph(FilterGraph *fg)
{
    AVFilterInOut *inputs, *outputs, *cur;
    AVFilterGraph *graph;
    int ret = 0;

    /* this graph is only used for determining the kinds of inputs
     * and outputs we have, and is discarded on exit from this function */
    graph = avfilter_graph_alloc();
    if (!graph)
        return AVERROR(ENOMEM);
    graph->nb_threads = 1;

    ret = avfilter_graph_parse2(graph, fg->graph_desc, &inputs, &outputs);
    if (ret < 0)
        goto fail;

    for (cur = inputs; cur; cur = cur->next)
        init_input_filter(fg, cur);

    for (cur = outputs; cur;) {
        GROW_ARRAY(fg->outputs, fg->nb_outputs);
        fg->outputs[fg->nb_outputs - 1] = av_mallocz(sizeof(*fg->outputs[0]));
        if (!fg->outputs[fg->nb_outputs - 1])
            exit_program(1);

        fg->outputs[fg->nb_outputs - 1]->graph   = fg;
        fg->outputs[fg->nb_outputs - 1]->out_tmp = cur;
        fg->outputs[fg->nb_outputs - 1]->type    = avfilter_pad_get_type(cur->filter_ctx->output_pads,
                                                                         cur->pad_idx);
        fg->outputs[fg->nb_outputs - 1]->name = describe_filter_link(fg, cur, 0);
        cur = cur->next;
        fg->outputs[fg->nb_outputs - 1]->out_tmp->next = NULL;
    }

fail:
    avfilter_inout_free(&inputs);
    avfilter_graph_free(&graph);
    return ret;
}

/**
 * @brief 插入trim filter.
 *
 * @param start_time trim的首帧开始时间.
 * @param duration  trim的最大输出时间.
 * @param last_filter AVFilterContext链表,指向链表尾部.
 * @param pad_idx 使用AVFilterContext的输入pad的下标,一般都是0.
 *          注,AVFilterContext可以有多个输入输出AVFilterPad,代表数据的流向,一般输入输出接口都是0.
 * @param filter_name trim filter的名字.
 *
 * @return 成功-0,注start_time,duration为无效值同样返回成功. 失败-负数
*/
static int insert_trim(int64_t start_time, int64_t duration,
                       AVFilterContext **last_filter, int *pad_idx,
                       const char *filter_name)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    AVFilterContext *ctx;
    const AVFilter *trim;
    // 1. 获取AVFilterContext下标为pad_idx输出口的媒体类型.
    // 注,AVFilterContext可以有多个输入输出口(即AVFilterPad).
    enum AVMediaType type = avfilter_pad_get_type((*last_filter)->output_pads, *pad_idx);
    const char *name = (type == AVMEDIA_TYPE_VIDEO) ? "trim" : "atrim";
    int ret = 0;

    //开始时间、时长两个值必须要有一个有效以上才会插入trim filter
    if (duration == INT64_MAX && start_time == AV_NOPTS_VALUE)
        return 0;

    // 2. 通过名字获取trim filter(AVFilter类型).类似"buffer","scale","buffersink"这些滤镜
    trim = avfilter_get_by_name(name);
    if (!trim) {
        av_log(NULL, AV_LOG_ERROR, "%s filter not present, cannot limit "
               "recording time.\n", name);
        return AVERROR_FILTER_NOT_FOUND;
    }

    // 3. 创建fliter_ctx，参1为graph,参2为avfilter_get_by_name返回的filter实例,参3为给这个实例自定义的名称，不然可能默认是null.
    //类似函数：ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
    //avfilter_graph_create_filter内部会调用avfilter_graph_alloc_filter,并且会进行初始化,看源码不难.
    ctx = avfilter_graph_alloc_filter(graph, trim, filter_name);
    if (!ctx)
        return AVERROR(ENOMEM);

    //设置输出最大时长, Maximum duration of the output(see libavfilter/trim.c)
    if (duration != INT64_MAX) {
        ret = av_opt_set_int(ctx, "durationi", duration,
                                AV_OPT_SEARCH_CHILDREN);
    }
    //设置首帧开始时间, Timestamp of the first frame that(see libavfilter/trim.c)
    if (ret >= 0 && start_time != AV_NOPTS_VALUE) {
        ret = av_opt_set_int(ctx, "starti", start_time,
                                AV_OPT_SEARCH_CHILDREN);
    }
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error configuring the %s filter", name);
        return ret;
    }

    // 初始化AVFilterContext. avfilter_graph_alloc_filter+avfilter_init_str 等价于 avfilter_graph_create_filter.
    ret = avfilter_init_str(ctx, NULL);//这里的字符串描述是NULL,因为也可以通过av_opt_set_int设置,
                                       //看了一下源码,传字符串描述最终也是通过av_opt_set设置的(大概看了一下).
    if (ret < 0)
        return ret;

    // 4. 连接AVFilterContext.
    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}

/**
 * @brief 新建AVFilterContext, 并进行filter之间进行连接.
 * 与ffplay的INSERT_FILT宏类似,不过INSERT_FILT宏是倒转过来连接的,
 * INSERT_FILT宏last_filter是向前连接,这里last_filter是向后连接.
 *
 * @param last_filter       上一个AVFilterContext.
 * @param pad_idx           输入AVFilterContext的 pad 下标
 * @param filter_name       AVFilterContext name.
 * @param args              新建AVFilterContext的字符串描述.
 *
 * @return 成功-0 失败负数
*/
static int insert_filter(AVFilterContext **last_filter, int *pad_idx,
                         const char *filter_name, const char *args)
{
    AVFilterGraph *graph = (*last_filter)->graph;
    AVFilterContext *ctx;
    int ret;

    // 1. 创建一个滤波器实例AVFilterContext，并添加到AVFilterGraph中
    ret = avfilter_graph_create_filter(&ctx,
                                       avfilter_get_by_name(filter_name),
                                       filter_name, args, NULL, graph);
    if (ret < 0)
        return ret;

    // 2. filter之间进行连接.将 last_filter 连接到 ctx.
    // srcpad,dstpad下标一般都是填0.
    ret = avfilter_link(*last_filter, *pad_idx, ctx, 0);
    if (ret < 0)
        return ret;

    // 3. 更新last_filter
    *last_filter = ctx;
    *pad_idx     = 0;
    return 0;
}

static int configure_output_video_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)
{
    char *pix_fmts;
    OutputStream *ost = ofilter->ost;
    OutputFile    *of = output_files[ost->file_index];
    AVFilterContext *last_filter = out->filter_ctx;
    int pad_idx = out->pad_idx;
    int ret;
    char name[255];

    snprintf(name, sizeof(name), "out_%d_%d", ost->file_index, ost->index);
    ret = avfilter_graph_create_filter(&ofilter->filter,
                                       avfilter_get_by_name("buffersink"),
                                       name, NULL, NULL, fg->graph);

    if (ret < 0)
        return ret;

    if (ofilter->width || ofilter->height) {
        char args[255];
        AVFilterContext *filter;
        AVDictionaryEntry *e = NULL;

        snprintf(args, sizeof(args), "%d:%d",
                 ofilter->width, ofilter->height);

        while ((e = av_dict_get(ost->sws_dict, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), ":%s=%s", e->key, e->value);
        }

        snprintf(name, sizeof(name), "scaler_out_%d_%d",
                 ost->file_index, ost->index);
        if ((ret = avfilter_graph_create_filter(&filter, avfilter_get_by_name("scale"),
                                                name, args, NULL, fg->graph)) < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx = 0;
    }

    if ((pix_fmts = choose_pix_fmts(ofilter))) {
        AVFilterContext *filter;
        snprintf(name, sizeof(name), "format_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&filter,
                                           avfilter_get_by_name("format"),
                                           "format", pix_fmts, NULL, fg->graph);
        av_freep(&pix_fmts);
        if (ret < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, pad_idx, filter, 0)) < 0)
            return ret;

        last_filter = filter;
        pad_idx     = 0;
    }

    if (ost->frame_rate.num && 0) {
        AVFilterContext *fps;
        char args[255];

        snprintf(args, sizeof(args), "fps=%d/%d", ost->frame_rate.num,
                 ost->frame_rate.den);
        snprintf(name, sizeof(name), "fps_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&fps, avfilter_get_by_name("fps"),
                                           name, args, NULL, fg->graph);
        if (ret < 0)
            return ret;

        ret = avfilter_link(last_filter, pad_idx, fps, 0);
        if (ret < 0)
            return ret;
        last_filter = fps;
        pad_idx = 0;
    }

    snprintf(name, sizeof(name), "trim_out_%d_%d",
             ost->file_index, ost->index);
    ret = insert_trim(of->start_time, of->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;


    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        return ret;

    return 0;
}

static int configure_output_audio_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)
{
    OutputStream *ost = ofilter->ost;
    OutputFile    *of = output_files[ost->file_index];
    AVCodecContext *codec  = ost->enc_ctx;
    AVFilterContext *last_filter = out->filter_ctx;
    int pad_idx = out->pad_idx;
    char *sample_fmts, *sample_rates, *channel_layouts;
    char name[255];
    int ret;

    snprintf(name, sizeof(name), "out_%d_%d", ost->file_index, ost->index);
    ret = avfilter_graph_create_filter(&ofilter->filter,
                                       avfilter_get_by_name("abuffersink"),
                                       name, NULL, NULL, fg->graph);
    if (ret < 0)
        return ret;
    if ((ret = av_opt_set_int(ofilter->filter, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        return ret;

#define AUTO_INSERT_FILTER(opt_name, filter_name, arg) do {                 \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    av_log(NULL, AV_LOG_INFO, opt_name " is forwarded to lavfi "            \
           "similarly to -af " filter_name "=%s.\n", arg);                  \
                                                                            \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(filter_name),   \
                                       filter_name, arg, NULL, fg->graph);  \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    ret = avfilter_link(last_filter, pad_idx, filt_ctx, 0);                 \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    last_filter = filt_ctx;                                                 \
    pad_idx = 0;                                                            \
} while (0)
    if (ost->audio_channels_mapped) {
        int i;
        AVBPrint pan_buf;
        av_bprint_init(&pan_buf, 256, 8192);
        av_bprintf(&pan_buf, "0x%"PRIx64,
                   av_get_default_channel_layout(ost->audio_channels_mapped));
        for (i = 0; i < ost->audio_channels_mapped; i++)
            if (ost->audio_channels_map[i] != -1)
                av_bprintf(&pan_buf, "|c%d=c%d", i, ost->audio_channels_map[i]);

        AUTO_INSERT_FILTER("-map_channel", "pan", pan_buf.str);
        av_bprint_finalize(&pan_buf, NULL);
    }

    if (codec->channels && !codec->channel_layout)
        codec->channel_layout = av_get_default_channel_layout(codec->channels);

    sample_fmts     = choose_sample_fmts(ofilter);
    sample_rates    = choose_sample_rates(ofilter);
    channel_layouts = choose_channel_layouts(ofilter);
    if (sample_fmts || sample_rates || channel_layouts) {
        AVFilterContext *format;
        char args[256];
        args[0] = 0;

        if (sample_fmts)
            av_strlcatf(args, sizeof(args), "sample_fmts=%s:",
                            sample_fmts);
        if (sample_rates)
            av_strlcatf(args, sizeof(args), "sample_rates=%s:",
                            sample_rates);
        if (channel_layouts)
            av_strlcatf(args, sizeof(args), "channel_layouts=%s:",
                            channel_layouts);

        av_freep(&sample_fmts);
        av_freep(&sample_rates);
        av_freep(&channel_layouts);

        snprintf(name, sizeof(name), "format_out_%d_%d",
                 ost->file_index, ost->index);
        ret = avfilter_graph_create_filter(&format,
                                           avfilter_get_by_name("aformat"),
                                           name, args, NULL, fg->graph);
        if (ret < 0)
            return ret;

        ret = avfilter_link(last_filter, pad_idx, format, 0);
        if (ret < 0)
            return ret;

        last_filter = format;
        pad_idx = 0;
    }

    if (audio_volume != 256 && 0) {
        char args[256];

        snprintf(args, sizeof(args), "%f", audio_volume / 256.);
        AUTO_INSERT_FILTER("-vol", "volume", args);
    }

    if (ost->apad && of->shortest) {
        char args[256];
        int i;

        for (i=0; i<of->ctx->nb_streams; i++)
            if (of->ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                break;

        if (i<of->ctx->nb_streams) {
            snprintf(args, sizeof(args), "%s", ost->apad);
            AUTO_INSERT_FILTER("-apad", "apad", args);
        }
    }

    snprintf(name, sizeof(name), "trim for output stream %d:%d",
             ost->file_index, ost->index);
    ret = insert_trim(of->start_time, of->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    if ((ret = avfilter_link(last_filter, pad_idx, ofilter->filter, 0)) < 0)
        return ret;

    return 0;
}

int configure_output_filter(FilterGraph *fg, OutputFilter *ofilter, AVFilterInOut *out)
{
    if (!ofilter->ost) {
        av_log(NULL, AV_LOG_FATAL, "Filter %s has an unconnected output\n", ofilter->name);
        exit_program(1);
    }

    switch (avfilter_pad_get_type(out->filter_ctx->output_pads, out->pad_idx)) {
    case AVMEDIA_TYPE_VIDEO: return configure_output_video_filter(fg, ofilter, out);
    case AVMEDIA_TYPE_AUDIO: return configure_output_audio_filter(fg, ofilter, out);
    default: av_assert0(0);
    }
}

/**
 * @brief 检查系统过滤器数组中，输出过滤器保存的输出流是否为空.
 *      可看init_simple_filtergraph函数对FilterGraph **filtergraphs的初始化。
 * @return 成功=void； 失败=程序退出
*/
void check_filter_outputs(void)
{
    int i;
    //1. 遍历系统过滤器数组
    for (i = 0; i < nb_filtergraphs; i++) {
        int n;
        // 2. 遍历系统过滤器数组中的输出过滤器数组
        for (n = 0; n < filtergraphs[i]->nb_outputs; n++) {
            OutputFilter *output = filtergraphs[i]->outputs[n];
            if (!output->ost) {
                av_log(NULL, AV_LOG_FATAL, "Filter %s has an unconnected output\n", output->name);
                exit_program(1);
            }
        }
    }
}

static int sub2video_prepare(InputStream *ist, InputFilter *ifilter)
{
    AVFormatContext *avf = input_files[ist->file_index]->ctx;
    int i, w, h;

    /* Compute the size of the canvas for the subtitles stream.
       If the subtitles codecpar has set a size, use it. Otherwise use the
       maximum dimensions of the video streams in the same file. */
    w = ifilter->width;
    h = ifilter->height;
    if (!(w && h)) {
        for (i = 0; i < avf->nb_streams; i++) {
            if (avf->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                w = FFMAX(w, avf->streams[i]->codecpar->width);
                h = FFMAX(h, avf->streams[i]->codecpar->height);
            }
        }
        if (!(w && h)) {
            w = FFMAX(w, 720);
            h = FFMAX(h, 576);
        }
        av_log(avf, AV_LOG_INFO, "sub2video: using %dx%d canvas\n", w, h);
    }
    ist->sub2video.w = ifilter->width  = w;
    ist->sub2video.h = ifilter->height = h;

    ifilter->width  = ist->dec_ctx->width  ? ist->dec_ctx->width  : ist->sub2video.w;
    ifilter->height = ist->dec_ctx->height ? ist->dec_ctx->height : ist->sub2video.h;

    /* rectangles are AV_PIX_FMT_PAL8, but we have no guarantee that the
       palettes for all rectangles are identical or compatible */
    ifilter->format = AV_PIX_FMT_RGB32;

    ist->sub2video.frame = av_frame_alloc();
    if (!ist->sub2video.frame)
        return AVERROR(ENOMEM);
    ist->sub2video.last_pts = INT64_MIN;
    ist->sub2video.end_pts  = INT64_MIN;
    return 0;
}

/**
 * @brief 配置输入视频过滤器.该函数执行完,filter链表大概是这样的:
 * buffer->insert_filter函数的滤镜(transpose,hflip,vflip,rotate)->yadif->trim->in->filter_ctx.
 * @param fg fg
 * @param ifilter InputFilter
 * @param in AVFilterInOut. 例如是avfilter_graph_parse2解析过滤器字符串时的传出参数inputs.
 *
 * @return 成功-0 失败-负数
*/
static int configure_input_video_filter(FilterGraph *fg, InputFilter *ifilter,
                                        AVFilterInOut *in)
{
    AVFilterContext *last_filter;
    // 1. 获取输入源filter(AVFilter类型)--->buffer
    const AVFilter *buffer_filt = avfilter_get_by_name("buffer");
    InputStream *ist = ifilter->ist;
    InputFile     *f = input_files[ist->file_index];
    AVRational tb = ist->framerate.num ? av_inv_q(ist->framerate) :
                                         ist->st->time_base;//av_inv_q是分子分母调转然后返回.帧率存在,使用帧率倒数作为时基,否则使用ist->st->time_base
    AVRational fr = ist->framerate;
    AVRational sar;
    AVBPrint args;//一个string buffer,当成C++的string理解
    char name[255];
    int ret, pad_idx = 0;
    int64_t tsoffset = 0;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);
    memset(par, 0, sizeof(*par));
    par->format = AV_PIX_FMT_NONE;

    // 2. 如果输入流是音频,返回.因为本函数是配置视频的filter
    if (ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_ERROR, "Cannot connect video filter to audio input\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (!fr.num)//若ist->framerate为空,会通过容器和编解码器猜出对应的帧率
        fr = av_guess_frame_rate(input_files[ist->file_index]->ctx, ist->st, NULL);

    // 3. 字幕相关,暂不研究,推流命令没用到.
    if (ist->dec_ctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        ret = sub2video_prepare(ist, ifilter);
        if (ret < 0)
            goto fail;
    }

    sar = ifilter->sample_aspect_ratio;// 宽高比率,一般都是{0,1}
    if(!sar.den)
        sar = (AVRational){0,1};

    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);
    //例如执行av_bprintf后:args.str="video_size=848x480:pix_fmt=0:time_base=1/1000:pixel_aspect=0/1:sws_param=flags=2"
    av_bprintf(&args,
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
             "pixel_aspect=%d/%d:sws_param=flags=%d",
             ifilter->width, ifilter->height, ifilter->format,
             tb.num, tb.den, sar.num, sar.den,
             SWS_BILINEAR + ((ist->dec_ctx->flags&AV_CODEC_FLAG_BITEXACT) ? SWS_BITEXACT:0));
    //追加帧率,例如: args.str="video_size=848x480:pix_fmt=0:time_base=1/1000:pixel_aspect=0/1:sws_param=flags=2:frame_rate=25/1"
    if (fr.num && fr.den)
        av_bprintf(&args, ":frame_rate=%d/%d", fr.num, fr.den);

    // 4 创建一个滤波器实例AVFilterContext，并添加到AVFilterGraph中.(这个是输入buffer filter)
    snprintf(name, sizeof(name), "graph %d input from stream %d:%d", fg->index,
             ist->file_index, ist->st->index);//要给创建的filter实例的实例名,可任意,不要与其它重复即可.
    if ((ret = avfilter_graph_create_filter(&ifilter->filter, buffer_filt, name,
                                            args.str, NULL, fg->graph)) < 0)
        goto fail;

    // 5. 将par参数传递给AVFilterContext.
    // 具体是传递给成员priv,priv是一个BufferSourceContext类型成员.
    /* av_buffersrc_parameters_set()(源码很简单):
     * 使用提供的参数初始化 buffersrc 或 abuffersrc filter。这个函数可以被调用多次，之后的调用会覆盖之前的调用。
     * 一些参数也可以通过AVOptions设置，然后最后使用的方法优先.
     * 参1: an instance of the buffersrc or abuffersrc filter.
     * 参2: 流参数。随后传递给这个过滤器的帧必须符合这些参数。所有在param中分配的字段仍然属于调用者，libavfilter将在必要时进行内部复制或引用.
     * return 0 on success, a negative AVERROR code on failure. */
    par->hw_frames_ctx = ifilter->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(ifilter->filter, par);
    if (ret < 0)
        goto fail;
    av_freep(&par);//av_buffersrc_parameters_alloc开辟的内存仍属于调用者,设置完后需要释放.
    last_filter = ifilter->filter;//保存该AVFilterContext

    // 6. 判断是否有旋转角度，如果有需要使用对应的过滤器进行处理；没有则不会添加.
    /*
     * 一 图片的相关旋转操作命令：
     * 1）垂直翻转：                      ffmpeg -i fan.jpg -vf vflip -y vflip.png
     * 2）水平翻转：                      ffmpeg -i fan.jpg -vf hflip -y hflip.png
     * 3）顺时针旋转60°(PI代表180°)：      ffmpeg -i fan.jpg -vf rotate=PI/3 -y rotate60.png
     * 4）顺时针旋转90°：                 ffmpeg -i fan.jpg -vf rotate=90*PI/180 -y rotate90.png
     * 5）逆时针旋转90°(负号代表逆时针，正号代表顺时针)：ffmpeg -i fan.jpg -vf rotate=-90*PI/180 -y rotate90-.png
     * 6）逆时针旋转90°：                  ffmpeg -i fan.jpg -vf transpose=2 -y transpose2.png
     * rotate、transpose的值具体使用ffmpeg -h filter=filtername去查看。
     * 注意1：上面的图片使用ffprobe去看不会有metadata元数据，所以自然不会有rotate与Side data里面的displaymatrix。只有视频才有。
     * 注意2：使用是rotate带有黑底的，例如上面的rotate60.png。图片的很好理解，都是以原图进行正常的旋转，没有难度。
     *
     *
     * 二 视频文件相关旋转的操作：
     * 1.1 使用rotate选项：
     * 1） ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='90' -codec copy 2_audio_rotate90.mp4
     * 但是这个命令实际效果是：画面变成逆时针的90°操作。使用ffprobe一看：
     *     Metadata:
              rotate          : 270
              handler_name    : VideoHandler
           Side data:
              displaymatrix: rotation of 90.00 degrees
           Stream #0:1(und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 184 kb/s (default)
            Metadata:
              handler_name    : 粤语
    * 可以看到rotate是270°，但displaymatrix确实是转了90°。
    *
    * 2）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='270' -codec copy 2_audio_rotate270.mp4
    * 同样rotate='270'时，画面变成顺时针90°的操作。rotate=90，displaymatrix=rotation of -90.00 degrees。
    *
    * 3）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='180' -codec copy 2_audio_rotate180.mp4
    * 而180的画面是倒转的，这个可以理解。rotate=180，displaymatrix=rotation of -180.00 degrees。
    *
    * 2.1 使用transpose选项
    * 1）ffmpeg -i 2_audio.mp4  -vf transpose=1 -codec copy 2_audio_transpose90.mp4(顺时针90°)
    * 2）ffmpeg -i 2_audio.mp4  -vf transpose=2 2_audio_transpose-90.mp4(逆时针90°，不能加-codec copy，否则与transpose冲突)
    * 上面命令按预期正常顺时针的旋转了90°和逆时针旋转90°的画面，但是使用ffprobe看不到rotate或者displaymatrix对应的角度。
    *
    * 3.1 使用rotate+transpose选项
    * 1） ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='90' -vcodec libx264 2_audio_rotate90.mp4
    * 2）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='180' -vcodec libx264 2_audio_rotate180.mp4
    * 3）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='270' -vcodec libx264 2_audio_rotate270.mp4
    * 只要使用了transpose选项，rotate就会失效。例如运行上面三个命令，实际只顺时针旋转了90°，即transpose=1的效果，并且，只要存在transpose，它和2.1一样，
    *   使用ffprobe看不到rotate或者displaymatrix对应的角度，这种情况是我们不愿意看到的。所以经过分析，我们最终还是得回到只有rotate选项的情况。
    *
    * 目前我们先记着1.1的三种情况的结果就行，后续有空再深入研究旋转，并且实时流一般都会返回theta=0，不会有旋转的操作。
    */
    if (ist->autorotate) {//与ffplay处理旋转角度是一样的.
        double theta = get_rotation(ist->st);

        if (fabs(theta - 90) < 1.0) {
            // 转换过滤器，clock代表，顺时针旋转，等价于命令transpose=1。
            // 可用ffmpeg -h filter=transpose查看。查看所有filter：ffmpeg -filters
            ret = insert_filter(&last_filter, &pad_idx, "transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "hflip", NULL);// 镜像左右反转过滤器
            if (ret < 0)
                return ret;
            ret = insert_filter(&last_filter, &pad_idx, "vflip", NULL);// 镜像上下反转过滤器，经过这个过滤器处理后，画面会反转，类似水中倒影。
        } else if (fabs(theta - 270) < 1.0) {
            ret = insert_filter(&last_filter, &pad_idx, "transpose", "cclock");// 逆时针旋转，等价于命令transpose=2.
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            ret = insert_filter(&last_filter, &pad_idx, "rotate", rotate_buf);// 旋转角度过滤器
        }
        if (ret < 0)
            return ret;
    }

    // 7. 若指定-deinterlace选项,创建yadif过滤器并添加到graph中.
    if (do_deinterlace) {// -deinterlace选项
        AVFilterContext *yadif;

        snprintf(name, sizeof(name), "deinterlace_in_%d_%d",
                 ist->file_index, ist->st->index);
        if ((ret = avfilter_graph_create_filter(&yadif,
                                                avfilter_get_by_name("yadif"),
                                                name, "", NULL,
                                                fg->graph)) < 0)
            return ret;

        if ((ret = avfilter_link(last_filter, 0, yadif, 0)) < 0)
            return ret;

        last_filter = yadif;
    }

    // 8. 指定了录像相关的选项,会插入trim filter.
    snprintf(name, sizeof(name), "trim_in_%d_%d",
             ist->file_index, ist->st->index);
    if (copy_ts) {
        //若用户指定start_time(-ss选项),则获取,否则为0.
        tsoffset = f->start_time == AV_NOPTS_VALUE ? 0 : f->start_time;
        //若用户指定-copyts选项,但没指定-start_at_zero选项,且f->ctx->start_time有效,
        //加上f->ctx->start_time的值保存在tsoffset.
        if (!start_at_zero && f->ctx->start_time != AV_NOPTS_VALUE)
            tsoffset += f->ctx->start_time;
    }
    // 没传-ss选项 或者 传了-ss但是没启用精确查找-accurate_seek选项,insert_trim的开始时间为AV_NOPTS_VALUE,否则为tsoffset.
    // 也就说只有传了-ss以及-accurate_seek选项才会指定trim的start_time.等价于语句:
    // f->start_time != AV_NOPTS_VALUE && f->accurate_seek ? tsoffset : AV_NOPTS_VALUE
    ret = insert_trim(((f->start_time == AV_NOPTS_VALUE) || !f->accurate_seek) ?
                      AV_NOPTS_VALUE : tsoffset, f->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    // 9. 连接.执行到这里,最终指向输入参数的in->filter_ctx.
    // in参数是由avfilter_graph_parse2解析过滤器字符串时的传出参数inputs.
    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)
        return ret;
    return 0;
fail:
    av_freep(&par);

    return ret;
}

/**
 * @brief 配置输入音频过滤器. 该函数执行完,filter链表大概是这样的:
 * abuffer->aresample->volume->trim->in->filter_ctx.
 *
 * @param fg        fg
 * @param ifilter   InputFilter
 * @param in        AVFilterInOut. 例如是avfilter_graph_parse2解析过滤器字符串时的传出参数inputs.
 *
 * @return 成功-0 失败-负数
*/
static int configure_input_audio_filter(FilterGraph *fg, InputFilter *ifilter,
                                        AVFilterInOut *in)
{
    AVFilterContext *last_filter;
    // 1. 获取输入源filter(AVFilter类型)--->abuffer.(注意视频的是buffer,这个是固定的)
    const AVFilter *abuffer_filt = avfilter_get_by_name("abuffer");
    InputStream *ist = ifilter->ist;
    InputFile     *f = input_files[ist->file_index];
    AVBPrint args;
    char name[255];
    int ret, pad_idx = 0;
    int64_t tsoffset = 0;

    //该输入过滤器所属的输入流的解码器不是音频,直接返回错误.
    if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_ERROR, "Cannot connect audio filter to non audio input\n");
        return AVERROR(EINVAL);
    }

    av_bprint_init(&args, 0, AV_BPRINT_SIZE_AUTOMATIC);//初始化字符串buffer
    av_bprintf(&args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s",
             1, ifilter->sample_rate,
             ifilter->sample_rate,
             av_get_sample_fmt_name(ifilter->format));//添加字符串描述到args.str
    //存在通道布局则在args.str末尾追加,否则追加通道数的字符串描述
    if (ifilter->channel_layout)
        av_bprintf(&args, ":channel_layout=0x%"PRIx64,
                   ifilter->channel_layout);
    else
        av_bprintf(&args, ":channels=%d", ifilter->channels);

    // 2. 创建一个滤波器实例AVFilterContext，并添加到AVFilterGraph中.(这个是输入abuffer filter)
    snprintf(name, sizeof(name), "graph_%d_in_%d_%d", fg->index,
             ist->file_index, ist->st->index);
    if ((ret = avfilter_graph_create_filter(&ifilter->filter, abuffer_filt,
                                            name, args.str, NULL,
                                            fg->graph)) < 0)
        return ret;
    last_filter = ifilter->filter;

    //该宏与insert_filter()函数功能是一样的.
#define AUTO_INSERT_FILTER_INPUT(opt_name, filter_name, arg) do {                 \
    AVFilterContext *filt_ctx;                                              \
                                                                            \
    av_log(NULL, AV_LOG_INFO, opt_name " is forwarded to lavfi "            \
           "similarly to -af " filter_name "=%s.\n", arg);                  \
                                                                            \
    snprintf(name, sizeof(name), "graph_%d_%s_in_%d_%d",      \
                fg->index, filter_name, ist->file_index, ist->st->index);   \
    ret = avfilter_graph_create_filter(&filt_ctx,                           \
                                       avfilter_get_by_name(filter_name),   \
                                       name, arg, NULL, fg->graph);         \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    ret = avfilter_link(last_filter, 0, filt_ctx, 0);                       \
    if (ret < 0)                                                            \
        return ret;                                                         \
                                                                            \
    last_filter = filt_ctx;                                                 \
} while (0)

    // 3. 若设置-async选项,添加aresample滤镜.
    if (audio_sync_method > 0) {
        char args[256] = {0};

        //async选项:简化1个参数音频时间戳匹配,0(禁用),1(填充和微调)，>1(每秒最大采样拉伸/压缩).该选项会被设置到SwrContext.async.
        av_strlcatf(args, sizeof(args), "async=%d", audio_sync_method);

        //若设置-adrift_threshold选项,则改变SwrContext.min_hard_compensation的值.
        if (audio_drift_threshold != 0.1)
            /* min_hard_comp: 设置时间戳和音频数据之间的最小差异(以秒为单位)，以触发填充/修剪数据.
             * 该选项会被设置到SwrContext.min_hard_compensation.
             * min_hard_compensation的解释: SWR最小值，低于此值将不会发生无声注入/样品下降. */
            av_strlcatf(args, sizeof(args), ":min_hard_comp=%f", audio_drift_threshold);

        //first_pts: 假设第一个pts应该是这个值(在样本中). 该选项会被设置到SwrContext.firstpts_in_samples.
        if (!fg->reconfiguration)
            av_strlcatf(args, sizeof(args), ":first_pts=0");

        AUTO_INSERT_FILTER_INPUT("-async", "aresample", args);
    }

//     if (ost->audio_channels_mapped) {
//         int i;
//         AVBPrint pan_buf;
//         av_bprint_init(&pan_buf, 256, 8192);
//         av_bprintf(&pan_buf, "0x%"PRIx64,
//                    av_get_default_channel_layout(ost->audio_channels_mapped));
//         for (i = 0; i < ost->audio_channels_mapped; i++)
//             if (ost->audio_channels_map[i] != -1)
//                 av_bprintf(&pan_buf, ":c%d=c%d", i, ost->audio_channels_map[i]);
//         AUTO_INSERT_FILTER_INPUT("-map_channel", "pan", pan_buf.str);
//         av_bprint_finalize(&pan_buf, NULL);
//     }

    // 4. 若指定-vol选项改变音量,添加volume滤镜.
    if (audio_volume != 256) {
        char args[256];

        av_log(NULL, AV_LOG_WARNING, "-vol has been deprecated. Use the volume "
               "audio filter instead.\n");

        //我们看到设置音量时是没有key的,只有一个value,有兴趣的可以看看最终设置到哪个变量.
        snprintf(args, sizeof(args), "%f", audio_volume / 256.);
        AUTO_INSERT_FILTER_INPUT("-vol", "volume", args);
    }

    // 5. 指定了录像相关的选项,会插入trim filter.这部分代码与视频是一样的.
    snprintf(name, sizeof(name), "trim for input stream %d:%d",
             ist->file_index, ist->st->index);
    if (copy_ts) {
        tsoffset = f->start_time == AV_NOPTS_VALUE ? 0 : f->start_time;
        if (!start_at_zero && f->ctx->start_time != AV_NOPTS_VALUE)
            tsoffset += f->ctx->start_time;
    }
    ret = insert_trim(((f->start_time == AV_NOPTS_VALUE) || !f->accurate_seek) ?
                      AV_NOPTS_VALUE : tsoffset, f->recording_time,
                      &last_filter, &pad_idx, name);
    if (ret < 0)
        return ret;

    // 6. 连接.执行到这里,最终指向输入参数的in->filter_ctx.
    // in参数是由avfilter_graph_parse2解析过滤器字符串时的传出参数inputs.
    if ((ret = avfilter_link(last_filter, 0, in->filter_ctx, in->pad_idx)) < 0)
        return ret;

    return 0;
}

static int configure_input_filter(FilterGraph *fg, InputFilter *ifilter,
                                  AVFilterInOut *in)
{
    // 1. 输入流的解码器没找到,返回错误.
    if (!ifilter->ist->dec) {
        av_log(NULL, AV_LOG_ERROR,
               "No decoder for stream #%d:%d, filtering impossible\n",
               ifilter->ist->file_index, ifilter->ist->st->index);
        return AVERROR_DECODER_NOT_FOUND;
    }

    // 2. 根据不同的媒体类型配置输入filter.
    switch (avfilter_pad_get_type(in->filter_ctx->input_pads, in->pad_idx)) {
    case AVMEDIA_TYPE_VIDEO: return configure_input_video_filter(fg, ifilter, in);
    case AVMEDIA_TYPE_AUDIO: return configure_input_audio_filter(fg, ifilter, in);
    default: av_assert0(0);
    }
}

static void cleanup_filtergraph(FilterGraph *fg)
{
    int i;
    for (i = 0; i < fg->nb_outputs; i++)
        fg->outputs[i]->filter = (AVFilterContext *)NULL;//思考一下fg->outputs[i]->filter是在哪里开辟的.
    for (i = 0; i < fg->nb_inputs; i++)
        fg->inputs[i]->filter = (AVFilterContext *)NULL;
    avfilter_graph_free(&fg->graph);
}

int configure_filtergraph(FilterGraph *fg)
{
    AVFilterInOut *inputs, *outputs, *cur;
    int ret, i, simple = filtergraph_is_simple(fg);//推流命令为simple=1
    const char *graph_desc = simple ? fg->outputs[0]->ost->avfilter :
                                      fg->graph_desc;

    // 1. 先清理上一次的FilterGraph,然后再开辟AVFilterGraph.
    cleanup_filtergraph(fg);
    if (!(fg->graph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    // 2. 如果是简单过滤器,需要组成对应的字符串描述,否则是复杂则不需要,因为本身就是字符串描述.
    // fg->graph_desc过滤器字符串描述为空表示简单过滤器,不为空表示复杂过滤器.
    if (simple) {
        OutputStream *ost = fg->outputs[0]->ost;
        char args[512];
        AVDictionaryEntry *e = NULL;

        fg->graph->nb_threads = filter_nbthreads;// -filter_threads选项,默认0,非复杂过滤器线程数.

        // 2.1 将用户传进的sws_dict字典参数拼接成字符串描述,格式为"key1=value1:key2=value2"
        args[0] = 0;
        while ((e = av_dict_get(ost->sws_dict, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args)-1] = 0;//末尾添加哨兵字符0,同时可以去掉末尾的":"

        // 2.2 应用该描述
        fg->graph->scale_sws_opts = av_strdup(args);

        // 下面是类似的
        args[0] = 0;
        while ((e = av_dict_get(ost->swr_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args)-1] = 0;

        av_opt_set(fg->graph, "aresample_swr_opts", args, 0);

        //resample_opts最终应用到哪?args会保存上面swr_opts/sws_dict这些字符串吗?留个疑问
        args[0] = '\0';
        while ((e = av_dict_get(fg->outputs[0]->ost->resample_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX))) {
            av_strlcatf(args, sizeof(args), "%s=%s:", e->key, e->value);
        }
        if (strlen(args))
            args[strlen(args) - 1] = '\0';

        //我们看libavfilter/avfiltergraph.c, 这个threads最终同样是被设置到上面的fg->graph->nb_threads.
        e = av_dict_get(ost->encoder_opts, "threads", NULL, 0);
        if (e)
            av_opt_set(fg->graph, "threads", e->value, 0);
    } else {
        fg->graph->nb_threads = filter_complex_nbthreads;//复杂过滤器的线程数,同样是设置到fg->graph->nb_threads
    }
    //这里我们知道,简单和复杂过滤器字符串描述的区别是:
    //简单是用户输入key=val的形式,然后ffmpeg再将这些组成字符串描述; 而复杂则是用户直接传字符串描述.

    // 3. 解析过滤器字符串
    // avfilter_graph_parse2的作用：1）解析字符串；2）并且将滤波图的集合放在inputs、outputs中。
    /* avfilter_graph_parse2(): 将字符串描述的图形添加到图形中。
     * 参1: 将解析图上下文链接到其中的过滤器图
     * 参2: 要解析的字符串
     * 参3:[out] 输入一个包含解析图的所有空闲(未链接)输入的链接列表将在这里返回。调用者将使用avfilter_inout_free()释放它。
     * 参4:[out] 输出一个链表，包含解析图的所有空闲(未链接)输出。调用者将使用avfilter_inout_free()释放它。
     * return: zero on success, a negative AVERROR code on error.
     *
     * 注意: 这个函数返回在解析图之后未链接的输入和输出，然后调用者处理它们.
     * 注意: 这个函数不引用graph中已经存在的部分，输入参数在返回时将包含图中新解析部分的输入。类似地，outputs参数将包含新创建的filters的输出。
    */
    if ((ret = avfilter_graph_parse2(fg->graph, graph_desc, &inputs, &outputs)) < 0)
        goto fail;

    // 4. 硬件相关,暂不分析
    // -filter_hw_device选项和是否有hw_device_ctx
    if (filter_hw_device || hw_device_ctx) {
        AVBufferRef *device = filter_hw_device ? filter_hw_device->device_ref
                                               : hw_device_ctx;
        for (i = 0; i < fg->graph->nb_filters; i++) {
            fg->graph->filters[i]->hw_device_ctx = av_buffer_ref(device);
            if (!fg->graph->filters[i]->hw_device_ctx) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
        }
    }

    // 5. 简单过滤器只能有1 input and 1 output.当不符合时,程序报错.
    // !inputs代表0个输入, inputs->next代表大于1个输入,输出同理.
    if (simple && (!inputs || inputs->next || !outputs || outputs->next)) {
        const char *num_inputs;
        const char *num_outputs;
        // 判断输入和输出过滤器的个数
        if (!outputs) {
            num_outputs = "0";
        } else if (outputs->next) {
            num_outputs = ">1";
        } else {
            num_outputs = "1";
        }
        if (!inputs) {
            num_inputs = "0";
        } else if (inputs->next) {
            num_inputs = ">1";
        } else {
            num_inputs = "1";
        }
        //简单过滤器只能有1 input and 1 output.
        av_log(NULL, AV_LOG_ERROR, "Simple filtergraph '%s' was expected "
               "to have exactly 1 input and 1 output."
               " However, it had %s input(s) and %s output(s)."
               " Please adjust, or use a complex filtergraph (-filter_complex) instead.\n",
               graph_desc, num_inputs, num_outputs);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    // 6. 此时AVFilterContext的链表会被保存在inputs、outputs,那么就开始配置它们.
    // 我们看AVFilterInOut的定义以及avfilter_graph_parse2()源码,ffmpeg大概是这样处理的(笔者没详细看,不一定完全准确):
    // 每一个AVFilterInOut保存一个AVFilterContext, 链表由AVFilterInOut *next链接.
    // 6.1 配置输入过滤器
    for (cur = inputs, i = 0; cur; cur = cur->next, i++)
        if ((ret = configure_input_filter(fg, fg->inputs[i], cur)) < 0) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            goto fail;
        }
    avfilter_inout_free(&inputs);

    // 6.2 配置输出过滤器
    for (cur = outputs, i = 0; cur; cur = cur->next, i++)
        configure_output_filter(fg, fg->outputs[i], cur);
    avfilter_inout_free(&outputs);

    if ((ret = avfilter_graph_config(fg->graph, NULL)) < 0)
        goto fail;

    /* limit the lists of allowed formats to the ones selected, to
     * make sure they stay the same if the filtergraph is reconfigured later */
    for (i = 0; i < fg->nb_outputs; i++) {
        OutputFilter *ofilter = fg->outputs[i];
        AVFilterContext *sink = ofilter->filter;

        ofilter->format = av_buffersink_get_format(sink);

        ofilter->width  = av_buffersink_get_w(sink);
        ofilter->height = av_buffersink_get_h(sink);

        ofilter->sample_rate    = av_buffersink_get_sample_rate(sink);
        ofilter->channel_layout = av_buffersink_get_channel_layout(sink);
    }

    fg->reconfiguration = 1;

    for (i = 0; i < fg->nb_outputs; i++) {
        OutputStream *ost = fg->outputs[i]->ost;
        if (!ost->enc) {
            /* identical to the same check in ffmpeg.c, needed because
               complex filter graphs are initialized earlier */
            av_log(NULL, AV_LOG_ERROR, "Encoder (codec %s) not found for output stream #%d:%d\n",
                     avcodec_get_name(ost->st->codecpar->codec_id), ost->file_index, ost->index);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(ost->filter->filter,
                                         ost->enc_ctx->frame_size);
    }

    for (i = 0; i < fg->nb_inputs; i++) {
        while (av_fifo_size(fg->inputs[i]->frame_queue)) {
            AVFrame *tmp;
            av_fifo_generic_read(fg->inputs[i]->frame_queue, &tmp, sizeof(tmp), NULL);
            ret = av_buffersrc_add_frame(fg->inputs[i]->filter, tmp);
            av_frame_free(&tmp);
            if (ret < 0)
                goto fail;
        }
    }

    /* send the EOFs for the finished inputs */
    for (i = 0; i < fg->nb_inputs; i++) {
        if (fg->inputs[i]->eof) {
            ret = av_buffersrc_add_frame(fg->inputs[i]->filter, NULL);
            if (ret < 0)
                goto fail;
        }
    }

    /* process queued up subtitle packets */
    for (i = 0; i < fg->nb_inputs; i++) {
        InputStream *ist = fg->inputs[i]->ist;
        if (ist->sub2video.sub_queue && ist->sub2video.frame) {
            while (av_fifo_size(ist->sub2video.sub_queue)) {
                AVSubtitle tmp;
                av_fifo_generic_read(ist->sub2video.sub_queue, &tmp, sizeof(tmp), NULL);
                sub2video_update(ist, &tmp);
                avsubtitle_free(&tmp);
            }
        }
    }

    return 0;

fail:
    cleanup_filtergraph(fg);
    return ret;
}

/**
 * @brief 从解码后的一帧获取相关参数保存到InputFilter,如果涉及到硬件,还会给其添加相关引用.
 * @param ifilter 封装的输入过滤器.
 * @param frame 解码后的一帧.
 * @return 成功-0 失败-负数.
*/
int ifilter_parameters_from_frame(InputFilter *ifilter, const AVFrame *frame)
{
    // 1. 若ifilter->hw_frames_ctx引用不为空,则先释放.
    /* av_buffer_unref(): 释放一个给定的引用，如果没有更多的引用，则自动释放缓冲区。
    @param buf被释放的引用。返回时将指针设置为NULL */
    av_buffer_unref(&ifilter->hw_frames_ctx);

    // 2. 从解码帧拷贝相关参数到InputFilter中.
    ifilter->format = frame->format;

    ifilter->width               = frame->width;
    ifilter->height              = frame->height;
    ifilter->sample_aspect_ratio = frame->sample_aspect_ratio;

    ifilter->sample_rate         = frame->sample_rate;
    ifilter->channels            = frame->channels;
    ifilter->channel_layout      = frame->channel_layout;

    // 3. 给ifilter->hw_frames_ctx添加引用.(硬件相关)
    if (frame->hw_frames_ctx) {
        ifilter->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
        if (!ifilter->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }

    return 0;
}

int ist_in_filtergraph(FilterGraph *fg, InputStream *ist)
{
    int i;
    for (i = 0; i < fg->nb_inputs; i++)
        if (fg->inputs[i]->ist == ist)
            return 1;
    return 0;
}

/**
 * @brief 判断FilterGraph的graph_desc描述是否为空.
 * @param fg ffmpeg封装的系统过滤器
 * @return =1 graph_desc为空；=0 graph_desc不为空
*/
int filtergraph_is_simple(FilterGraph *fg)
{
    return !fg->graph_desc;
}
