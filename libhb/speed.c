/* speed.c

   Playback speed filter (0.25x - 4x): scales video timestamps via the
   avfilter setpts alias. Audio tempo (atempo, pitch-preserving) is
   applied in the audio decoder; subtitle timestamps are scaled in sync.
   Runs after VFR so the framerate shaper normalizes the source first
   and nothing downstream resamples back to the source rate.

   Copyright (c) 2003-2026 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the COPYING file or visit <http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/common.h"
#include "handbrake/avfilter_priv.h"

static int speed_init(hb_filter_object_t * filter, hb_filter_init_t * init);

const char speed_template[] =
    "factor=^"HB_FLOAT_REG"$";

hb_filter_object_t hb_filter_speed =
{
    .id                = HB_FILTER_SPEED,
    .enforce_order     = 1,
    .skip              = 1,
    .name              = "Speed",
    .settings          = NULL,
    .init              = speed_init,
    .work              = hb_avfilter_null_work,
    .close             = hb_avfilter_alias_close,
    .settings_template = speed_template,
};

static int speed_init(hb_filter_object_t * filter, hb_filter_init_t * init)
{
    hb_filter_private_t * pv = NULL;
    hb_dict_t           * settings = filter->settings;
    double                factor = 1.0;

    pv = calloc(1, sizeof(struct hb_filter_private_s));
    filter->private_data = pv;
    if (pv == NULL)
    {
        return 1;
    }
    pv->input = *init;

    hb_dict_extract_double(&factor, settings, "factor");
    if (factor < 0.25 || factor > 4.0)
    {
        hb_error("Speed: factor %f out of range (0.25 - 4.0)", factor);
        return 1;
    }
    if (factor == 1.0)
    {
        // Nothing to do; the alias stays skipped and contributes no
        // avfilter settings to the combined graph.
        return 0;
    }

    char expr[64];
    snprintf(expr, sizeof(expr), "PTS/%.6f", factor);

    hb_dict_t * avfilter = hb_dict_init();
    hb_dict_t * avsettings = hb_dict_init();
    hb_dict_set(avsettings, "expr", hb_value_string(expr));
    hb_dict_set(avfilter, "setpts", avsettings);
    pv->avfilters = avfilter;

    // The output timeline compresses by the factor, so the effective
    // frame rate scales with it; sync/CFR uses this to keep every
    // frame instead of resampling back to the source rate.
    {
        double fps = (double)pv->input.vrate.num * factor /
                     pv->input.vrate.den;
        init->vrate.num = (int)(fps * 1000 + 0.5);
        init->vrate.den = 1000;
        hb_reduce(&init->vrate.num, &init->vrate.den,
                  init->vrate.num, init->vrate.den);
    }
    pv->output = *init;

    return 0;
}
