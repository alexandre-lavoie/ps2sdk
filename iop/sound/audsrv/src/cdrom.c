/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2005, ps2dev - http://www.ps2dev.org
# Licenced under GNU Library General Public License version 2
*/

/**
 * @file
 * cdsrv IOP server.
 */

#include <stdio.h>
#include <thbase.h>
#include <thsemap.h>
#include <loadcore.h>
#include <sysmem.h>
#include <intrman.h>
#include <sifcmd.h>
#include <libsd.h>
#include <cdvdman.h>
#include <sysclib.h>

#include <audsrv.h>
#include "cdrom.h"
#include "common.h"
#include "rpc_server.h"
#include "rpc_client.h"
#include "upsamplers.h"
#include "spu.h"
#include "hw.h"

/* cdda */
/** initialization status */
static int cdda_initialized = 0;
/** cd status */
static int cd_playing = 0;
/** cd is paused (but playing) */
static volatile int cd_paused = 0;
/** thread id for cdda player */
static int cdda_play_tid = -1;
/** current sector being played */
static volatile int cdda_pos = 0;
/** first sector played */
static int cdda_play_start = 0;
/** last sector to be played */
static int cdda_play_end = 0;
/** toc for cdda */
static cdda_toc toc;
/** unparsed toc data */
static unsigned char raw_toc[3000];

/** SPU2 cd transfer complete semaphore */
static int cd_transfer_sema = -1;

/** cdda ring buffer */
static u8 cd_ringbuf[SECTOR_SIZE*8] __attribute__((aligned (16)));
/** used upon overflow */
static u8 cd_sparebuf[1880] __attribute__((aligned (16)));
static u8 core0_buf[0x1000] __attribute__((aligned (16)));

static short cd_rendered_left[512];
static short cd_rendered_right[512];

static int audsrv_cd_init()
{
	toc.num_tracks = 0;

	if (cd_transfer_sema < 0)
	{
		cd_transfer_sema = CreateMutex(0);
		if (cd_transfer_sema < 0)
		{
			return AUDSRV_ERR_OUT_OF_MEMORY;
		}
	}

	return 0;
}

/** Transfer complete callback for cdda
 * @param arg     not used
 * @returns true, always

 * Generated by SPU2, when a block was transmitted and now putting
 * to upload a second block to the other buffer.
 */
static int cd_transfer_complete(void *arg)
{
	(void)arg;

	iSignalSema(cd_transfer_sema);
	return 1;
}

/** Returns the type of disc in drive
 * @returns disc type (according to native PS2 values) or -AUDSRV_ERR_NO_DISC
 */
int audsrv_get_cd_type()
{
	int type = sceCdGetDiskType();

	if (type == 0)
	{
		/* no disc inserted */
		return -AUDSRV_ERR_NO_DISC;
	}

	return type;
}

/** Returns CD drive status
 * @returns status according to PS2 native values
 */
int audsrv_get_cd_status()
{
	return sceCdStatus();
}

/** Internal function to process raw toc loaded from disc
 * @returns status code
 */
static int process_toc()
{
	int track;

	/* retrieve toc and parse it */
	sceCdGetToc(raw_toc);
	toc.num_tracks = btoi(raw_toc[17]);

	//print_hex_buffer(raw_toc, sizeof(raw_toc));

	for (track=0; track<toc.num_tracks; track++)
	{
		unsigned char *ptr;

		ptr = raw_toc + 37 + (10*track);
		toc.tracks[track].track = track;
		toc.tracks[track].minute = btoi(ptr[0]);
		toc.tracks[track].second = btoi(ptr[1]);
		toc.tracks[track].sector = btoi(ptr[2]);
	}

	// FIXME: add extra track
	toc.tracks[toc.num_tracks].track = toc.num_tracks;
	toc.tracks[toc.num_tracks].minute = btoi(raw_toc[34]);
	toc.tracks[toc.num_tracks].second = btoi(raw_toc[35]);
	toc.tracks[toc.num_tracks].sector = btoi(raw_toc[36]);

	printf("audsrv: found %d tracks\n", toc.num_tracks);
	return 0;
}

static int initialize_cdda()
{
	int err, dummy, type;

	if (cdda_initialized)
	{
		/* no need to reinitialize */
		return 0;
	}

	printf("initializing cdda\n");

	err = audsrv_cd_init();
	if (err < 0)
	{
		return err;
	}

	/* initialize cdrom and set media = cd */
	sceCdInit(0);

	/* make sure disc is inserted, and check for cdda */
	sceCdTrayReq(2, (u32 *)&dummy);
	printf("TrayReq returned %d\n", dummy);

	sceCdDiskReady(0);
	sceCdSync(0);

	type = audsrv_get_cd_type();
	printf("audsrv: disc type: %d\n", type);

	if (type != 0x11 && type != 0x13 && type != 0xfd)
	{
		/* not a cdda disc, or a ps1/ps2 with cdda tracks */
		printf("audsrv: not a cdda disc!\n");
		return -AUDSRV_ERR_ARGS;
	}

	process_toc();

	cdda_initialized = 1;
	printf("audsrv: cdda initialization completed successfully\n");
	return 0;
}

