/* hwaccel_vaapi.c
 *
 * Copyright (c) 2026 Amit Haridas
 * This file is part of the HandBrake source code, Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/common.h"
#include "handbrake/hbffmpeg.h"
#include "handbrake/hwaccel.h"
#include "handbrake/hwaccel_vaapi.h"
#include "handbrake/ports.h"

#if HB_PROJECT_FEATURE_VAAPI

#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

// VCE precedent (vce_common.c): dlopen the vendor runtime rather than
// linking, so libhb carries no extra link deps for optional hardware.
#define LIBVA_SO       "libva.so.2"
#define LIBVA_DRM_SO   "libva-drm.so.2"

static int  vaapi_h264_available   = -1;
static int  vaapi_h265_available   = -1;
static int  vaapi_h26510_available = -1;
static int  vaapi_av1_available    = -1;
static int  vaapi_av110_available  = -1;

typedef VADisplay   (*vaGetDisplayDRM_fn)(int);
typedef VAStatus    (*vaInitialize_fn)(VADisplay, int *, int *);
typedef VAStatus    (*vaTerminate_fn)(VADisplay);
typedef VAStatus    (*vaQueryConfigEntrypoints_fn)(VADisplay, VAProfile,
                                                   VAEntrypoint *, int *);

static int vaapi_probe_profile(VAProfile profile, VAEntrypoint entrypoint)
{
    void               *libva = NULL, *libva_drm = NULL;
    vaGetDisplayDRM_fn          vaGetDisplayDRM;
    vaInitialize_fn             vaInitialize;
    vaTerminate_fn              vaTerminate;
    vaQueryConfigEntrypoints_fn vaQueryConfigEntrypoints;
    VADisplay          dpy = NULL;
    // libva 2.x has no VAEntrypointMax sentinel; 32 covers every
    // entrypoint the driver can report, and the count is in/out.
    VAEntrypoint       entrypoints[32];
    int                num_entrypoints = 32, ii, major, minor;
    int                fd = -1, result = 0;

    libva = hb_dlopen(LIBVA_SO);
    libva_drm = hb_dlopen(LIBVA_DRM_SO);
    if (libva == NULL || libva_drm == NULL)
    {
        goto done;
    }
    vaGetDisplayDRM = (vaGetDisplayDRM_fn)hb_dlsym(libva_drm, "vaGetDisplayDRM");
    vaInitialize = (vaInitialize_fn)hb_dlsym(libva, "vaInitialize");
    vaTerminate = (vaTerminate_fn)hb_dlsym(libva, "vaTerminate");
    vaQueryConfigEntrypoints = (vaQueryConfigEntrypoints_fn)
                               hb_dlsym(libva, "vaQueryConfigEntrypoints");
    if (vaGetDisplayDRM == NULL || vaInitialize == NULL ||
        vaTerminate == NULL || vaQueryConfigEntrypoints == NULL)
    {
        goto done;
    }

    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0)
    {
        goto done;
    }
    dpy = vaGetDisplayDRM(fd);
    if (dpy == NULL || vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS)
    {
        goto done;
    }

    VAStatus status = vaQueryConfigEntrypoints(dpy, profile,
                                               entrypoints, &num_entrypoints);
    if (status != VA_STATUS_SUCCESS)
    {
        goto done;
    }

    for (ii = 0; ii < num_entrypoints; ii++)
    {
        if (entrypoints[ii] == entrypoint)
        {
            result = 1;
            break;
        }
    }

done:
    if (dpy != NULL)
    {
        vaTerminate(dpy);
    }
    if (fd >= 0)
    {
        close(fd);
    }
    if (libva != NULL)
    {
        hb_dlclose(libva);
    }
    if (libva_drm != NULL)
    {
        hb_dlclose(libva_drm);
    }
    return result;
}

int hb_vaapi_h264_available(void)
{
    if (vaapi_h264_available == -1)
    {
        vaapi_h264_available = vaapi_probe_profile(VAProfileH264High,
                                                   VAEntrypointEncSlice);
        hb_log("vaapi: h264 encode %savailable",
               vaapi_h264_available ? "" : "not ");
    }
    return vaapi_h264_available;
}

int hb_vaapi_h265_available(void)
{
    if (vaapi_h265_available == -1)
    {
        vaapi_h265_available = vaapi_probe_profile(VAProfileHEVCMain,
                                                   VAEntrypointEncSlice);
        hb_log("vaapi: hevc encode %savailable",
               vaapi_h265_available ? "" : "not ");
    }
    return vaapi_h265_available;
}

int hb_vaapi_h265_10bit_available(void)
{
    if (vaapi_h26510_available == -1)
    {
        vaapi_h26510_available = vaapi_probe_profile(VAProfileHEVCMain10,
                                                     VAEntrypointEncSlice);
        hb_log("vaapi: hevc 10-bit encode %savailable",
               vaapi_h26510_available ? "" : "not ");
    }
    return vaapi_h26510_available;
}

int hb_vaapi_av1_available(void)
{
    if (vaapi_av1_available == -1)
    {
        vaapi_av1_available = vaapi_probe_profile(VAProfileAV1Profile0,
                                                  VAEntrypointEncSlice);
        hb_log("vaapi: av1 encode %savailable",
               vaapi_av1_available ? "" : "not ");
    }
    return vaapi_av1_available;
}

int hb_vaapi_av1_10bit_available(void)
{
    if (vaapi_av110_available == -1)
    {
        vaapi_av110_available = vaapi_probe_profile(VAProfileAV1Profile0,
                                                    VAEntrypointEncSlice);
        hb_log("vaapi: av1 10-bit encode %savailable",
               vaapi_av110_available ? "" : "not ");
    }
    return vaapi_av110_available;
}

static const int va_encoders[] =
{
    HB_VCODEC_FFMPEG_VAAPI_H264,
    HB_VCODEC_FFMPEG_VAAPI_H265, HB_VCODEC_FFMPEG_VAAPI_H265_10BIT,
    HB_VCODEC_FFMPEG_VAAPI_AV1,  HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_vaapi =
{
    .id         = HB_DECODE_VAAPI,
    .name       = "vaapi hwaccel",
    .encoders   = va_encoders,
    .type       = AV_HWDEVICE_TYPE_VAAPI,
    .hw_pix_fmt = AV_PIX_FMT_VAAPI,
    .caps       = 0,
};

#else // !HB_PROJECT_FEATURE_VAAPI

int hb_vaapi_h264_available(void)       { return 0; }
int hb_vaapi_h265_available(void)       { return 0; }
int hb_vaapi_h265_10bit_available(void) { return 0; }
int hb_vaapi_av1_available(void)        { return 0; }
int hb_vaapi_av1_10bit_available(void)  { return 0; }

#endif

int hb_hwaccel_vaapi_is_encoder(int vcodec)
{
#if HB_PROJECT_FEATURE_VAAPI
    return vcodec == HB_VCODEC_FFMPEG_VAAPI_H264  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_H265  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_AV1  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT;
#else
    return 0;
#endif
}
