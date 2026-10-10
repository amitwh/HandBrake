/* timestamp.c

   Timestamp burn-in rendered with FreeType directly (no fontconfig):
   white glyphs with black outline, optional background box. Tokens in
   the format string: {clock} (base time + accumulated pts, strftime
   patterns inside), {pts} (seconds.mmm), {frame}, {file} (current join
   input basename).

   Copyright (c) 2003-2026 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the COPYING file or visit <http://www.gnu.org/licenses/gpl-2.0.html
 */

#include <time.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "handbrake/common.h"
#include "handbrake/internal.h"

static int timestamp_init(hb_filter_object_t *filter, hb_filter_init_t *init);
static int timestamp_work(hb_filter_object_t *filter,
                          hb_buffer_t **buf_in, hb_buffer_t **buf_out);
static void timestamp_close(hb_filter_object_t *filter);

const char timestamp_template[] =
    "format=^"HB_ALL_REG"$:"
    "font=^"HB_ALL_REG"$:"
    "size=^"HB_INT_REG"$:"
    "position=^([1-9]|center)$:"
    "margin=^"HB_INT_REG"$:"
    "outline=^"HB_INT_REG"$:"
    "box=^(0|1)$:"
    "base=^"HB_ALL_REG"$";

#define TIMESTAMP_DEFAULT_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"

hb_filter_object_t hb_filter_timestamp =
{
    .id                = HB_FILTER_TIMESTAMP,
    .enforce_order     = 1,
    .skip              = 1,
    .name              = "Timestamp burn-in",
    .settings          = NULL,
    .init              = timestamp_init,
    .work              = timestamp_work,
    .close             = timestamp_close,
    .settings_template = timestamp_template,
};

#define GLYPH_CACHE_SIZE 256

typedef struct
{
    int      code;
    uint8_t *bitmap;   // 8-bit coverage map
    int      width;
    int      height;
    int      left;     // bearing
    int      top;
    int      advance;  // horizontal advance in pixels
} glyph_t;

struct hb_filter_private_s
{
    hb_job_t *job;

    FT_Library library;
    FT_Face    face;
    int        font_size;

    glyph_t    glyphs[GLYPH_CACHE_SIZE];  // code % GLYPH_CACHE_SIZE

    char       format[128];
    int        position;
    int        margin;
    int        outline;
    int        box;
    int64_t    base_time;    // epoch seconds for {clock}
    int64_t    frame_count;

    // Scratch coverage maps: white glyphs and black outline
    uint8_t   *text;
    uint8_t   *text_outline;
    int        text_width;
    int        text_height;
    int        pen_x;        // text origin offset in frame
    int        pen_y;
};
typedef struct hb_filter_private_s timestamp_private_t;

static glyph_t * glyph_for(timestamp_private_t *pv, int code)
{
    glyph_t *slot = &pv->glyphs[code % GLYPH_CACHE_SIZE];

    if (slot->bitmap != NULL && slot->code == code)
    {
        return slot;
    }
    if (FT_Load_Char(pv->face, code, FT_LOAD_RENDER) != 0)
    {
        return NULL;
    }
    FT_GlyphSlot g = pv->face->glyph;
    free(slot->bitmap);
    slot->code    = code;
    slot->width   = g->bitmap.width;
    slot->height  = g->bitmap.rows;
    slot->left    = g->bitmap_left;
    slot->top     = g->bitmap_top;
    slot->advance = (g->advance.x + 31) >> 6;
    slot->bitmap  = NULL;
    if (slot->width > 0 && slot->height > 0)
    {
        slot->bitmap = malloc((size_t)slot->width * slot->height);
        if (slot->bitmap != NULL)
        {
            // FT gray bitmaps carry 1 byte per pixel with 8.8 steps
            for (int yy = 0; yy < slot->height; yy++)
            {
                for (int xx = 0; xx < slot->width; xx++)
                {
                    slot->bitmap[yy * slot->width + xx] =
                        g->bitmap.buffer[yy * g->bitmap.pitch + xx];
                }
            }
        }
    }
    return slot;
}

