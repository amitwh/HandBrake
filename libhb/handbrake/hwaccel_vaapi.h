/* hwaccel_vaapi.h
 *
 * Copyright (c) 2026 Amit Haridas
 * This file is part of the HandBrake source code, Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HANDBRAKE_HWACCEL_VAAPI_H
#define HANDBRAKE_HWACCEL_VAAPI_H

int hb_vaapi_h264_available(void);
int hb_vaapi_h265_available(void);
int hb_vaapi_h265_10bit_available(void);
int hb_vaapi_av1_available(void);
int hb_vaapi_av1_10bit_available(void);

int hb_hwaccel_vaapi_is_encoder(int vcodec);

#endif // HANDBRAKE_HWACCEL_VAAPI_H