/** Returns the number of tracks available on the CD in tray
 * @returns positive track count, or negative error status code
 */
int audsrv_get_numtracks()
{
	int err = initialize_cdda();
	if (err < 0)
	{
		return err;
	}

	return toc.num_tracks;
}

/** Returns the first sector for the given track
 * @param track   track index, must be between 1 and the trackcount
 * @returns sector number, or negative status code
 */
int audsrv_get_track_offset(int track)
{
	int err;
	int offset;

	err = initialize_cdda();
	if (err < 0)
	{
		return err;
	}

	if (track < 1 || track > toc.num_tracks)
	{
		return -AUDSRV_ERR_ARGS;
	}

	offset = (toc.tracks[track].minute) * 60 + toc.tracks[track].second;
	offset = (offset * 75) + toc.tracks[track].sector - 150;
	return offset;
}

/** Reads several sectors from CDDA track
 * @param dest    output buffer, must be atleast count*2352 bytes long
 * @param sector  first sector
 * @param count   amount of sectors to read
 * @returns total sectors read, negative on error
 */
static int read_sectors(void *dest, int sector, int count)
{
	sceCdRMode mode;
	int max_retries = 32;
	int tries = 0;

	mode.trycount = max_retries;
	mode.spindlctrl = SCECdSpinNom;
	mode.datapattern = SCECdSecS2048;
	mode.pad = 0;

	while (tries < max_retries)
	{
		/* wait until CD is ready to receive commands */
		sceCdDiskReady(0);

		if (sceCdReadCDDA(sector, count, dest, &mode))
		{
			/* success! */
			break;
		}

		tries++;
	}

	sceCdSync(0);

	if (tries == max_retries)
	{
		return -1;
	}

	return count;
}

/** Stops cd SPU transmission */
static void audsrv_stop_cd_stream()
{
	cd_playing = 0;
	cdda_pos = 0;
	cdda_play_start = 0;
	cdda_play_end = 0;

	/* disable streaming callbacks */
	sceSdSetTransCallback(SD_CORE_0, NULL);
	sceSdBlockTrans(SD_CORE_0, SD_TRANS_STOP, 0, 0, 0);
}

/** Pauses CD playing
 * @returns status code
 */