static void text_layout(timestamp_private_t *pv, const char *text)
{
    int x = pv->outline + 2;
    int ascender = pv->face->size->metrics.ascender >> 6;
    int descender = pv->face->size->metrics.descender >> 6;
    int width = 2 * (pv->outline + 2);

    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        glyph_t *g = glyph_for(pv, *p);
        width += g ? g->advance : (pv->font_size / 2);
    }
    pv->text_height = ascender - descender + 2 * (pv->outline + 2);
    pv->text_width = width;
}

static void text_render(timestamp_private_t *pv, uint8_t *layer,
                         const char *text, int dx, int dy)
{
    // Rasterize coverage into the scratch buffer at offset (dx, dy)
    int x = pv->outline + 2 + dx;
    int baseline = (pv->face->size->metrics.ascender >> 6) + pv->outline + 2
                   + dy;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        glyph_t *g = glyph_for(pv, *p);
        if (g == NULL || g->bitmap == NULL)
        {
            x += pv->font_size / 2;
            continue;
        }
        for (int yy = 0; yy < g->height; yy++)
        {
            int ty = baseline - g->top + yy;
            if (ty < 0 || ty >= pv->text_height)
            {
                continue;
            }
            for (int xx = 0; xx < g->width; xx++)
            {
                int tx = x + g->left + xx;
                if (tx < 0 || tx >= pv->text_width)
                {
                    continue;
                }
                int cov = g->bitmap[yy * g->width + xx];
                int idx = ty * pv->text_width + tx;
                if (cov > layer[idx])
                {
                    layer[idx] = cov;
                }
            }
        }
        x += g->advance;
    }
}

static void expand_format(timestamp_private_t *pv, char *out, size_t out_sz,
                          hb_buffer_t *buf)
{
    const char *src = pv->format;
    char *dst = out;
    char *end = out + out_sz - 1;

    while (*src && dst < end)
    {
        if (*src != '{')
        {
            *dst++ = *src++;
            continue;
        }
        const char *close = strchr(src, '}');
        if (close == NULL)
        {
            *dst++ = *src++;
            continue;
        }
        size_t tok_len = close - src - 1;
        const char *tok = src + 1;
        if (tok_len == 5 && !strncmp(tok, "clock", 5))
        {
            int64_t t = pv->base_time +
                        (buf->s.start == AV_NOPTS_VALUE ? 0 :
                         buf->s.start / 90000);
            struct tm tm;
            time_t tt = t;
            localtime_r(&tt, &tm);
            dst += strftime(dst, end - dst, "%Y-%m-%d %H:%M:%S", &tm);
        }
        else if (tok_len == 3 && !strncmp(tok, "pts", 3))
        {
            int64_t pts = buf->s.start == AV_NOPTS_VALUE ? 0 : buf->s.start;
            dst += snprintf(dst, end - dst, "%"PRId64".%03d",
                            pts / 90000, (int)((pts % 90000) / 90));
        }
        else if (tok_len == 5 && !strncmp(tok, "frame", 5))
        {
            dst += snprintf(dst, end - dst, "%"PRId64, pv->frame_count);
        }
        else if (tok_len == 4 && !strncmp(tok, "file", 4))
        {
            const char *name = "";
            hb_join_input_t *input = pv->job->join_inputs != NULL ?
                hb_list_item(pv->job->join_inputs,
                             pv->job->join_current_input) : NULL;
            if (input == NULL && pv->job->join_inputs != NULL)
            {
                input = hb_list_item(pv->job->join_inputs, 0);
            }
            if (input == NULL && pv->job->title != NULL &&
                pv->job->title->path != NULL)
            {
                name = strrchr(pv->job->title->path, '/');
                name = name ? name + 1 : pv->job->title->path;
            }
            else if (input != NULL)
            {
                name = strrchr(input->path, '/');
                name = name ? name + 1 : input->path;
            }
            dst += snprintf(dst, end - dst, "%s", name);
        }
        else
        {
            // Unknown token: copy verbatim
            memcpy(dst, src, close - src + 1);
            dst += close - src + 1;
        }
        src = close + 1;
    }
    *dst = 0;
}

