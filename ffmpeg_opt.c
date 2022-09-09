
/*
 * ffmpeg option parsing
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
#include "cmdutils.h"

#include "libavformat/avformat.h"

#include "libavcodec/avcodec.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/fifo.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

#define DEFAULT_PASS_LOGFILENAME_PREFIX "ffmpeg2pass"

/**
 * @brief 这里看到，MATCH_PER_STREAM_OPT的作用是，遍历用户所有的编解码器名字所属的音视频类型是否都与该流st的st->codecpar->codec_type一样，
 *              如果存在一个不一样，那么程序直接退出；否则符合，当用户输入多个编解码器名字时，只会取最后一个编解码器名字作为返回.
 * 例如用户传入：-vcodec libx265 -vcodec libx264，此时假设st是视频流，虽然h265,h264都是属于视频流类型的编解码器，但是只会返回用户最后一个
 * 编解码器名字，即outvar="libx264"作为传出参数
 */
#define MATCH_PER_STREAM_OPT(name, type, outvar, fmtctx, st)\
{\
    int i, ret;\
    for (i = 0; i < o->nb_ ## name; i++) {\
        char *spec = o->name[i].specifier;\
        if ((ret = check_stream_specifier(fmtctx, st, spec)) > 0)\
            outvar = o->name[i].u.type;\
        else if (ret < 0)\
            exit_program(1);\
    }\
}

#define MATCH_PER_TYPE_OPT(name, type, outvar, fmtctx, mediatype)\
{\
    int i;\
    for (i = 0; i < o->nb_ ## name; i++) {\
        char *spec = o->name[i].specifier;\
        if (!strcmp(spec, mediatype))\
            outvar = o->name[i].u.type;\
    }\
}

const HWAccel hwaccels[] = {
#if CONFIG_VIDEOTOOLBOX
    { "videotoolbox", videotoolbox_init, HWACCEL_VIDEOTOOLBOX, AV_PIX_FMT_VIDEOTOOLBOX },
#endif
#if CONFIG_LIBMFX
    { "qsv",   qsv_init,   HWACCEL_QSV,   AV_PIX_FMT_QSV },
#endif
#if CONFIG_CUVID
    { "cuvid", cuvid_init, HWACCEL_CUVID, AV_PIX_FMT_CUDA },
#endif
    { 0 },
};
AVBufferRef *hw_device_ctx;
HWDevice *filter_hw_device;

char *vstats_filename;
char *sdp_filename;

float audio_drift_threshold = 0.1;
float dts_delta_threshold   = 10;
float dts_error_threshold   = 3600*30;  //帧能被解码的最大阈值大小？

int audio_volume      = 256;            // -vol选项,默认256
int audio_sync_method = 0;              // 音频同步方法.默认0
int video_sync_method = VSYNC_AUTO;
float frame_drop_threshold = 0;
int do_deinterlace    = 0;              // -deinterlace选项,默认0.
int do_benchmark      = 0;
int do_benchmark_all  = 0;              // -benchmark_all选项，默认0
int do_hex_dump       = 0;
int do_pkt_dump       = 0;
int copy_ts           = 0;
int start_at_zero     = 0;
int copy_tb           = -1;
int debug_ts          = 0;              // 是否打印相关时间戳.-debug_ts选项
int exit_on_error     = 0;              // xerror选项
int abort_on_flags    = 0;
int print_stats       = -1;
int qp_hist           = 0;
int stdin_interaction = 1;              // 可认为是否是交互模式.除pipe、以及/dev/stdin值为0，其它例如普通文件、实时流都是1
int frame_bits_per_raw_sample = 0;
float max_error_rate  = 2.0/3;
int filter_nbthreads = 0;               // -filter_threads选项,默认0,非复杂过滤器线程数.
int filter_complex_nbthreads = 0;       // -filter_complex_threads选项,默认0,复杂过滤器线程数.
int vstats_version = 2;


static int intra_only         = 0;
static int file_overwrite     = 0;      // -y选项，重写输出文件，即覆盖该输出文件。0=不重写，1=重写，
                                        // 不过为0时且no_file_overwrite=0时会在终端询问用户是否重写
static int no_file_overwrite  = 0;      // -n选项，不重写输出文件。0=不重写，1=重写
static int do_psnr            = 0;
static int input_sync;
static int input_stream_potentially_available = 0;// 标记，=1代表该输入文件可能是可用的
static int ignore_unknown_streams = 0;
static int copy_unknown_streams = 0;
static int find_stream_info = 1;

void tyy_print_AVDirnary(AVDictionary *d){
    if(!d){
        return;
    }

    AVDictionaryEntry *t = NULL;
    while((t = av_dict_get(d, "", t, AV_DICT_IGNORE_SUFFIX))){
        printf("tyy_print_AVDirnary, t->key: %s, t->value: %s\n", t->key, t->value);
    }
}

static void uninit_options(OptionsContext *o)
{
    const OptionDef *po = options;
    int i;

    /* all OPT_SPEC and OPT_STRING can be freed in generic way */
    while (po->name) {
        void *dst = (uint8_t*)o + po->u.off;

        // 1. 保存在SpecifierOpt结构的，需要被释放
        if (po->flags & OPT_SPEC) {
            SpecifierOpt **so = dst;
            int i, *count = (int*)(so + 1);
            for (i = 0; i < *count; i++) {
                av_freep(&(*so)[i].specifier);
                if (po->flags & OPT_STRING)
                    av_freep(&(*so)[i].u.str);
            }
            av_freep(so);
            *count = 0;
        } else if (po->flags & OPT_OFFSET && po->flags & OPT_STRING)
            // 2. 具有OPT_OFFSET标志且是字符串，同样需要被释放
            av_freep(dst);
        po++;
    }

    for (i = 0; i < o->nb_stream_maps; i++)
        av_freep(&o->stream_maps[i].linklabel);
    av_freep(&o->stream_maps);
    av_freep(&o->audio_channel_maps);
    av_freep(&o->streamid_map);
    av_freep(&o->attachments);
}

static void init_options(OptionsContext *o)
{
    memset(o, 0, sizeof(*o));

    o->stop_time = INT64_MAX;
    o->mux_max_delay  = 0.7;
    o->start_time     = AV_NOPTS_VALUE;
    o->start_time_eof = AV_NOPTS_VALUE;
    o->recording_time = INT64_MAX;
    o->limit_filesize = UINT64_MAX;
    o->chapters_input_file = INT_MAX;
    o->accurate_seek  = 1;
}

static int show_hwaccels(void *optctx, const char *opt, const char *arg)
{
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    int i;

    printf("Hardware acceleration methods:\n");
    while ((type = av_hwdevice_iterate_types(type)) !=
           AV_HWDEVICE_TYPE_NONE)
        printf("%s\n", av_hwdevice_get_type_name(type));
    for (i = 0; hwaccels[i].name; i++)
        printf("%s\n", hwaccels[i].name);
    printf("\n");
    return 0;
}

/**
 * @brief 将传进来的字典内的键值对，去掉流说明符后，设置到新的字典进行返回，传进来的字典内容保持不变。
 * @param dict 原字典
 * @return 返回一个新的没有流说明符的字典
*/
/* return a copy of the input with the stream specifiers removed from the keys-返回输入的副本，并从键中删除流说明符 */
static AVDictionary *strip_specifiers(AVDictionary *dict)
{
    AVDictionaryEntry *e = NULL;
    AVDictionary    *ret = NULL;

    while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        char *p = strchr(e->key, ':');

        /*将流说明符去掉.例如e->key="b:v"，p是指向":v"的，那么*p=0后，e->key="b"*/
        if (p)
            *p = 0;

        /*所以这里设置到ret的，必定是去掉流说明符"v"的(冒号是分隔符当然也会去掉)，例如e->key="b"，e->value="128K"*/
        av_dict_set(&ret, e->key, e->value, 0);

        /*上面设置完毕后，将该p指向的地址恢复为分隔符":"，此时会恢复原来的值：e->key="b:v"*/
        if (p)
            *p = ':';
    }
    return ret;
}

static int opt_abort_on(void *optctx, const char *opt, const char *arg)
{
    static const AVOption opts[] = {
        { "abort_on"        , NULL, 0, AV_OPT_TYPE_FLAGS, { .i64 = 0 }, INT64_MIN, INT64_MAX, .unit = "flags" },
        { "empty_output"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = ABORT_ON_FLAG_EMPTY_OUTPUT     },    .unit = "flags" },
        { NULL },
    };
    static const AVClass class = {
        .class_name = "",
        .item_name  = av_default_item_name,
        .option     = opts,
        .version    = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *pclass = &class;

    return av_opt_eval_flags(&pclass, &opts[0], arg, &abort_on_flags);
}

static int opt_sameq(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_ERROR, "Option '%s' was removed. "
           "If you are looking for an option to preserve the quality (which is not "
           "what -%s was for), use -qscale 0 or an equivalent quality factor option.\n",
           opt, opt);
    return AVERROR(EINVAL);
}

static int opt_video_channel(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "This option is deprecated, use -channel.\n");
    return opt_default(optctx, "channel", arg);
}

static int opt_video_standard(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "This option is deprecated, use -standard.\n");
    return opt_default(optctx, "standard", arg);
}

static int opt_audio_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:a", arg, options);
}

static int opt_video_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:v", arg, options);
}

static int opt_subtitle_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:s", arg, options);
}

static int opt_data_codec(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "codec:d", arg, options);
}

static int opt_map(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    StreamMap *m = NULL;
    int i, negative = 0, file_idx, disabled = 0;
    int sync_file_idx = -1, sync_stream_idx = 0;
    char *p, *sync;
    char *map;
    char *allow_unused;

    if (*arg == '-') {
        negative = 1;
        arg++;
    }
    map = av_strdup(arg);
    if (!map)
        return AVERROR(ENOMEM);

    /* parse sync stream first, just pick first matching stream */
    if (sync = strchr(map, ',')) {
        *sync = 0;
        sync_file_idx = strtol(sync + 1, &sync, 0);
        if (sync_file_idx >= nb_input_files || sync_file_idx < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid sync file index: %d.\n", sync_file_idx);
            exit_program(1);
        }
        if (*sync)
            sync++;
        for (i = 0; i < input_files[sync_file_idx]->nb_streams; i++)
            if (check_stream_specifier(input_files[sync_file_idx]->ctx,
                                       input_files[sync_file_idx]->ctx->streams[i], sync) == 1) {
                sync_stream_idx = i;
                break;
            }
        if (i == input_files[sync_file_idx]->nb_streams) {
            av_log(NULL, AV_LOG_FATAL, "Sync stream specification in map %s does not "
                                       "match any streams.\n", arg);
            exit_program(1);
        }
        if (input_streams[input_files[sync_file_idx]->ist_index + sync_stream_idx]->user_set_discard == AVDISCARD_ALL) {
            av_log(NULL, AV_LOG_FATAL, "Sync stream specification in map %s matches a disabled input "
                                       "stream.\n", arg);
            exit_program(1);
        }
    }


    if (map[0] == '[') {
        /* this mapping refers to lavfi output */
        const char *c = map + 1;
        GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
        m = &o->stream_maps[o->nb_stream_maps - 1];
        m->linklabel = av_get_token(&c, "]");
        if (!m->linklabel) {
            av_log(NULL, AV_LOG_ERROR, "Invalid output link label: %s.\n", map);
            exit_program(1);
        }
    } else {
        if (allow_unused = strchr(map, '?'))
            *allow_unused = 0;
        file_idx = strtol(map, &p, 0);
        if (file_idx >= nb_input_files || file_idx < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index: %d.\n", file_idx);
            exit_program(1);
        }
        if (negative)
            /* disable some already defined maps */
            for (i = 0; i < o->nb_stream_maps; i++) {
                m = &o->stream_maps[i];
                if (file_idx == m->file_index &&
                    check_stream_specifier(input_files[m->file_index]->ctx,
                                           input_files[m->file_index]->ctx->streams[m->stream_index],
                                           *p == ':' ? p + 1 : p) > 0)
                    m->disabled = 1;
            }
        else
            for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
                if (check_stream_specifier(input_files[file_idx]->ctx, input_files[file_idx]->ctx->streams[i],
                            *p == ':' ? p + 1 : p) <= 0)
                    continue;
                if (input_streams[input_files[file_idx]->ist_index + i]->user_set_discard == AVDISCARD_ALL) {
                    disabled = 1;
                    continue;
                }
                GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
                m = &o->stream_maps[o->nb_stream_maps - 1];

                m->file_index   = file_idx;
                m->stream_index = i;

                if (sync_file_idx >= 0) {
                    m->sync_file_index   = sync_file_idx;
                    m->sync_stream_index = sync_stream_idx;
                } else {
                    m->sync_file_index   = file_idx;
                    m->sync_stream_index = i;
                }
            }
    }

    if (!m) {
        if (allow_unused) {
            av_log(NULL, AV_LOG_VERBOSE, "Stream map '%s' matches no streams; ignoring.\n", arg);
        } else if (disabled) {
            av_log(NULL, AV_LOG_FATAL, "Stream map '%s' matches disabled streams.\n"
                                       "To ignore this, add a trailing '?' to the map.\n", arg);
            exit_program(1);
        } else {
            av_log(NULL, AV_LOG_FATAL, "Stream map '%s' matches no streams.\n"
                                       "To ignore this, add a trailing '?' to the map.\n", arg);
            exit_program(1);
        }
    }

    av_freep(&map);
    return 0;
}

static int opt_attach(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    GROW_ARRAY(o->attachments, o->nb_attachments);
    o->attachments[o->nb_attachments - 1] = arg;
    return 0;
}

static int opt_map_channel(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    int n;
    AVStream *st;
    AudioChannelMap *m;
    char *allow_unused;
    char *mapchan;
    mapchan = av_strdup(arg);
    if (!mapchan)
        return AVERROR(ENOMEM);

    GROW_ARRAY(o->audio_channel_maps, o->nb_audio_channel_maps);
    m = &o->audio_channel_maps[o->nb_audio_channel_maps - 1];

    /* muted channel syntax */
    n = sscanf(arg, "%d:%d.%d", &m->channel_idx, &m->ofile_idx, &m->ostream_idx);
    if ((n == 1 || n == 3) && m->channel_idx == -1) {
        m->file_idx = m->stream_idx = -1;
        if (n == 1)
            m->ofile_idx = m->ostream_idx = -1;
        av_free(mapchan);
        return 0;
    }

    /* normal syntax */
    n = sscanf(arg, "%d.%d.%d:%d.%d",
               &m->file_idx,  &m->stream_idx, &m->channel_idx,
               &m->ofile_idx, &m->ostream_idx);

    if (n != 3 && n != 5) {
        av_log(NULL, AV_LOG_FATAL, "Syntax error, mapchan usage: "
               "[file.stream.channel|-1][:syncfile:syncstream]\n");
        exit_program(1);
    }

    if (n != 5) // only file.stream.channel specified
        m->ofile_idx = m->ostream_idx = -1;

    /* check input */
    if (m->file_idx < 0 || m->file_idx >= nb_input_files) {
        av_log(NULL, AV_LOG_FATAL, "mapchan: invalid input file index: %d\n",
               m->file_idx);
        exit_program(1);
    }
    if (m->stream_idx < 0 ||
        m->stream_idx >= input_files[m->file_idx]->nb_streams) {
        av_log(NULL, AV_LOG_FATAL, "mapchan: invalid input file stream index #%d.%d\n",
               m->file_idx, m->stream_idx);
        exit_program(1);
    }
    st = input_files[m->file_idx]->ctx->streams[m->stream_idx];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_FATAL, "mapchan: stream #%d.%d is not an audio stream.\n",
               m->file_idx, m->stream_idx);
        exit_program(1);
    }
    /* allow trailing ? to map_channel */
    if (allow_unused = strchr(mapchan, '?'))
        *allow_unused = 0;
    if (m->channel_idx < 0 || m->channel_idx >= st->codecpar->channels ||
        input_streams[input_files[m->file_idx]->ist_index + m->stream_idx]->user_set_discard == AVDISCARD_ALL) {
        if (allow_unused) {
            av_log(NULL, AV_LOG_VERBOSE, "mapchan: invalid audio channel #%d.%d.%d\n",
                    m->file_idx, m->stream_idx, m->channel_idx);
        } else {
            av_log(NULL, AV_LOG_FATAL,  "mapchan: invalid audio channel #%d.%d.%d\n"
                    "To ignore this, add a trailing '?' to the map_channel.\n",
                    m->file_idx, m->stream_idx, m->channel_idx);
            exit_program(1);
        }

    }
    av_free(mapchan);
    return 0;
}

static int opt_sdp_file(void *optctx, const char *opt, const char *arg)
{
    av_free(sdp_filename);
    sdp_filename = av_strdup(arg);
    return 0;
}

#if CONFIG_VAAPI
static int opt_vaapi_device(void *optctx, const char *opt, const char *arg)
{
    HWDevice *dev;
    const char *prefix = "vaapi:";
    char *tmp;
    int err;
    tmp = av_asprintf("%s%s", prefix, arg);
    if (!tmp)
        return AVERROR(ENOMEM);
    err = hw_device_init_from_string(tmp, &dev);
    av_free(tmp);
    if (err < 0)
        return err;
    hw_device_ctx = av_buffer_ref(dev->device_ref);
    if (!hw_device_ctx)
        return AVERROR(ENOMEM);
    return 0;
}
#endif

static int opt_init_hw_device(void *optctx, const char *opt, const char *arg)
{
    if (!strcmp(arg, "list")) {
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        printf("Supported hardware device types:\n");
        while ((type = av_hwdevice_iterate_types(type)) !=
               AV_HWDEVICE_TYPE_NONE)
            printf("%s\n", av_hwdevice_get_type_name(type));
        printf("\n");
        exit_program(0);
    } else {
        return hw_device_init_from_string(arg, NULL);
    }
}

