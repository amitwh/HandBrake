/* joinreader.c

   Composite reader for join jobs: reads N inputs behind the existing
   reader interface, enforcing per-input time ranges and rewriting every
   buffer's timestamps onto one continuous composite timeline so that the
   downstream sync -> filters -> encoder -> muxer chain runs unmodified.

   Copyright (c) 2003-2026 HandBrake Team
   This file is part of the HandBrake source code
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License v2.
   For full terms see the COPYING file or visit <http://www.gnu.org/licenses/gpl-2.0.html>
 */

#include "libavutil/avutil.h"
#include "handbrake/handbrake.h"
#include "handbrake/internal.h"

typedef struct
{
    hb_stream_t *stream;
    hb_title_t  *title;       // per-input title stashed at scan time
    hb_psdemux_t demux;
    int64_t      duration;    // input duration from the scan
    int64_t      range_start; // resolved start pts of the current range
    int64_t      range_stop;  // resolved stop pts of the current range
    int64_t      last_stop;   // last video stop seen in this range
    int          cur_range;
} join_input_state_t;

struct hb_joinreader_s
{
    hb_job_t           *job;
    hb_title_t         *title;     // composite title
    int                 count;     // number of inputs
    join_input_state_t *state;
    int                 cur;       // current input index
    int64_t             out_base;  // composite timeline base of current range
    int                 chapter;   // next composite chapter number (1-based)
    int                 range_open; // current range has not seen video yet
    int                 copy_mode; // HB_VCODEC_COPY_JOIN semantics
    int64_t             last_emit_stop; // last emitted video stop (composite)
};

static int join_advance( hb_joinreader_t * j )
{
    while ( j->cur < j->count )
    {
        join_input_state_t *st = &j->state[j->cur];
        hb_join_input_t    *input = hb_list_item( j->job->join_inputs, j->cur );
        int                 range_count = input->range_count > 0 ?
                                          input->range_count : 1;

        // Input 0's stream stays open for the lifetime of the job: the
        // composite title's opaque_priv points at its AVFormatContext and
        // the decoders consult that context during the whole encode. It
        // is closed in hb_joinreader_close, mirroring how the regular
        // reader's stream outlives the pipeline.
        if ( st->stream != NULL && j->cur != 0 )
        {
            hb_stream_close( &st->stream );
        }
        if ( st->cur_range >= range_count )
        {
            j->cur++;
            continue;
        }
        st->range_start = input->range_count > 0 ?
                          input->ranges[2 * st->cur_range] : 0;
        st->range_stop = input->range_count > 0 ?
                         input->ranges[2 * st->cur_range + 1] : 0;
        if ( st->range_stop == 0 || st->range_stop > st->duration )
        {
            st->range_stop = st->duration;
        }
        if ( st->range_stop <= 0 )
        {
            // Unknown duration (or broken scan): read to end of file.
            st->range_stop = INT64_MAX;
        }
        st->last_stop = st->range_start;
        st->title = j->cur == 0 ? j->title :
                    hb_list_item( j->title->join_titles, j->cur - 1 );
        st->stream = hb_stream_open( j->job->h, input->path, st->title, 0 );
        if ( st->stream == NULL )
        {
            hb_error( "joinreader: input %d (%s) failed to open",
                      j->cur + 1, input->path );
            st->cur_range = range_count; // skip to the next input
            continue;
        }
        st->cur_range++;
        j->range_open = 1;
        j->job->join_current_input = j->cur;
        return 1;
    }
    return 0;
}

hb_joinreader_t * hb_joinreader_open( hb_job_t * job )
{
    hb_joinreader_t * j;
    int               count = hb_list_count( job->join_inputs );

    if ( count < 1 || job->title == NULL )
    {
        return NULL;
    }
    if ( count > 1 && ( job->title->join_titles == NULL ||
         hb_list_count( job->title->join_titles ) != count - 1 ) )
    {
        hb_error( "joinreader: composite title is missing per-input titles; "
                  "rescan with hb_scan_join" );
        return NULL;
    }

    j = calloc( 1, sizeof( hb_joinreader_t ) );
    j->job     = job;
    j->title   = job->title;
    j->count   = count;
    j->cur     = 0;
    j->out_base = 0;
    j->chapter = 1;
    j->copy_mode = ( job->vcodec == HB_VCODEC_COPY_JOIN );
    j->last_emit_stop = AV_NOPTS_VALUE;
    j->state   = calloc( count, sizeof( join_input_state_t ) );

    for ( int ii = 0; ii < count; ii++ )
    {
        hb_title_t *title = ii == 0 ? job->title :
                            hb_list_item( job->title->join_titles, ii - 1 );

        if ( title == NULL )
        {
            hb_error( "joinreader: missing title for input %d", ii + 1 );
            hb_joinreader_close( &j );
            return NULL;
        }
        j->state[ii].duration = title->duration;
        j->state[ii].title    = title;
    }

    if ( !join_advance( j ) )
    {
        hb_joinreader_close( &j );
        return NULL;
    }
    return j;
}