static void timestamp_blit(timestamp_private_t *pv, hb_buffer_t *buf)
{
    if (pv->text == NULL || pv->text_width <= 0 || pv->text_height <= 0)
    {
        return;
    }
    int fw = buf->f.width;
    int fh = buf->f.height;
    int x0 = pv->pen_x;
    int y0 = pv->pen_y;

    for (int yy = 0; yy < pv->text_height; yy++)
    {
        int fy = y0 + yy;
        if (fy < 0 || fy >= fh)
        {
            continue;
        }
        uint8_t *luma = buf->plane[0].data + fy * buf->plane[0].stride;
        for (int xx = 0; xx < pv->text_width; xx++)
        {
            int fx = x0 + xx;
            if (fx < 0 || fx >= fw)
            {
                continue;
            }
            int idx = yy * pv->text_width + xx;
            int cov_o = pv->text_outline != NULL ? pv->text_outline[idx] : 0;
            int cov_t = pv->text[idx];
            if (cov_o <= 0 && cov_t <= 0)
            {
                continue;
            }
            int y = luma[fx];
            if (cov_o > 0)
            {
                y = (y * (255 - cov_o) + 16 * cov_o + 127) / 255;
            }
            if (cov_t > 0)
            {
                y = (y * (255 - cov_t) + 235 * cov_t + 127) / 255;
            }
            luma[fx] = y;
        }
    }
    // Keep chroma neutral inside the text box
    if (buf->f.max_plane >= 2 && buf->plane[1].data != NULL)
    {
        int cx0 = x0 / 2, cy0 = y0 / 2;
        int cw = pv->text_width / 2, ch = pv->text_height / 2;
        for (int pp = 1; pp <= 2 && pp <= buf->f.max_plane; pp++)
        {
            for (int yy = 0; yy < ch; yy++)
            {
                int fy = cy0 + yy;
                if (fy < 0 || fy >= buf->plane[pp].height)
                {
                    continue;
                }
                uint8_t *chroma = buf->plane[pp].data +
                                  fy * buf->plane[pp].stride;
                for (int xx = 0; xx < cw; xx++)
                {
                    int fx = cx0 + xx;
                    if (fx >= 0 && fx < buf->plane[pp].width)
                    {
                        chroma[fx] = 128;
                    }
                }
            }
        }
    }
}

static void timestamp_position(timestamp_private_t *pv, int frame_w,
                               int frame_h)
{
    int col = (pv->position - 1) % 3;
    int row = (pv->position - 1) / 3;

    if (pv->position == 0)
    {
        pv->pen_x = (frame_w - pv->text_width) / 2;
        pv->pen_y = (frame_h - pv->text_height) / 2;
        return;
    }
    pv->pen_x = col == 0 ? pv->margin :
                col == 1 ? (frame_w - pv->text_width) / 2 :
                           frame_w - pv->text_width - pv->margin;
    pv->pen_y = row == 0 ? pv->margin :
                row == 1 ? (frame_h - pv->text_height) / 2 :
                           frame_h - pv->text_height - pv->margin;
    if (pv->pen_x < 0) pv->pen_x = 0;
    if (pv->pen_y < 0) pv->pen_y = 0;
}