static int opt_filter_hw_device(void *optctx, const char *opt, const char *arg)
{
    if (filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Only one filter device can be used.\n");
        return AVERROR(EINVAL);
    }
    filter_hw_device = hw_device_get_by_name(arg);
    if (!filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Invalid filter device %s.\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

/**
 * Parse a metadata specifier passed as 'arg' parameter.
 * @param arg  metadata string to parse
 * @param type metadata type is written here -- g(lobal)/s(tream)/c(hapter)/p(rogram)
 * @param index for type c/p, chapter/program index is written here
 * @param stream_spec for type s, the stream specifier is written here
 */
static void parse_meta_type(char *arg, char *type, int *index, const char **stream_spec)
{
    if (*arg) {
        *type = *arg;
        switch (*arg) {
        case 'g':
            break;
        case 's':
            if (*(++arg) && *arg != ':') {
                av_log(NULL, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", arg);
                exit_program(1);
            }
            *stream_spec = *arg == ':' ? arg + 1 : "";
            break;
        case 'c':
        case 'p':
            if (*(++arg) == ':')
                *index = strtol(++arg, NULL, 0);
            break;
        default:
            av_log(NULL, AV_LOG_FATAL, "Invalid metadata type %c.\n", *arg);
            exit_program(1);
        }
    } else
        *type = 'g';
}

static int copy_metadata(char *outspec, char *inspec, AVFormatContext *oc, AVFormatContext *ic, OptionsContext *o)
{
    AVDictionary **meta_in = NULL;
    AVDictionary **meta_out = NULL;
    int i, ret = 0;
    char type_in, type_out;
    const char *istream_spec = NULL, *ostream_spec = NULL;
    int idx_in = 0, idx_out = 0;

    parse_meta_type(inspec,  &type_in,  &idx_in,  &istream_spec);
    parse_meta_type(outspec, &type_out, &idx_out, &ostream_spec);

    if (!ic) {
        if (type_out == 'g' || !*outspec)
            o->metadata_global_manual = 1;
        if (type_out == 's' || !*outspec)
            o->metadata_streams_manual = 1;
        if (type_out == 'c' || !*outspec)
            o->metadata_chapters_manual = 1;
        return 0;
    }

    if (type_in == 'g' || type_out == 'g')
        o->metadata_global_manual = 1;
    if (type_in == 's' || type_out == 's')
        o->metadata_streams_manual = 1;
    if (type_in == 'c' || type_out == 'c')
        o->metadata_chapters_manual = 1;

    /* ic is NULL when just disabling automatic mappings */
    if (!ic)
        return 0;

#define METADATA_CHECK_INDEX(index, nb_elems, desc)\
    if ((index) < 0 || (index) >= (nb_elems)) {\
        av_log(NULL, AV_LOG_FATAL, "Invalid %s index %d while processing metadata maps.\n",\
                (desc), (index));\
        exit_program(1);\
    }

#define SET_DICT(type, meta, context, index)\
        switch (type) {\
        case 'g':\
            meta = &context->metadata;\
            break;\
        case 'c':\
            METADATA_CHECK_INDEX(index, context->nb_chapters, "chapter")\
            meta = &context->chapters[index]->metadata;\
            break;\
        case 'p':\
            METADATA_CHECK_INDEX(index, context->nb_programs, "program")\
            meta = &context->programs[index]->metadata;\
            break;\
        case 's':\
            break; /* handled separately below */ \
        default: av_assert0(0);\
        }\

    SET_DICT(type_in, meta_in, ic, idx_in);
    SET_DICT(type_out, meta_out, oc, idx_out);

    /* for input streams choose first matching stream */
    if (type_in == 's') {
        for (i = 0; i < ic->nb_streams; i++) {
            if ((ret = check_stream_specifier(ic, ic->streams[i], istream_spec)) > 0) {
                meta_in = &ic->streams[i]->metadata;
                break;
            } else if (ret < 0)
                exit_program(1);
        }
        if (!meta_in) {
            av_log(NULL, AV_LOG_FATAL, "Stream specifier %s does not match  any streams.\n", istream_spec);
            exit_program(1);
        }
    }

    if (type_out == 's') {
        for (i = 0; i < oc->nb_streams; i++) {
            if ((ret = check_stream_specifier(oc, oc->streams[i], ostream_spec)) > 0) {
                meta_out = &oc->streams[i]->metadata;
                av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);
            } else if (ret < 0)
                exit_program(1);
        }
    } else
        av_dict_copy(meta_out, *meta_in, AV_DICT_DONT_OVERWRITE);

    return 0;
}

static int opt_recording_timestamp(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char buf[128];
    int64_t recording_timestamp = parse_time_or_die(opt, arg, 0) / 1E6;
    struct tm time = *gmtime((time_t*)&recording_timestamp);
    if (!strftime(buf, sizeof(buf), "creation_time=%Y-%m-%dT%H:%M:%S%z", &time))
        return -1;
    parse_option(o, "metadata", buf, options);

    av_log(NULL, AV_LOG_WARNING, "%s is deprecated, set the 'creation_time' metadata "
                                 "tag instead.\n", opt);
    return 0;
}

/**
 * @brief
 * @param name 要寻找的编解码器名字
 * @param type 媒体类型
 * @param encoder 要进行编码还是解码.0 解码， 1 编码
 * @return 成功 返回找到的编解码器; 失败 程序退出
*/
static AVCodec *find_codec_or_die(const char *name, enum AVMediaType type, int encoder)
{
    const AVCodecDescriptor *desc;
    const char *codec_string = encoder ? "encoder" : "decoder";
    AVCodec *codec;

    // 1. 根据名字寻找编解码器
    codec = encoder ?
        avcodec_find_encoder_by_name(name) :
        avcodec_find_decoder_by_name(name);

    // 2. 如果根据名字找不到，则使用名字先找编解码器的描述符，再使用描述符寻找编解码器.
    if (!codec && (desc = avcodec_descriptor_get_by_name(name))) {// 根据名字获取编解码器的描述符
        codec = encoder ? avcodec_find_encoder(desc->id) :
                          avcodec_find_decoder(desc->id);
        if (codec)
            av_log(NULL, AV_LOG_VERBOSE, "Matched %s '%s' for codec '%s'.\n",
                   codec_string, codec->name, desc->name);
    }

    if (!codec) {
        av_log(NULL, AV_LOG_FATAL, "Unknown %s '%s'\n", codec_string, name);
        exit_program(1);
    }

    // 3. 对比.根据用户传进的名字找到的编解码器的媒体类型，若与用户传进的不一致，退出程序.
    // 例如用户传进:-codec:a libx264，那么MATCH_PER_TYPE_OPT匹配时，audio_codec_name="libx264"
    // 虽然找到编解码器，但音频是没有libx264的，这个找到编解码器是视频的，所以退出.
    // codec->type是通过用户输入的名字找到的编解码器名字的类型
    if (codec->type != type) {
        av_log(NULL, AV_LOG_FATAL, "Invalid %s type '%s'\n", codec_string, name);
        exit_program(1);
    }
    return codec;
}

// 例如将choose_decoder的MATCH_PER_STREAM_OPT展开来，用于debug
/**
 * @brief 这里看到，MATCH_PER_STREAM_OPT的作用是，遍历用户所有的编解码器名字所属的音视频类型是否都与该流st的st->codecpar->codec_type一样，
 *              如果存在一个不一样，那么程序直接退出；否则当用户输入多个编解码器名字时，只会取最后一个编解码器名字作为返回.
 * 例如用户传入：-vcodec libx265 -vcodec libx264，此时假设st是视频流，虽然h265,h264都是属于视频流类型的编解码器，但是只会返回用户最后一个
 * 编解码器名字，即outvar="libx264"作为传出参数
 */
void MATCH_PER_STREAM_OPT_TYYCODE(OptionsContext *o,const char* name, const char *type,
                                  char *outvar, AVFormatContext* fmtctx, AVStream *st){
    int i, ret;
    for (i = 0; i < o->nb_codec_names; i++) {
        char *spec = o->codec_names[i].specifier;
        if ((ret = check_stream_specifier(fmtctx, st, spec)) > 0)
            outvar = o->codec_names[i].u.str;
        else if (ret < 0)
            exit_program(1);
    }
}

/**
 * @brief 选择解码器以及对应的解码器id.两种方案：
 * 1.用户传进解码器，那么返回用户选择的解码器，并且st保存了用户选择的解码器的codec_id；
 * 2.用户无传进解码器，则以流中的codec_id作为选择解码器返回, codec_id无需改变.
 * 当用户传了 解码器选项时，若音视频类型与流的音视频类型不一致 或者 解码器不被ffmpeg支持，程序会退出.
 *
 * 实际上该函数不关心返回值的话，就是应用强制解码器id(apply forced codec ids)
*/
static AVCodec *choose_decoder(OptionsContext *o, AVFormatContext *s, AVStream *st)
{
    char *codec_name = NULL;

    MATCH_PER_STREAM_OPT(codec_names, str, codec_name, s, st);// 判断音视频类型是否与流一致.例如-vcodec libx264,这里是判断'v'是否与st的codec_type一致
    if (codec_name) {
        AVCodec *codec = find_codec_or_die(codec_name, st->codecpar->codec_type, 0);// 判断解码器是否被ffmpeg支持.例如-vcodec libx264,这里是判断 libx264是否被ffmpeg支持
        st->codecpar->codec_id = codec->id;
        return codec;
    } else
        return avcodec_find_decoder(st->codecpar->codec_id);
}

/* Add all the streams from the given input file to the global
 * list of input streams. */
// 该函数实际上是对输入流中解码器相关参数的处理，然后封装到InputStream结构体中
static void  add_input_streams(OptionsContext *o, AVFormatContext *ic)
{
    int i, ret;

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        AVCodecParameters *par = st->codecpar;
        InputStream *ist = av_mallocz(sizeof(*ist));
        char *framerate = NULL, *hwaccel_device = NULL;
        const char *hwaccel = NULL;
        char *hwaccel_output_format = NULL;
        char *codec_tag = NULL;
        char *next;
        char *discard_str = NULL;
        const AVClass *cc = avcodec_get_class();// 获取AVCodecContext的AVClass
        const AVOption *discard_opt = av_opt_find(&cc, "skip_frame", NULL, 0, 0);

        if (!ist)
            exit_program(1);

        // 1. 往二维数组input_streams**增加一个input_streams*元素
        GROW_ARRAY(input_streams, nb_input_streams);//注：这里只是开辟了InputStream *字节大小(64bit为8字节)，而不是开辟InputStream字节大小(sizeof(InputStream))
        input_streams[nb_input_streams - 1] = ist;

        // 2. 利用o对InputStream内部进行赋值或者直接赋默认值
        ist->st = st;
        ist->file_index = nb_input_files;
        ist->discard = 1;
        st->discard  = AVDISCARD_ALL;/*设置了AVDISCARD_ALL，表示用户丢弃该流*/
        ist->nb_samples = 0;
        ist->min_pts = INT64_MAX;
        ist->max_pts = INT64_MIN;

        ist->ts_scale = 1.0;
        MATCH_PER_STREAM_OPT(ts_scale, dbl, ist->ts_scale, ic, st);

        ist->autorotate = 1;
        MATCH_PER_STREAM_OPT(autorotate, i, ist->autorotate, ic, st);

        MATCH_PER_STREAM_OPT(codec_tags, str, codec_tag, ic, st);
        if (codec_tag) {
            //strtlo see https://blog.csdn.net/weixin_37921201/article/details/119718403
            uint32_t tag = strtol(codec_tag, &next, 0);// next指向非法字符串开始及后面的字符串.
            if (*next)// codec_tag出现非法字符串的处理
                tag = AV_RL32(codec_tag);
            st->codecpar->codec_tag = tag;
        }

        // 3. 保存解码器
        ist->dec = choose_decoder(o, ic, st);

        // 4. 保存解码器选项。将o->g->codec_opts保存的解码器选项通过返回值设置到decoder_opts中。
        /*这里看到，每个流都保存了一份o->g->codec_opts*/
        ist->decoder_opts = filter_codec_opts(o->g->codec_opts, ist->st->codecpar->codec_id, ic, st, ist->dec);

        ist->reinit_filters = -1;
        MATCH_PER_STREAM_OPT(reinit_filters, i, ist->reinit_filters, ic, st);

        MATCH_PER_STREAM_OPT(discard, str, discard_str, ic, st);
        ist->user_set_discard = AVDISCARD_NONE;/*默认该流是不丢弃的*/

        // 5. 判断是否丢弃该流.
        // 只要有视频或者音频或者字幕或者数据被禁用，那么user_set_discard标记为AVDISCARD_ALL
        if ((o->video_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ||
            (o->audio_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ||
            (o->subtitle_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) ||
            (o->data_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA))
                ist->user_set_discard = AVDISCARD_ALL;/*设置了AVDISCARD_ALL，表示用户丢弃该流*/
        /*检测ffmpeg设置后user_set_discard的值是否与用户的一致，不一致则程序退出*/
        if (discard_str && av_opt_eval_int(&cc, discard_opt, discard_str, &ist->user_set_discard) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error parsing discard %s.\n",
                    discard_str);
            exit_program(1);
        }

        ist->filter_in_rescale_delta_last = AV_NOPTS_VALUE;

        // 6. 为解码器上下文开辟内存
        ist->dec_ctx = avcodec_alloc_context3(ist->dec);
        if (!ist->dec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Error allocating the decoder context.\n");
            exit_program(1);
        }

        // 7. 从流中拷贝参数到解码器上下文
        ret = avcodec_parameters_to_context(ist->dec_ctx, par);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing the decoder context.\n");
            exit_program(1);
        }

        if (o->bitexact)
            ist->dec_ctx->flags |= AV_CODEC_FLAG_BITEXACT;

        // 8. 给不同媒体类型的InputStream中的编解码器相关成员赋值.(忽略硬件相关内容)
        switch (par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if(!ist->dec)// 一般在choose_decoder时已经找到，注open_input_files找到的解码器是ic中的，这里是ist
                ist->dec = avcodec_find_decoder(par->codec_id);
#if FF_API_LOWRES
            if (st->codec->lowres) {
                ist->dec_ctx->lowres = st->codec->lowres;
                ist->dec_ctx->width  = st->codec->width;
                ist->dec_ctx->height = st->codec->height;
                ist->dec_ctx->coded_width  = st->codec->coded_width;
                ist->dec_ctx->coded_height = st->codec->coded_height;
            }
#endif

            // avformat_find_stream_info() doesn't set this for us anymore.
            // avformat_find_stream_info()不再为我们设置这个。
            ist->dec_ctx->framerate = st->avg_frame_rate;

            MATCH_PER_STREAM_OPT(frame_rates, str, framerate, ic, st);
            if (framerate && av_parse_video_rate(&ist->framerate,
                                                 framerate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error parsing framerate %s.\n",
                       framerate);
                exit_program(1);
            }

            ist->top_field_first = -1;
            MATCH_PER_STREAM_OPT(top_field_first, i, ist->top_field_first, ic, st);

            // 硬件加速相关.
            // 寻找硬件设备的id
            // 可参考https://blog.csdn.net/u012117034/article/details/123470108
            MATCH_PER_STREAM_OPT(hwaccels, str, hwaccel, ic, st);
            if (hwaccel) {
                // The NVDEC hwaccels use a CUDA device, so remap the name here.
                // NVDEC hwaccels使用CUDA设备，所以在这里重新映射名称。
                // NVDEC是英伟达提供的一种视频解码器引擎，作用是用来解码，可认为是一个SDK库。see https://blog.csdn.net/qq_18998145/article/details/108535188
                // cuda则是一种架构，使用CPU+GPU去处理各种复杂的运算。see https://baike.baidu.com/item/CUDA/1186262?fr=aladdin
                // 所以这里再重新看这段代码：因为nvdec内部使用的是cuda架构，所以重新映射hwaccel字符串
                if (!strcmp(hwaccel, "nvdec"))
                    hwaccel = "cuda";

                if (!strcmp(hwaccel, "none"))
                    ist->hwaccel_id = HWACCEL_NONE;
                else if (!strcmp(hwaccel, "auto"))
                    ist->hwaccel_id = HWACCEL_AUTO;
                else {// 用户指定了具体的硬件解码器
                    enum AVHWDeviceType type;
                    int i;
                    for (i = 0; hwaccels[i].name; i++) {
                        if (!strcmp(hwaccels[i].name, hwaccel)) {
                            ist->hwaccel_id = hwaccels[i].id;
                            break;
                        }
                    }

                    // hwaccel_id=HWACCEL_NONE时
                    if (!ist->hwaccel_id) {
                        // 根据设备类型名称查找，该函数不区分大小写。找不到会返回AV_HWDEVICE_TYPE_NONE
                        type = av_hwdevice_find_type_by_name(hwaccel);
                        if (type != AV_HWDEVICE_TYPE_NONE) {
                            ist->hwaccel_id = HWACCEL_GENERIC;
                            ist->hwaccel_device_type = type;
                        }
                    }

                    // hwaccel_id还是为HWACCEL_NONE时，打印相关错误提示然后程序退出
                    if (!ist->hwaccel_id) {
                        av_log(NULL, AV_LOG_FATAL, "Unrecognized hwaccel: %s.\n",
                               hwaccel);
                        av_log(NULL, AV_LOG_FATAL, "Supported hwaccels: ");
                        type = AV_HWDEVICE_TYPE_NONE;
                        // av_hwdevice_iterate_types函数每次会返回在hw_table中，上一个id为prev的硬件解码器id
                        // hw_table是源码中的一个硬件映射表，保存了各个解码器的处理函数
                        while ((type = av_hwdevice_iterate_types(type)) !=
                               AV_HWDEVICE_TYPE_NONE)
                            av_log(NULL, AV_LOG_FATAL, "%s ",
                                   av_hwdevice_get_type_name(type));


                        for (i = 0; hwaccels[i].name; i++)
                            av_log(NULL, AV_LOG_FATAL, "%s ", hwaccels[i].name);
                        av_log(NULL, AV_LOG_FATAL, "\n");
                        exit_program(1);
                    }
                }
            }//<== if (hwaccel) end ==>

            // 保存硬件设备名？
            MATCH_PER_STREAM_OPT(hwaccel_devices, str, hwaccel_device, ic, st);
            if (hwaccel_device) {
                ist->hwaccel_device = av_strdup(hwaccel_device);
                if (!ist->hwaccel_device)
                    exit_program(1);
            }

            MATCH_PER_STREAM_OPT(hwaccel_output_formats, str,
                                 hwaccel_output_format, ic, st);
            if (hwaccel_output_format) {
                ist->hwaccel_output_format = av_get_pix_fmt(hwaccel_output_format);
                if (ist->hwaccel_output_format == AV_PIX_FMT_NONE) {
                    av_log(NULL, AV_LOG_FATAL, "Unrecognised hwaccel output "
                           "format: %s", hwaccel_output_format);
                }
            } else {
                ist->hwaccel_output_format = AV_PIX_FMT_NONE;
            }

            ist->hwaccel_pix_fmt = AV_PIX_FMT_NONE;

            break;
        case AVMEDIA_TYPE_AUDIO:
            ist->guess_layout_max = INT_MAX;
            MATCH_PER_STREAM_OPT(guess_layout_max, i, ist->guess_layout_max, ic, st);
            guess_input_channel_layout(ist);// 设置通道布局到ist中
            break;
        case AVMEDIA_TYPE_DATA:
        case AVMEDIA_TYPE_SUBTITLE: {
            char *canvas_size = NULL;
            if(!ist->dec)// 一般在choose_decoder时已经找到
                ist->dec = avcodec_find_decoder(par->codec_id);
            MATCH_PER_STREAM_OPT(fix_sub_duration, i, ist->fix_sub_duration, ic, st);
            MATCH_PER_STREAM_OPT(canvas_sizes, str, canvas_size, ic, st);
            // av_parse_video_size:解析字符串canvas_size，并将值赋值给参1、参2
            if (canvas_size &&
                av_parse_video_size(&ist->dec_ctx->width, &ist->dec_ctx->height, canvas_size) < 0) {
                av_log(NULL, AV_LOG_FATAL, "Invalid canvas size: %s.\n", canvas_size);
                exit_program(1);
            }
            break;
        }
        case AVMEDIA_TYPE_ATTACHMENT:
        case AVMEDIA_TYPE_UNKNOWN:
            break;
        default:
            abort();
        }//<== switch (par->codec_type) end ==>

        // 上面看到，一开始是使用avcodec_parameters_to_context(ist->dec_ctx, par)将流中
        // 的信息拷贝到解码器上下文，然后对解码器上下文里的参数赋值后，重新拷贝到流中。
        // par是st->codecpar流中的信息.
        /*9.对解码器上下文里的参数赋值后，重新拷贝到流中*/
        ret = avcodec_parameters_from_context(par, ist->dec_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing the decoder context.\n");
            exit_program(1);
        }
    }//<== for (i = 0; i < ic->nb_streams; i++) { end ==>
}

/**
 * @brief 判断是否重写文件。对实时流无效。
 * @param filename 输出文件名
 * @return 成功=0，代表输出文件可以重写。失败=程序退出
 *
 * @note 该函数很简单，就是用于判断文件是否重写，只要函数能返回，
 *          就代表该输出文件可以重写。重写即重新创建文件是在调用该函数后，在调
 *          用avio_open2完成的。
 * 并且注意，重写是指输出文件，而不是输入文件。
*/
static void assert_file_overwrite(const char *filename)
{
    // 1. 获取协议名字，例如rtmp://xxx，那么proto_name="rtmp"
    const char *proto_name = avio_find_protocol_name(filename);

    // 2. -y以及-n都提供，程序退出
    if (file_overwrite && no_file_overwrite) {
        fprintf(stderr, "Error, both -y and -n supplied. Exiting.\n");
        exit_program(1);
    }

    /* 3.若没指定-y且-n也没指定，那么会使用avio_check判断该文件是否存在，若存在会在终端询问用户是否重写，
         用户输入y代表重写，那么该函数返回后会在avio_open2将该文件重新创建。
         指定了-y，那么直接跳过判断，说明可以直接写了.
    */
    /*
    * avio_check：返回AVIO_FLAG_*访问标志，对应url中资源的访问权限，失败时返回负值，对应AVERROR代码。
    * 返回的访问标志被flags中的值屏蔽。
    * @note这个函数本质上是不安全的，因为被检查的资源可能会在一次调用到另一次调用时改变它的存在或权限状态。
    * 因此，您不应该信任返回的值，除非您确定没有其他进程正在访问所检查的资源。
    * 即意思是：你刚好检测完权限，下一秒就被人改掉这个权限了。
    * 并且笔者测试过，avio_check内部会检测该输出文件是否存在，若存在返回0；不存在返回其它.
    */
    if (!file_overwrite) {
        /*协议存在 且 是文件 且 avio_check(filename, 0) == 0*/
        //int tyycodeCheck = avio_check(filename, 0);//实时流要注掉，不然会卡主
        if (proto_name && !strcmp(proto_name, "file") && avio_check(filename, 0) == 0) {
            /*若交互模式且没强制不重写文件，会主动询问用户是否要重写文件*/
            if (stdin_interaction && !no_file_overwrite) {
                fprintf(stderr,"File '%s' already exists. Overwrite ? [y/N] ", filename);//在界面提示该字符串
                fflush(stderr);//清空stderr的缓冲区
                term_exit();
                signal(SIGINT, SIG_DFL);
                if (!read_yesno()) {/*返回0不重写文件；1重写*/
                    av_log(NULL, AV_LOG_FATAL, "Not overwriting - exiting\n");
                    exit_program(1);
                }
                term_init();// 该函数很简单，就是注册相关信号回调函数
            }
            else {
                av_log(NULL, AV_LOG_FATAL, "File '%s' already exists. Exiting.\n", filename);
                exit_program(1);
            }
        }
    }

    // 4. 检测输入文件与输出文件名是否一致，若一致则程序退出。一些特定于设备的特殊文件不考虑.
    /*文件流程，实时流不会进该if*/
    if (proto_name && !strcmp(proto_name, "file")) {
        /*判断每个输入文件是否是特殊的文件*/
        for (int i = 0; i < nb_input_files; i++) {
             InputFile *file = input_files[i];
             /*输入文件的flags是否包含AVFMT_NOFILE
                AVFMT_NOFILE：一些特定于设备的特殊文件*/
             if (file->ctx->iformat->flags & AVFMT_NOFILE)
                 continue;

             if (!strcmp(filename, file->ctx->url)) {/*判断输出文件名与输入上下文保存的文件名(输入的文件名)是否一致*/
                 av_log(NULL, AV_LOG_FATAL, "Output %s same as Input #%d - exiting\n", filename, i);
                 av_log(NULL, AV_LOG_WARNING, "FFmpeg cannot edit existing files in-place.\n");
                 exit_program(1);
             }
        }
    }
}

static void dump_attachment(AVStream *st, const char *filename)
{
    int ret;
    AVIOContext *out = NULL;
    AVDictionaryEntry *e;

    if (!st->codecpar->extradata_size) {
        av_log(NULL, AV_LOG_WARNING, "No extradata to dump in stream #%d:%d.\n",
               nb_input_files - 1, st->index);
        return;
    }
    if (!*filename && (e = av_dict_get(st->metadata, "filename", NULL, 0)))
        filename = e->value;
    if (!*filename) {
        av_log(NULL, AV_LOG_FATAL, "No filename specified and no 'filename' tag"
               "in stream #%d:%d.\n", nb_input_files - 1, st->index);
        exit_program(1);
    }

    assert_file_overwrite(filename);

    if ((ret = avio_open2(&out, filename, AVIO_FLAG_WRITE, &int_cb, NULL)) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not open file %s for writing.\n",
               filename);
        exit_program(1);
    }

    avio_write(out, st->codecpar->extradata, st->codecpar->extradata_size);
    avio_flush(out);
    avio_close(out);
}

