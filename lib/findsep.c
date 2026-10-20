/* findsep.c
 * library routines for find silent points in mp3 data
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Portions are adapted from minimad.c, included with the 
 * libmad library, distributed under the GNU General Public License.
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include "mad.h"
#include "findsep.h"
#include "srtypes.h"
#include "debug.h"
#include "list.h"

#define MIN_RMS_SILENCE		100
#define MAX_RMS_SILENCE        2048 /* Reject ordinary music as silence. */
#define NUM_SILTRACKERS		30
#define READSIZE	2000
// #define READSIZE	1000

/* Uncomment to dump an mp3 of the search window. */
/* Define MAKE_DUMP_MP3 locally to inspect a search window. */

typedef struct FRAME_LIST_struct FRAME_LIST;
struct FRAME_LIST_struct
{
    const unsigned char* m_framepos;
    long m_samples;
    long m_pcmpos;
    LIST m_list;
};

typedef struct SILENCETRACKERst
{
    long insilencecount;
    double silencevol;
    unsigned long silstart_samp;
    BOOL foundsil;
} SILENCETRACKER;

typedef struct DECODE_STRUCTst
{
    unsigned char* mpgbuf;  /* Input buffer to be checked for silence */
    long  mpgsize;          /* Size for mpgbuf */
    long  mpgpos_next;      /* Position for next write to decoder */
    long  mpgpos_curr;      /* Position for current chunk being decoded */
    long len_to_sw_ms;
    long searchwindow_ms;
    long  silence_ms;
    long  silence_samples;
    unsigned long len_to_sw_start_samp;
    unsigned long len_to_sw_end_samp;
    unsigned long  pcmpos;
    long  samplerate;
    short prev_sample;
    SILENCETRACKER siltrackers[NUM_SILTRACKERS];
    LIST frame_list;
    int fed;
} DECODE_STRUCT;

typedef struct GET_BITRATE_STRUCTst
{
    unsigned long bitrate;
    unsigned char* mpgbuf;
    long mpgsize;
} GET_BITRATE_STRUCT;

/*****************************************************************************
 * Public functions
 *****************************************************************************/

/*****************************************************************************
 * Private functions
 *****************************************************************************/
static void init_siltrackers(SILENCETRACKER* siltrackers);
static void apply_padding (DECODE_STRUCT* ds, unsigned long silstart,
			   long padding1, long padding2,
			   u_long* pos1, u_long* pos2);
static void free_frame_list (DECODE_STRUCT* ds);
static enum mad_flow input(void *data, struct mad_stream *ms);
static void search_for_silence(DECODE_STRUCT *ds, double vol);
static signed int scale(mad_fixed_t sample);
static enum mad_flow output(void *data, struct mad_header const *header,
			    struct mad_pcm *pcm);
static enum mad_flow filter (void *data, struct mad_stream const *ms,
			     struct mad_frame *frame);
static enum mad_flow error(void *data, struct mad_stream *ms, 
			   struct mad_frame *frame);
static enum mad_flow header(void *data, struct mad_header const *pheader);
static enum mad_flow input_get_bitrate (void *data, struct mad_stream *stream);
static enum mad_flow header_get_bitrate (void *data, 
					 struct mad_header const *pheader);

/*****************************************************************************
 * Private Vars
 *****************************************************************************/

/*****************************************************************************
 * Functions
 *****************************************************************************/