static void join_rewrite_and_filter( hb_joinreader_t * j,
                                     hb_buffer_list_t * out_list,
                                     hb_buffer_list_t * in_list )
{
    join_input_state_t *st = &j->state[j->cur];
    hb_title_t         *title = st->title;
    hb_buffer_t        *buf;
    int64_t             offset = j->out_base - st->range_start;

    while ( ( buf = hb_buffer_list_rem_head( in_list ) ) != NULL )
    {
        if ( buf->s.start == AV_NOPTS_VALUE ||
             buf->s.start <  st->range_start ||
             buf->s.start >= st->range_stop )
        {
            hb_buffer_close( &buf );
            continue;
        }

        buf->s.start += offset;
        if ( buf->s.stop != AV_NOPTS_VALUE )
        {
            buf->s.stop += offset;
        }
        if ( buf->s.renderOffset != AV_NOPTS_VALUE )
        {
            buf->s.renderOffset += offset;
        }
        else if ( j->copy_mode )
        {
            // Copy mode forwards raw demux timestamps to the muxer;
            // sources whose leading packets carry no DTS (e.g. mkv)
            // would trip the muxer's monotonicity check at a seam.
            // Synthesize a DTS from the composite start.
            buf->s.renderOffset = buf->s.start;
        }

        if ( buf->s.id == title->video_id )
        {
            // Copy mode cuts must start on a keyframe: at each range
            // entry, drop video until the first keyframe at/after the
            // requested in-point.
            if ( j->copy_mode && j->range_open &&
                 !( buf->s.frametype & ( HB_FRAME_IDR | HB_FRAME_I ) ) )
            {
                hb_buffer_close( &buf );
                continue;
            }
            // The joinreader owns chapter marks on the composite
            // timeline: the first video buffer of each kept range starts
            // a chapter, everything else carries no chapter info
            // regardless of what the input demuxer thought.
            if ( j->range_open )
            {
                buf->s.new_chap = j->chapter++;
                j->range_open = 0;
            }
            else
            {
                buf->s.new_chap = 0;
            }
            // Track progress in the INPUT's own timeline: range_stop and
            // the out_base accounting below are input-local.
            if ( buf->s.stop != AV_NOPTS_VALUE )
            {
                st->last_stop = buf->s.stop - offset;
            }
            else
            {
                st->last_stop = buf->s.start - offset;
            }
            if ( buf->s.stop != AV_NOPTS_VALUE )
            {
                j->last_emit_stop = buf->s.stop;
            }
        }
        else
        {
            // Copy mode: at a seam, the next input's encoder priming
            // maps below the range's composite base (the previous
            // range's exact stop); dropping those keeps timestamps
            // monotonic. Normal interleaved audio always sits at or
            // after the base and must pass through untouched.
            if ( j->copy_mode && buf->s.start < j->out_base )
            {
                hb_buffer_close( &buf );
                continue;
            }
            buf->s.new_chap = 0;
        }
        hb_buffer_list_append( out_list, buf );
    }
}

int hb_joinreader_read( hb_joinreader_t * j, hb_buffer_list_t * out_list )
{
    while ( j->cur < j->count )
    {
        join_input_state_t *st = &j->state[j->cur];
        hb_buffer_t        *raw;
        hb_buffer_list_t    list;
        int                 demuxer;

        // A range is finished once a buffer touches its stop point; the
        // out_base for the next range spans what we actually kept.
        if ( st->range_stop > 0 && st->last_stop >= st->range_stop )
        {
            j->out_base += st->range_stop - st->range_start;
            if ( !join_advance( j ) )
            {
                break;
            }
            continue;
        }

        raw = hb_stream_read( st->stream );
        if ( raw == NULL )
        {
            // End of file inside an open range: account for what was
            // actually kept, then move on.
            j->out_base += st->last_stop - st->range_start;
            if ( !join_advance( j ) )
            {
                break;
            }
            continue;
        }

        hb_buffer_list_clear( &list );
        demuxer = st->title->demuxer;
        ( hb_demux[demuxer] )( raw, &list, &st->demux );
        join_rewrite_and_filter( j, out_list, &list );
        hb_buffer_list_close( &list );

        if ( hb_buffer_list_count( out_list ) > 0 )
        {
            return 1;
        }
        // Everything was filtered out (still before the range start):
        // keep reading.
    }
    return 0;
}

void hb_joinreader_close( hb_joinreader_t ** _j )
{
    if ( _j == NULL || *_j == NULL )
    {
        return;
    }
    hb_joinreader_t * j = *_j;

    for ( int ii = 0; ii < j->count; ii++ )
    {
        if ( j->state[ii].stream != NULL )
        {
            hb_stream_close( &j->state[ii].stream );
        }
    }
    free( j->state );
    free( j );
    *_j = NULL;
}