int audsrv_cd_pause()
{
	cd_paused = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Resumes CD playing
 * @returns status code
 */
int audsrv_cd_resume()
{
	cd_paused = 0;
	return AUDSRV_ERR_NOERROR;
}

/** Internal cdda feeding thread
 * @param arg    (not used)
 */
static void cdda_procedure(void *arg)
{
	int sz;
	int nsectors;
	int intr_state;
	int offset, last_read;
	upsample_t up;
	upsampler_t up44k1;

	(void)arg;

	printf("cdda_procedure started with %d, %d\n", cdda_play_start, cdda_play_end);

	if (cdda_play_start >= cdda_play_end)
	{
		/* nothing to play :| */
		return;
	}

	/* audio discs are 44100hz, 16bit, stereo */
	up44k1 = find_upsampler(44100, 16, 2);

	cdda_pos = cdda_play_start;
	printf("playing sectors %d - %d (%d sectors)\n", cdda_play_start, cdda_play_end, cdda_play_end - cdda_play_start);

	/* fill entire buffer before starting to play */
	offset = 0;
	nsectors = sizeof(cd_ringbuf) / SECTOR_SIZE;
	printf("filling buffer with nsectors %d..\n", nsectors);
	if (read_sectors(cd_ringbuf, cdda_pos, nsectors) != nsectors)
	{
		printf("failed to read %d sectors..\n", nsectors);
		return;
	}

	cdda_pos = cdda_pos + nsectors;

	printf("sectors read.. now setting callbacks\n");

	/* kick cd streaming */
	sceSdSetTransCallback(SD_CORE_0, (void *)cd_transfer_complete);
	sceSdBlockTrans(SD_CORE_0, SD_TRANS_LOOP, core0_buf, sizeof(core0_buf), 0);

	printf("callbacks kicked, starting loop\n");
	last_read = 0;

	memset(cd_rendered_left, 0, sizeof(cd_rendered_left));
	memset(cd_rendered_right, 0, sizeof(cd_rendered_right));

	while (cdda_pos < cdda_play_end)
	{
		int block;
		u8 *bufptr;

		/* wait until it's safe to transmit */
		WaitSema(cd_transfer_sema);

		/* suspend all interrupts */
		CpuSuspendIntr(&intr_state);

		block = 1 - (sceSdBlockTransStatus(SD_CORE_0, 0) >> 24);
		bufptr = core0_buf + (block << 11);

		if (cd_paused == 0)
		{
			up.left = (short *)cd_rendered_left;
			up.right = (short *)cd_rendered_right;

			if ((unsigned int)(offset + 1880) < sizeof(cd_ringbuf))
			{
				/* enough bytes in ringbuffer */
				up.src = cd_ringbuf + offset;
				offset = offset + up44k1(&up);
			}
			else
			{
				/* two portions */
				sz = sizeof(cd_ringbuf) - offset;
				wmemcpy(cd_sparebuf, cd_ringbuf + offset, sz);
				wmemcpy(cd_sparebuf + sz, cd_ringbuf + 0, 1880 - sz);
				offset = 1880 - sz;
				up.src = cd_sparebuf;
				up44k1(&up);
			}

			/* upsample 44k1 -> 48k0 */
			wmemcpy(bufptr +    0, cd_rendered_left + 0, 512);
			wmemcpy(bufptr +  512, cd_rendered_right + 0, 512);
			wmemcpy(bufptr + 1024, cd_rendered_left + 256, 512);
			wmemcpy(bufptr + 1536, cd_rendered_right + 256, 512);
		}
		else
		{
			/* paused, send mute */
			memset(bufptr, '\0', 2048);
		}

		CpuResumeIntr(intr_state);

		if ((offset / SECTOR_SIZE) != last_read)
		{
			/* read another sector.. */
			if (read_sectors(cd_ringbuf + (last_read * SECTOR_SIZE), cdda_pos, 1) != 1)
			{
				printf("failed to read 1 sector\n");
				break;
			}

			last_read = (offset / SECTOR_SIZE);
			cdda_pos++;
		}
	}

	audsrv_stop_cd();

	/* notify cdda ended */
	call_client_callback(AUDSRV_CDDA_CALLBACK);
}

/** Returns the current sector being played
 * @return sector number, or negative value on error
 */
int audsrv_get_cdpos()
{
	if (cd_playing == 0)
	{
		/* cdrom not playing */
		return 0;
	}

	return cdda_pos;
}

/** Returns the current sector being played, relative to first sector in track
 * @return sector number, or negative value on error
 */
int audsrv_get_trackpos()
{
	if (cd_playing == 0)
	{
		/* cdrom not playing */
		return 0;
	}

	return cdda_pos - cdda_play_start;
}

/** Starts playing cdda sectors from disc
 * @param start  first sector to play
 * @param end    stop playing at this sector
 * @returns status code
 *
 * Starts the cdda feeding thread at the given sector. Any previously
 * set up playback is stopped. If a callback was set, it will NOT be
 * called, as the thread is stopped abnormally.
 */
int audsrv_cd_play_sectors(int start, int end)
{
	printf("audsrv: cd_play_sectors: %d %d\n", start, end);

	if (initialize_cdda() < 0)
	{
		printf("audsrv: initialized cdda failed\n");
		return AUDSRV_ERR_NOT_INITIALIZED; //FIXME
	}

	/* stop previous track (and delete running thread) */
	audsrv_stop_cd();

	cdda_play_start = start;
	cdda_play_end = end;
	cdda_pos = start;

	printf("audsrv: creating cdda feed thread\n");

	/* .. and start a new one! */
	cdda_play_tid = create_thread(cdda_procedure, 48, 0);
	if (cdda_play_tid < 0)
	{
		return AUDSRV_ERR_OUT_OF_MEMORY;
	}

	printf("cdda thread 0x%x started\n", cdda_play_tid);

	cd_playing = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Plays CD audio track
 * @param track    segment to play [1 .. 99]
 * @returns error status
 */
int audsrv_play_cd(int track)
{
	int type;
	int ret;
	int start, end;

	printf("request to play track %d\n", track);

	if (initialize_cdda() < 0)
	{
		printf("audsrv: initialized cdda failed\n");
		return AUDSRV_ERR_NOT_INITIALIZED; //FIXME
	}

	if (track < 1 || track > toc.num_tracks)
	{
		/* invalid track selected */
		return AUDSRV_ERR_ARGS;
	}

	type = sceCdGetDiskType();
	if (track == 1 && (type == 11 || type == 0x13))
	{
		/* first track is data */
		printf("audsrv: request to play data track\n");
		return AUDSRV_ERR_ARGS;
	}

	start = audsrv_get_track_offset(track - 1);
	end = audsrv_get_track_offset(track);

	if (start < 0 || end < 0 || end < start)
	{
		printf("audsrv: invalid track offsets %d, %d\n", start, end);
		return AUDSRV_ERR_ARGS; //FIXME:
	}

	ret = audsrv_cd_play_sectors(start, end);

	/* core0 input (known as Evilo's patch #1) */
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLL, 0x7fff);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLR, 0x7fff);

	/* set master volume for core 0 */
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLL, MAX_VOLUME);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLR, MAX_VOLUME);

	return ret;
}

/** Stops CD play
 * @returns 0, always
 *
 * Stops CD from being played; this has no effect on other music
 * audsrv is currently playing
 */
int audsrv_stop_cd()
{
	audsrv_stop_cd_stream();

	/* stop playing thread */
	if (cdda_play_tid >= 0)
	{
		TerminateThread(cdda_play_tid);
		DeleteThread(cdda_play_tid);
		cdda_play_tid = -1;
	}

	return 0;
}