static char* 
mad_error_string (enum mad_error mad_err)
{
    switch (mad_err) {
    case MAD_ERROR_NONE:
	return "MAD_ERROR_NONE";
    case MAD_ERROR_BUFLEN:
	return "MAD_ERROR_BUFPTR";
    case MAD_ERROR_NOMEM:
	return "MAD_ERROR_NOMEM";
    case MAD_ERROR_LOSTSYNC:
	return "MAD_ERROR_LOSTSYNC";
    case MAD_ERROR_BADLAYER:
	return "MAD_ERROR_BADLAYER";
    case MAD_ERROR_BADBITRATE:
	return "MAD_ERROR_BADBITRAT";
    case MAD_ERROR_BADSAMPLERATE:
	return "MAD_ERROR_BADSAMPLERATE";
    case MAD_ERROR_BADEMPHASIS:
	return "MAD_ERROR_BADEMPHASIS";
    case MAD_ERROR_BADCRC:
	return "MAD_ERROR_BADCRC";
    case MAD_ERROR_BADBITALLOC:
	return "MAD_ERROR_BADBITALLOC";
    case MAD_ERROR_BADSCALEFACTOR:
	return "MAD_ERROR_BADSCALEFACTOR";
#if defined (MAD_ERROR_BADMODE)
    case MAD_ERROR_BADMODE:
	return "MAD_ERROR_BADMODE";
#endif
    case MAD_ERROR_BADFRAMELEN:
	return "MAD_ERROR_BADFRAMELEN";
    case MAD_ERROR_BADBIGVALUES:
	return "MAD_ERROR_BADBIGVALUES";
    case MAD_ERROR_BADBLOCKTYPE:
	return "MAD_ERROR_BADBLOCKTYPE";
    case MAD_ERROR_BADSCFSI:
	return "MAD_ERROR_BADSCFSI";
    case MAD_ERROR_BADDATAPTR:
	return "MAD_ERROR_BADDATAPTR";
    case MAD_ERROR_BADPART3LEN:
	return "MAD_ERROR_BADPART3LEN";
    case MAD_ERROR_BADHUFFTABLE:
	return "MAD_ERROR_BADHUFFTABLE";
    case MAD_ERROR_BADHUFFDATA:
	return "MAD_ERROR_BADHUFFDATA";
    case MAD_ERROR_BADSTEREO:
	return "MAD_ERROR_BADSTEREO";
    default:
	return "(Unknown libmad error)";
    }
}

