/* watermark.c

   Static image watermark (PNG/JPEG) composited over every frame using
   the same alpha blender as the subtitle renderer. Controls: source
   file, 9-anchor position with margin, scale percent, opacity percent.

   Copyright (c) 2003-2026 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the COPYING file or visit <http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/common.h"
#include "handbrake/hbffmpeg.h"
#include "handbrake/internal.h"

static int watermark_init(hb_filter_object_t *filter, hb_filter_init_t *init);
static int watermark_work(hb_filter_object_t *filter,
                          hb_buffer_t **buf_in, hb_buffer_t **buf_out);
static void watermark_close(hb_filter_object_t *filter);

const char watermark_template[] =
    "source=^"HB_ALL_REG"$:"
    "position=^([1-9]|center)$:"
    "margin=^"HB_INT_REG"$:"
    "scale=^(100|[1-9][0-9]?|1[0-9]{2}|2[0-4][0-9]|250)$:"
    "opacity=^"HB_INT_REG"$";

hb_filter_object_t hb_filter_watermark =
{
    .id                = HB_FILTER_WATERMARK,
    .enforce_order     = 1,
    .skip              = 1,
    .name              = "Watermark",
    .settings          = NULL,
    .init              = watermark_init,
    .work              = watermark_work,
    .close             = watermark_close,
    .settings_template = watermark_template,
};

struct hb_filter_private_s
{
    hb_blend_object_t *blend;
    hb_buffer_t       *overlay;
    hb_buffer_list_t   overlays;
    int                position;   // 1-9 grid or 0 for center
    int                margin;
};
typedef struct hb_filter_private_s watermark_private_t;

// Decode a still image (PNG or JPEG) into an AVFrame.
static AVFrame * decode_image(const char *path)
{
    const AVCodec *codec = NULL;
    AVCodecContext *ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    uint8_t *data = NULL;
    long size;
    FILE *f = fopen(path, "rb");
    AVFrame *result = NULL;

    if (f == NULL)
    {
        hb_error("Watermark: cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return NULL;
    }
    data = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (data == NULL || fread(data, 1, size, f) != (size_t)size)
    {
        fclose(f);
        av_free(data);
        return NULL;
    }
    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    fclose(f);

    codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == NULL)
    {
        codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    }
    if (codec == NULL)
    {
        hb_error("Watermark: no PNG/JPEG decoder available");
        goto done;
    }
    ctx = avcodec_alloc_context3(codec);
    if (ctx == NULL || avcodec_open2(ctx, codec, NULL) < 0)
    {
        goto done;
    }
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (frame == NULL || pkt == NULL)
    {
        goto done;
    }
    pkt->data = data;
    pkt->size = size;
    if (avcodec_send_packet(ctx, pkt) < 0)
    {
        goto done;
    }
    if (avcodec_receive_frame(ctx, frame) < 0)
    {
        hb_error("Watermark: failed to decode %s", path);
        goto done;
    }
    // Detach the frame from the decoder's packet buffer
    result = av_frame_clone(frame);

done:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    av_free(data);
    return result;
}

static hb_buffer_t * prepare_overlay(const char *path, int scale,
                                     int opacity)
{
    AVFrame *image = decode_image(path);
    AVFrame *conv = NULL;
    struct SwsContext *sws = NULL;
    hb_buffer_t *overlay = NULL;

    if (image == NULL)
    {
        return NULL;
    }
    int width  = image->width  * scale / 100;
    int height = image->height * scale / 100;
    if (width < 1 || height < 1)
    {
        av_frame_free(&image);
        return NULL;
    }
    // Align even dimensions for planar chroma
    width &= ~1;
    height &= ~1;

    conv = av_frame_alloc();
    if (conv == NULL)
    {
        av_frame_free(&image);
        return NULL;
    }
    conv->format = AV_PIX_FMT_YUVA420P;
    conv->width = width;
    conv->height = height;
    if (av_frame_get_buffer(conv, 32) < 0)
    {
        av_frame_free(&conv);
        av_frame_free(&image);
        return NULL;
    }
    sws = sws_getContext(image->width, image->height, image->format,
                         width, height, conv->format,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (sws == NULL || sws_scale(sws, (const uint8_t *const *)image->data,
                         image->linesize, 0, image->height, conv->data,
                         conv->linesize) < 0)
    {
        sws_freeContext(sws);
        av_frame_free(&conv);
        av_frame_free(&image);
        return NULL;
    }
    sws_freeContext(sws);
    av_frame_free(&image);

    if (opacity < 100)
    {
        uint8_t *alpha = conv->data[3];
        int stride = conv->linesize[3];
        for (int yy = 0; yy < height; yy++)
        {
            for (int xx = 0; xx < width; xx++)
            {
                alpha[yy * stride + xx] = (uint8_t)
                    ((int)alpha[yy * stride + xx] * opacity / 100);
            }
        }
    }

    overlay = hb_avframe_to_video_buffer(conv, (AVRational){1, 90000});
    av_frame_free(&conv);
    return overlay;
}

static void position_overlay(hb_filter_private_t *pv, int frame_w,
                             int frame_h)
{
    if (pv->overlay == NULL)
    {
        return;
    }
    int w = pv->overlay->f.width;
    int h = pv->overlay->f.height;
    int col = (pv->position - 1) % 3;   // 0 left, 1 center, 2 right
    int row = (pv->position - 1) / 3;   // 0 top, 1 middle, 2 bottom

    if (pv->position == 0)
    {
        // "center"
        pv->overlay->f.x = (frame_w - w) / 2;
        pv->overlay->f.y = (frame_h - h) / 2;
        return;
    }
    pv->overlay->f.x = col == 0 ? pv->margin :
                       col == 1 ? (frame_w - w) / 2 : frame_w - w - pv->margin;
    pv->overlay->f.y = row == 0 ? pv->margin :
                       row == 1 ? (frame_h - h) / 2 : frame_h - h - pv->margin;
    if (pv->overlay->f.x < 0) pv->overlay->f.x = 0;
    if (pv->overlay->f.y < 0) pv->overlay->f.y = 0;
}

static int watermark_init(hb_filter_object_t *filter, hb_filter_init_t *init)
{
    hb_filter_private_t *pv;
    hb_dict_t *settings = filter->settings;
    char *source = NULL;
    char *position = NULL;
    int scale = 100, opacity = 100;

    pv = calloc(1, sizeof(watermark_private_t));
    filter->private_data = pv;
    if (pv == NULL)
    {
        return 1;
    }

    hb_dict_extract_string(&source, settings, "source");
    hb_dict_extract_string(&position, settings, "position");
    hb_dict_extract_int(&scale, settings, "scale");
    hb_dict_extract_int(&opacity, settings, "opacity");
    pv->margin = 0;
    hb_dict_extract_int(&pv->margin, settings, "margin");

    if (source == NULL)
    {
        hb_error("Watermark: no source image given");
        free(source);
        free(position);
        return 1;
    }
    if (position == NULL || !strcmp(position, "center"))
    {
        pv->position = 0;
    }
    else
    {
        pv->position = atoi(position);
        if (pv->position < 1 || pv->position > 9)
        {
            pv->position = 9;
        }
    }
    if (scale < 5 || scale > 250)
    {
        scale = 100;
    }
    if (opacity < 0 || opacity > 100)
    {
        opacity = 100;
    }

    pv->overlay = prepare_overlay(source, scale, opacity);
    free(source);
    free(position);
    if (pv->overlay == NULL)
    {
        hb_error("Watermark: failed to prepare overlay");
        return 1;
    }
    position_overlay(pv, init->geometry.width, init->geometry.height);

    // Initialize the blender on the filter chain's runtime geometry
    pv->blend = malloc(sizeof(hb_blend_object_t));
    if (pv->blend == NULL)
    {
        return 1;
    }
    memcpy(pv->blend, &hb_blend, sizeof(hb_blend_object_t));
    if (pv->blend->init(pv->blend, init->geometry.width,
                        init->geometry.height, init->pix_fmt,
                        init->chroma_location, init->color_range,
                        AV_PIX_FMT_YUVA420P))
    {
        hb_error("Watermark: blend init failed");
        return 1;
    }
    hb_buffer_list_append(&pv->overlays, pv->overlay);
    hb_log("Watermark: %dx%d at %s, margin %d, %d%% opacity",
           pv->overlay->f.width, pv->overlay->f.height,
           pv->position == 0 ? "center" : "grid", pv->margin, opacity);
    filter->skip = 0;
    return 0;
}

static int watermark_work(hb_filter_object_t *filter,
                          hb_buffer_t **buf_in, hb_buffer_t **buf_out)
{
    hb_filter_private_t *pv = filter->private_data;
    hb_buffer_t *in = *buf_in;

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_out = in;
        *buf_in = NULL;
        return HB_WORK_DONE;
    }

    hb_buffer_t *out = pv->blend->work(pv->blend, in, &pv->overlays, 0);
    *buf_out = out;
    *buf_in = NULL;
    return HB_WORK_OK;
}

static void watermark_close(hb_filter_object_t *filter)
{
    hb_filter_private_t *pv = filter->private_data;

    if (pv == NULL)
    {
        return;
    }
    if (pv->blend != NULL)
    {
        pv->blend->close(pv->blend);
        free(pv->blend);
    }
    hb_buffer_close(&pv->overlay);
    free(pv);
    filter->private_data = NULL;
}