static int open_input_file(OptionsContext *o, const char *filename)
{
    InputFile *f;
    AVFormatContext *ic;
    AVInputFormat *file_iformat = NULL;
    int err, i, ret;
    int64_t timestamp;
    AVDictionary *unused_opts = NULL;
    AVDictionaryEntry *e = NULL;
    char *   video_codec_name = NULL;
    char *   audio_codec_name = NULL;
    char *subtitle_codec_name = NULL;
    char *    data_codec_name = NULL;
    int scan_all_pmts_set = 0;

    // -t and -to cannot be used together; using -t
    // -t是录制时长.例如:ffmpeg -rtsp_transport tcp -y -re -i rtsp://xxx -vcodec copy -t 00:00:10.00 -f mp4 output.mp4
    if (o->stop_time != INT64_MAX && o->recording_time != INT64_MAX) {
        o->stop_time = INT64_MAX;
        av_log(NULL, AV_LOG_WARNING, "-t and -to cannot be used together; using -t.\n");
    }

    // -to value smaller than -ss; aborting.
    if (o->stop_time != INT64_MAX && o->recording_time == INT64_MAX) {
        int64_t start_time = o->start_time == AV_NOPTS_VALUE ? 0 : o->start_time;
        if (o->stop_time <= start_time) {
            av_log(NULL, AV_LOG_ERROR, "-to value smaller than -ss; aborting.\n");
            exit_program(1);
        } else {
            o->recording_time = o->stop_time - start_time;
        }
    }

    /*在options[]中对应“f”，指定输入文件的格式“-f”*/
    if (o->format) {
        if (!(file_iformat = av_find_input_format(o->format))) {
            av_log(NULL, AV_LOG_FATAL, "Unknown input format: '%s'\n", o->format);
            exit_program(1);
        }
    }

    // 如果是管道,输入文件名为“-”
    if (!strcmp(filename, "-"))
        filename = "pipe:";

    /*stdin_interaction针对参数-stdin*/
    // 运算符优先级,记忆口决："单算移关与，异或逻条赋", 具体看https://blog.csdn.net/sgbl888/article/details/123997358
    // 不过下面语句等价于：stdin_interaction = stdin_interaction & (strncmp(filename, "pipe:", 5) && strcmp(filename, "/dev/stdin"));
    // 意思是：如果filename不是pipe:开头，那么再判断filename是否是/dev/stdin.
    // 故下面意思是：除了是pipe或者是/dev/stdin不是交互模式，其它都是。故文件、实时流stdin_interaction都为1
    //auto tyy1 = strncmp(filename, "pipe:", 5) && strcmp(filename, "/dev/stdin");
    //stdin_interaction = stdin_interaction & (strncmp(filename, "pipe:", 5) && strcmp(filename, "/dev/stdin"));// 等价于下面语句
    stdin_interaction &= strncmp(filename, "pipe:", 5) &&
                         strcmp(filename, "/dev/stdin");

    // 1. 为输入文件开辟相关内存
    /* get default parameters from command line */
    ic = avformat_alloc_context();
    if (!ic) {
        print_error(filename, AVERROR(ENOMEM));
        exit_program(1);
    }

    // 1.1 解复用相关参数设置(不过平时输入文件很少设置过多的参数,输出文件才会)
    /*对应参数“-ar”设置音频采样率*/
    if (o->nb_audio_sample_rate) {
        av_dict_set_int(&o->g->format_opts, "sample_rate", o->audio_sample_rate[o->nb_audio_sample_rate - 1].u.i, 0);
    }

    /*对应参数"-ac"*/
    if (o->nb_audio_channels) {
        /* because we set audio_channels based on both the "ac" and
         * "channel_layout" options, we need to check that the specified
         * demuxer actually has the "channels" option before setting it */
        if (file_iformat && file_iformat->priv_class &&
            av_opt_find(&file_iformat->priv_class, "channels", NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set_int(&o->g->format_opts, "channels", o->audio_channels[o->nb_audio_channels - 1].u.i, 0);
        }
    }

    /*对应参数"r"帧率，注与re是不一样的，re对应成员是rate_emu*/
    if (o->nb_frame_rates) {
        /* set the format-level framerate option;
         * this is important for video grabbers, e.g. x11 */
        if (file_iformat && file_iformat->priv_class &&
            av_opt_find(&file_iformat->priv_class, "framerate", NULL, 0,
                        AV_OPT_SEARCH_FAKE_OBJ)) {
            av_dict_set(&o->g->format_opts, "framerate",
                        o->frame_rates[o->nb_frame_rates - 1].u.str, 0);
        }
    }

    //对应参数"s"
    if (o->nb_frame_sizes) {
        av_dict_set(&o->g->format_opts, "video_size", o->frame_sizes[o->nb_frame_sizes - 1].u.str, 0);
    }

    //对应参数"pix_fmt"
    if (o->nb_frame_pix_fmts)
        av_dict_set(&o->g->format_opts, "pixel_format", o->frame_pix_fmts[o->nb_frame_pix_fmts - 1].u.str, 0);

    //对应参数"c"和"codec" 或"c:[v/a/s/d]"和"codec:[v/a/s/d]"
    //最终通过参数3传出.假设我们传-codec libx264, 那么video_codec_name="libx264"
    //因为传入时o->name[i].u.str会指向"libx264"
    MATCH_PER_TYPE_OPT(codec_names, str,    video_codec_name, ic, "v");
    MATCH_PER_TYPE_OPT(codec_names, str,    audio_codec_name, ic, "a");
    MATCH_PER_TYPE_OPT(codec_names, str, subtitle_codec_name, ic, "s");
    MATCH_PER_TYPE_OPT(codec_names, str,     data_codec_name, ic, "d");

    /*
     下面第2，第3步是对ic的video_codec,audio_codec,subtitle_codec,data_codec以及
     video_codec_id,audio_codec_id,subtitle_codec_id,data_codec赋值
    */
    // 2. 根据用户输入指定的编解码器格式，寻找对应的编解码器
    // 找到则赋值给解复用结构体ic，找不到会在find_codec_or_die直接退出
    if (video_codec_name)
        ic->video_codec    = find_codec_or_die(video_codec_name   , AVMEDIA_TYPE_VIDEO   , 0);
    if (audio_codec_name)
        ic->audio_codec    = find_codec_or_die(audio_codec_name   , AVMEDIA_TYPE_AUDIO   , 0);
    if (subtitle_codec_name)
        ic->subtitle_codec = find_codec_or_die(subtitle_codec_name, AVMEDIA_TYPE_SUBTITLE, 0);
    if (data_codec_name)
        ic->data_codec     = find_codec_or_die(data_codec_name    , AVMEDIA_TYPE_DATA    , 0);

    // 来到这说明成功找到编解码器(因为失败会在find_codec_or_die直接退出出现)
    // 3. 若指定输入文件的编解码器，则将编解码器中的id赋值给解复用的id; 若无指定，解码器的id为xxx_ID_NONE
    ic->video_codec_id     = video_codec_name    ? ic->video_codec->id    : AV_CODEC_ID_NONE;
    ic->audio_codec_id     = audio_codec_name    ? ic->audio_codec->id    : AV_CODEC_ID_NONE;
    ic->subtitle_codec_id  = subtitle_codec_name ? ic->subtitle_codec->id : AV_CODEC_ID_NONE;
    ic->data_codec_id      = data_codec_name     ? ic->data_codec->id     : AV_CODEC_ID_NONE;

    /*ffmpeg的avformat_open_input()和av_read_frame()默认是阻塞的.
     * 1)用户可以通过设置"ic->flags |= AVFMT_FLAG_NONBLOCK;"设置成非阻塞(通常是不推荐的)；
     * 2)或者是设置超时时间；
     * 3)或者是设置interrupt_callback定义返回机制。
    */
    ic->flags |= AVFMT_FLAG_NONBLOCK;
    if (o->bitexact)
        ic->flags |= AVFMT_FLAG_BITEXACT;
    ic->interrupt_callback = int_cb;// 设置中断函数

    // 4. avformat_open_input打开输入文件，打开时可以设置解复用的相关参数
    // scan_all_pmts是mpegts的一个选项，表示扫描全部的ts流的"Program Map Table"表。这里在没有设定该选项的时候，强制设为1。
    if (!av_dict_get(o->g->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&o->g->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    /* open the input file with generic avformat function */
    err = avformat_open_input(&ic, filename, file_iformat, &o->g->format_opts);// 这里看到，解复用的参数最终使用该函数去设置的
    if (err < 0) {
        print_error(filename, err);
        if (err == AVERROR_PROTOCOL_NOT_FOUND)
            av_log(NULL, AV_LOG_ERROR, "Did you mean file:%s?\n", filename);
        exit_program(1);
    }
    // 打开输入文件后，将该选项置空
    if (scan_all_pmts_set)
        av_dict_set(&o->g->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    remove_avoptions(&o->g->format_opts, o->g->codec_opts);
    // 上面我们会将有效的条目置空，所以此时还有条目，代表该条目是不合法的。
    assert_avoptions(o->g->format_opts);

    // 5. 若用户设置解码器，会查找解码器，并把找到解码器的id赋值到streams中
    /* apply forced codec ids */
    for (i = 0; i < ic->nb_streams; i++)
        choose_decoder(o, ic, ic->streams[i]);

    // 6. 是否查找流信息.
    if (find_stream_info) {
        /*这里会为输入流开辟每一个编解码器选项，以返回值进行返回*/
        AVDictionary **opts = setup_find_stream_info_opts(ic, o->g->codec_opts);
        int orig_nb_streams = ic->nb_streams;

        /*avformat_find_stream_info这里会用到解码器选项o->g->codec_opts，因为opts就是从
        o->g->codec_opts过滤得到的*/
        /* If not enough info to get the stream parameters, we decode the
           first frames to get it. (used in mpeg case for example) */
        ret = avformat_find_stream_info(ic, opts);

        // 释放setup_find_stream_info_opts内部开辟的内存
        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "%s: could not find codec parameters\n", filename);
            if (ic->nb_streams == 0) {
                avformat_close_input(&ic);
                exit_program(1);
            }
        }
    }

    //对应参数"sseof",设置相对于结束的开始时间
    if (o->start_time != AV_NOPTS_VALUE && o->start_time_eof != AV_NOPTS_VALUE) {
        av_log(NULL, AV_LOG_WARNING, "Cannot use -ss and -sseof both, using -ss for %s\n", filename);
        o->start_time_eof = AV_NOPTS_VALUE;
    }

    if (o->start_time_eof != AV_NOPTS_VALUE) {
        if (o->start_time_eof >= 0) {
            av_log(NULL, AV_LOG_ERROR, "-sseof value must be negative; aborting\n");
            exit_program(1);
        }
        if (ic->duration > 0) {
            o->start_time = o->start_time_eof + ic->duration;
            if (o->start_time < 0) {
                av_log(NULL, AV_LOG_WARNING, "-sseof value seeks to before start of file %s; ignored\n", filename);
                o->start_time = AV_NOPTS_VALUE;
            }
        } else
            av_log(NULL, AV_LOG_WARNING, "Cannot use -sseof, duration of %s not known\n", filename);
    }
    timestamp = (o->start_time == AV_NOPTS_VALUE) ? 0 : o->start_time;
    /* add the stream start time */
    if (!o->seek_timestamp && ic->start_time != AV_NOPTS_VALUE)
        timestamp += ic->start_time;

    // 如果开始时间指定的话, 需要做seek
    /* if seeking requested, we execute it */
    if (o->start_time != AV_NOPTS_VALUE) {
        int64_t seek_timestamp = timestamp;

        if (!(ic->iformat->flags & AVFMT_SEEK_TO_PTS)) {
            int dts_heuristic = 0;
            for (i=0; i<ic->nb_streams; i++) {
                const AVCodecParameters *par = ic->streams[i]->codecpar;
                if (par->video_delay) {
                    dts_heuristic = 1;
                    break;
                }
            }
            if (dts_heuristic) {
                //笔者猜测这里意思是：若ffmpeg存在视频帧延迟(估计是ffmpeg内部缓冲？还是解码造成dts、pts的差距？)，
                //那么seek时，应当稍微减少一点seek_timestamp. 3/23大概是3帧的时长，*AV_TIME_BASE是单位转成秒.
                seek_timestamp -= 3*AV_TIME_BASE / 23;
            }
        }
        ret = avformat_seek_file(ic, -1, INT64_MIN, seek_timestamp, seek_timestamp, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    /* update the current parameters so that they match the one of the input stream */
    // 7. 将输入文件的每个流封装到InputStream中。InputStream->st是直接指向输入文件的AVStream的，不会再开辟内存.
    // 执行完该函数后，输入文件中各个流就会被保存到input_streams**这个二维数组
    // (ic->streams[]与input_streams都保存这输入文件的各个输入流)
    add_input_streams(o, ic);

    /* dump the file content */
    // 参4：0-代表dump输入文件，1-代表dump输出文件。可以看源码.
    av_dump_format(ic, nb_input_files, filename, 0);

    /*8.对InputFile内剩余成员的相关初始化工作*/
    //int tyysize = sizeof (*input_files);//8
    GROW_ARRAY(input_files, nb_input_files);// 开辟sizeof(InputFile*)大小的内存，且输入文件个数+1
    f = av_mallocz(sizeof(*f));// 真正开辟InputFile结构体的内存
    if (!f)
        exit_program(1);
    input_files[nb_input_files - 1] = f;

    // 下面就是给InputFile结构体赋值
    f->ctx        = ic;
    f->ist_index  = nb_input_streams - ic->nb_streams;/*输入文件的流的第一个流下标.基本是0，因为在add_input_streams中看到，
                                                        nb_input_streams的个数由ic->nb_streams决定*/
    f->start_time = o->start_time;
    f->recording_time = o->recording_time;
    f->input_ts_offset = o->input_ts_offset;
    f->ts_offset  = o->input_ts_offset - (copy_ts ? (start_at_zero && ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0) : timestamp);
    f->nb_streams = ic->nb_streams;
    f->rate_emu   = o->rate_emu;// -re
    f->accurate_seek = o->accurate_seek;
    f->loop = o->loop;
    f->duration = 0;
    f->time_base = (AVRational){ 1, 1 };
#if HAVE_THREADS
    f->thread_queue_size = o->thread_queue_size > 0 ? o->thread_queue_size : 8;
#endif

    /*9.检测所有编解码器选项是否已经被使用(看open_output_file)*/
    /* check if all codec options have been used */
    // 因输入文件一般很少设置编解码器选项， 这里可以后续读者自行研究，看懂意思就行，不难
    unused_opts = strip_specifiers(o->g->codec_opts);// 将用户输入的参数去掉流分隔符
    /*这里意思是：因为在add_input_streams时，内部已经将用户输入的参数过滤后，匹配给对应的流(一般情
     * 况各个输入流中都保存一份o->g->codec_opts副本)，所以这里根据各个输入流保存的选项，去清除用户实际
     * 输入的选项unused_opts，当发现unused_opts还有选项时，那么该选项就是未使用的。
    */
    for (i = f->ist_index; i < nb_input_streams; i++) {
        e = NULL;
        while ((e = av_dict_get(input_streams[i]->decoder_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX)))
            av_dict_set(&unused_opts, e->key, NULL, 0);
    }

    /*这个while就是对多余的选项进行一些警告处理*/
    e = NULL;
    while ((e = av_dict_get(unused_opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
        const AVClass *class = avcodec_get_class();// 获取编解码器的AVClass
        const AVOption *option = av_opt_find(&class, e->key, NULL, 0,
                                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        const AVClass *fclass = avformat_get_class();// 获取解复用器的AVClass
        const AVOption *foption = av_opt_find(&fclass, e->key, NULL, 0,
                                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        if (!option || foption)
            continue;

        // 若选项option即e->key不是解码器选项，程序退出
        if (!(option->flags & AV_OPT_FLAG_DECODING_PARAM)) {
            av_log(NULL, AV_LOG_ERROR, "Codec AVOption %s (%s) specified for "
                   "input file #%d (%s) is not a decoding option.\n", e->key,
                   option->help ? option->help : "", nb_input_files - 1,
                   filename);
            exit_program(1);
        }

        av_log(NULL, AV_LOG_WARNING, "Codec AVOption %s (%s) specified for "
               "input file #%d (%s) has not been used for any stream. The most "
               "likely reason is either wrong type (e.g. a video option with "
               "no video streams) or that it is a private option of some decoder "
               "which was not actually used for any stream.\n", e->key,
               option->help ? option->help : "", nb_input_files - 1, filename);
    }
    av_dict_free(&unused_opts);

    //对应参数"dump_attachment"
    for (i = 0; i < o->nb_dump_attachment; i++) {
        int j;

        for (j = 0; j < ic->nb_streams; j++) {
            AVStream *st = ic->streams[j];

            if (check_stream_specifier(ic, st, o->dump_attachment[i].specifier) == 1)
                dump_attachment(st, o->dump_attachment[i].u.str);
        }
    }

    /*10.input_stream_potentially_available 置为1，记录该输入文件可能是能用的*/
    // 打开后，记录该输入文件可能是能用的
    input_stream_potentially_available = 1;

    return 0;
}

static uint8_t *get_line(AVIOContext *s)
{
    AVIOContext *line;
    uint8_t *buf;
    char c;

    if (avio_open_dyn_buf(&line) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not alloc buffer for reading preset.\n");
        exit_program(1);
    }

    while ((c = avio_r8(s)) && c != '\n')
        avio_w8(line, c);
    avio_w8(line, 0);
    avio_close_dyn_buf(line, &buf);

    return buf;
}

static int get_preset_file_2(const char *preset_name, const char *codec_name, AVIOContext **s)
{
    int i, ret = -1;
    char filename[1000];
    const char *base[3] = { getenv("AVCONV_DATADIR"),
                            getenv("HOME"),
                            AVCONV_DATADIR,
                            };

    for (i = 0; i < FF_ARRAY_ELEMS(base) && ret < 0; i++) {
        if (!base[i])
            continue;
        if (codec_name) {
            snprintf(filename, sizeof(filename), "%s%s/%s-%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", codec_name, preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
        if (ret < 0) {
            snprintf(filename, sizeof(filename), "%s%s/%s.avpreset", base[i],
                     i != 1 ? "" : "/.avconv", preset_name);
            ret = avio_open2(s, filename, AVIO_FLAG_READ, &int_cb, NULL);
        }
    }
    return ret;
}

/**
 * @brief 选择编码器，找到编码器最终保存在ost->enc中
 * @param o 保存着用户命令行的参数上下文
 * @param s 输出文件上下文
 * @param ost ffmpeg封装的输出流
 * @return 0=成功；没有指定编码器但找不到编码器=返回负数；指定编码器但找不到编码器=程序退出
 */
static int choose_encoder(OptionsContext *o, AVFormatContext *s, OutputStream *ost)
{
    enum AVMediaType type = ost->st->codecpar->codec_type;
    char *codec_name = NULL;

    /*1.只有音视频、字幕才能进行转码，否则其它类型默认为copy选项不转码*/
    if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE) {
        /*2.获取用户指定输出的编码类型*/
        MATCH_PER_STREAM_OPT(codec_names, str, codec_name, s, ost->st);
        /*2.1若用户没指定，则通过stream的编码器id查找*/
        if (!codec_name) {
            /*av_guess_codec: 猜测编解码器id,依赖oc->oformat->name(-f参数，例如flv)以及文件名.
             视频通过av_guess_codec处理后，看源码得出，视频一般返回的是AVOutputFormat->video_codec*/
            ost->st->codecpar->codec_id = av_guess_codec(s->oformat, NULL, s->url,
                                                         NULL, ost->st->codecpar->codec_type);
            ost->enc = avcodec_find_encoder(ost->st->codecpar->codec_id);
            if (!ost->enc) {
                av_log(NULL, AV_LOG_FATAL, "Automatic encoder selection failed for "
                       "output stream #%d:%d. Default encoder for format %s (codec %s) is "
                       "probably disabled. Please choose an encoder manually.\n",
                       ost->file_index, ost->index, s->oformat->name,
                       avcodec_get_name(ost->st->codecpar->codec_id));
                return AVERROR_ENCODER_NOT_FOUND;
            }
        } else if (!strcmp(codec_name, "copy"))/*2.2用户指定不转码*/
            ost->stream_copy = 1;
        else {
            /*2.3指定编解码器，则使用find_codec_or_die查找，找不到会直接退出程序*/
            ost->enc = find_codec_or_die(codec_name, ost->st->codecpar->codec_type, 1);
            ost->st->codecpar->codec_id = ost->enc->id;
        }
        ost->encoding_needed = !ost->stream_copy;
    } else {
        /* no encoding supported for other media types */
        ost->stream_copy     = 1;
        ost->encoding_needed = 0;
    }

    return 0;
}

/**
 * @brief 创建一个OutputStream类型的输出流，并使用用户传进的参数即o对该输出流的成员进行初始化
 * @param o 用户输入的参数上下文
 * @param oc 输出文件上下文
 * @param type 媒体类型
 * @param source_index 输入文件的流下标
 *
 * @return 成功=返回创建好的OutputStream*； 失败=程序退出
*/
static OutputStream *new_output_stream(OptionsContext *o, AVFormatContext *oc, enum AVMediaType type, int source_index)
{
    OutputStream *ost;
    /*1.创建新的输出流*/
    AVStream *st = avformat_new_stream(oc, NULL);//这里调用后，oc->nb_streams自动加1.例如avformat_alloc_output_context2时是0，这里后变成1
    int idx      = oc->nb_streams - 1, ret = 0;
    const char *bsfs = NULL, *time_base = NULL;
    char *next, *codec_tag = NULL;
    double qscale = -1;
    int i;

    if (!st) {
        av_log(NULL, AV_LOG_FATAL, "Could not alloc stream.\n");
        exit_program(1);
    }

    if (oc->nb_streams - 1 < o->nb_streamid_map)
        st->id = o->streamid_map[oc->nb_streams - 1];

    GROW_ARRAY(output_streams, nb_output_streams);//在二级数组中添加一个一级指针大小的元素
    if (!(ost = av_mallocz(sizeof(*ost))))
        exit_program(1);
    output_streams[nb_output_streams - 1] = ost;// 给一级指针赋值

    /*2.给OutputStream相关成员赋值*/
    ost->file_index = nb_output_files - 1;//这里有机会可以试试多个输出时，debug file_index的值
    ost->index      = idx;// 使用流下标赋值
    ost->st         = st;
    ost->forced_kf_ref_pts = AV_NOPTS_VALUE;
    st->codecpar->codec_type = type;

    /*3.寻找编码器*/
    ret = choose_encoder(o, oc, ost);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error selecting an encoder for stream "
               "%d:%d\n", ost->file_index, ost->index);
        exit_program(1);
    }

    /*4.开辟编码器上下文*/
    ost->enc_ctx = avcodec_alloc_context3(ost->enc);
    if (!ost->enc_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Error allocating the encoding context.\n");
        exit_program(1);
    }
    ost->enc_ctx->codec_type = type;

    /*5.给OutputStream中的编码器相关参数开辟空间*/
    ost->ref_par = avcodec_parameters_alloc();
    if (!ost->ref_par) {
        av_log(NULL, AV_LOG_ERROR, "Error allocating the encoding parameters.\n");
        exit_program(1);
    }

    /*6.从o->g->codec_opts过滤选项，保存到OutputStream的encoder_opts中*/
    if (ost->enc) {
        AVIOContext *s = NULL;
        char *buf = NULL, *arg = NULL, *preset = NULL;

        /*这里是将o中编码器选项转移到OutputStream中，很重要*/
        ost->encoder_opts  = filter_codec_opts(o->g->codec_opts, ost->enc->id, oc, st, ost->enc);

        //该presets应该与选项-preset veryfast无关因为-preset是被存放在o->g->codec_opts中.故这里后续分析
        MATCH_PER_STREAM_OPT(presets, str, preset, oc, st);
        if (preset && (!(ret = get_preset_file_2(preset, ost->enc->name, &s)))) {
            do  {
                buf = get_line(s);
                if (!buf[0] || buf[0] == '#') {
                    av_free(buf);
                    continue;
                }
                if (!(arg = strchr(buf, '='))) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid line found in the preset file.\n");
                    exit_program(1);
                }
                *arg++ = 0;
                av_dict_set(&ost->encoder_opts, buf, arg, AV_DICT_DONT_OVERWRITE);
                av_free(buf);
            } while (!s->eof_reached);
            avio_closep(&s);
        }
        if (ret) {
            av_log(NULL, AV_LOG_FATAL,
                   "Preset %s specified for stream %d:%d, but could not be opened.\n",
                   preset, ost->file_index, ost->index);
            exit_program(1);
        }
    } else {
        ost->encoder_opts = filter_codec_opts(o->g->codec_opts, AV_CODEC_ID_NONE, oc, st, NULL);
    }

    /*tyycode*/
    AVDictionaryEntry *t = NULL;
    while((t = av_dict_get(ost->encoder_opts, "", t, AV_DICT_IGNORE_SUFFIX))){
        printf("tyytest, t->key: %s, t->value: %s\n", t->key, t->value);
    }

    /*7.这里到函数结尾都是给OutputStream相关成员赋值*/
    if (o->bitexact)
        ost->enc_ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    /*是否设置流的时基，一般不设置默认是{0,0}*/
    /**
     * av_parse_ratio: 解析str并将解析的比率存储在q中。请注意，无穷大(1/0)或负数的比率为被认为是有效的，
     * 想要排除这些值,所以你应该检查返回值。未定义的值可以使用“0:0”字符串表示。
     *
     @param[in,out] q 解析字符串后，得到的时基从该参数传出
     @param[in] str 它必须是一个字符串的格式，格式可以是: num:den或者浮点数或表达式
     @param[in] max 最大允许的分子和分母
     @param[in] log_offset 日志级别偏移量，应用于log_ctx的日志级别
     @param[in] log_ctx 父日志上下文
     @return >= 0表示成功，否则返回负的错误码 **/
    MATCH_PER_STREAM_OPT(time_bases, str, time_base, oc, st);
    if (time_base) {
        AVRational q;
        if (av_parse_ratio(&q, time_base, INT_MAX, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid time base: %s\n", time_base);
            exit_program(1);
        }
        /*AVStream结构体每个字段解释 see https://blog.csdn.net/weixin_44517656/article/details/109645489*/
        st->time_base = q;
    }

    /*是否设置编码时的时基，一般不设置默认是{0,0}*/
    MATCH_PER_STREAM_OPT(enc_time_bases, str, time_base, oc, st);
    if (time_base) {
        AVRational q;
        if (av_parse_ratio(&q, time_base, INT_MAX, 0, NULL) < 0 ||
            q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid time base: %s\n", time_base);
            exit_program(1);
        }
        ost->enc_timebase = q;
    }

    /*-frames选项？暂时未遇到，不过应该与截图相关*/
    //对应参数"frames"，"vframes"，设置输出的帧数。
    ost->max_frames = INT64_MAX;
    MATCH_PER_STREAM_OPT(max_frames, i64, ost->max_frames, oc, st);
    for (i = 0; i<o->nb_max_frames; i++) {
        char *p = o->max_frames[i].specifier;
        if (!*p && type != AVMEDIA_TYPE_VIDEO) {
            av_log(NULL, AV_LOG_WARNING, "Applying unspecific -frames to non video streams, maybe you meant -vframes ?\n");
            break;
        }
    }

    //对应参数"copypriorss"，"copy or discard frames before start time"
    ost->copy_prior_start = -1;
    MATCH_PER_STREAM_OPT(copy_prior_start, i, ost->copy_prior_start, oc ,st);

    //对应参数"bsf"，"absf"，"vbsf"
    /*这里应该与转码+滤镜相关，后续详细分析，可简单参考https://www.cnblogs.com/zhibei/p/12551810.html*/
    MATCH_PER_STREAM_OPT(bitstream_filters, str, bsfs, oc, st);
    while (bsfs && *bsfs) {
        const AVBitStreamFilter *filter;
        char *bsf, *bsf_options_str, *bsf_name;

        bsf = av_get_token(&bsfs, ",");
        if (!bsf)
            exit_program(1);
        bsf_name = av_strtok(bsf, "=", &bsf_options_str);
        if (!bsf_name)
            exit_program(1);

        filter = av_bsf_get_by_name(bsf_name);
        if (!filter) {
            av_log(NULL, AV_LOG_FATAL, "Unknown bitstream filter %s\n", bsf_name);
            exit_program(1);
        }

        ost->bsf_ctx = av_realloc_array(ost->bsf_ctx,
                                        ost->nb_bitstream_filters + 1,
                                        sizeof(*ost->bsf_ctx));
        if (!ost->bsf_ctx)
            exit_program(1);

        ret = av_bsf_alloc(filter, &ost->bsf_ctx[ost->nb_bitstream_filters]);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error allocating a bitstream filter context\n");
            exit_program(1);
        }

        ost->nb_bitstream_filters++;

        if (bsf_options_str && filter->priv_class) {
            const AVOption *opt = av_opt_next(ost->bsf_ctx[ost->nb_bitstream_filters-1]->priv_data, NULL);
            const char * shorthand[2] = {NULL};

            if (opt)
                shorthand[0] = opt->name;

            ret = av_opt_set_from_string(ost->bsf_ctx[ost->nb_bitstream_filters-1]->priv_data, bsf_options_str, shorthand, "=", ":");
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error parsing options for bitstream filter %s\n", bsf_name);
                exit_program(1);
            }
        }
        av_freep(&bsf);

        if (*bsfs)
            bsfs++;
    }

    //对应参数"tag"
    MATCH_PER_STREAM_OPT(codec_tags, str, codec_tag, oc, st);
    if (codec_tag) {
        uint32_t tag = strtol(codec_tag, &next, 0);
        if (*next)
            tag = AV_RL32(codec_tag);
        ost->st->codecpar->codec_tag =
        ost->enc_ctx->codec_tag = tag;
    }

    //对应参数"qscale:[v:a:s:d]"/"q" 以<数值>质量为基础的VBR，取值0.01-255，越小质量越好
    /*qscale、disposition、max_muxing_queue_size暂时未研究*/
    MATCH_PER_STREAM_OPT(qscale, dbl, qscale, oc, st);
    if (qscale >= 0) {
        ost->enc_ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ost->enc_ctx->global_quality = FF_QP2LAMBDA * qscale;
    }

    //对应参数"disposition"
    MATCH_PER_STREAM_OPT(disposition, str, ost->disposition, oc, st);
    ost->disposition = av_strdup(ost->disposition);

    ost->max_muxing_queue_size = 128;
    MATCH_PER_STREAM_OPT(max_muxing_queue_size, i, ost->max_muxing_queue_size, oc, st);
    ost->max_muxing_queue_size *= sizeof(AVPacket);

    /*编码器设置全局头，一般推rtmp都会默认设置*/
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ost->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /*拷贝对应的字典到OutputStream，编解码器选项的字段在上面的filter_codec_opts处理了*/
    av_dict_copy(&ost->sws_dict, o->g->sws_dict, 0);

    av_dict_copy(&ost->swr_opts, o->g->swr_opts, 0);
    if (ost->enc && av_get_exact_bits_per_sample(ost->enc->id) == 24)
        av_dict_set(&ost->swr_opts, "output_sample_bits", "24", 0);

    av_dict_copy(&ost->resample_opts, o->g->resample_opts, 0);

    ost->source_index = source_index;// 输入文件对应的流下标.视频时，该流下标就是对应该文件中的视频流下标
    if (source_index >= 0) {
        ost->sync_ist = input_streams[source_index];//要同步的输入流，这里看到，输出流中会有成员指向对应输入流的信息
        input_streams[source_index]->discard = 0;
        input_streams[source_index]->st->discard = input_streams[source_index]->user_set_discard;
    }
    ost->last_mux_dts = AV_NOPTS_VALUE;

    /*给复用队列开辟内存*/
    ost->muxing_queue = av_fifo_alloc(8 * sizeof(AVPacket));
    if (!ost->muxing_queue)
        exit_program(1);

    return ost;
}

static void parse_matrix_coeffs(uint16_t *dest, const char *str)
{
    int i;
    const char *p = str;
    for (i = 0;; i++) {
        dest[i] = atoi(p);
        if (i == 63)
            break;
        p = strchr(p, ',');
        if (!p) {
            av_log(NULL, AV_LOG_FATAL, "Syntax error in matrix \"%s\" at coeff %d\n", str, i);
            exit_program(1);
        }
        p++;
    }
}

/**
 * @brief 简单了解一下即可
*/
/* read file contents into a string */
static uint8_t *read_file(const char *filename)
{
    AVIOContext *pb      = NULL;
    AVIOContext *dyn_buf = NULL;

    /**
    *avio_open：创建并初始化AVIOContext用于访问url表示的资源。
    @note 当url表示的资源以 读+写 方式打开后，AVIOContext只能用于写。
    @param s 用于返回创建的AVIOContext的指针。在失败的情况下，指向的值被设置为NULL。
    @param url url.
    @param flags 控制url所指示的资源如何打开的标志
    * @return >= 0表示成功，如果失败则为一个负值，对应一个AVERROR代码
    */
    /*1.只读方式打开*/
    int ret = avio_open(&pb, filename, AVIO_FLAG_READ);
    uint8_t buf[1024], *str;
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error opening file %s.\n", filename);
        return NULL;
    }

    /**
     * avio_open_dyn_buf：Open a write only memory stream.
     *
     * @param s new IO context
     * @return zero if no error.
     */
    /*2.打开一个只写的内存流*/
    ret = avio_open_dyn_buf(&dyn_buf);
    if (ret < 0) {
        avio_closep(&pb);
        return NULL;
    }

    /**
     * avio_read：Read size bytes from AVIOContext into buf.
     * @return number of bytes read or AVERROR
     */
    /*3.从pb中读sizeof(buf)字节到buf中后，ret是实际读到的字节数，然后将读到的实际字节数，写进dyn_buf*/
    while ((ret = avio_read(pb, buf, sizeof(buf))) > 0)
        avio_write(dyn_buf, buf, ret);
    avio_w8(dyn_buf, 0);
    avio_closep(&pb);

    /*4.关闭dyn_buf并得到读到的内容，str指向该内容*/
    ret = avio_close_dyn_buf(dyn_buf, &str);
    if (ret < 0)
        return NULL;
    return str;
}

/**
 * @brief 获取输出流对应过滤器字符串的描述。
 * @param o 参数上下文
 * @param oc 输出文件上下文
 * @param ost 输出流封装结构体
 * @return 成功=得到对应的字符串描述，注意没有设置过滤器描述时，会返回"null"或者"anull"，这也是成功的； 失败=程序退出
*/
static char *get_ost_filters(OptionsContext *o, AVFormatContext *oc,
                             OutputStream *ost)
{
    AVStream *st = ost->st;

    /*1.同时设置-filter and -filter_scrip会报错，程序退出*/
    if (ost->filters_script && ost->filters) {
        av_log(NULL, AV_LOG_ERROR, "Both -filter and -filter_script set for "
               "output stream #%d:%d.\n", nb_output_files, st->index);
        exit_program(1);
    }

    /*2.若过滤器描述是使用脚本，则读取里面的内容进行返回*/
    if (ost->filters_script)
        return read_file(ost->filters_script);
    else if (ost->filters)/*3若过滤器描述是使用字符串直接描述，则开辟内存后直接返回*/
        return av_strdup(ost->filters);

    /*4.若没设置过滤器描述，则返回对应媒体类型的空字符串描述*/
    return av_strdup(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ?
                     "null" : "anull");
}

/**
 * @brief 检测指定copy不转码时，是否使用了过滤器相关选项.
 * @param o 参数上下文
 * @param oc 输出文件上下文
 * @param ost 输出流封装结构体
 * @param type 媒体类型
 * @return void
 *
 * @note 若指定了copy，但还使用了过滤器相关功能，程序会直接退出
*/
static void check_streamcopy_filters(OptionsContext *o, AVFormatContext *oc,
                                     const OutputStream *ost, enum AVMediaType type)
{
    /*1.这里看到，若使用copy不转码，就无法再使用过滤器相关的功能*/
    if (ost->filters_script || ost->filters) {
        av_log(NULL, AV_LOG_ERROR,
               "%s '%s' was defined for %s output stream %d:%d but codec copy was selected.\n"
               "Filtering and streamcopy cannot be used together.\n",
               ost->filters ? "Filtergraph" : "Filtergraph script",
               ost->filters ? ost->filters : ost->filters_script,
               av_get_media_type_string(type), ost->file_index, ost->index);
        exit_program(1);
    }
}

/**
 * @brief 创建一个OutputStream，并将o中的参数转移到OutputStream中，以此初始化OutputStream.
 * @param o 参数上下文
 * @param oc 输出文件上下文
 * @param source_index 输入文件中视频流的下标
 * @return 成功=返回OutputStream*；失败=程序退出
*/
static OutputStream *new_video_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    AVStream *st;
    OutputStream *ost;
    AVCodecContext *video_enc;
    char *frame_rate = NULL, *frame_aspect_ratio = NULL;

    /*1.为视频创建一个输出流OutputStream，通过返回值返回.
    注，oc->streams同样会有该AVStream *st，即OutputStream与oc结构都保存了该AVStream *st.
    ost->st与oc->streams中的指向肯定不一样，因为前者是一级指针，后者是二级指针(可取oc->streams[0]看指向是一样的)
    */
    ost = new_output_stream(o, oc, AVMEDIA_TYPE_VIDEO, source_index);
    st  = ost->st;
    video_enc = ost->enc_ctx;

    /*获取帧率-r选项*/
    MATCH_PER_STREAM_OPT(frame_rates, str, frame_rate, oc, st);
    if (frame_rate && av_parse_video_rate(&ost->frame_rate, frame_rate) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid framerate value: %s\n", frame_rate);
        exit_program(1);
    }
    /*使用-vsync 0和-r会产生无效的输出文件，两者不能同时使用*/
    if (frame_rate && video_sync_method == VSYNC_PASSTHROUGH)
        av_log(NULL, AV_LOG_ERROR, "Using -vsync 0 and -r can produce invalid output files\n");

    /*对应参数"aspect"，设置指定的显示比例*/
    /*aspect可以是浮点数字的字符串，或一个字符串的比值，比值分别是纵横比的分子和分母。
     For example "4:3", "16:9", "1.3333", and "1.7777" are valid argument values.*/
    MATCH_PER_STREAM_OPT(frame_aspect_ratios, str, frame_aspect_ratio, oc, st);
    if (frame_aspect_ratio) {
        AVRational q;
        if (av_parse_ratio(&q, frame_aspect_ratio, 255, 0, NULL) < 0 ||
            q.num <= 0 || q.den <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid aspect ratio: %s\n", frame_aspect_ratio);
            exit_program(1);
        }
        ost->frame_aspect_ratio = q;
    }

    /*过滤器相关*/
    /*对应参数"filter_script"和"filter"*/
    MATCH_PER_STREAM_OPT(filter_scripts, str, ost->filters_script, oc, st);//new_audio_stream也会调用到
    MATCH_PER_STREAM_OPT(filters,        str, ost->filters,        oc, st);
    if (o->nb_filters > 1)
        av_log(NULL, AV_LOG_ERROR, "Only '-vf %s' read, ignoring remaining -vf options: Use ',' to separate filters\n", ost->filters);

    /*2.用户没有输入-xxx copy不转码，那么就转码*/
    if (!ost->stream_copy) {
        const char *p = NULL;
        char *frame_size = NULL;
        char *frame_pix_fmt = NULL;
        char *intra_matrix = NULL, *inter_matrix = NULL;
        char *chroma_intra_matrix = NULL;
        int do_pass = 0;
        int i;

        /*解析-s分辨率选项到编解码器上下文video_enc中*/
        MATCH_PER_STREAM_OPT(frame_sizes, str, frame_size, oc, st);
        if (frame_size && av_parse_video_size(&video_enc->width, &video_enc->height, frame_size) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
            exit_program(1);
        }

        /*对应参数"bits_per_raw_sample"，暂未研究*/
        video_enc->bits_per_raw_sample = frame_bits_per_raw_sample;
        /*对应参数"pix_fmt"，视频帧格式，暂未研究，但是平时比较常用*/
        MATCH_PER_STREAM_OPT(frame_pix_fmts, str, frame_pix_fmt, oc, st);
        if (frame_pix_fmt && *frame_pix_fmt == '+') {
            ost->keep_pix_fmt = 1;
            if (!*++frame_pix_fmt)
                frame_pix_fmt = NULL;
        }
        if (frame_pix_fmt && (video_enc->pix_fmt = av_get_pix_fmt(frame_pix_fmt)) == AV_PIX_FMT_NONE) {
            av_log(NULL, AV_LOG_FATAL, "Unknown pixel format requested: %s.\n", frame_pix_fmt);
            exit_program(1);
        }
        /*video_enc->sample_aspect_ratio参数暂未研究.默认值是{0,1}*/
        st->sample_aspect_ratio = video_enc->sample_aspect_ratio;

        //对应参数"intra"，“-g 1”，只有I帧
        if (intra_only)
            video_enc->gop_size = 0;//video_enc->gop_size默认是-1

        /*以下三个矩阵相关的参数暂未研究.推流没用到，读者有兴趣可自行研究*/
        //对应参数"intra_matrix"
        MATCH_PER_STREAM_OPT(intra_matrices, str, intra_matrix, oc, st);
        if (intra_matrix) {
            if (!(video_enc->intra_matrix = av_mallocz(sizeof(*video_enc->intra_matrix) * 64))) {
                av_log(NULL, AV_LOG_FATAL, "Could not allocate memory for intra matrix.\n");
                exit_program(1);
            }
            parse_matrix_coeffs(video_enc->intra_matrix, intra_matrix);
        }
        //对应参数"chroma_intra_matrix"
        MATCH_PER_STREAM_OPT(chroma_intra_matrices, str, chroma_intra_matrix, oc, st);
        if (chroma_intra_matrix) {
            uint16_t *p = av_mallocz(sizeof(*video_enc->chroma_intra_matrix) * 64);
            if (!p) {
                av_log(NULL, AV_LOG_FATAL, "Could not allocate memory for intra matrix.\n");
                exit_program(1);
            }
            video_enc->chroma_intra_matrix = p;
            parse_matrix_coeffs(p, chroma_intra_matrix);
        }
        //对应参数"inter_matrix"
        MATCH_PER_STREAM_OPT(inter_matrices, str, inter_matrix, oc, st);
        if (inter_matrix) {
            if (!(video_enc->inter_matrix = av_mallocz(sizeof(*video_enc->inter_matrix) * 64))) {
                av_log(NULL, AV_LOG_FATAL, "Could not allocate memory for inter matrix.\n");
                exit_program(1);
            }
            parse_matrix_coeffs(video_enc->inter_matrix, inter_matrix);
        }

        /*rc_overrides参数暂未研究.推流没用到*/
        //对应参数"rc_override"
        /*-rc_override[:stream_specifier] override (output,per-stream)
        速率控制，覆盖指定的时间间隔，以'逗号分隔的int,int,int'列表格式。
        前两个值是开始和结束的帧号，最后一个如果是整数，表示用量。如果是负数表示品质因素*/
        MATCH_PER_STREAM_OPT(rc_overrides, str, p, oc, st);
        for (i = 0; p; i++) {
            int start, end, q;
            int e = sscanf(p, "%d,%d,%d", &start, &end, &q);
            if (e != 3) {
                av_log(NULL, AV_LOG_FATAL, "error parsing rc_override\n");
                exit_program(1);
            }
            video_enc->rc_override =
                av_realloc_array(video_enc->rc_override,
                                 i + 1, sizeof(RcOverride));
            if (!video_enc->rc_override) {
                av_log(NULL, AV_LOG_FATAL, "Could not (re)allocate memory for rc_override.\n");
                exit_program(1);
            }
            video_enc->rc_override[i].start_frame = start;
            video_enc->rc_override[i].end_frame   = end;
            if (q > 0) {
                video_enc->rc_override[i].qscale         = q;
                video_enc->rc_override[i].quality_factor = 1.0;
            }
            else {
                video_enc->rc_override[i].qscale         = 0;
                video_enc->rc_override[i].quality_factor = -q/100.0;
            }
            p = strchr(p, '/');
            if (p) p++;
        }
        video_enc->rc_override_count = i;//没设置一般默认是0

        /*是否要或上AV_CODEC_FLAG_PSNR宏*/
        //对应参数"psnr"，计算压缩的帧PSNR，表示视频的质量
        if (do_psnr)
            video_enc->flags|= AV_CODEC_FLAG_PSNR;

        /* two pass mode */
        /*do_passs参数暂未研究.推流没用到*/
        MATCH_PER_STREAM_OPT(pass, i, do_pass, oc, st);
        if (do_pass) {
            if (do_pass & 1) {
                video_enc->flags |= AV_CODEC_FLAG_PASS1;
                av_dict_set(&ost->encoder_opts, "flags", "+pass1", AV_DICT_APPEND);
            }
            if (do_pass & 2) {
                video_enc->flags |= AV_CODEC_FLAG_PASS2;
                av_dict_set(&ost->encoder_opts, "flags", "+pass2", AV_DICT_APPEND);
            }
        }

        /*passlogfile参数暂未研究.推流没用到*/
        //对应参数“passlogfile”
        MATCH_PER_STREAM_OPT(passlogfiles, str, ost->logfile_prefix, oc, st);
        if (ost->logfile_prefix &&
            !(ost->logfile_prefix = av_strdup(ost->logfile_prefix)))
            exit_program(1);

        /*do_passs参数暂未研究.推流没用到*/
        if (do_pass) {
            char logfilename[1024];
            FILE *f;

            // 拼接日志文件名
            snprintf(logfilename, sizeof(logfilename), "%s-%d.log",
                     ost->logfile_prefix ? ost->logfile_prefix :
                                           DEFAULT_PASS_LOGFILENAME_PREFIX,
                     i);
            if (!strcmp(ost->enc->name, "libx264")) {
                /*若编码器是libx264,重写stats=logfilename*/
                av_dict_set(&ost->encoder_opts, "stats", logfilename, AV_DICT_DONT_OVERWRITE);
            } else {
                if (video_enc->flags & AV_CODEC_FLAG_PASS2) {
                    char  *logbuffer = read_file(logfilename);

                    if (!logbuffer) {
                        av_log(NULL, AV_LOG_FATAL, "Error reading log file '%s' for pass-2 encoding\n",
                               logfilename);
                        exit_program(1);
                    }
                    video_enc->stats_in = logbuffer;
                }
                if (video_enc->flags & AV_CODEC_FLAG_PASS1) {
                    /*打开文件并保存该文件句柄到ost->logfile*/
                    f = av_fopen_utf8(logfilename, "wb");
                    if (!f) {
                        av_log(NULL, AV_LOG_FATAL,
                               "Cannot write log file '%s' for pass-1 encoding: %s\n",
                               logfilename, strerror(errno));
                        exit_program(1);
                    }
                    ost->logfile = f;
                }
            }
        }

        /*检测是否有强制关键帧参数，若有则需要重新开辟一份内存.
        因为未开辟时，ost->forced_keyframes指向的是o中的内存.参数暂未研究.*/
        //对应参数"force_key_frames"，
        /*
        ‘-force_key_frames[:stream_specifier] time[,time...] (output,per-stream)’
        ‘-force_key_frames[:stream_specifier] expr:expr (output,per-stream)’在指定的时间戳强制关键帧
        */
        MATCH_PER_STREAM_OPT(forced_key_frames, str, ost->forced_keyframes, oc, st);
        if (ost->forced_keyframes)
            ost->forced_keyframes = av_strdup(ost->forced_keyframes);

        /*该参数暂未研究.*/
        //对应参数"force_fps"，强制设置视频帧率
        MATCH_PER_STREAM_OPT(force_fps, i, ost->force_fps, oc, st);

        /*该参数暂未研究.*/
        //对应参数"top"
        ost->top_field_first = -1;
        MATCH_PER_STREAM_OPT(top_field_first, i, ost->top_field_first, oc, st);

        /*get_ost_filters流程很简单*/
        ost->avfilter = get_ost_filters(o, oc, ost);
        if (!ost->avfilter)
            exit_program(1);
    } else {
        /*未研究过该参数*/
        //流拷贝，不需要重新编码的处理
        //对应参数"copyinkf"，复制初始非关键帧
        MATCH_PER_STREAM_OPT(copy_initial_nonkeyframes, i, ost->copy_initial_nonkeyframes, oc ,st);
    }

    /*3.不转码流程*/
    if (ost->stream_copy)
        check_streamcopy_filters(o, oc, ost, AVMEDIA_TYPE_VIDEO);

    return ost;
}

static OutputStream *new_audio_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    int n;
    AVStream *st;
    OutputStream *ost;
    AVCodecContext *audio_enc;

    /*1.根据输入音频流创建一个输出音频流*/
    ost = new_output_stream(o, oc, AVMEDIA_TYPE_AUDIO, source_index);
    st  = ost->st;

    audio_enc = ost->enc_ctx;
    audio_enc->codec_type = AVMEDIA_TYPE_AUDIO;

    MATCH_PER_STREAM_OPT(filter_scripts, str, ost->filters_script, oc, st);
    MATCH_PER_STREAM_OPT(filters,        str, ost->filters,        oc, st);
    if (o->nb_filters > 1)
        av_log(NULL, AV_LOG_ERROR, "Only '-af %s' read, ignoring remaining -af options: Use ',' to separate filters\n", ost->filters);

    /*2.若转码，则初始化音频流的流程*/
    /*若指定-vcodec libx264，音频也会进来这里(因为视频使stream_copy=0了)，但实际不会音频没有太多实际的代码运行.
    可以额外添加对应通道、采样格式、采样率的参数*/
    /*例如可以在推流命令基础上添加：-ar 48000 -ac 2 -sample_fmt s16p。
    但是注意sample_fmt选项，可以通过ffmpegC -sample_fmts查看ffmpeg支持的采样格式.
    例如笔者查看后，ffmpeg是支持s16格式的，将上面s16p改成s16会报错，原因是笔者的输入文件的音频是mp3，而ffmpeg会用到libmp3lame这个编解码库，
    它是不支持s16格式的，所以会报错：Specified sample format s16 is invalid or not supported*/
    if (!ost->stream_copy) {
        /*音频转码无非就是音频三元组（采样率，采样大小和通道数）的改变.采样大小也叫采样位数(采样格式)*/
        char *sample_fmt = NULL;

        /*-ac通道号选项*/
        MATCH_PER_STREAM_OPT(audio_channels, i, audio_enc->channels, oc, st);

        /*-sample_fmt采样格式选项，例如s16p*/
        MATCH_PER_STREAM_OPT(sample_fmts, str, sample_fmt, oc, st);
        if (sample_fmt &&
            (audio_enc->sample_fmt = av_get_sample_fmt(sample_fmt)) == AV_SAMPLE_FMT_NONE) {
            av_log(NULL, AV_LOG_FATAL, "Invalid sample format '%s'\n", sample_fmt);
            exit_program(1);
        }

        /*-ar采样率选项*/
        MATCH_PER_STREAM_OPT(audio_sample_rate, i, audio_enc->sample_rate, oc, st);

        MATCH_PER_STREAM_OPT(apad, str, ost->apad, oc, st);
        ost->apad = av_strdup(ost->apad);

        /*获取音视频过滤器描述*/
        ost->avfilter = get_ost_filters(o, oc, ost);
        if (!ost->avfilter)
            exit_program(1);

        /* check for channel mapping for this audio stream */
        for (n = 0; n < o->nb_audio_channel_maps; n++) {
            /*map参数暂未研究，后续补充，推流没用到*/
            AudioChannelMap *map = &o->audio_channel_maps[n];/*从获取输入的channel mapping数组获取一个*/
            if ((map->ofile_idx   == -1 || ost->file_index == map->ofile_idx) &&
                (map->ostream_idx == -1 || ost->st->index  == map->ostream_idx)) {
                InputStream *ist;

                if (map->channel_idx == -1) {
                    ist = NULL;
                } else if (ost->source_index < 0) {
                    av_log(NULL, AV_LOG_FATAL, "Cannot determine input stream for channel mapping %d.%d\n",
                           ost->file_index, ost->st->index);
                    continue;
                } else {
                    ist = input_streams[ost->source_index];
                }

                if (!ist || (ist->file_index == map->file_idx && ist->st->index == map->stream_idx)) {
                    if (av_reallocp_array(&ost->audio_channels_map,
                                          ost->audio_channels_mapped + 1,
                                          sizeof(*ost->audio_channels_map)
                                          ) < 0 )
                        exit_program(1);

                    ost->audio_channels_map[ost->audio_channels_mapped++] = map->channel_idx;
                }
            }
        }
    }//<== if (!ost->stream_copy) end ==>

    /*3.若不转码，则初始化音频流的流程比较简单*/
    if (ost->stream_copy)
        check_streamcopy_filters(o, oc, ost, AVMEDIA_TYPE_AUDIO);

    return ost;
}

static OutputStream *new_data_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    OutputStream *ost;

    ost = new_output_stream(o, oc, AVMEDIA_TYPE_DATA, source_index);
    if (!ost->stream_copy) {
        av_log(NULL, AV_LOG_FATAL, "Data stream encoding not supported yet (only streamcopy)\n");
        exit_program(1);
    }

    return ost;
}

static OutputStream *new_unknown_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    OutputStream *ost;

    ost = new_output_stream(o, oc, AVMEDIA_TYPE_UNKNOWN, source_index);
    if (!ost->stream_copy) {
        av_log(NULL, AV_LOG_FATAL, "Unknown stream encoding not supported yet (only streamcopy)\n");
        exit_program(1);
    }

    return ost;
}

