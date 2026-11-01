/* hwaccel_vaapi.c
 *
 * Copyright (c) 2026 Amit Haridas
 * This file is part of the HandBrake source code, Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/common.h"
#include "handbrake/hwaccel.h"

#if HB_PROJECT_FEATURE_VAAPI

// VAAPI hardware decode acceleration. Encoder discovery lives in
// upstream's vaapi_common.c (hb_vaapi_encoder_available); this module
// only registers the decode hwaccel and the VAAPI encoders that can
// consume its hardware frames directly.
static const int va_encoders[] =
{
    HB_VCODEC_FFMPEG_VAAPI_H264,
    HB_VCODEC_FFMPEG_VAAPI_H265,
    HB_VCODEC_FFMPEG_VAAPI_AV1,
    HB_VCODEC_FFMPEG_VAAPI_VP8,
    HB_VCODEC_FFMPEG_VAAPI_VP9,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_vaapi =
{
    .id         = HB_DECODE_VAAPI,
    .name       = "vaapi hwaccel",
    .encoders   = va_encoders,
    .type       = AV_HWDEVICE_TYPE_VAAPI,
    .hw_pix_fmt = AV_PIX_FMT_VAAPI,
    .caps       = HB_HWACCEL_CAP_SCAN,
};

#endif // HB_PROJECT_FEATURE_VAAPI