error_code
findsep_silence (const char* mpgbuf,
		 long mpgsize,
		 long len_to_sw,
		 long searchwindow,
		 long silence_length,
		 long padding1,
		 long padding2,
		 u_long* pos1,
		 u_long* pos2
		 )
{
    DECODE_STRUCT ds = {0};
    struct mad_decoder decoder;
    int result;
    unsigned long silstart;
    int i;
    
    if (!mpgbuf || mpgsize <= 0 || !pos1 || !pos2 || len_to_sw < 0 ||
        searchwindow <= 0 || silence_length <= 0 ||
        len_to_sw > 3600000 || searchwindow > 3600000 || silence_length > 3600000 ||
        padding1 < -3600000 || padding1 > 3600000 ||
        padding2 < -3600000 || padding2 > 3600000)
        return SR_ERROR_INVALID_PARAM;
    ds.mpgbuf = calloc((size_t)mpgsize + MAD_BUFFER_GUARD, 1);
    if (!ds.mpgbuf)
        return SR_ERROR_CANT_ALLOC_MEMORY;
    memcpy(ds.mpgbuf, mpgbuf, (size_t)mpgsize);
    ds.mpgsize = mpgsize;
    ds.pcmpos = 0;
    ds.mpgpos_curr = -1;
    ds.mpgpos_next = 0;
    ds.samplerate = 0;
    ds.prev_sample = 0;
    ds.len_to_sw_ms = len_to_sw;
    ds.searchwindow_ms = searchwindow;
    ds.silence_ms = silence_length;
    INIT_LIST_HEAD (&ds.frame_list);

    debug_printf ("FINDSEP 1: %p -> %p (0x%lx)\n",
	(void *)(mpgbuf), (void *)(mpgbuf+mpgsize), mpgsize);

    init_siltrackers(ds.siltrackers);

#if defined (MAKE_DUMP_MP3)
    {
	FILE* fp = fopen("dump.mp3", "wb");
	fwrite(mpgbuf, mpgsize, 1, fp);
	fclose(fp);
    }
#endif

    /* Run decoder */
    mad_decoder_init (&decoder, &ds, input, header, filter, output, 
	error, NULL);
    result = mad_decoder_run (&decoder, MAD_DECODER_MODE_SYNC);
    mad_decoder_finish (&decoder);
    if (result != 0 || list_empty(&ds.frame_list) || !ds.samplerate) {
        free_frame_list(&ds);
        free(ds.mpgbuf);
        return SR_ERROR_DECODE_FAILURE;
    }

    debug_printf ("total length:    %lu\n", ds.pcmpos);
    debug_printf ("silence_length:  %ld ms\n", ds.silence_ms);
    debug_printf ("silence_samples: %ld\n", ds.silence_samples);

    /* Search through siltrackers to find minimum volume point */
    assert(ds.mpgsize != 0);
    unsigned long center = MIN(ds.len_to_sw_start_samp +
        (ds.len_to_sw_end_samp - ds.len_to_sw_start_samp) / 2, ds.pcmpos / 2);
    silstart = center > (unsigned long)ds.silence_samples / 2 ?
        center - (unsigned long)ds.silence_samples / 2 : 0;
    for (i = 0; i < NUM_SILTRACKERS; i++) {
	debug_printf("SILT: %2d/%8g, pcm=%4lu, found=%d, insil=%ld\n",
	    i,
	    ds.siltrackers[i].silencevol,
	    ds.siltrackers[i].silstart_samp,
	    ds.siltrackers[i].foundsil,
	    ds.siltrackers[i].insilencecount
	    );
	if (ds.siltrackers[i].foundsil) {
	    debug_printf("found!\n");
	    silstart = ds.siltrackers[i].silstart_samp;
	    break;
	}
    }

    if (i == NUM_SILTRACKERS) {
	debug_printf("warning: no silence found between tracks\n");
    }

    /* Now that we have the start of the silence, let's add the padding */
    apply_padding (&ds, silstart, padding1, padding2, pos1, pos2);

    /* Free the list of frame info */
    free_frame_list (&ds);
    free(ds.mpgbuf);

    return SR_SUCCESS;
}

static void 
init_siltrackers(SILENCETRACKER* siltrackers)
{
    int i;
    long stepsize = (MAX_RMS_SILENCE - MIN_RMS_SILENCE) / (NUM_SILTRACKERS-1);
    long rms = MIN_RMS_SILENCE;
    for (i = 0; i < NUM_SILTRACKERS; i++, rms += stepsize) {
	siltrackers[i].foundsil = 0;
	siltrackers[i].silstart_samp = 0;
	siltrackers[i].insilencecount = 0;
	siltrackers[i].silencevol = rms;
    }
}

static void
apply_padding(DECODE_STRUCT *ds, unsigned long silsplit,
    long padding1, long padding2, u_long *pos1, u_long *pos2)
{
    silsplit += (unsigned long)(ds->silence_samples / 2);
    long long target1 = (long long)silsplit + padding1 * ds->samplerate / 1000;
    long long target2 = (long long)silsplit - padding2 * ds->samplerate / 1000;
    unsigned long boundary1 = 0, boundary2 = 0;
    struct list_head *node;
    list_for_each(node, &ds->frame_list) {
        FRAME_LIST *frame = list_entry(node, FRAME_LIST, m_list);
        unsigned long offset = (unsigned long)(frame->m_framepos - ds->mpgbuf);
        if ((long long)frame->m_pcmpos <= target1)
            boundary1 = offset;
        if ((long long)frame->m_pcmpos <= target2)
            boundary2 = offset;
    }
    *pos1 = boundary1 ? boundary1 - 1 : 0;
    *pos2 = boundary2;
}