static int timestamp_init(hb_filter_object_t *filter, hb_filter_init_t *init)
{
    timestamp_private_t *pv;
    hb_dict_t *settings = filter->settings;
    char *source_fmt = NULL, *font = NULL, *position = NULL, *base = NULL;
    int size = 24, box = 0;
    hb_job_t *job = init->job;

    pv = calloc(1, sizeof(timestamp_private_t));
    filter->private_data = pv;
    if (pv == NULL)
    {
        return 1;
    }
    pv->job = job;

    hb_dict_extract_string(&source_fmt, settings, "format");
    hb_dict_extract_string(&font, settings, "font");
    hb_dict_extract_int(&size, settings, "size");
    hb_dict_extract_string(&position, settings, "position");
    hb_dict_extract_int(&pv->margin, settings, "margin");
    hb_dict_extract_int(&pv->outline, settings, "outline");
    hb_dict_extract_int(&box, settings, "box");
    hb_dict_extract_string(&base, settings, "base");

    snprintf(pv->format, sizeof(pv->format), "%s",
             source_fmt ? source_fmt : "{clock}");
    pv->font_size = size > 8 && size < 200 ? size : 24;
    pv->outline = pv->outline > 0 && pv->outline < 10 ? pv->outline : 2;
    pv->box = box;
    pv->position = position == NULL || !strcmp(position, "center") ? 0 :
                   MAX(1, MIN(9, atoi(position)));

    // Base time for {clock}: explicit base=... or the first input's mtime
    if (base != NULL)
    {
        struct tm tm = { .tm_isdst = -1 };
        if (sscanf(base, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon,
                   &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6)
        {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            pv->base_time = mktime(&tm);
        }
    }
    if (pv->base_time == 0)
    {
        hb_stat_t sb;
        const char *path = NULL;
        if (job != NULL)
        {
            if (job->join_inputs != NULL)
            {
                hb_join_input_t *input = hb_list_item(job->join_inputs, 0);
                path = input ? input->path : NULL;
            }
            else if (job->title != NULL)
            {
                path = job->title->path;
            }
        }
        if (path != NULL && hb_stat(path, &sb) == 0)
        {
            pv->base_time = sb.st_mtime;
        }
        else
        {
            time_t now = time(NULL);
            pv->base_time = now;
        }
    }

    if (FT_Init_FreeType(&pv->library) != 0)
    {
        hb_error("Timestamp: FreeType init failed");
        goto fail;
    }
    if (FT_New_Face(pv->library,
                    font ? font : TIMESTAMP_DEFAULT_FONT,
                    0, &pv->face) != 0)
    {
        hb_error("Timestamp: cannot load font '%s'",
                 font ? font : TIMESTAMP_DEFAULT_FONT);
        goto fail;
    }
    FT_Set_Pixel_Sizes(pv->face, 0, pv->font_size);

    // Measure a representative string for the initial position
    text_layout(pv, "0000-00-00 00:00:00.000");
    pv->text = calloc(1, (size_t)pv->text_width * pv->text_height);
    if (pv->text == NULL)
    {
        goto fail;
    }
    timestamp_position(pv, init->geometry.width, init->geometry.height);

    free(source_fmt);
    free(font);
    free(position);
    free(base);

    hb_log("Timestamp: \"%s\" size %d, position %d",
           pv->format, pv->font_size, pv->position);
    filter->skip = 0;
    return 0;

fail:
    free(source_fmt);
    free(font);
    free(position);
    free(base);
    return 1;
}

static int timestamp_work(hb_filter_object_t *filter,
                          hb_buffer_t **buf_in, hb_buffer_t **buf_out)
{
    timestamp_private_t *pv = filter->private_data;
    hb_buffer_t *in = *buf_in;

    if (in->s.flags & HB_BUF_FLAG_EOF)
    {
        *buf_out = in;
        *buf_in = NULL;
        return HB_WORK_DONE;
    }

    char text[256];
    expand_format(pv, text, sizeof(text), in);

    // Compose coverage maps: box + outline (black) under glyphs (white)
    text_layout(pv, text);
    size_t map_size = (size_t)pv->text_width * pv->text_height;
    uint8_t *text_map = realloc(pv->text, map_size);
    uint8_t *outline_map = realloc(pv->text_outline, map_size);
    if (text_map != NULL && outline_map != NULL)
    {
        pv->text = text_map;
        pv->text_outline = outline_map;
        memset(text_map, 0, map_size);
        memset(outline_map, 0, map_size);
        timestamp_position(pv, in->f.width, in->f.height);
        if (pv->box)
        {
            for (int yy = 0; yy < pv->text_height; yy++)
            {
                memset(&outline_map[yy * pv->text_width], 200, pv->text_width);
            }
        }
        for (int dy = -pv->outline; dy <= pv->outline; dy++)
        {
            for (int dx = -pv->outline; dx <= pv->outline; dx++)
            {
                if (dx == 0 && dy == 0)
                {
                    continue;
                }
                text_render(pv, outline_map, text, dx, dy);
            }
        }
        text_render(pv, text_map, text, 0, 0);
        timestamp_blit(pv, in);
    }
    pv->frame_count++;

    *buf_out = in;
    *buf_in = NULL;
    return HB_WORK_OK;
}

static void timestamp_close(hb_filter_object_t *filter)
{
    timestamp_private_t *pv = filter->private_data;

    if (pv == NULL)
    {
        return;
    }
    for (int ii = 0; ii < GLYPH_CACHE_SIZE; ii++)
    {
        free(pv->glyphs[ii].bitmap);
    }
    if (pv->face != NULL)
    {
        FT_Done_Face(pv->face);
    }
    if (pv->library != NULL)
    {
        FT_Done_FreeType(pv->library);
    }
    free(pv->text);
    free(pv->text_outline);
    free(pv);
    filter->private_data = NULL;
}