static OutputStream *new_attachment_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    OutputStream *ost = new_output_stream(o, oc, AVMEDIA_TYPE_ATTACHMENT, source_index);
    ost->stream_copy = 1;
    ost->finished    = 1;
    return ost;
}

/**
 * @brief 创建一个输出字幕流。
 * @param o
 * @param oc
 * @param source_index
 */
static OutputStream *new_subtitle_stream(OptionsContext *o, AVFormatContext *oc, int source_index)
{
    AVStream *st;
    OutputStream *ost;
    AVCodecContext *subtitle_enc;

    /*1.利用输入字幕流创建一个输出字幕流*/
    ost = new_output_stream(o, oc, AVMEDIA_TYPE_SUBTITLE, source_index);
    st  = ost->st;
    subtitle_enc = ost->enc_ctx;

    subtitle_enc->codec_type = AVMEDIA_TYPE_SUBTITLE;

    MATCH_PER_STREAM_OPT(copy_initial_nonkeyframes, i, ost->copy_initial_nonkeyframes, oc, st);

    /*2.若转码，则进入if*/
    if (!ost->stream_copy) {
        char *frame_size = NULL;

        /*这里看到，若输入了-s选项，字幕也会使用*/
        MATCH_PER_STREAM_OPT(frame_sizes, str, frame_size, oc, st);
        if (frame_size && av_parse_video_size(&subtitle_enc->width, &subtitle_enc->height, frame_size) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid frame size: %s.\n", frame_size);
            exit_program(1);
        }
    }

    return ost;
}