static void 
free_frame_list (DECODE_STRUCT* ds)
{
    FRAME_LIST *pos, *n;
    /* GCS: This seems to be the best way to go through a list.
       Note no compiler warnings. */
    list_for_each_entry_safe (pos, FRAME_LIST, n, &(ds->frame_list), m_list) {
	list_del (&(pos->m_list));
	free (pos);
    }
}

static enum mad_flow
input(void *data, struct mad_stream *ms)
{
    DECODE_STRUCT *ds = data;
    if (ds->fed)
        return MAD_FLOW_STOP;
    ds->fed = 1;
    mad_stream_buffer(ms, ds->mpgbuf, (unsigned long)ds->mpgsize + MAD_BUFFER_GUARD);
    return MAD_FLOW_CONTINUE;
}

static void
search_for_silence (DECODE_STRUCT *ds, double vol)
{
    int i;
    for(i = 0; i < NUM_SILTRACKERS; i++) {
	SILENCETRACKER *pstracker = &ds->siltrackers[i];

	if (pstracker->foundsil)
	    continue;

	if (vol < pstracker->silencevol) {
	    if (pstracker->insilencecount == 0) {
		pstracker->silstart_samp = ds->pcmpos;
	    }
	    pstracker->insilencecount++;
	} else {
	    pstracker->insilencecount = 0;
	}

	if (pstracker->insilencecount > ds->silence_samples) {
	    pstracker->foundsil = TRUE;
	}
    }
}

static signed int
scale (mad_fixed_t sample)
{
    /* round */
    sample += (1L << (MAD_F_FRACBITS - 16));

    /* clip */
    if (sample >= MAD_F_ONE)
	sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)
	sample = -MAD_F_ONE;

    /* quantize */
    return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static enum mad_flow 
