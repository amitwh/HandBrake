/* encjoincopy.c

   Passthrough "encoder" for lossless join jobs: the reader's encoded
   video packets (already rewritten onto the composite timeline, chapter
   marks included) flow straight through to the muxer. No decode, no
   filters, no re-encode. The audio/subtitle legs run their normal
   pipelines.

   Copyright (c) 2003-2026 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the COPYING file or visit <http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/internal.h"
#include "handbrake/extradata.h"

static int encjoincopyInit( hb_work_object_t * w, hb_job_t * job );
static int encjoincopyWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                            hb_buffer_t ** buf_out );
static void encjoincopyClose( hb_work_object_t * w );

hb_work_object_t hb_encjoincopy =
{
    .id       = WORK_ENCCOPYJOIN,
    .name     = "Join stream copy",
    .init     = encjoincopyInit,
    .work     = encjoincopyWork,
    .close    = encjoincopyClose,
    .info     = NULL,
    .bsinfo   = NULL,
    .flush    = NULL
};

static int encjoincopyInit( hb_work_object_t * w, hb_job_t * job )
{
    hb_join_input_t *input;
    hb_stream_t     *stream;
    hb_data_t       *extradata;

    if ( job->join_inputs == NULL || hb_list_count( job->join_inputs ) < 1 )
    {
        hb_error( "join-copy: no join inputs on the job" );
        return 1;
    }
    if ( !job->title->join_eligible )
    {
        hb_error( "join-copy: inputs are not eligible for lossless copy "
                  "(%s)", job->title->join_eligibility_reason );
        return 1;
    }

    // The muxer needs the input's parameter sets. Titles don't carry
    // them, so pull them from the first input now.
    input = hb_list_item( job->join_inputs, 0 );
    stream = hb_stream_open( job->h, input->path, job->title, 1 );
    if ( stream == NULL )
    {
        hb_error( "join-copy: input %s failed to open", input->path );
        return 1;
    }
    extradata = hb_stream_video_extradata( stream );
    hb_stream_close( &stream );
    if ( extradata == NULL )
    {
        hb_error( "join-copy: no video parameter sets found in %s",
                  input->path );
        return 1;
    }
    hb_set_extradata( w->extradata, extradata->bytes, extradata->size );
    hb_data_close( &extradata );

    hb_log( "join-copy: lossless passthrough of %d input(s)",
            hb_list_count( job->join_inputs ) );
    return 0;
}

static int encjoincopyWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                            hb_buffer_t ** buf_out )
{
    hb_buffer_t * in = *buf_in;

    if ( in == NULL )
    {
        return HB_WORK_OK;
    }
    if ( in->s.flags & HB_BUF_FLAG_EOF )
    {
        *buf_out = in;
        *buf_in  = NULL;
        return HB_WORK_DONE;
    }
    // Ownership transfers to the output fifo; the timestamps and
    // chapter marks were set by the join reader.
    *buf_out = in;
    *buf_in  = NULL;
    return HB_WORK_OK;
}

static void encjoincopyClose( hb_work_object_t * w )
{
    // Nothing to clean up; the muxer owns the extradata copy.
    (void)w;
}