/* arg format is "output-stream-index:streamid-value". */
static int opt_streamid(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    int idx;
    char *p;
    char idx_str[16];

    av_strlcpy(idx_str, arg, sizeof(idx_str));
    p = strchr(idx_str, ':');
    if (!p) {
        av_log(NULL, AV_LOG_FATAL,
               "Invalid value '%s' for option '%s', required syntax is 'index:value'\n",
               arg, opt);
        exit_program(1);
    }
    *p++ = '\0';
    idx = parse_number_or_die(opt, idx_str, OPT_INT, 0, MAX_STREAMS-1);
    o->streamid_map = grow_array(o->streamid_map, sizeof(*o->streamid_map), &o->nb_streamid_map, idx+1);
    o->streamid_map[idx] = parse_number_or_die(opt, p, OPT_INT, 0, INT_MAX);
    return 0;
}

static int copy_chapters(InputFile *ifile, OutputFile *ofile, int copy_metadata)
{
    AVFormatContext *is = ifile->ctx;
    AVFormatContext *os = ofile->ctx;
    AVChapter **tmp;
    int i;

    tmp = av_realloc_f(os->chapters, is->nb_chapters + os->nb_chapters, sizeof(*os->chapters));
    if (!tmp)
        return AVERROR(ENOMEM);
    os->chapters = tmp;

    for (i = 0; i < is->nb_chapters; i++) {
        AVChapter *in_ch = is->chapters[i], *out_ch;
        int64_t start_time = (ofile->start_time == AV_NOPTS_VALUE) ? 0 : ofile->start_time;
        int64_t ts_off   = av_rescale_q(start_time - ifile->ts_offset,
                                       AV_TIME_BASE_Q, in_ch->time_base);
        int64_t rt       = (ofile->recording_time == INT64_MAX) ? INT64_MAX :
                           av_rescale_q(ofile->recording_time, AV_TIME_BASE_Q, in_ch->time_base);


        if (in_ch->end < ts_off)
            continue;
        if (rt != INT64_MAX && in_ch->start > rt + ts_off)
            break;

        out_ch = av_mallocz(sizeof(AVChapter));
        if (!out_ch)
            return AVERROR(ENOMEM);

        out_ch->id        = in_ch->id;
        out_ch->time_base = in_ch->time_base;
        out_ch->start     = FFMAX(0,  in_ch->start - ts_off);
        out_ch->end       = FFMIN(rt, in_ch->end   - ts_off);

        if (copy_metadata)
            av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);

        os->chapters[os->nb_chapters++] = out_ch;
    }
    return 0;
}

static void init_output_filter(OutputFilter *ofilter, OptionsContext *o,
                               AVFormatContext *oc)
{
    OutputStream *ost;

    switch (ofilter->type) {
    case AVMEDIA_TYPE_VIDEO: ost = new_video_stream(o, oc, -1); break;
    case AVMEDIA_TYPE_AUDIO: ost = new_audio_stream(o, oc, -1); break;
    default:
        av_log(NULL, AV_LOG_FATAL, "Only video and audio filters are supported "
               "currently.\n");
        exit_program(1);
    }

    ost->source_index = -1;
    ost->filter       = ofilter;

    ofilter->ost      = ost;
    ofilter->format   = -1;

    if (ost->stream_copy) {
        av_log(NULL, AV_LOG_ERROR, "Streamcopy requested for output stream %d:%d, "
               "which is fed from a complex filtergraph. Filtering and streamcopy "
               "cannot be used together.\n", ost->file_index, ost->index);
        exit_program(1);
    }

    if (ost->avfilter && (ost->filters || ost->filters_script)) {
        const char *opt = ost->filters ? "-vf/-af/-filter" : "-filter_script";
        av_log(NULL, AV_LOG_ERROR,
               "%s '%s' was specified through the %s option "
               "for output stream %d:%d, which is fed from a complex filtergraph.\n"
               "%s and -filter_complex cannot be used together for the same stream.\n",
               ost->filters ? "Filtergraph" : "Filtergraph script",
               ost->filters ? ost->filters : ost->filters_script,
               opt, ost->file_index, ost->index, opt);
        exit_program(1);
    }

    avfilter_inout_free(&ofilter->out_tmp);
}