filter (void *data, struct mad_stream const *ms, struct mad_frame *frame)
{
    (void)frame;
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    FRAME_LIST* fl;

    fl = (FRAME_LIST*) malloc (sizeof(FRAME_LIST));
    if (!fl)
        return MAD_FLOW_BREAK;
    fl->m_framepos = ms->this_frame;
    fl->m_samples = 0;
    fl->m_pcmpos = 0;
    list_add_tail (&(fl->m_list), &(ds->frame_list));

#if defined (commentout)
    debug_printf ("FLT:  %p | 0x%x | %p | %p | 0x%x\n", 
	ms->buffer, 
	ms->bufend - ms->buffer, 
	ms->this_frame, 
	ms->next_frame,
	ms->skiplen
    );
    debug_printf (
	"FLT:  (%02x%02x) | 0x%x | 0x%x | 0x%x | 0x%x\n", 
	ms->this_frame[0], ms->this_frame[1], 
	ds->mpgpos_curr,
	ms->this_frame - ms->buffer,
	ds->mpgpos_curr + (ms->this_frame - ms->buffer),
	ds->mpgpos_next
    );
#endif

    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
output (void *data, struct mad_header const *header,
	struct mad_pcm *pcm)
{
    (void)header;
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    FRAME_LIST *fl;
    unsigned int nchannels, nsamples;
    mad_fixed_t const *left_ch, *right_ch;
    signed int sample;
    double v;

    nchannels = pcm->channels;
    nsamples  = pcm->length;
    left_ch   = pcm->samples[0];
    right_ch  = pcm->samples[1];

    /* Get frame entry */
    fl = list_entry (ds->frame_list.prev, FRAME_LIST, m_list);
    fl->m_samples = nsamples;
    fl->m_pcmpos = ds->pcmpos;

#if defined (commentout)
    if (ds->pcmpos > ds->len_to_sw_start_samp
	&& ds->pcmpos < ds->len_to_sw_end_samp) {
	debug_printf ("DEC *:  pcmpos 0x%x, nsamp 0x%x\n", 
	    fl->m_pcmpos, fl->m_samples);
    } else {
	debug_printf ("DEC -:  pcmpos 0x%x, nsamp 0x%x\n", 
	    fl->m_pcmpos, fl->m_samples);
    }
#endif

    while (nsamples--) {
	/* output sample(s) in 16-bit signed little-endian PCM */
	/* GCS FIX: Does this work on big endian machines??? */
	sample = (short) scale (*left_ch++);
	//	fwrite(&sample, sizeof(short), 1, fp);

	if (nchannels == 2) {
	    // make mono
	    int right = scale(*right_ch++);
            if (abs(right) > abs(sample))
                sample = right;
	}

	// get the instantanous volume
	v = (double)ds->prev_sample * ds->prev_sample + (double)sample * sample;
	v = sqrt(v / 2);
	if (ds->pcmpos > ds->len_to_sw_start_samp
	    && ds->pcmpos < ds->len_to_sw_end_samp)
	{
	    search_for_silence(ds, v);
	}
	ds->pcmpos++;
	ds->prev_sample = sample;
    }
    
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow 
header (void *data, struct mad_header const *pheader)
{
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    if (!ds->samplerate) {
	ds->samplerate = pheader->samplerate;
	ds->silence_samples = ds->silence_ms * ds->samplerate / 1000;
	ds->len_to_sw_start_samp = ds->len_to_sw_ms * ds->samplerate / 1000;
	ds->len_to_sw_end_samp = (ds->len_to_sw_ms + ds->searchwindow_ms) * ds->samplerate / 1000;
	debug_printf ("Setting samplerate: %ld\n",ds->samplerate);
    }
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
error (void *data, struct mad_stream *ms, struct mad_frame *frame)
{
    (void)data;
    (void)frame;
    if (MAD_RECOVERABLE(ms->error)) {
	debug_printf ("mad error 0x%04x %s\n", ms->error, 
	    mad_error_string (ms->error));
	return MAD_FLOW_CONTINUE;
    }

    debug_printf ("unrecoverable mad error 0x%04x %s\n",
        ms->error, mad_error_string(ms->error));
    return MAD_FLOW_BREAK;
}

/* The following routines have nothing to do with finding a separation 
 * point. Instead, they have to do with finding the bitrate.  However, 
 * they are included here because they are "mad" related.
 */
error_code
find_bitrate (unsigned long* bitrate, const char* mpgbuf, long mpgsize)
{
    struct mad_decoder decoder;
    GET_BITRATE_STRUCT gbs;
    int result;
    
    /* initialize and start decoder */
    gbs.mpgbuf = (unsigned char*) mpgbuf;
    gbs.mpgsize = mpgsize;
    gbs.bitrate = 0;
    mad_decoder_init (
	&decoder,
	&gbs,
	input_get_bitrate /* input */,
	header_get_bitrate /* header */,
	NULL /* filter */,
	NULL /* output */,
	NULL /* error */,
	NULL /* message */);
    result = mad_decoder_run (&decoder, MAD_DECODER_MODE_SYNC);
    mad_decoder_finish (&decoder);
    if (result != 0)
        return SR_ERROR_DECODE_FAILURE;
    *bitrate = gbs.bitrate;
    return SR_SUCCESS;
}

static enum mad_flow
input_get_bitrate (void *data, struct mad_stream *stream)
{
    GET_BITRATE_STRUCT* gbs = (GET_BITRATE_STRUCT*) data;

    if (!gbs->mpgsize)
	return MAD_FLOW_STOP;

    mad_stream_buffer(stream, gbs->mpgbuf, gbs->mpgsize);
    gbs->mpgsize = 0;
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
header_get_bitrate (void *data, struct mad_header const *pheader)
{
    GET_BITRATE_STRUCT* gbs = (GET_BITRATE_STRUCT*) data;

    gbs->bitrate = pheader->bitrate;	/* stream bitrate (bps) */
    debug_printf ("Decoded bitrate from stream: %ld\n", gbs->bitrate);
    return MAD_FLOW_STOP;
}