static int init_complex_filters(void)
{
    int i, ret = 0;

    for (i = 0; i < nb_filtergraphs; i++) {
        ret = init_complex_filtergraph(filtergraphs[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int open_output_file(OptionsContext *o, const char *filename)
{
    AVFormatContext *oc;
    int i, j, err;
    OutputFile *of;
    OutputStream *ost;
    InputStream  *ist;
    AVDictionary *unused_opts = NULL;
    AVDictionaryEntry *e = NULL;
    int format_flags = 0;

    //o->stop_time对应参数"-to"，o->recording_time对应参数"t"
    if (o->stop_time != INT64_MAX && o->recording_time != INT64_MAX) {
        o->stop_time = INT64_MAX;
        av_log(NULL, AV_LOG_WARNING, "-t and -to cannot be used together; using -t.\n");
    }

    if (o->stop_time != INT64_MAX && o->recording_time == INT64_MAX) {
        //o->start_time对应参数"-ss"
        int64_t start_time = o->start_time == AV_NOPTS_VALUE ? 0 : o->start_time;
        if (o->stop_time <= start_time) {
            av_log(NULL, AV_LOG_ERROR, "-to value smaller than -ss; aborting.\n");
            exit_program(1);
        } else {
            o->recording_time = o->stop_time - start_time;
        }
    }

    /*1.为一个输出文件开辟内存，并进行一些必要的初始化*/
    GROW_ARRAY(output_files, nb_output_files);// 开辟sizeof(OutputFile*)大小的内存，nb_output_files自动+1
    of = av_mallocz(sizeof(*of));//开辟OutputFile结构体
    if (!of)
        exit_program(1);
    output_files[nb_output_files - 1] = of;

    /*对of赋值*/
    of->ost_index      = nb_output_streams;         // 保存首个输出流的流下标
    of->recording_time = o->recording_time;
    of->start_time     = o->start_time;
    of->limit_filesize = o->limit_filesize;
    of->shortest       = o->shortest;               // -shortest选项
    av_dict_copy(&of->opts, o->g->format_opts, 0);  // 解复用选项.这里看到输出会保留一份解复用字典，
                                                    // 而输入直接在avformat_open_input就使用，不会保存到InputFile

    if (!strcmp(filename, "-"))
        filename = "pipe:";

    // 2. 给输出文件开辟解复用上下文.
    // o->format就是-f选项的值,例如-f flv，那么o->format="flv"
    err = avformat_alloc_output_context2(&oc, NULL, o->format, filename);
    if (!oc) {
        print_error(filename, err);
        exit_program(1);
    }

    of->ctx = oc;// 保存解复用的上下文
    if (o->recording_time != INT64_MAX)
        oc->duration = o->recording_time;// 设置流的时长.

    oc->interrupt_callback = int_cb;

    // 判断用户是否设置了fflags解复用选项.这里要具体看懂，还是得去看av_opt_find+av_opt_eval_flags的源码
    e = av_dict_get(o->g->format_opts, "fflags", NULL, 0);
    if (e) {
        // 在oc中的AVClass查找fflags.注：ffmpeg的第一个成员都是AVClass
        // 所以传oc就是等价于传AVClass.
        const AVOption *o = av_opt_find(oc, "fflags", NULL, 0, 0);
        /*
            这组函数可用于评估选项字符串并从中获取数字。
            它们与av_opt_set（）执行相同的操作，除了将结果写入到调用者提供的指针中。
            参数：obj：一个结构体，其第一个元素是指向AVClass的指针。
           o：要评估字符串的选项。
           val：要评估的字符串。
           *_val:字符串的值将写在这里。
            返回：0成功，负数为失败。
        */
        av_opt_eval_flags(oc, o, e->value, &format_flags);// 这个函数实现在源码找不到
    }
    if (o->bitexact) {
        format_flags |= AVFMT_FLAG_BITEXACT;
        oc->flags    |= AVFMT_FLAG_BITEXACT;
    }

    /* 3. create streams for all unlabeled output pads */
    /* 滤镜相关.
        参数"filter", "filter_script", "reinit_filter", "filter_complex",
        "lavfi", "filter_complex_script"会涉及到此处处理*/
    // 推流未使用到，后续再研究.
    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        for (j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *ofilter = fg->outputs[j];

            if (!ofilter->out_tmp || ofilter->out_tmp->name)
                continue;

            switch (ofilter->type) {
            case AVMEDIA_TYPE_VIDEO:    o->video_disable    = 1; break;
            case AVMEDIA_TYPE_AUDIO:    o->audio_disable    = 1; break;
            case AVMEDIA_TYPE_SUBTITLE: o->subtitle_disable = 1; break;
            }
            init_output_filter(ofilter, o, oc);
        }
    }

    /*4.对应参数"-map".*/
    /*1.不传map参数时，默认nb_stream_maps=0，传时，则不为0*/
    /*例如-y -i 1.mkv -map 0:0 -map 0:1 -map 0:2 -c:v libx264 -c:a:0 aac -b:a:0 128k -c:s mov_text map.mp4
    备注，mp4不支持ass、srt这些类型的字幕，需要转成mov_text，不想转则将输出格式mp4换成mkv即可.*/
    /*map相关可参考 https://blog.csdn.net/m0_60259116/article/details/125642026*/
    if (!o->nb_stream_maps) {
        char *subtitle_codec_name = NULL;

        /* pick the "best" stream of each type */

        /*4.1. 若存在多个输入视频流，则选择一个最好的视频流，来创建新的输出视频流*/
        /* video: highest resolution */
        // 没有禁用视频，那么就去猜测编解码器id,依赖oc->oformat->name(-f参数，例如flv)以及文件名，且猜测的id不能是AV_CODEC_ID_NONE.
        // 视频通过av_guess_codec处理后，看源码得出，视频一般返回的是AVOutputFormat->video_codec
        if (!o->video_disable && av_guess_codec(oc->oformat, NULL, filename, NULL, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
            int area = 0, idx = -1;
            /*
             * 测试给定的容器是否可以存储该编解码器(该函数不用看源码，虽然不复杂，但是有回调函数的判断，且该函数意思可以直接看懂)。
             * @param ofmt 用来检查兼容性的容器
             * @param codec_id 可能存储在容器中的编解码器
             * @param std_compliance 标准遵从级别，FF_COMPLIANCE_*之一
             * @return 如果codec_id的编解码器可以存储在ofmt中，则返回1，如果不能，则返回0。如果该信息不可用，则为负数。
            */
            int qcr = avformat_query_codec(oc->oformat, oc->oformat->video_codec, 0);

            for (i = 0; i < nb_input_streams; i++) {
                int new_area;
                ist = input_streams[i];
                // codec_info_nb_frames:avformat_find_stream_info()过程中被解复用的帧数
                // disposition:?
                // 单目运算符即逻辑非! 是比 算术运算符乘号* 优先级高的。see https://blog.csdn.net/sgbl888/article/details/123997358
                // auto tyycode = !ist->st->codec_info_nb_frames;
                // auto tyycode1 = !!ist->st->codec_info_nb_frames;
                // 100000000*!!ist->st->codec_info_nb_frames意思：codec_info_nb_frames为非零，取值100000000；为零，取值0.
                // 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT)意思：含有AV_DISPOSITION_DEFAULT，取值5000000；不含，取值0.
                new_area = ist->st->codecpar->width * ist->st->codecpar->height + 100000000*!!ist->st->codec_info_nb_frames
                           + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);/*new_area的作用与音频的score一样，请看下面音频的解释*/
                if (ist->user_set_discard == AVDISCARD_ALL)// 若用户要丢弃该输入流，那么输出流则跳过不处理
                    continue;

                // 包含附属图，new_area标记为1
                // 在这里qcr肯定不等于MKTAG('A', 'P', 'I', 'C')
                if((qcr!=MKTAG('A', 'P', 'I', 'C')) && (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                    new_area = 1;

                /* MKTAG宏示例：
                 * ascii码：'A'=65,'P'=80,'I'=73,'C'=67*/
                // 01000001 | (01010000 << 8) | (01001001 << 16) | (01000011 << 24)
                if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    new_area > area) {
                    // 这里qcr必定不等于MKTAG('A', 'P', 'I', 'C')，故不会进入(这种特殊的视频比较少见，大家可以自行研究)
                    if((qcr==MKTAG('A', 'P', 'I', 'C')) && !(ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                        continue;

                    // 视频正常往这里走
                    area = new_area;
                    idx = i;
                }
            }//<== for (i = 0; i < nb_input_streams; i++) end ==>

            if (idx >= 0)
                new_video_stream(o, oc, idx);//idx是视频流的下标.不需要处理返回值是因为，oc中也保存着一份新创建的流的地址.即oc->streams[]二级指针中

        }//<== if (!o->video_disable && av_guess_codec(oc->oformat, NULL, filename, NULL, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) end ==>

        /*4.2. 若存在多个输入音频流，则选择一个最好的音频流，来创建新的输出音频流*/
        /*进入这里要把-an去掉(输入输出文件都去掉)，且输入文件要有音频*/
        /* audio: most channels */
        enum AVCodecID tyycode1 = av_guess_codec(oc->oformat, NULL, filename, NULL, AVMEDIA_TYPE_AUDIO);//例如0X15001是AV_CODEC_ID_MP3
        if (!o->audio_disable && av_guess_codec(oc->oformat, NULL, filename, NULL, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
            int best_score = 0, idx = -1;/*best_score用于保存输入音频流得分最高的分数，从而选择最好的输入音频流*/
            for (i = 0; i < nb_input_streams; i++) {
                int score;
                ist = input_streams[i];
                score = ist->st->codecpar->channels + 100000000*!!ist->st->codec_info_nb_frames
                        + 5000000*!!(ist->st->disposition & AV_DISPOSITION_DEFAULT);
                if (ist->user_set_discard == AVDISCARD_ALL)
                    continue;

                /*从这里看到，socre的作用是：
                 * 当输入文件存在多个音频流，会对比各个音频流的得分，选择得分最高的音频流作为输入文件的音频流。
                 * 得分规则：
                 * 1)默认加上ist->st->codecpar->channels通道数的分数。
                 * 2)codec_info_nb_frames不为0：加10^8分.
                 * 3)disposition存在AV_DISPOSITION_DEFAULT宏：加5x10^6分.
                 * 上面看到，存在codec_info_nb_frames优先选择的，其次是AV_DISPOSITION_DEFAULT，若都不存在，则考虑通道数。
                  */
                if (ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                    score > best_score) {
                    best_score = score;
                    /*这里看到，若存在多个输入音频流，idx只会保存最后一个有效的音频流下标*/
                    idx = i;
                }
            }
            if (idx >= 0)
                new_audio_stream(o, oc, idx);
        }
        /*所以经过上面两步，ffmpeg为你选择了最好的视频流+音频流来创建输出文件*/


        /*4.3. 为字幕流开辟输出流，若存在多个字幕流，ffmpeg只会取第一个字幕流*/
        /*字幕相关只是简单分析，不会深入研究*/
        /* subtitles: pick first(选择第一个字幕流) */
        /*获取用户指定的字幕编解码器名字*/
        MATCH_PER_TYPE_OPT(codec_names, str, subtitle_codec_name, oc, "s");
        AVCodec * tyycodeSc = avcodec_find_encoder(oc->oformat->subtitle_codec);
        /*这个if条件很简单，1.不管有无字幕，都先判断有无禁用该字幕；2.无禁用，则再判断输入文件是否包含字幕流.*/
        /*这里可以在推流命令基础添加：-scodec text.注意，因为我们推rtmp，-f是指定为flv，而在ffmpeg中的flv是不支持text以外的编解码器，
        所以当你-scodec ass指定字幕编解码器为ass，会报错：Subtitle codec '%s' for stream %d is not compatible with FLV.
        这是笔者从ffmpeg源码分析得出的.
        不过有个奇怪的点，当用基础推流命令加上-scodec text，vlc有画面输出(没字幕)，但ffplay播放rtmp却只能显示音频图，而没画面输出，
        播flv、hls则没问题，流媒体服务器是srs.
        笔者找到原因是加上了-scodec text，去掉后ffplay播放重新有画面.根本原因有兴趣的可以自行看ffplay源码分析.这里是为了使流程往下走*/
        if (!o->subtitle_disable && (avcodec_find_encoder(oc->oformat->subtitle_codec) || subtitle_codec_name)) {
            for (i = 0; i < nb_input_streams; i++)
                /*1.判断该输入流是否字幕流*/
                if (input_streams[i]->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    /*1.1利用输入的编解码器id获取输入描述符*/
                    AVCodecDescriptor const *input_descriptor =
                        avcodec_descriptor_get(input_streams[i]->st->codecpar->codec_id);
                    AVCodecDescriptor const *output_descriptor = NULL;

                    AVCodec const *output_codec =
                        avcodec_find_encoder(oc->oformat->subtitle_codec);
                    int input_props = 0, output_props = 0;

                    /*如果流数据的user_set_discard设为AVDISCARD_ALL，那么该流被丢弃，不做处理*/
                    if (input_streams[i]->user_set_discard == AVDISCARD_ALL)
                        continue;

                    /*1.2利用输出的编解码器id获取输出描述符*/
                    if (output_codec)
                        output_descriptor = avcodec_descriptor_get(output_codec->id);

                    /*1.3获取输入以及输出的属性，保存到局部变量中*/
                    /*注，与上这两个宏目的是为了清除其它宏的标志位*/
                    if (input_descriptor)
                        input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
                    if (output_descriptor)
                        output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);

                    /*1.4用户指定字幕编解码器(且输入文件存在字幕即上面的if条件)，就会new一个字幕流*/
                    /* &&、||、!即与或非，相同的逻辑表达式时，求值顺序是从左往右；不同的逻辑运算符时，非最高，然后是&&，再到||*/
                    /*int a = TRUE && !FALSE;// 1
                    int a1 = TRUE && (!FALSE);// 1 故通过a、a1的测试得出，当存在&&以及!运算时，!的优先级比&&高
                    int x = TRUE || TRUE && FALSE;// 1
                    int x1 = TRUE || (TRUE && FALSE);// 1
                    int x2 = (TRUE || TRUE) && FALSE;// 0 故通过x、x1、x2的测试得出，当存在||以及&&运算时，&&的优先级比||高
                    */
                    /*int tyycodeSub = ((subtitle_codec_name || input_props & output_props) ||
                            ((input_descriptor && output_descriptor) &&
                            (!input_descriptor->props ||
                             !output_descriptor->props)));*///等价于下面的if语句.

                    if (subtitle_codec_name ||/*用户指定字幕编解码器*/
                        input_props & output_props ||/*input_props & output_props代表：它们是否都同时至少具有一个宏AV_CODEC_PROP_TEXT_SUB或者AV_CODEC_PROP_BITMAP_SUB*/
                        // Map dvb teletext which has neither property to any output subtitle encoder
                        input_descriptor && output_descriptor && /*输入输出描述符都存在*/
                        (!input_descriptor->props ||
                         !output_descriptor->props))//注意，当output_descriptor为空不会来到这里，所以不会存在段错误
                    {
                        new_subtitle_stream(o, oc, i);
                        break;//这里看到，只会拿第一个字幕流
                    }
                }
        }

        /*4.4 为数据流开辟输出流。若存在输入文件多个数据流，同样会为其开辟相同个数的数据流。
         * 暂未研究，且比较少见*/
        /* Data only if codec id match */
        if (!o->data_disable ) {
            enum AVCodecID codec_id = av_guess_codec(oc->oformat, NULL, filename, NULL, AVMEDIA_TYPE_DATA);
            for (i = 0; codec_id != AV_CODEC_ID_NONE && i < nb_input_streams; i++) {
                if (input_streams[i]->user_set_discard == AVDISCARD_ALL)
                    continue;
                if (input_streams[i]->st->codecpar->codec_type == AVMEDIA_TYPE_DATA
                    && input_streams[i]->st->codecpar->codec_id == codec_id )
                    new_data_stream(o, oc, i);
            }
        }

    } else {
        /*暂不研究map，后续补充*/
        for (i = 0; i < o->nb_stream_maps; i++) {
            StreamMap *map = &o->stream_maps[i];

            if (map->disabled)
                continue;

            if (map->linklabel) {
                FilterGraph *fg;
                OutputFilter *ofilter = NULL;
                int j, k;

                for (j = 0; j < nb_filtergraphs; j++) {
                    fg = filtergraphs[j];
                    for (k = 0; k < fg->nb_outputs; k++) {
                        AVFilterInOut *out = fg->outputs[k]->out_tmp;
                        if (out && !strcmp(out->name, map->linklabel)) {
                            ofilter = fg->outputs[k];
                            goto loop_end;
                        }
                    }
                }
loop_end:
                if (!ofilter) {
                    av_log(NULL, AV_LOG_FATAL, "Output with label '%s' does not exist "
                           "in any defined filter graph, or was already used elsewhere.\n", map->linklabel);
                    exit_program(1);
                }
                init_output_filter(ofilter, o, oc);
            } else {
                int src_idx = input_files[map->file_index]->ist_index + map->stream_index;

                ist = input_streams[input_files[map->file_index]->ist_index + map->stream_index];
                if (ist->user_set_discard == AVDISCARD_ALL) {
                    av_log(NULL, AV_LOG_FATAL, "Stream #%d:%d is disabled and cannot be mapped.\n",
                           map->file_index, map->stream_index);
                    exit_program(1);
                }
                if(o->subtitle_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                    continue;
                if(o->   audio_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                    continue;
                if(o->   video_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                    continue;
                if(o->    data_disable && ist->st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
                    continue;

                ost = NULL;
                switch (ist->st->codecpar->codec_type) {
                case AVMEDIA_TYPE_VIDEO:      ost = new_video_stream     (o, oc, src_idx); break;
                case AVMEDIA_TYPE_AUDIO:      ost = new_audio_stream     (o, oc, src_idx); break;
                case AVMEDIA_TYPE_SUBTITLE:   ost = new_subtitle_stream  (o, oc, src_idx); break;
                case AVMEDIA_TYPE_DATA:       ost = new_data_stream      (o, oc, src_idx); break;
                case AVMEDIA_TYPE_ATTACHMENT: ost = new_attachment_stream(o, oc, src_idx); break;
                case AVMEDIA_TYPE_UNKNOWN:
                    if (copy_unknown_streams) {
                        ost = new_unknown_stream   (o, oc, src_idx);
                        break;
                    }
                default:
                    av_log(NULL, ignore_unknown_streams ? AV_LOG_WARNING : AV_LOG_FATAL,
                           "Cannot map stream #%d:%d - unsupported type.\n",
                           map->file_index, map->stream_index);
                    if (!ignore_unknown_streams) {
                        av_log(NULL, AV_LOG_FATAL,
                               "If you want unsupported types ignored instead "
                               "of failing, please use the -ignore_unknown option\n"
                               "If you want them copied, please use -copy_unknown\n");
                        exit_program(1);
                    }
                }
                if (ost)
                    ost->sync_ist = input_streams[  input_files[map->sync_file_index]->ist_index
                                                  + map->sync_stream_index];
            }
        }
    }

    /*5. 对应参数"attach"，添加一个附件到输出文件*/
    /*处理附加文件，推流没用到，暂未研究，读者可自行研究*/
    /* handle attached files */
    for (i = 0; i < o->nb_attachments; i++) {
        AVIOContext *pb;
        uint8_t *attachment;
        const char *p;
        int64_t len;

        if ((err = avio_open2(&pb, o->attachments[i], AVIO_FLAG_READ, &int_cb, NULL)) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Could not open attachment file %s.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        if ((len = avio_size(pb)) <= 0) {
            av_log(NULL, AV_LOG_FATAL, "Could not get size of the attachment %s.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        if (len > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE ||
            !(attachment = av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE))) {
            av_log(NULL, AV_LOG_FATAL, "Attachment %s too large.\n",
                   o->attachments[i]);
            exit_program(1);
        }
        avio_read(pb, attachment, len);
        memset(attachment + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        ost = new_attachment_stream(o, oc, -1);
        ost->stream_copy               = 0;
        ost->attachment_filename       = o->attachments[i];
        ost->st->codecpar->extradata      = attachment;
        ost->st->codecpar->extradata_size = len;

        p = strrchr(o->attachments[i], '/');
        av_dict_set(&ost->st->metadata, "filename", (p && *p) ? p + 1 : o->attachments[i], AV_DICT_DONT_OVERWRITE);
        avio_closep(&pb);
    }

#if FF_API_LAVF_AVCTX
    /*6. 若满足一定条件，则为该输出流的编解码器设置flags选项*/
    /*nb_output_streams一般与oc->nb_streams相等*/
    /*推流没用到，暂未研究*/
    for (i = nb_output_streams - oc->nb_streams; i < nb_output_streams; i++) { //for all streams of this output file(对于该输出文件的所有流)
        AVDictionaryEntry *e;
        ost = output_streams[i];

        if ((ost->stream_copy || ost->attachment_filename)/*有不转码请求或者附属文件名不为空*/
            && (e = av_dict_get(o->g->codec_opts, "flags", NULL, AV_DICT_IGNORE_SUFFIX))/*存在忽略大小写的flags编解码器选项*/
            && (!e->key[5] || check_stream_specifier(oc, ost->st, e->key+6))){/*因为上面调用后，e->key可能是父串，"flags"作为子串，
            故!e->key[5]的意思是排除"flags"作为子串的可能，即刚好是"flags"，则程序退出.
            或者e->key+6后的字符串与对应的流不匹配(+6是因为跳过对应分隔符)，程序也退出.*/
            if (av_opt_set(ost->st->codec, "flags", e->value, 0) < 0)
                exit_program(1);
        }

    }
#endif

    /*7. 检测，当输出流数为0，且(对(格式不需要任何流)取反)即格式需要流的情况，那么程序退出*/
    if (!oc->nb_streams && !(oc->oformat->flags & AVFMT_NOSTREAMS)) {
        av_dump_format(oc, nb_output_files - 1, oc->url, 1);
        av_log(NULL, AV_LOG_ERROR, "Output file #%d does not contain any stream\n", nb_output_files - 1);
        exit_program(1);
    }

    /* check if all codec options have been used */
    /*8. 得到用户输入并且去掉流说明符的字典*/
    unused_opts = strip_specifiers(o->g->codec_opts);
    /*遍历每个输出流，利用每个输出流保存的编解码器选项，将用户输入的选项unused_opts设置为空
    因为会在调用以视频为例，new_video_stream的new_output_stream中，看到：
    ost->encoder_opts  = filter_codec_opts(o->g->codec_opts, ost->enc->id, oc, st, ost->enc);调用，
    这样每个流都保存了属于自己的编解码器选项。
    那么，现在，只要将保存的选项去清除用户的选项，就知道用户输入的选项剩余哪些没使用了*/
    for (i = of->ost_index; i < nb_output_streams; i++) {
        e = NULL;
        while ((e = av_dict_get(output_streams[i]->encoder_opts, "", e,
                                AV_DICT_IGNORE_SUFFIX)))
            av_dict_set(&unused_opts, e->key, NULL, 0);
    }

    /*9. 检测用户剩余未使用的选项，对剩余的选项进行检测判断忽略该选项或者程序退出*/
    /*实际下面的判断逻辑很简单，1.判断该选项是否是编解码器选项，不是则直接忽略；
     * 是则再判断是不是解复用选项，是的话，那么也忽略；是编解码器选项且不是的解复用的话，那么肯定是编解码器选项，且这里是输出流需要编码，
     * 所以继续往下判断，若不是编码选项，则程序退出*/
    e = NULL;
    while ((e = av_dict_get(unused_opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
        const AVClass *class = avcodec_get_class();
        const AVOption *option = av_opt_find(&class, e->key, NULL, 0,
                                             AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        const AVClass *fclass = avformat_get_class();
        const AVOption *foption = av_opt_find(&fclass, e->key, NULL, 0,
                                              AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ);
        /*1.在avcodec的AVClass找不到该选项，则忽略它；
          2.在avcodec的AVClass找到该选项，则看它是否在avformat的AVClass中找到：
                2.1）若在avformat的AVClass中找到，则忽略；
                2.2）若在avformat的AVClass中找不到，则往下。说明是avcodec的AVClass的选项*/
        if (!option || foption)
            continue;

        /*该选项不是编码选项(换句话说是解码器选项)，程序退出*/
        if (!(option->flags & AV_OPT_FLAG_ENCODING_PARAM)) {
            av_log(NULL, AV_LOG_ERROR, "Codec AVOption %s (%s) specified for "
                   "output file #%d (%s) is not an encoding option.\n", e->key,
                   option->help ? option->help : "", nb_output_files - 1,
                   filename);
            exit_program(1);
        }

        // gop_timecode is injected by generic code but not always used
        // gop_timecode是由泛型代码注入的，但并不总是使用
        if (!strcmp(e->key, "gop_timecode"))
            continue;

        av_log(NULL, AV_LOG_WARNING, "Codec AVOption %s (%s) specified for "
               "output file #%d (%s) has not been used for any stream. The most "
               "likely reason is either wrong type (e.g. a video option with "
               "no video streams) or that it is a private option of some encoder "
               "which was not actually used for any stream.\n", e->key,
               option->help ? option->help : "", nb_output_files - 1, filename);
    }
    av_dict_free(&unused_opts);

    /* set the decoding_needed flags and create simple filtergraphs */
    /*10. 设置decoing_needed标志并创建简单的过滤器*/
    /* 这个for循环主要工作：判断每个输出流是否需要编码，是的话会对该输出流的过滤器进行初始化,
     * 但只能是视频或者音频流*/
    /*10.1遍历输出文件的每个输出流*/
    for (i = of->ost_index; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        /*10.2判断该输出流是否需要重新编码，并且要求该输出流对应的输入流下标>=0*/
        if (ost->encoding_needed && ost->source_index >= 0) {
            /*10.3获取该输出流对应的输入流ist，并标记该输入流需要解码*/
            InputStream *ist = input_streams[ost->source_index];
            ist->decoding_needed |= DECODING_FOR_OST;

            /*10.4媒体类型是视频或者音频，需要利用输入输出流初始化简单过滤器.
            所以字幕一般来到这里，但不属于音视频类型，所以字幕不会创建过滤器*/
            if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                err = init_simple_filtergraph(ist, ost);/*利用输入输出流创建过滤器*/
                if (err < 0) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Error initializing a simple filtergraph between streams "
                           "%d:%d->%d:%d\n", ist->file_index, ost->source_index,
                           nb_output_files - 1, ost->st->index);
                    exit_program(1);
                }
            }
        }

        /*10.5 当上面需要初始化过滤器后，这里会利用编码器的参数设置过滤器的输出条件。
         * 主要是：视频的帧率、分辨率、帧格式；音频的采样格式、采样率、通道布局*/
        /* set the filter output constraints */
        if (ost->filter) {
            OutputFilter *f = ost->filter;//fg->outputs[0]
            int count;
            // 下面全是为输出过滤器f赋值的
            switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                f->frame_rate = ost->frame_rate;
                f->width      = ost->enc_ctx->width;
                f->height     = ost->enc_ctx->height;
                /*利用编码器上下文或者编码器，给输出过滤器获取支持的像素格式。
                 * 若编码器上下文已经有像素格式则直接获取(保存在format变量)，否则从编码器中获取(保存在formats数组)*/
                if (ost->enc_ctx->pix_fmt != AV_PIX_FMT_NONE) {
                    f->format = ost->enc_ctx->pix_fmt;// 注，这里是变量
                } else if (ost->enc->pix_fmts) {
                    count = 0;
                    /*遍历该编码器支持的像素格式，得到支持像素格式数组的大小count*/
                    /*以推流使用libx264编码器为例：我们在libavcodec/allcodecs.c文件找到，extern AVCodec ff_libx264_encoder编码器对象，
                    在X264_init_static()中，看到会根据x264的版本，支持不同的像素格式，我这里是x264dll是164版本，所以指向pix_fmts_all，共15个元素(不包含AV_PIX_FMT_NONE)*/
                    while (ost->enc->pix_fmts[count] != AV_PIX_FMT_NONE)
                        count++;
                    f->formats = av_mallocz_array(count + 1, sizeof(*f->formats));
                    if (!f->formats)
                        exit_program(1);
                    memcpy(f->formats, ost->enc->pix_fmts, (count + 1) * sizeof(*f->formats));// 注，这里是formats数组
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                /*1.获取音频像素格式、采样率、通道布局.获取方法与视频同理*/
                if (ost->enc_ctx->sample_fmt != AV_SAMPLE_FMT_NONE) {
                    f->format = ost->enc_ctx->sample_fmt;
                } else if (ost->enc->sample_fmts) {
                    count = 0;
                    while (ost->enc->sample_fmts[count] != AV_SAMPLE_FMT_NONE)
                        count++;
                    f->formats = av_mallocz_array(count + 1, sizeof(*f->formats));
                    if (!f->formats)
                        exit_program(1);
                    memcpy(f->formats, ost->enc->sample_fmts, (count + 1) * sizeof(*f->formats));
                }
                /*获取音频的采样率*/
                if (ost->enc_ctx->sample_rate) {
                    f->sample_rate = ost->enc_ctx->sample_rate;
                } else if (ost->enc->supported_samplerates) {
                    count = 0;
                    while (ost->enc->supported_samplerates[count])
                        count++;
                    f->sample_rates = av_mallocz_array(count + 1, sizeof(*f->sample_rates));
                    if (!f->sample_rates)
                        exit_program(1);
                    memcpy(f->sample_rates, ost->enc->supported_samplerates,
                           (count + 1) * sizeof(*f->sample_rates));
                }
                /*获取音频的通道布局*/
                if (ost->enc_ctx->channels) {
                    f->channel_layout = av_get_default_channel_layout(ost->enc_ctx->channels);
                } else if (ost->enc->channel_layouts) {
                    count = 0;
                    while (ost->enc->channel_layouts[count])
                        count++;
                    f->channel_layouts = av_mallocz_array(count + 1, sizeof(*f->channel_layouts));
                    if (!f->channel_layouts)
                        exit_program(1);
                    memcpy(f->channel_layouts, ost->enc->channel_layouts,
                           (count + 1) * sizeof(*f->channel_layouts));
                }
                break;
            }//<== switch (ost->enc_ctx->codec_type) end ==>
        }//<== if (ost->filter) end ==>
    }//<== for (i = of->ost_index; i < nb_output_streams; i++) emd ==>

    /*11. 如果需要图像编号，请检查文件名.有兴趣的请看av_filename_number_test源码，推流没用到*/
    /* check filename in case of an image number is expected */
    if (oc->oformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(oc->url)) {
            print_error(oc->url, AVERROR(EINVAL));
            exit_program(1);
        }
    }

    /*12. 输出需要流 且 输入流为空，程序退出。
    input_stream_potentially_available=0代表输入文件没有输入流*/
    if (!(oc->oformat->flags & AVFMT_NOSTREAMS) && !input_stream_potentially_available) {
        av_log(NULL, AV_LOG_ERROR,
               "No input streams but output needs an input stream\n");
        exit_program(1);
    }

    /*13. 判断该输出文件能否重写，若不能程序直接退出；能则将输出文件重置，等待后面的流程输入数据*/
    /*输出文件的AVOutputFormat不包含AVFMT_NOFILE，那么进入if流程(实时流和文件一般都会进入)*/
    /*AVFMT_NOFILE：大概搜了源码以及百度，意思是：指一些特定于设备的特殊文件，
    且AVIOContext表示字节流输入/输出的上下文，在muxers和demuxers的数据成员flags有设置AVFMT_NOFILE时，
    这个成员变量pb就不需要设置，因为muxers和demuxers会使用其它的方式处理输入/输出。*/
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        /* test if it already exists to avoid losing precious files */
        assert_file_overwrite(filename);

        /* open the file */
        /*因为没有AVFMT_NOFILE，所以需要调avio_open2给oc->pb赋值*/
        /*经过assert_file_overwrite断言后，说明用户是认可重写该文件的，那么经过avio_open2处理后，
        输入文件就会被重新创建，debug此时看到文件变成0KB的大小*/
        if ((err = avio_open2(&oc->pb, filename, AVIO_FLAG_WRITE,
                              &oc->interrupt_callback,
                              &of->opts)) < 0) {
            print_error(filename, err);
            exit_program(1);
        }
    } else if (strcmp(oc->oformat->name, "image2")==0 && !av_filename_number_test(filename))
        assert_file_overwrite(filename);

    if (o->mux_preload) {
        av_dict_set_int(&of->opts, "preload", o->mux_preload*AV_TIME_BASE, 0);
    }
    oc->max_delay = (int)(o->mux_max_delay * AV_TIME_BASE);

    /* copy metadata */
    /*14. copy_metadata()拷贝元数据，推流没用到，有兴趣可参考：
    https://blog.csdn.net/feiyu5323/article/details/118352974以及
    https://www.jianshu.com/p/bd752c86f3e7
    */
    for (i = 0; i < o->nb_metadata_map; i++) {
        char *p;
        int in_file_index = strtol(o->metadata_map[i].u.str, &p, 0);

        if (in_file_index >= nb_input_files) {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index %d while processing metadata maps\n", in_file_index);
            exit_program(1);
        }
        copy_metadata(o->metadata_map[i].specifier, *p ? p + 1 : p, oc,
                      in_file_index >= 0 ?
                      input_files[in_file_index]->ctx : NULL, o);
    }

    /* 15. copy chapters */
    /*推流没用到，暂不研究*/
    if (o->chapters_input_file >= nb_input_files) {
        if (o->chapters_input_file == INT_MAX) {
            /* copy chapters from the first input file that has them*/
            o->chapters_input_file = -1;
            for (i = 0; i < nb_input_files; i++)
                if (input_files[i]->ctx->nb_chapters) {
                    o->chapters_input_file = i;
                    break;
                }
        } else {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index %d in chapter mapping.\n",
                   o->chapters_input_file);
            exit_program(1);
        }
    }
    /*推流没用到，暂不研究*/
    if (o->chapters_input_file >= 0)
        copy_chapters(input_files[o->chapters_input_file], of,
                      !o->metadata_chapters_manual);

    /*16. 这里就是将输入文件的全局元数据拷贝到输出文件的全局元数据*/
    /* copy global metadata by default */
    printf("============ global metadata ===========\n");
    printf("copy before, input_files[0]->ctx->metadata: \n");
    if(input_files[0]->ctx->metadata)
        tyy_print_AVDirnary(input_files[0]->ctx->metadata);
    else
        printf("input_files[0]->ctx->metadata is null\n");

    printf("copy before, oc->metadata: \n");
    if(oc->metadata)
        tyy_print_AVDirnary(oc->metadata);
    else
        printf("oc->metadata is null\n");
    if (!o->metadata_global_manual && nb_input_files){
        av_dict_copy(&oc->metadata, input_files[0]->ctx->metadata,
                     AV_DICT_DONT_OVERWRITE);
        //av_dict_set(&oc->metadata, "MAJOR_BRAND", "cwj", 0);
        if(o->recording_time != INT64_MAX)
            av_dict_set(&oc->metadata, "duration", NULL, 0);
        av_dict_set(&oc->metadata, "creation_time", NULL, 0);
    }
    printf("+++++++++ copy after, input_files[0]->ctx->metadata: \n");
    tyy_print_AVDirnary(input_files[0]->ctx->metadata);
    printf("copy after, oc->metadata: \n");
    tyy_print_AVDirnary(oc->metadata);
    printf("============ global metadata ===========\n");

    /*17. 这里就是将输入文件的每一个流的元数据拷贝到输出文件对应每一个流的元数据中*/
    if (!o->metadata_streams_manual)
        for (i = of->ost_index; i < nb_output_streams; i++) {
            InputStream *ist;
            if (output_streams[i]->source_index < 0)         /* this is true e.g. for attached files */
                continue;

            ist = input_streams[output_streams[i]->source_index];//根据输出流中保存的输入流下标，得到对应的输入流

            printf("============ stream metadata %d ===========\n", i);
            printf("copy before, ist->st->metadata: \n");
            if(ist->st->metadata)
                tyy_print_AVDirnary(ist->st->metadata);
            else
                printf("ist->st->metadata is null\n");

            printf("copy before, output_streams[i]->st->metadata: \n");
            if(output_streams[i]->st->metadata)
                tyy_print_AVDirnary(output_streams[i]->st->metadata);
            else
                printf("output_streams[i]->st->metadata is null\n");
            av_dict_copy(&output_streams[i]->st->metadata, ist->st->metadata, AV_DICT_DONT_OVERWRITE);//就是这里进行拷贝的

            // 拷贝之后应该也要检查是否为空的，这里就不写得这么仔细了
            printf("+++++++++ copy after, ist->st->metadata: \n");
            tyy_print_AVDirnary(ist->st->metadata);
            printf("copy after, output_streams[i]->st->metadata: \n");
            tyy_print_AVDirnary(output_streams[i]->st->metadata);
            printf("============ stream metadata %d ===========\n", i);

            if (!output_streams[i]->stream_copy) {
                /*转码则把元数据的encoder删掉，因为后续会在set_encoder_id()中为每个流增加该选项*/
                av_dict_set(&output_streams[i]->st->metadata, "encoder", NULL, 0);
            }
        }

    /*18 process manually set programs，推流没用到，暂不研究*/
    /* process manually set programs */
    for (i = 0; i < o->nb_program; i++) {
        const char *p = o->program[i].u.str;
        int progid = i+1;
        AVProgram *program;

        while(*p) {
            const char *p2 = av_get_token(&p, ":");
            const char *to_dealloc = p2;
            char *key;
            if (!p2)
                break;

            if(*p) p++;

            key = av_get_token(&p2, "=");
            if (!key || !*p2) {
                av_freep(&to_dealloc);
                av_freep(&key);
                break;
            }
            p2++;

            if (!strcmp(key, "program_num"))
                progid = strtol(p2, NULL, 0);
            av_freep(&to_dealloc);
            av_freep(&key);
        }

        program = av_new_program(oc, progid);

        p = o->program[i].u.str;
        while(*p) {
            const char *p2 = av_get_token(&p, ":");
            const char *to_dealloc = p2;
            char *key;
            if (!p2)
                break;
            if(*p) p++;

            key = av_get_token(&p2, "=");
            if (!key) {
                av_log(NULL, AV_LOG_FATAL,
                       "No '=' character in program string %s.\n",
                       p2);
                exit_program(1);
            }
            if (!*p2)
                exit_program(1);
            p2++;

            if (!strcmp(key, "title")) {
                av_dict_set(&program->metadata, "title", p2, 0);
            } else if (!strcmp(key, "program_num")) {
            } else if (!strcmp(key, "st")) {
                int st_num = strtol(p2, NULL, 0);
                av_program_add_stream_index(oc, progid, st_num);
            } else {
                av_log(NULL, AV_LOG_FATAL, "Unknown program key %s.\n", key);
                exit_program(1);
            }
            av_freep(&to_dealloc);
            av_freep(&key);
        }
    }//<== for (i = 0; i < o->nb_program; i++) end ==>

    /*
     * 看上面提到的这两篇文章：
      https://blog.csdn.net/feiyu5323/article/details/118352974以及
      https://www.jianshu.com/p/bd752c86f3e7
      推流没用到，暂不研究
      只要我们不考虑元数据(除了必要的拷贝外)，这部分代码基本可以去掉。
    */
    /* 19. process manually set metadata */
    for (i = 0; i < o->nb_metadata; i++) {
        AVDictionary **m;
        char type, *val;
        const char *stream_spec;
        int index = 0, j, ret = 0;

        val = strchr(o->metadata[i].u.str, '=');
        if (!val) {
            av_log(NULL, AV_LOG_FATAL, "No '=' character in metadata string %s.\n",
                   o->metadata[i].u.str);
            exit_program(1);
        }
        *val++ = 0;

        parse_meta_type(o->metadata[i].specifier, &type, &index, &stream_spec);
        if (type == 's') {
            for (j = 0; j < oc->nb_streams; j++) {
                ost = output_streams[nb_output_streams - oc->nb_streams + j];
                if ((ret = check_stream_specifier(oc, oc->streams[j], stream_spec)) > 0) {
                    if (!strcmp(o->metadata[i].u.str, "rotate")) {
                        char *tail;
                        double theta = av_strtod(val, &tail);
                        if (!*tail) {
                            ost->rotate_overridden = 1;
                            ost->rotate_override_value = theta;
                        }
                    } else {
                        av_dict_set(&oc->streams[j]->metadata, o->metadata[i].u.str, *val ? val : NULL, 0);
                    }
                } else if (ret < 0)
                    exit_program(1);
            }
        }
        else {
            switch (type) {
            case 'g':
                m = &oc->metadata;
                break;
            case 'c':
                if (index < 0 || index >= oc->nb_chapters) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid chapter index %d in metadata specifier.\n", index);
                    exit_program(1);
                }
                m = &oc->chapters[index]->metadata;
                break;
            case 'p':
                if (index < 0 || index >= oc->nb_programs) {
                    av_log(NULL, AV_LOG_FATAL, "Invalid program index %d in metadata specifier.\n", index);
                    exit_program(1);
                }
                m = &oc->programs[index]->metadata;
                break;
            default:
                av_log(NULL, AV_LOG_FATAL, "Invalid metadata specifier %s.\n", o->metadata[i].specifier);
                exit_program(1);
            }
            av_dict_set(m, o->metadata[i].u.str, *val ? val : NULL, 0);
        }
    }

    return 0;
}

static int opt_target(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    enum { PAL, NTSC, FILM, UNKNOWN } norm = UNKNOWN;
    static const char *const frame_rates[] = { "25", "30000/1001", "24000/1001" };

    if (!strncmp(arg, "pal-", 4)) {
        norm = PAL;
        arg += 4;
    } else if (!strncmp(arg, "ntsc-", 5)) {
        norm = NTSC;
        arg += 5;
    } else if (!strncmp(arg, "film-", 5)) {
        norm = FILM;
        arg += 5;
    } else {
        /* Try to determine PAL/NTSC by peeking in the input files */
        if (nb_input_files) {
            int i, j;
            for (j = 0; j < nb_input_files; j++) {
                for (i = 0; i < input_files[j]->nb_streams; i++) {
                    AVStream *st = input_files[j]->ctx->streams[i];
                    int64_t fr;
                    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                        continue;
                    fr = st->time_base.den * 1000LL / st->time_base.num;
                    if (fr == 25000) {
                        norm = PAL;
                        break;
                    } else if ((fr == 29970) || (fr == 23976)) {
                        norm = NTSC;
                        break;
                    }
                }
                if (norm != UNKNOWN)
                    break;
            }
        }
        if (norm != UNKNOWN)
            av_log(NULL, AV_LOG_INFO, "Assuming %s for target.\n", norm == PAL ? "PAL" : "NTSC");
    }

    if (norm == UNKNOWN) {
        av_log(NULL, AV_LOG_FATAL, "Could not determine norm (PAL/NTSC/NTSC-Film) for target.\n");
        av_log(NULL, AV_LOG_FATAL, "Please prefix target with \"pal-\", \"ntsc-\" or \"film-\",\n");
        av_log(NULL, AV_LOG_FATAL, "or set a framerate with \"-r xxx\".\n");
        exit_program(1);
    }

    if (!strcmp(arg, "vcd")) {
        opt_video_codec(o, "c:v", "mpeg1video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "vcd", options);

        parse_option(o, "s", norm == PAL ? "352x288" : "352x240", options);
        parse_option(o, "r", frame_rates[norm], options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "1150000");
        opt_default(NULL, "maxrate:v", "1150000");
        opt_default(NULL, "minrate:v", "1150000");
        opt_default(NULL, "bufsize:v", "327680"); // 40*1024*8;

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);
        parse_option(o, "ac", "2", options);

        opt_default(NULL, "packetsize", "2324");
        opt_default(NULL, "muxrate", "1411200"); // 2352 * 75 * 8;

        /* We have to offset the PTS, so that it is consistent with the SCR.
           SCR starts at 36000, but the first two packs contain only padding
           and the first pack from the other stream, respectively, may also have
           been written before.
           So the real data starts at SCR 36000+3*1200. */
        o->mux_preload = (36000 + 3 * 1200) / 90000.0; // 0.44
    } else if (!strcmp(arg, "svcd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "svcd", options);

        parse_option(o, "s", norm == PAL ? "480x576" : "480x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "2040000");
        opt_default(NULL, "maxrate:v", "2516000");
        opt_default(NULL, "minrate:v", "0"); // 1145000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;
        opt_default(NULL, "scan_offset", "1");

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);

        opt_default(NULL, "packetsize", "2324");

    } else if (!strcmp(arg, "dvd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "ac3");
        parse_option(o, "f", "dvd", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "6000000");
        opt_default(NULL, "maxrate:v", "9000000");
        opt_default(NULL, "minrate:v", "0"); // 1500000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;

        opt_default(NULL, "packetsize", "2048");  // from www.mpucoder.com: DVD sectors contain 2048 bytes of data, this is also the size of one pack.
        opt_default(NULL, "muxrate", "10080000"); // from mplex project: data_rate = 1260000. mux_rate = data_rate * 8

        opt_default(NULL, "b:a", "448000");
        parse_option(o, "ar", "48000", options);

    } else if (!strncmp(arg, "dv", 2)) {

        parse_option(o, "f", "dv", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "pix_fmt", !strncmp(arg, "dv50", 4) ? "yuv422p" :
                          norm == PAL ? "yuv420p" : "yuv411p", options);
        parse_option(o, "r", frame_rates[norm], options);

        parse_option(o, "ar", "48000", options);
        parse_option(o, "ac", "2", options);

    } else {
        av_log(NULL, AV_LOG_ERROR, "Unknown target: %s\n", arg);
        return AVERROR(EINVAL);
    }

    av_dict_copy(&o->g->codec_opts,  codec_opts, AV_DICT_DONT_OVERWRITE);
    av_dict_copy(&o->g->format_opts, format_opts, AV_DICT_DONT_OVERWRITE);

    return 0;
}

static int opt_vstats_file(void *optctx, const char *opt, const char *arg)
{
    av_free (vstats_filename);
    vstats_filename = av_strdup (arg);
    return 0;
}

static int opt_vstats(void *optctx, const char *opt, const char *arg)
{
    char filename[40];
    time_t today2 = time(NULL);
    struct tm *today = localtime(&today2);

    if (!today) { // maybe tomorrow
        av_log(NULL, AV_LOG_FATAL, "Unable to get current time: %s\n", strerror(errno));
        exit_program(1);
    }

    snprintf(filename, sizeof(filename), "vstats_%02d%02d%02d.log", today->tm_hour, today->tm_min,
             today->tm_sec);
    return opt_vstats_file(NULL, opt, filename);
}

static int opt_video_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:v", arg, options);
}

static int opt_audio_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:a", arg, options);
}

static int opt_data_frames(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "frames:d", arg, options);
}

static int opt_default_new(OptionsContext *o, const char *opt, const char *arg)
{
    int ret;
    AVDictionary *cbak = codec_opts;
    AVDictionary *fbak = format_opts;
    codec_opts = NULL;
    format_opts = NULL;

    ret = opt_default(NULL, opt, arg);

    av_dict_copy(&o->g->codec_opts , codec_opts, 0);
    av_dict_copy(&o->g->format_opts, format_opts, 0);
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);
    codec_opts = cbak;
    format_opts = fbak;

    return ret;
}

static int opt_preset(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    FILE *f=NULL;
    char filename[1000], line[1000], tmp_line[1000];
    const char *codec_name = NULL;

    tmp_line[0] = *opt;
    tmp_line[1] = 0;
    MATCH_PER_TYPE_OPT(codec_names, str, codec_name, NULL, tmp_line);

    if (!(f = get_preset_file(filename, sizeof(filename), arg, *opt == 'f', codec_name))) {
        if(!strncmp(arg, "libx264-lossless", strlen("libx264-lossless"))){
            av_log(NULL, AV_LOG_FATAL, "Please use -preset <speed> -qp 0\n");
        }else
            av_log(NULL, AV_LOG_FATAL, "File for preset '%s' not found\n", arg);
        exit_program(1);
    }

    while (fgets(line, sizeof(line), f)) {
        char *key = tmp_line, *value, *endptr;

        if (strcspn(line, "#\n\r") == 0)
            continue;
        av_strlcpy(tmp_line, line, sizeof(tmp_line));
        if (!av_strtok(key,   "=",    &value) ||
            !av_strtok(value, "\r\n", &endptr)) {
            av_log(NULL, AV_LOG_FATAL, "%s: Invalid syntax: '%s'\n", filename, line);
            exit_program(1);
        }
        av_log(NULL, AV_LOG_DEBUG, "ffpreset[%s]: set '%s' = '%s'\n", filename, key, value);

        if      (!strcmp(key, "acodec")) opt_audio_codec   (o, key, value);
        else if (!strcmp(key, "vcodec")) opt_video_codec   (o, key, value);
        else if (!strcmp(key, "scodec")) opt_subtitle_codec(o, key, value);
        else if (!strcmp(key, "dcodec")) opt_data_codec    (o, key, value);
        else if (opt_default_new(o, key, value) < 0) {
            av_log(NULL, AV_LOG_FATAL, "%s: Invalid option or argument: '%s', parsed as '%s' = '%s'\n",
                   filename, line, key, value);
            exit_program(1);
        }
    }

    fclose(f);

    return 0;
}

static int opt_old2new(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char *s = av_asprintf("%s:%c", opt + 1, *opt);
    int ret = parse_option(o, s, arg, options);
    av_free(s);
    return ret;
}

static int opt_bitrate(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;

    if(!strcmp(opt, "ab")){
        av_dict_set(&o->g->codec_opts, "b:a", arg, 0);
        return 0;
    } else if(!strcmp(opt, "b")){
        av_log(NULL, AV_LOG_WARNING, "Please use -b:a or -b:v, -b is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "b:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

static int opt_qscale(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char *s;
    int ret;
    if(!strcmp(opt, "qscale")){
        av_log(NULL, AV_LOG_WARNING, "Please use -q:a or -q:v, -qscale is ambiguous\n");
        return parse_option(o, "q:v", arg, options);
    }
    s = av_asprintf("q%s", opt + 6);
    ret = parse_option(o, s, arg, options);
    av_free(s);
    return ret;
}

static int opt_profile(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    if(!strcmp(opt, "profile")){
        av_log(NULL, AV_LOG_WARNING, "Please use -profile:a or -profile:v, -profile is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "profile:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

static int opt_video_filters(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "filter:v", arg, options);
}

static int opt_audio_filters(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "filter:a", arg, options);
}

static int opt_vsync(void *optctx, const char *opt, const char *arg)
{
    if      (!av_strcasecmp(arg, "cfr"))         video_sync_method = VSYNC_CFR;
    else if (!av_strcasecmp(arg, "vfr"))         video_sync_method = VSYNC_VFR;
    else if (!av_strcasecmp(arg, "passthrough")) video_sync_method = VSYNC_PASSTHROUGH;
    else if (!av_strcasecmp(arg, "drop"))        video_sync_method = VSYNC_DROP;

    if (video_sync_method == VSYNC_AUTO)
        video_sync_method = parse_number_or_die("vsync", arg, OPT_INT, VSYNC_AUTO, VSYNC_VFR);
    return 0;
}

static int opt_timecode(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char *tcr = av_asprintf("timecode=%s", arg);
    int ret = parse_option(o, "metadata:g", tcr, options);
    if (ret >= 0)
        ret = av_dict_set(&o->g->codec_opts, "gop_timecode", arg, 0);
    av_free(tcr);
    return ret;
}

static int opt_channel_layout(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    char layout_str[32];
    char *stream_str;
    char *ac_str;
    int ret, channels, ac_str_size;
    uint64_t layout;

    layout = av_get_channel_layout(arg);
    if (!layout) {
        av_log(NULL, AV_LOG_ERROR, "Unknown channel layout: %s\n", arg);
        return AVERROR(EINVAL);
    }
    snprintf(layout_str, sizeof(layout_str), "%"PRIu64, layout);
    ret = opt_default_new(o, opt, layout_str);
    if (ret < 0)
        return ret;

    /* set 'ac' option based on channel layout */
    channels = av_get_channel_layout_nb_channels(layout);
    snprintf(layout_str, sizeof(layout_str), "%d", channels);
    stream_str = strchr(opt, ':');
    ac_str_size = 3 + (stream_str ? strlen(stream_str) : 0);
    ac_str = av_mallocz(ac_str_size);
    if (!ac_str)
        return AVERROR(ENOMEM);
    av_strlcpy(ac_str, "ac", 3);
    if (stream_str)
        av_strlcat(ac_str, stream_str, ac_str_size);
    ret = parse_option(o, ac_str, layout_str, options);
    av_free(ac_str);

    return ret;
}

static int opt_audio_qscale(void *optctx, const char *opt, const char *arg)
{
    OptionsContext *o = optctx;
    return parse_option(o, "q:a", arg, options);
}

static int opt_filter_complex(void *optctx, const char *opt, const char *arg)
{
    GROW_ARRAY(filtergraphs, nb_filtergraphs);
    if (!(filtergraphs[nb_filtergraphs - 1] = av_mallocz(sizeof(*filtergraphs[0]))))
        return AVERROR(ENOMEM);
    filtergraphs[nb_filtergraphs - 1]->index      = nb_filtergraphs - 1;
    filtergraphs[nb_filtergraphs - 1]->graph_desc = av_strdup(arg);
    if (!filtergraphs[nb_filtergraphs - 1]->graph_desc)
        return AVERROR(ENOMEM);

    input_stream_potentially_available = 1;

    return 0;
}

static int opt_filter_complex_script(void *optctx, const char *opt, const char *arg)
{
    uint8_t *graph_desc = read_file(arg);
    if (!graph_desc)
        return AVERROR(EINVAL);

    GROW_ARRAY(filtergraphs, nb_filtergraphs);
    if (!(filtergraphs[nb_filtergraphs - 1] = av_mallocz(sizeof(*filtergraphs[0]))))
        return AVERROR(ENOMEM);
    filtergraphs[nb_filtergraphs - 1]->index      = nb_filtergraphs - 1;
    filtergraphs[nb_filtergraphs - 1]->graph_desc = graph_desc;

    input_stream_potentially_available = 1;

    return 0;
}

void show_help_default(const char *opt, const char *arg)
{
    /* per-file options have at least one of those set */
    const int per_file = OPT_SPEC | OPT_OFFSET | OPT_PERFILE;
    int show_advanced = 0, show_avoptions = 0;

    if (opt && *opt) {
        if (!strcmp(opt, "long"))
            show_advanced = 1;
        else if (!strcmp(opt, "full"))
            show_advanced = show_avoptions = 1;
        else
            av_log(NULL, AV_LOG_ERROR, "Unknown help option '%s'.\n", opt);
    }

    show_usage();

    printf("Getting help:\n"
           "    -h      -- print basic options\n"
           "    -h long -- print more options\n"
           "    -h full -- print all options (including all format and codec specific options, very long)\n"
           "    -h type=name -- print all options for the named decoder/encoder/demuxer/muxer/filter/bsf\n"
           "    See man %s for detailed description of the options.\n"
           "\n", program_name);

    show_help_options(options, "Print help / information / capabilities:",
                      OPT_EXIT, 0, 0);

    show_help_options(options, "Global options (affect whole program "
                      "instead of just one file:",
                      0, per_file | OPT_EXIT | OPT_EXPERT, 0);
    if (show_advanced)
        show_help_options(options, "Advanced global options:", OPT_EXPERT,
                          per_file | OPT_EXIT, 0);

    show_help_options(options, "Per-file main options:", 0,
                      OPT_EXPERT | OPT_AUDIO | OPT_VIDEO | OPT_SUBTITLE |
                      OPT_EXIT, per_file);
    if (show_advanced)
        show_help_options(options, "Advanced per-file options:",
                          OPT_EXPERT, OPT_AUDIO | OPT_VIDEO | OPT_SUBTITLE, per_file);

    show_help_options(options, "Video options:",
                      OPT_VIDEO, OPT_EXPERT | OPT_AUDIO, 0);
    if (show_advanced)
        show_help_options(options, "Advanced Video options:",
                          OPT_EXPERT | OPT_VIDEO, OPT_AUDIO, 0);

    show_help_options(options, "Audio options:",
                      OPT_AUDIO, OPT_EXPERT | OPT_VIDEO, 0);
    if (show_advanced)
        show_help_options(options, "Advanced Audio options:",
                          OPT_EXPERT | OPT_AUDIO, OPT_VIDEO, 0);
    show_help_options(options, "Subtitle options:",
                      OPT_SUBTITLE, 0, 0);
    printf("\n");

    if (show_avoptions) {
        int flags = AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM;
        show_help_children(avcodec_get_class(), flags);
        show_help_children(avformat_get_class(), flags);
#if CONFIG_SWSCALE
        show_help_children(sws_get_class(), flags);
#endif
#if CONFIG_SWRESAMPLE
        show_help_children(swr_get_class(), AV_OPT_FLAG_AUDIO_PARAM);
#endif
        show_help_children(avfilter_get_class(), AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM);
        show_help_children(av_bsf_get_class(), AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_BSF_PARAM);
    }
}

void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Hyper fast Audio and Video encoder\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] [[infile options] -i infile]... {[outfile options] outfile}...\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

enum OptGroup {
    GROUP_OUTFILE,
    GROUP_INFILE,
};

static const OptionGroupDef groups[] = {
    [GROUP_OUTFILE] = { "output url",  NULL, OPT_OUTPUT },
    [GROUP_INFILE]  = { "input url",   "i",  OPT_INPUT },
};

// ret = open_files(&octx.groups[GROUP_INFILE], "input", open_input_file);
static int open_files(OptionGroupList *l, const char *inout,
                      int (*open_file)(OptionsContext*, const char*))
{
    int i, ret;

    /*1.经过对命令行的分析将输入文件链表放到了l中，在此逐一对输入文件进行处理*/
    /*l->nb_groups指输入文件个数*/
    for (i = 0; i < l->nb_groups; i++) {
        OptionGroup *g = &l->groups[i];// 拿到一个输入文件进行处理.
        OptionsContext o;

        /*2.对OptionsContext 中的变量做默认设置，这个结构体很重要，它和ffmpeg_opt.c中定义的const OptionDef options[]相对应。
         * 其中设置的偏移量就是针对这个结构体*/
        init_options(&o);
        o.g = g;
        printf("open_files &o.codec_names: %#X, o.codec_names: %#X\n", &o.codec_names, o.codec_names);

        /*3.将g中存的，从命令行中获取的针对这个输入文件的参数放到o中*/
        ret = parse_optgroup(&o, g);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error parsing options for %s file "
                   "%s.\n", inout, g->arg);
            uninit_options(&o);
            return ret;
        }

//        // 这里可以看到，推流命令测试中，经过parse_optgroup后，编解码器选项从9个变成11个.
//        AVDictionaryEntry *t = NULL;
//        while((t = av_dict_get(o.g->codec_opts, "", t, AV_DICT_IGNORE_SUFFIX))){
//            printf("tyytest, t->key: %s, t->value: %s\n", t->key, t->value);
//        }

        av_log(NULL, AV_LOG_DEBUG, "Opening an %s file: %s.\n", inout, g->arg);
        ret = open_file(&o, g->arg);
        uninit_options(&o);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error opening %s file %s.\n",
                   inout, g->arg);
            return ret;
        }
        av_log(NULL, AV_LOG_DEBUG, "Successfully opened the file.\n");
    }

    return 0;
}

int ffmpeg_parse_options(int argc, char **argv)
{
    OptionParseContext octx;
    uint8_t error[128];
    int ret;

    memset(&octx, 0, sizeof(octx));

    // 1. 分割命令行
    /* split the commandline into an internal representation */
    ret = split_commandline(&octx, argc, argv, options, groups,
                            FF_ARRAY_ELEMS(groups));
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error splitting the argument list: ");
        goto fail;
    }

    // 2. 应用全局选项
    /* apply global options */
    ret = parse_optgroup(NULL, &octx.global_opts);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error parsing global options: ");
        goto fail;
    }

    // 3. 相关信号注册.这里不深入研究.
    /* configure terminal and setup signal handlers */
    term_init();

    // 4. 打开一个或者多个输入文件.
    /* open input files */
    ret = open_files(&octx.groups[GROUP_INFILE], "input", open_input_file);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error opening input files: ");
        goto fail;
    }

    /* 5. create the complex filtergraphs-创建复杂的过滤图 */
    // 推流模块没用到滤镜，后续再将滤镜相关.
    // 没用到的话，nb_filtergraphs=0，内部不会没有任何处理
    ret = init_complex_filters();
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error initializing complex filters.\n");
        goto fail;
    }

    // 6. 打开一个或者多个输出文件.
    /* open output files */
    ret = open_files(&octx.groups[GROUP_OUTFILE], "output", open_output_file);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Error opening output files: ");
        goto fail;
    }

    check_filter_outputs();

fail:
    uninit_parse_context(&octx);
    if (ret < 0) {
        av_strerror(ret, error, sizeof(error));
        av_log(NULL, AV_LOG_FATAL, "%s\n", error);
    }
    return ret;
}

static int opt_progress(void *optctx, const char *opt, const char *arg)
{
    AVIOContext *avio = NULL;
    int ret;

    if (!strcmp(arg, "-"))
        arg = "pipe:";
    ret = avio_open2(&avio, arg, AVIO_FLAG_WRITE, &int_cb, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open progress URL \"%s\": %s\n",
               arg, av_err2str(ret));
        return ret;
    }
    progress_avio = avio;
    return 0;
}

// offsetof是一个C函数:返回字节对齐后，成员在结构体中的偏移量.
// detail see https://blog.csdn.net/weixin_45275802/article/details/113528695
#define OFFSET(x) offsetof(OptionsContext, x)
const OptionDef options[] = {
    /* main options */
    CMDUTILS_COMMON_OPTIONS
    { "f",              HAS_ARG | OPT_STRING | OPT_OFFSET |
                        OPT_INPUT | OPT_OUTPUT,                      { .off       = OFFSET(format) },
        "force format", "fmt" },
    { "y",              OPT_BOOL,                                    {              &file_overwrite },
        "overwrite output files" },
    { "n",              OPT_BOOL,                                    {              &no_file_overwrite },
        "never overwrite output files" },
    { "ignore_unknown", OPT_BOOL,                                    {              &ignore_unknown_streams },
        "Ignore unknown stream types" },
    { "copy_unknown",   OPT_BOOL | OPT_EXPERT,                       {              &copy_unknown_streams },
        "Copy unknown stream types" },
    { "c",              HAS_ARG | OPT_STRING | OPT_SPEC |
                        OPT_INPUT | OPT_OUTPUT,                      { .off       = OFFSET(codec_names) },
        "codec name", "codec" },
    { "codec",          HAS_ARG | OPT_STRING | OPT_SPEC |
                        OPT_INPUT | OPT_OUTPUT,                      { .off       = OFFSET(codec_names) },
        "codec name", "codec" },
    { "pre",            HAS_ARG | OPT_STRING | OPT_SPEC |
                        OPT_OUTPUT,                                  { .off       = OFFSET(presets) },
        "preset name", "preset" },
    { "map",            HAS_ARG | OPT_EXPERT | OPT_PERFILE |
                        OPT_OUTPUT,                                  { .func_arg = opt_map },
        "set input stream mapping",
        "[-]input_file_id[:stream_specifier][,sync_file_id[:stream_specifier]]" },
    { "map_channel",    HAS_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_map_channel },
        "map an audio channel from one stream to another", "file.stream.channel[:syncfile.syncstream]" },
    { "map_metadata",   HAS_ARG | OPT_STRING | OPT_SPEC |
                        OPT_OUTPUT,                                  { .off       = OFFSET(metadata_map) },
        "set metadata information of outfile from infile",
        "outfile[,metadata]:infile[,metadata]" },
    { "map_chapters",   HAS_ARG | OPT_INT | OPT_EXPERT | OPT_OFFSET |
                        OPT_OUTPUT,                                  { .off = OFFSET(chapters_input_file) },
        "set chapters mapping", "input_file_index" },
    { "t",              HAS_ARG | OPT_TIME | OPT_OFFSET |
                        OPT_INPUT | OPT_OUTPUT,                      { .off = OFFSET(recording_time) },
        "record or transcode \"duration\" seconds of audio/video",
        "duration" },
    { "to",             HAS_ARG | OPT_TIME | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,  { .off = OFFSET(stop_time) },
        "record or transcode stop time", "time_stop" },
    { "fs",             HAS_ARG | OPT_INT64 | OPT_OFFSET | OPT_OUTPUT, { .off = OFFSET(limit_filesize) },
        "set the limit file size in bytes", "limit_size" },
    { "ss",             HAS_ARG | OPT_TIME | OPT_OFFSET |
                        OPT_INPUT | OPT_OUTPUT,                      { .off = OFFSET(start_time) },
        "set the start time offset", "time_off" },
    { "sseof",          HAS_ARG | OPT_TIME | OPT_OFFSET |
                        OPT_INPUT,                                   { .off = OFFSET(start_time_eof) },
        "set the start time offset relative to EOF", "time_off" },
    { "seek_timestamp", HAS_ARG | OPT_INT | OPT_OFFSET |
                        OPT_INPUT,                                   { .off = OFFSET(seek_timestamp) },
        "enable/disable seeking by timestamp with -ss" },
    { "accurate_seek",  OPT_BOOL | OPT_OFFSET | OPT_EXPERT |
                        OPT_INPUT,                                   { .off = OFFSET(accurate_seek) },
        "enable/disable accurate seeking with -ss" },
    { "itsoffset",      HAS_ARG | OPT_TIME | OPT_OFFSET |
                        OPT_EXPERT | OPT_INPUT,                      { .off = OFFSET(input_ts_offset) },
        "set the input ts offset", "time_off" },
    { "itsscale",       HAS_ARG | OPT_DOUBLE | OPT_SPEC |
                        OPT_EXPERT | OPT_INPUT,                      { .off = OFFSET(ts_scale) },
        "set the input ts scale", "scale" },
    { "timestamp",      HAS_ARG | OPT_PERFILE | OPT_OUTPUT,          { .func_arg = opt_recording_timestamp },
        "set the recording timestamp ('now' to set the current time)", "time" },
    { "metadata",       HAS_ARG | OPT_STRING | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(metadata) },
        "add metadata", "string=string" },
    { "program",        HAS_ARG | OPT_STRING | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(program) },
        "add program with specified streams", "title=string:st=number..." },
    { "dframes",        HAS_ARG | OPT_PERFILE | OPT_EXPERT |
                        OPT_OUTPUT,                                  { .func_arg = opt_data_frames },
        "set the number of data frames to output", "number" },
    { "benchmark",      OPT_BOOL | OPT_EXPERT,                       { &do_benchmark },
        "add timings for benchmarking" },
    { "benchmark_all",  OPT_BOOL | OPT_EXPERT,                       { &do_benchmark_all },
      "add timings for each task" },
    { "progress",       HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_progress },
      "write program-readable progress information", "url" },
    { "stdin",          OPT_BOOL | OPT_EXPERT,                       { &stdin_interaction },
      "enable or disable interaction on standard input" },
    { "timelimit",      HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_timelimit },
        "set max runtime in seconds", "limit" },
    { "dump",           OPT_BOOL | OPT_EXPERT,                       { &do_pkt_dump },
        "dump each input packet" },
    { "hex",            OPT_BOOL | OPT_EXPERT,                       { &do_hex_dump },
        "when dumping packets, also dump the payload" },
    { "re",             OPT_BOOL | OPT_EXPERT | OPT_OFFSET |
                        OPT_INPUT,                                   { .off = OFFSET(rate_emu) },
        "read input at native frame rate", "" },
    { "target",         HAS_ARG | OPT_PERFILE | OPT_OUTPUT,          { .func_arg = opt_target },
        "specify target file type (\"vcd\", \"svcd\", \"dvd\", \"dv\" or \"dv50\" "
        "with optional prefixes \"pal-\", \"ntsc-\" or \"film-\")", "type" },
    { "vsync",          HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_vsync },
        "video sync method", "" },
    { "frame_drop_threshold", HAS_ARG | OPT_FLOAT | OPT_EXPERT,      { &frame_drop_threshold },
        "frame drop threshold", "" },
    { "async",          HAS_ARG | OPT_INT | OPT_EXPERT,              { &audio_sync_method },
        "audio sync method", "" },
    { "adrift_threshold", HAS_ARG | OPT_FLOAT | OPT_EXPERT,          { &audio_drift_threshold },
        "audio drift threshold", "threshold" },
    { "copyts",         OPT_BOOL | OPT_EXPERT,                       { &copy_ts },
        "copy timestamps" },
    { "start_at_zero",  OPT_BOOL | OPT_EXPERT,                       { &start_at_zero },
        "shift input timestamps to start at 0 when using copyts" },
    { "copytb",         HAS_ARG | OPT_INT | OPT_EXPERT,              { &copy_tb },
        "copy input stream time base when stream copying", "mode" },
    { "shortest",       OPT_BOOL | OPT_EXPERT | OPT_OFFSET |
                        OPT_OUTPUT,                                  { .off = OFFSET(shortest) },
        "finish encoding within shortest input" },
    { "bitexact",       OPT_BOOL | OPT_EXPERT | OPT_OFFSET |
                        OPT_OUTPUT | OPT_INPUT,                      { .off = OFFSET(bitexact) },
        "bitexact mode" },
    { "apad",           OPT_STRING | HAS_ARG | OPT_SPEC |
                        OPT_OUTPUT,                                  { .off = OFFSET(apad) },
        "audio pad", "" },
    { "dts_delta_threshold", HAS_ARG | OPT_FLOAT | OPT_EXPERT,       { &dts_delta_threshold },
        "timestamp discontinuity delta threshold", "threshold" },
    { "dts_error_threshold", HAS_ARG | OPT_FLOAT | OPT_EXPERT,       { &dts_error_threshold },
        "timestamp error delta threshold", "threshold" },
    { "xerror",         OPT_BOOL | OPT_EXPERT,                       { &exit_on_error },
        "exit on error", "error" },
    { "abort_on",       HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_abort_on },
        "abort on the specified condition flags", "flags" },
    { "copyinkf",       OPT_BOOL | OPT_EXPERT | OPT_SPEC |
                        OPT_OUTPUT,                                  { .off = OFFSET(copy_initial_nonkeyframes) },
        "copy initial non-keyframes" },
    { "copypriorss",    OPT_INT | HAS_ARG | OPT_EXPERT | OPT_SPEC | OPT_OUTPUT,   { .off = OFFSET(copy_prior_start) },
        "copy or discard frames before start time" },
    { "frames",         OPT_INT64 | HAS_ARG | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(max_frames) },
        "set the number of frames to output", "number" },
    { "tag",            OPT_STRING | HAS_ARG | OPT_SPEC |
                        OPT_EXPERT | OPT_OUTPUT | OPT_INPUT,         { .off = OFFSET(codec_tags) },
        "force codec tag/fourcc", "fourcc/tag" },
    { "q",              HAS_ARG | OPT_EXPERT | OPT_DOUBLE |
                        OPT_SPEC | OPT_OUTPUT,                       { .off = OFFSET(qscale) },
        "use fixed quality scale (VBR)", "q" },
    { "qscale",         HAS_ARG | OPT_EXPERT | OPT_PERFILE |
                        OPT_OUTPUT,                                  { .func_arg = opt_qscale },
        "use fixed quality scale (VBR)", "q" },
    { "profile",        HAS_ARG | OPT_EXPERT | OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_profile },
        "set profile", "profile" },
    { "filter",         HAS_ARG | OPT_STRING | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(filters) },
        "set stream filtergraph", "filter_graph" },
    { "filter_threads",  HAS_ARG | OPT_INT,                          { &filter_nbthreads },
        "number of non-complex filter threads" },
    { "filter_script",  HAS_ARG | OPT_STRING | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(filter_scripts) },
        "read stream filtergraph description from a file", "filename" },
    { "reinit_filter",  HAS_ARG | OPT_INT | OPT_SPEC | OPT_INPUT,    { .off = OFFSET(reinit_filters) },
        "reinit filtergraph on input parameter changes", "" },
    { "filter_complex", HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_filter_complex },
        "create a complex filtergraph", "graph_description" },
    { "filter_complex_threads", HAS_ARG | OPT_INT,                   { &filter_complex_nbthreads },
        "number of threads for -filter_complex" },
    { "lavfi",          HAS_ARG | OPT_EXPERT,                        { .func_arg = opt_filter_complex },
        "create a complex filtergraph", "graph_description" },
    { "filter_complex_script", HAS_ARG | OPT_EXPERT,                 { .func_arg = opt_filter_complex_script },
        "read complex filtergraph description from a file", "filename" },
    { "stats",          OPT_BOOL,                                    { &print_stats },
        "print progress report during encoding", },
    { "attach",         HAS_ARG | OPT_PERFILE | OPT_EXPERT |
                        OPT_OUTPUT,                                  { .func_arg = opt_attach },
        "add an attachment to the output file", "filename" },
    { "dump_attachment", HAS_ARG | OPT_STRING | OPT_SPEC |
                         OPT_EXPERT | OPT_INPUT,                     { .off = OFFSET(dump_attachment) },
        "extract an attachment into a file", "filename" },
    { "stream_loop", OPT_INT | HAS_ARG | OPT_EXPERT | OPT_INPUT |
                        OPT_OFFSET,                                  { .off = OFFSET(loop) }, "set number of times input stream shall be looped", "loop count" },
    { "debug_ts",       OPT_BOOL | OPT_EXPERT,                       { &debug_ts },
        "print timestamp debugging info" },
    { "max_error_rate",  HAS_ARG | OPT_FLOAT,                        { &max_error_rate },
        "ratio of errors (0.0: no errors, 1.0: 100% errors) above which ffmpeg returns an error instead of success.", "maximum error rate" },
    { "discard",        OPT_STRING | HAS_ARG | OPT_SPEC |
                        OPT_INPUT,                                   { .off = OFFSET(discard) },
        "discard", "" },
    { "disposition",    OPT_STRING | HAS_ARG | OPT_SPEC |
                        OPT_OUTPUT,                                  { .off = OFFSET(disposition) },
        "disposition", "" },
    { "thread_queue_size", HAS_ARG | OPT_INT | OPT_OFFSET | OPT_EXPERT | OPT_INPUT,
                                                                     { .off = OFFSET(thread_queue_size) },
        "set the maximum number of queued packets from the demuxer" },
    { "find_stream_info", OPT_BOOL | OPT_PERFILE | OPT_INPUT | OPT_EXPERT, { &find_stream_info },
        "read and decode the streams to fill missing information with heuristics" },

    /* video options */
    { "vframes",      OPT_VIDEO | HAS_ARG  | OPT_PERFILE | OPT_OUTPUT,           { .func_arg = opt_video_frames },
        "set the number of video frames to output", "number" },
    { "r",            OPT_VIDEO | HAS_ARG  | OPT_STRING | OPT_SPEC |
                      OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(frame_rates) },
        "set frame rate (Hz value, fraction or abbreviation)", "rate" },
    { "s",            OPT_VIDEO | HAS_ARG | OPT_SUBTITLE | OPT_STRING | OPT_SPEC |
                      OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(frame_sizes) },
        "set frame size (WxH or abbreviation)", "size" },
    { "aspect",       OPT_VIDEO | HAS_ARG  | OPT_STRING | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(frame_aspect_ratios) },
        "set aspect ratio (4:3, 16:9 or 1.3333, 1.7777)", "aspect" },
    { "pix_fmt",      OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_STRING | OPT_SPEC |
                      OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(frame_pix_fmts) },
        "set pixel format", "format" },
    { "bits_per_raw_sample", OPT_VIDEO | OPT_INT | HAS_ARG,                      { &frame_bits_per_raw_sample },
        "set the number of bits per raw sample", "number" },
    { "intra",        OPT_VIDEO | OPT_BOOL | OPT_EXPERT,                         { &intra_only },
        "deprecated use -g 1" },
    { "vn",           OPT_VIDEO | OPT_BOOL  | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,{ .off = OFFSET(video_disable) },
        "disable video" },
    { "rc_override",  OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_STRING | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(rc_overrides) },
        "rate control override for specific intervals", "override" },
    { "vcodec",       OPT_VIDEO | HAS_ARG  | OPT_PERFILE | OPT_INPUT |
                      OPT_OUTPUT,                                                { .func_arg = opt_video_codec },
        "force video codec ('copy' to copy stream)", "codec" },
    { "sameq",        OPT_VIDEO | OPT_EXPERT ,                                   { .func_arg = opt_sameq },
        "Removed" },
    { "same_quant",   OPT_VIDEO | OPT_EXPERT ,                                   { .func_arg = opt_sameq },
        "Removed" },
    { "timecode",     OPT_VIDEO | HAS_ARG | OPT_PERFILE | OPT_OUTPUT,            { .func_arg = opt_timecode },
        "set initial TimeCode value.", "hh:mm:ss[:;.]ff" },
    { "pass",         OPT_VIDEO | HAS_ARG | OPT_SPEC | OPT_INT | OPT_OUTPUT,     { .off = OFFSET(pass) },
        "select the pass number (1 to 3)", "n" },
    { "passlogfile",  OPT_VIDEO | HAS_ARG | OPT_STRING | OPT_EXPERT | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(passlogfiles) },
        "select two pass log file name prefix", "prefix" },
    { "deinterlace",  OPT_VIDEO | OPT_BOOL | OPT_EXPERT,                         { &do_deinterlace },
        "this option is deprecated, use the yadif filter instead" },
    { "psnr",         OPT_VIDEO | OPT_BOOL | OPT_EXPERT,                         { &do_psnr },
        "calculate PSNR of compressed frames" },
    { "vstats",       OPT_VIDEO | OPT_EXPERT ,                                   { .func_arg = opt_vstats },
        "dump video coding statistics to file" },
    { "vstats_file",  OPT_VIDEO | HAS_ARG | OPT_EXPERT ,                         { .func_arg = opt_vstats_file },
        "dump video coding statistics to file", "file" },
    { "vstats_version",  OPT_VIDEO | OPT_INT | HAS_ARG | OPT_EXPERT ,            { &vstats_version },
        "Version of the vstats format to use."},
    { "vf",           OPT_VIDEO | HAS_ARG  | OPT_PERFILE | OPT_OUTPUT,           { .func_arg = opt_video_filters },
        "set video filters", "filter_graph" },
    { "intra_matrix", OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_STRING | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(intra_matrices) },
        "specify intra matrix coeffs", "matrix" },
    { "inter_matrix", OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_STRING | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(inter_matrices) },
        "specify inter matrix coeffs", "matrix" },
    { "chroma_intra_matrix", OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_STRING | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(chroma_intra_matrices) },
        "specify intra matrix coeffs", "matrix" },
    { "top",          OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_INT| OPT_SPEC |
                      OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(top_field_first) },
        "top=1/bottom=0/auto=-1 field first", "" },
    { "vtag",         OPT_VIDEO | HAS_ARG | OPT_EXPERT  | OPT_PERFILE |
                      OPT_INPUT | OPT_OUTPUT,                                    { .func_arg = opt_old2new },
        "force video tag/fourcc", "fourcc/tag" },
    { "qphist",       OPT_VIDEO | OPT_BOOL | OPT_EXPERT ,                        { &qp_hist },
        "show QP histogram" },
    { "force_fps",    OPT_VIDEO | OPT_BOOL | OPT_EXPERT  | OPT_SPEC |
                      OPT_OUTPUT,                                                { .off = OFFSET(force_fps) },
        "force the selected framerate, disable the best supported framerate selection" },
    { "streamid",     OPT_VIDEO | HAS_ARG | OPT_EXPERT | OPT_PERFILE |
                      OPT_OUTPUT,                                                { .func_arg = opt_streamid },
        "set the value of an outfile streamid", "streamIndex:value" },
    { "force_key_frames", OPT_VIDEO | OPT_STRING | HAS_ARG | OPT_EXPERT |
                          OPT_SPEC | OPT_OUTPUT,                                 { .off = OFFSET(forced_key_frames) },
        "force key frames at specified timestamps", "timestamps" },
    { "ab",           OPT_VIDEO | HAS_ARG | OPT_PERFILE | OPT_OUTPUT,            { .func_arg = opt_bitrate },
        "audio bitrate (please use -b:a)", "bitrate" },
    { "b",            OPT_VIDEO | HAS_ARG | OPT_PERFILE | OPT_OUTPUT,            { .func_arg = opt_bitrate },
        "video bitrate (please use -b:v)", "bitrate" },
    { "hwaccel",          OPT_VIDEO | OPT_STRING | HAS_ARG | OPT_EXPERT |
                          OPT_SPEC | OPT_INPUT,                                  { .off = OFFSET(hwaccels) },
        "use HW accelerated decoding", "hwaccel name" },
    { "hwaccel_device",   OPT_VIDEO | OPT_STRING | HAS_ARG | OPT_EXPERT |
                          OPT_SPEC | OPT_INPUT,                                  { .off = OFFSET(hwaccel_devices) },
        "select a device for HW acceleration", "devicename" },
    { "hwaccel_output_format", OPT_VIDEO | OPT_STRING | HAS_ARG | OPT_EXPERT |
                          OPT_SPEC | OPT_INPUT,                                  { .off = OFFSET(hwaccel_output_formats) },
        "select output format used with HW accelerated decoding", "format" },
#if CONFIG_VIDEOTOOLBOX
    { "videotoolbox_pixfmt", HAS_ARG | OPT_STRING | OPT_EXPERT, { &videotoolbox_pixfmt}, "" },
#endif
    { "hwaccels",         OPT_EXIT,                                              { .func_arg = show_hwaccels },
        "show available HW acceleration methods" },
    { "autorotate",       HAS_ARG | OPT_BOOL | OPT_SPEC |
                          OPT_EXPERT | OPT_INPUT,                                { .off = OFFSET(autorotate) },
        "automatically insert correct rotate filters" },

    /* audio options */
    { "aframes",        OPT_AUDIO | HAS_ARG  | OPT_PERFILE | OPT_OUTPUT,           { .func_arg = opt_audio_frames },
        "set the number of audio frames to output", "number" },
    { "aq",             OPT_AUDIO | HAS_ARG  | OPT_PERFILE | OPT_OUTPUT,           { .func_arg = opt_audio_qscale },
        "set audio quality (codec-specific)", "quality", },
    { "ar",             OPT_AUDIO | HAS_ARG  | OPT_INT | OPT_SPEC |
                        OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(audio_sample_rate) },
        "set audio sampling rate (in Hz)", "rate" },
    { "ac",             OPT_AUDIO | HAS_ARG  | OPT_INT | OPT_SPEC |
                        OPT_INPUT | OPT_OUTPUT,                                    { .off = OFFSET(audio_channels) },
        "set number of audio channels", "channels" },
    { "an",             OPT_AUDIO | OPT_BOOL | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT,{ .off = OFFSET(audio_disable) },
        "disable audio" },
    { "acodec",         OPT_AUDIO | HAS_ARG  | OPT_PERFILE |
                        OPT_INPUT | OPT_OUTPUT,                                    { .func_arg = opt_audio_codec },
        "force audio codec ('copy' to copy stream)", "codec" },
    { "atag",           OPT_AUDIO | HAS_ARG  | OPT_EXPERT | OPT_PERFILE |
                        OPT_OUTPUT,                                                { .func_arg = opt_old2new },
        "force audio tag/fourcc", "fourcc/tag" },
    { "vol",            OPT_AUDIO | HAS_ARG  | OPT_INT,                            { &audio_volume },
        "change audio volume (256=normal)" , "volume" },
    { "sample_fmt",     OPT_AUDIO | HAS_ARG  | OPT_EXPERT | OPT_SPEC |
                        OPT_STRING | OPT_INPUT | OPT_OUTPUT,                       { .off = OFFSET(sample_fmts) },
        "set sample format", "format" },
    { "channel_layout", OPT_AUDIO | HAS_ARG  | OPT_EXPERT | OPT_PERFILE |
                        OPT_INPUT | OPT_OUTPUT,                                    { .func_arg = opt_channel_layout },
        "set channel layout", "layout" },
    { "af",             OPT_AUDIO | HAS_ARG  | OPT_PERFILE | OPT_OUTPUT,           { .func_arg = opt_audio_filters },
        "set audio filters", "filter_graph" },
    { "guess_layout_max", OPT_AUDIO | HAS_ARG | OPT_INT | OPT_SPEC | OPT_EXPERT | OPT_INPUT, { .off = OFFSET(guess_layout_max) },
      "set the maximum number of channels to try to guess the channel layout" },

    /* subtitle options */
    { "sn",     OPT_SUBTITLE | OPT_BOOL | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT, { .off = OFFSET(subtitle_disable) },
        "disable subtitle" },
    { "scodec", OPT_SUBTITLE | HAS_ARG  | OPT_PERFILE | OPT_INPUT | OPT_OUTPUT, { .func_arg = opt_subtitle_codec },
        "force subtitle codec ('copy' to copy stream)", "codec" },
    { "stag",   OPT_SUBTITLE | HAS_ARG  | OPT_EXPERT  | OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_old2new }
        , "force subtitle tag/fourcc", "fourcc/tag" },
    { "fix_sub_duration", OPT_BOOL | OPT_EXPERT | OPT_SUBTITLE | OPT_SPEC | OPT_INPUT, { .off = OFFSET(fix_sub_duration) },
        "fix subtitles duration" },
    { "canvas_size", OPT_SUBTITLE | HAS_ARG | OPT_STRING | OPT_SPEC | OPT_INPUT, { .off = OFFSET(canvas_sizes) },
        "set canvas size (WxH or abbreviation)", "size" },

    /* grab options */
    { "vc", HAS_ARG | OPT_EXPERT | OPT_VIDEO, { .func_arg = opt_video_channel },
        "deprecated, use -channel", "channel" },
    { "tvstd", HAS_ARG | OPT_EXPERT | OPT_VIDEO, { .func_arg = opt_video_standard },
        "deprecated, use -standard", "standard" },
    { "isync", OPT_BOOL | OPT_EXPERT, { &input_sync }, "this option is deprecated and does nothing", "" },

    /* muxer options */
    { "muxdelay",   OPT_FLOAT | HAS_ARG | OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT, { .off = OFFSET(mux_max_delay) },
        "set the maximum demux-decode delay", "seconds" },
    { "muxpreload", OPT_FLOAT | HAS_ARG | OPT_EXPERT | OPT_OFFSET | OPT_OUTPUT, { .off = OFFSET(mux_preload) },
        "set the initial demux-decode delay", "seconds" },
    { "sdp_file", HAS_ARG | OPT_EXPERT | OPT_OUTPUT, { .func_arg = opt_sdp_file },
        "specify a file in which to print sdp information", "file" },

    { "time_base", HAS_ARG | OPT_STRING | OPT_EXPERT | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(time_bases) },
        "set the desired time base hint for output stream (1:24, 1:48000 or 0.04166, 2.0833e-5)", "ratio" },
    { "enc_time_base", HAS_ARG | OPT_STRING | OPT_EXPERT | OPT_SPEC | OPT_OUTPUT, { .off = OFFSET(enc_time_bases) },
        "set the desired time base for the encoder (1:24, 1:48000 or 0.04166, 2.0833e-5). "
        "two special values are defined - "
        "0 = use frame rate (video) or sample rate (audio),"
        "-1 = match source time base", "ratio" },

    { "bsf", HAS_ARG | OPT_STRING | OPT_SPEC | OPT_EXPERT | OPT_OUTPUT, { .off = OFFSET(bitstream_filters) },
        "A comma-separated list of bitstream filters", "bitstream_filters" },
    { "absf", HAS_ARG | OPT_AUDIO | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_old2new },
        "deprecated", "audio bitstream_filters" },
    { "vbsf", OPT_VIDEO | HAS_ARG | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_old2new },
        "deprecated", "video bitstream_filters" },

    { "apre", HAS_ARG | OPT_AUDIO | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT,    { .func_arg = opt_preset },
        "set the audio options to the indicated preset", "preset" },
    { "vpre", OPT_VIDEO | HAS_ARG | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT,    { .func_arg = opt_preset },
        "set the video options to the indicated preset", "preset" },
    { "spre", HAS_ARG | OPT_SUBTITLE | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT, { .func_arg = opt_preset },
        "set the subtitle options to the indicated preset", "preset" },
    { "fpre", HAS_ARG | OPT_EXPERT| OPT_PERFILE | OPT_OUTPUT,                { .func_arg = opt_preset },
        "set options from indicated preset file", "filename" },

    { "max_muxing_queue_size", HAS_ARG | OPT_INT | OPT_SPEC | OPT_EXPERT | OPT_OUTPUT, { .off = OFFSET(max_muxing_queue_size) },
        "maximum number of packets that can be buffered while waiting for all streams to initialize", "packets" },

    /* data codec support */
    { "dcodec", HAS_ARG | OPT_DATA | OPT_PERFILE | OPT_EXPERT | OPT_INPUT | OPT_OUTPUT, { .func_arg = opt_data_codec },
        "force data codec ('copy' to copy stream)", "codec" },
    { "dn", OPT_BOOL | OPT_VIDEO | OPT_OFFSET | OPT_INPUT | OPT_OUTPUT, { .off = OFFSET(data_disable) },
        "disable data" },

#if CONFIG_VAAPI
    { "vaapi_device", HAS_ARG | OPT_EXPERT, { .func_arg = opt_vaapi_device },
        "set VAAPI hardware device (DRM path or X11 display name)", "device" },
#endif

#if CONFIG_QSV
    { "qsv_device", HAS_ARG | OPT_STRING | OPT_EXPERT, { &qsv_device },
        "set QSV hardware device (DirectX adapter index, DRM path or X11 display name)", "device"},
#endif

    { "init_hw_device", HAS_ARG | OPT_EXPERT, { .func_arg = opt_init_hw_device },
        "initialise hardware device", "args" },
    { "filter_hw_device", HAS_ARG | OPT_EXPERT, { .func_arg = opt_filter_hw_device },
        "set hardware device used when filtering", "device" },

    { NULL, },
};
