/*
 * Copyright(c) 2014 Tim Ruehsen
 * Copyright(c) 2015-2019 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * Progress bar routines
 *
 * Changelog
 * 18.10.2014  Tim Ruehsen  created from src/bar.c
 *
 */

#include <config.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>
#include <wchar.h>

#include <wget.h>
#include "private.h"

/**
 * \file
 * \brief Progress Bar Routines
 * \defgroup libwget-progress Progress Display Functions
 * @{
 *
 * Methods for creating and printing a progress bar display.
 */


// We use enums to define the progress bar parameters because they are the
// closest thing we have to defining true constants in C without using
// preprocessor macros. The advantage of enums is that they will create a
// symbol in the symbol table making debugging a whole lot easier.

// Define the parameters for how the progress bar looks
enum _BAR_SIZES {
	_BAR_FILENAME_SIZE  = 20,
	_BAR_RATIO_SIZE     =  3,
	_BAR_METER_COST     =  2,
	_BAR_DOWNBYTES_SIZE =  8,
	_BAR_SPEED_SIZE     =  8,
};

// Define the cost (in number of columns) of the progress bar decorations. This
// includes all the elements that are not the progress indicator itself.
enum _BAR_DECOR_SIZE {
	_BAR_DECOR_COST =
		_BAR_FILENAME_SIZE  + 1 + \
		_BAR_RATIO_SIZE     + 2 + \
		_BAR_METER_COST     + 1 + \
		_BAR_DOWNBYTES_SIZE + 1 + \
		_BAR_SPEED_SIZE     + 3
};

enum _SCREEN_WIDTH {
	DEFAULT_SCREEN_WIDTH = 70,
	MINIMUM_SCREEN_WIDTH = 45,
};

enum _bar_slot_status_t {
	EMPTY = 0,
	DOWNLOADING = 1,
	COMPLETE = 2
};

/** The settings for drawing the progress bar.
 *
 *  This includes things like how often it is updated, how many values are
 *  stored in the speed ring, etc.
 */
enum _BAR_SETTINGS {
	/// The number of values to store in the speed ring
	SPEED_RING_SIZE   =  24,
};

typedef struct {
	char
		*progress,
		*filename,
		speed_buf[_BAR_SPEED_SIZE],
		human_size[_BAR_DOWNBYTES_SIZE];
	uint64_t
		file_size,
		time_ring[SPEED_RING_SIZE],
		bytes_ring[SPEED_RING_SIZE],
		bytes_downloaded;
	int
		ring_pos,
		tick,
		numfiles;
	enum _bar_slot_status_t
		status;
	bool
		redraw : 1;
} _bar_slot_t;

struct _wget_bar_st {
	_bar_slot_t
		*slots;
	char
		*progress_mem_holder,
		*unknown_size,
		*known_size,
		*spaces;
	int
		nslots,
		max_width;
	wget_thread_mutex_t
		mutex;
};

static char report_speed_type = WGET_REPORT_SPEED_BYTES;
static char report_speed_type_char = 'B';
static unsigned short speed_modifier = 1000;

// The progress bar may be redrawn if the window size changes.
// XXX: Don't handle that case currently. Instead, later test
// what happens if we don't explicitly redraw in such a case.
// For fast downloads, it doesn't matter. For slow downloads,
// the progress bar will maybe span across two lines till it
// gets redrawn. Ideally, this should be a part of the client
// code logic and not in the library.
// Tl;dr: Move window size detection to client. Allow client to
// specify rate at which speed stats should be updated. Speed
// ring size will remain constant (Don't want second heap allocation)
//  - darnir 29/07/2018
static void _bar_update_speed_stats(_bar_slot_t *slotp)
{
	int ring_pos = slotp->ring_pos;
	// In case this function is called with no downloaded bytes,
	// exit early
	if (slotp->bytes_downloaded == slotp->bytes_ring[ring_pos]) {
		return;
	}
	uint64_t curtime = wget_get_timemillis();

	// Increment the position pointer
	if (++ring_pos == SPEED_RING_SIZE)
		ring_pos = 0;

	slotp->bytes_ring[ring_pos] = slotp->bytes_downloaded;
	slotp->time_ring[ring_pos] = curtime;

	int next_pos = (ring_pos + 1 == SPEED_RING_SIZE) ? 0 : ring_pos + 1;
	if (slotp->bytes_ring[next_pos] == 0) {
		sprintf(slotp->speed_buf, " --.-K");
	} else {
		size_t bytes = slotp->bytes_ring[ring_pos] - slotp->bytes_ring[next_pos];
		size_t time = slotp->time_ring[ring_pos] - slotp->time_ring[next_pos];
		size_t speed = (bytes * speed_modifier) / time;

		wget_human_readable(slotp->speed_buf, sizeof(slotp->speed_buf), speed);
	}
	slotp->ring_pos = ring_pos;
}

static volatile sig_atomic_t winsize_changed;

static inline G_GNUC_WGET_ALWAYS_INLINE void
_restore_cursor_position(void)
{
	// ESC 8: Restore cursor position
	fputs("\0338", stdout);
}

static inline G_GNUC_WGET_ALWAYS_INLINE void
_bar_print_slot(const wget_bar_t *bar, int slot)
{
	// ESC 7: Save cursor
	// CSI <n> A: Cursor up
	// CSI <n> G: Cursor horizontal absolute
	wget_fprintf(stdout, "\0337\033[%dA\033[1G", bar->nslots - slot);
}

static inline G_GNUC_WGET_ALWAYS_INLINE void
_bar_set_progress(const wget_bar_t *bar, int slot)
{
	_bar_slot_t *slotp = &bar->slots[slot];

	if (slotp->file_size > 0) {
		size_t bytes = slotp->bytes_downloaded;
		int cols = (int) ((bytes / (double) slotp->file_size) * bar->max_width);
		if (cols > bar->max_width)
			cols = bar->max_width;
		else if (cols <= 0)
			cols = 1;

		// Write one extra byte for \0. This has already been accounted for
		// when initializing the progress storage.
		memcpy(slotp->progress, bar->known_size, cols - 1);
		slotp->progress[cols - 1] = '>';
		if (cols < bar->max_width)
			memset(slotp->progress + cols, ' ', bar->max_width - cols);
	} else {
		int ind = slotp->tick % (bar->max_width * 2 - 6);
		int pre_space;

		if (ind <= bar->max_width - 3)
			pre_space = ind;
		else
			pre_space = bar->max_width - (ind - bar->max_width + 5);

		memset(slotp->progress, ' ', bar->max_width);
		memcpy(slotp->progress + pre_space, "<=>", 3);
	}

	slotp->progress[bar->max_width] = 0;
}

/**
 * \param[in] s String possibly containing multibyte characters (eg UTF-8)
 * \param[in] available_space Number of columns available for display of s
 * \param[out] inspectedp where to store number of characters inspected from s
 * \param[out] padp where to store amount of white space padding
 *
 * Inspect that part of the multibyte string s which will consume up to
 * available_space columns on the screen
 * Each multibyte character can consume 0 or more columns on the screen
 * If the string as displayed is shorter than available_space, padding
 * will be required
 *
 * Starting with the first, each (possibly) multibyte sequence in s is
 * converted to the corresponding wide character.
 * Two values are derived in this process:
 * mblen: length of multi-byte sequence (eg 1 for ordinary ASCII)
 * wcwidth(wide): number of columns occupied by the wide character (>= 0)
 * The mblen values are summed up to determine how much of s has been
 * used in the inspection so far and the wcwidth(wide) values are summed up
 * to determine the position of a (virtual) cursor in the available space.
 */
static void
inspect_multibyte(char *s, size_t available_space, size_t *inspectedp, size_t *padp)
{
	unsigned int displayed = 0; /* number of columns displayed so far */
	int inspected = 0;          /* total number of bytes inspected from s */
	wchar_t wide;               /* wide character made from initial multibyte section */
	int mblen;                  /* length of initial multibyte section which was converted to "wide" */
	size_t remaining;

	if (!s) {
		*inspectedp = inspected;
		*padp = available_space;
		return;
	}

	remaining = strlen(s);	/* a slight optimization */

	/* while we have another character ... */
	while ((mblen = mbtowc(&wide, &s[inspected], remaining)) > 0) {
	    int wid = wcwidth(wide);

	    /*
	     * If we have filled exactly "available_size" columns
	     * and the next character is a zero-width character ...
	     * ... or ...
	     * if appending the wide character would exceed the given available_space ...
	     */
	    if ((wid == 0 && displayed == available_space) || displayed + wid > available_space)
		break; /* ... we're done */

	    /* we're not done, so advance in s ... */
	    inspected += mblen;
	    remaining -= mblen;

	    /* ... and advance cursor */
	    displayed += wid;
	}

	/*
	 * When we come here, we either have processed the entire multibyte
	 * string, then we will need to pad, or we have filled the available
	 * space, then there will be no padding.
	 */
	*inspectedp = inspected;
	*padp = available_space - displayed;
}

static void _bar_update_slot(const wget_bar_t *bar, int slot)
{
	_bar_slot_t *slotp = &bar->slots[slot];

	// We only print a progress bar for the slot if a context has been
	// registered for it
	if (slotp->status == DOWNLOADING || slotp->status == COMPLETE) {
		uint64_t max, cur;
		int ratio;
		size_t consumed, pad;

		max = slotp->file_size;
		cur = slotp->bytes_downloaded;

		ratio = max ? (int) ((100 * cur) / max) : 0;

		wget_human_readable(slotp->human_size, sizeof(slotp->human_size), cur);

		_bar_update_speed_stats(slotp);

		_bar_set_progress(bar, slot);

		_bar_print_slot(bar, slot);

		// The progress bar looks like this:
		//
		// filename   xxx% [======>      ] xxx.xxK
		//
		// It is made of the following elements:
		// filename     _BAR_FILENAME_SIZE      Name of local file
		// xxx%         _BAR_RATIO_SIZE + 1     Amount of file downloaded
		// []           _BAR_METER_COST         Bar Decorations
		// xxx.xxK      _BAR_DOWNBYTES_SIZE     Number of downloaded bytes
		// xxx.xxKB/s   _BAR_SPEED_SIZE         Download speed
		// ===>         Remaining               Progress Meter

		inspect_multibyte(slotp->filename, _BAR_FILENAME_SIZE, &consumed, &pad);
		wget_fprintf(stdout, "%-*.*s %*d%% [%s] %*s %*s%c/s",
				(int) (consumed+pad), (int) (consumed+pad), slotp->filename,
				_BAR_RATIO_SIZE, ratio,
				slotp->progress,
				_BAR_DOWNBYTES_SIZE, slotp->human_size,
				_BAR_SPEED_SIZE, slotp->speed_buf, report_speed_type_char);

		_restore_cursor_position();
		fflush(stdout);
		slotp->tick++;
	}
}

static int _bar_get_width(void)
{
	int width = DEFAULT_SCREEN_WIDTH;

	if (wget_get_screen_size(&width, NULL) == 0) {
		if (width < MINIMUM_SCREEN_WIDTH)
			width = MINIMUM_SCREEN_WIDTH;
		else
			width--; // leave one space at the end, else we see a linebreak on Windows
	}

	return width - _BAR_DECOR_COST;
}

static void _bar_update_winsize(wget_bar_t *bar, bool slots_changed) {

	if (winsize_changed || slots_changed) {
		int max_width = _bar_get_width();

		if (bar->max_width < max_width) {
			xfree(bar->known_size);
			bar->known_size = xmalloc(max_width);
			memset(bar->known_size, '=', max_width);

			xfree(bar->unknown_size);
			bar->unknown_size = xmalloc(max_width);
			memset(bar->unknown_size, '*', max_width);

			xfree(bar->spaces);
			bar->spaces = xmalloc(max_width);
			memset(bar->spaces, ' ', max_width);
		}
		if (bar->max_width < max_width || slots_changed) {
			xfree(bar->progress_mem_holder);
			// Add one extra byte to hold the \0 character
			bar->progress_mem_holder = xcalloc(bar->nslots, max_width + 1);
			for (int i = 0; i < bar->nslots; i++) {
				bar->slots[i].progress = bar->progress_mem_holder + (i * max_width);
			}
		}

		bar->max_width = max_width;
	}
	winsize_changed = 0;

}

static void _bar_update(wget_bar_t *bar)
{
	_bar_update_winsize(bar, false);
	for (int i = 0; i < bar->nslots; i++) {
		if (bar->slots[i].redraw || winsize_changed) {
			_bar_update_slot(bar, i);
			bar->slots[i].redraw = 0;
		}
	}
}

/**
 * \param[in] bar Pointer to a \p wget_bar_t object
 * \param[in] nslots Number of progress bars
 * \return Pointer to a \p wget_bar_t object
 *
 * Initialize a new progress bar instance for Wget. If \p bar is a NULL
 * pointer, it will be allocated on the heap and a pointer to the newly
 * allocated memory will be returned. To free this memory, call either the
 *  wget_bar_deinit() or wget_bar_free() functions based on your needs.
 *
 * \p nslots is the number of screen lines to reserve for printing the progress
 * bars. This may be any number, but you generally want at least as many slots
 * as there are downloader threads.
 */
wget_bar_t *wget_bar_init(wget_bar_t *bar, int nslots)
{
	/* Initialize screen_width if this hasn't been done or if it might
	   have changed, as indicated by receiving SIGWINCH.  */
	int max_width = _bar_get_width();

	if (nslots < 1 || max_width < 1)
		return NULL;

	if (!bar)
		bar = xcalloc(1, sizeof(*bar));
	else
		memset(bar, 0, sizeof(*bar));

	wget_thread_mutex_init(&bar->mutex);
	wget_bar_set_slots(bar, nslots);

	return bar;
}

/**
 * \param[in] bar Pointer to a wget_bar_t object
 * \param[in] nslots The new number of progress bars that should be drawn
 *
 * Update the number of progress bar lines that are drawn on the screen.
 * This is useful when the number of downloader threads changes dynamically or
 * to change the number of reserved lines. Calling this function will
 * immediately reserve \p nslots lines on the screen. However if \p nslots is
 * lower than the existing value, nothing will be done.
 */
void wget_bar_set_slots(wget_bar_t *bar, int nslots)
{
	wget_thread_mutex_lock(bar->mutex);
	int more_slots = nslots - bar->nslots;

	if (more_slots > 0) {
		bar->slots = wget_realloc(bar->slots, nslots * sizeof(_bar_slot_t));
		memset(bar->slots + bar->nslots, 0, more_slots * sizeof(_bar_slot_t));
		bar->nslots = nslots;

		for (int i = 0; i < more_slots; i++)
			fputs("\n", stdout);

		_bar_update_winsize(bar, true);
		_bar_update(bar);
	}
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to a wget_bar_t object
 * \param[in] slot The slot number to use
 * \param[in] filename The file name to display in the given \p slot
 * \param[in] new_file if this is the start of a download of the body of a new file
 * \param[in] file_size The file size that would be 100%
 *
 * Initialize the given \p slot of the \p bar object with it's (file) name to display
 * and the (file) size to be assumed 100%.
 */
void wget_bar_slot_begin(wget_bar_t *bar, int slot, const char *filename, int new_file, ssize_t file_size)
{
	wget_thread_mutex_lock(bar->mutex);
	_bar_slot_t *slotp = &bar->slots[slot];

	xfree(slotp->filename);
	if (new_file)
	    slotp->numfiles++;
	if (slotp->numfiles == 1) {
	    slotp->filename = wget_strdup(filename);
	} else {
	    char tag[20];	/* big enough to hold "xxx files\0" */
	    snprintf(tag, sizeof(tag), "%d files", slotp->numfiles);
	    slotp->filename = wget_strdup(tag);
	}
	slotp->tick = 0;
	slotp->file_size += file_size;
	slotp->status = DOWNLOADING;
	slotp->redraw = 1;
	slotp->ring_pos = 0;

	memset(&slotp->time_ring, 0, sizeof(slotp->time_ring));
	memset(&slotp->bytes_ring, 0, sizeof(slotp->bytes_ring));

	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to a wget_bar_t object
 * \param[in] slot The slot number to use
 * \param[in] nbytes The number of bytes downloaded since the last invocation of this function
 *
 * Set the current number of bytes for \p slot for the next update of
 * the bar/slot.
 */
void wget_bar_slot_downloaded(wget_bar_t *bar, int slot, size_t nbytes)
{
	wget_thread_mutex_lock(bar->mutex);
	bar->slots[slot].bytes_downloaded += nbytes;
	bar->slots[slot].redraw = 1;
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to a wget_bar_t object
 * \param[in] slot The slot number to use
 *
 * Redraw the given \p slot as being completed.
 */
void wget_bar_slot_deregister(wget_bar_t *bar, int slot)
{
	wget_thread_mutex_lock(bar->mutex);
	if (slot >= 0 && slot < bar->nslots) {
		_bar_slot_t *slotp = &bar->slots[slot];

		slotp->status = COMPLETE;
		_bar_update_slot(bar, slot);
	}
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to a wget_bar_t object
 *
 * Redraw the parts of the \p bar that have been changed so far.
 */
void wget_bar_update(wget_bar_t *bar)
{
	wget_thread_mutex_lock(bar->mutex);
	_bar_update(bar);
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to \p wget_bar_t
 *
 * Free the various progress bar data structures
 * without freeing \p bar itself.
 */
void wget_bar_deinit(wget_bar_t *bar)
{
	if (bar) {
		for (int i = 0; i < bar->nslots; i++) {
			xfree(bar->slots[i].filename);
		}
		xfree(bar->progress_mem_holder);
		xfree(bar->spaces);
		xfree(bar->known_size);
		xfree(bar->unknown_size);
		xfree(bar->slots);
		wget_thread_mutex_destroy(&bar->mutex);
	}
}

/**
 * \param[in] bar Pointer to \p wget_bar_t
 *
 * Free the various progress bar data structures
 * including the \p bar pointer itself.
 */
void wget_bar_free(wget_bar_t **bar)
{
	if (bar) {
		wget_bar_deinit(*bar);
		xfree(*bar);
	}
}

/**
 * \param[in] bar Pointer to \p wget_bar_t
 * \param[in] slot The slot number to use
 * \param[in] display The string to be displayed in the given slot
 *
 * Displays the \p display string in the given \p slot.
 */
void wget_bar_print(wget_bar_t *bar, int slot, const char *display)
{
	wget_thread_mutex_lock(bar->mutex);
	_bar_print_slot(bar, slot);
	// CSI <n> G: Cursor horizontal absolute
	wget_fprintf(stdout, "\033[27G[%-*.*s]", bar->max_width, bar->max_width, display);
	_restore_cursor_position();
	fflush(stdout);
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * \param[in] bar Pointer to \p wget_bar_t
 * \param[in] slot The slot number to use
 * \param[in] fmt Printf-like format to build the display string
 * \param[in] args Arguments matching the \p fmt format string
 *
 * Displays the \p string build using the printf-style \p fmt and \p args.
 */
void wget_bar_vprintf(wget_bar_t *bar, int slot, const char *fmt, va_list args)
{
	char text[bar->max_width + 1];

	wget_vsnprintf(text, sizeof(text), fmt, args);
	wget_bar_print(bar, slot, text);
}

/**
 * \param[in] bar Pointer to \p wget_bar_t
 * \param[in] slot The slot number to use
 * \param[in] fmt Printf-like format to build the display string
 * \param[in] ... List of arguments to match \p fmt
 *
 * Displays the \p string build using the printf-style \p fmt and the given arguments.
 */
void wget_bar_printf(wget_bar_t *bar, int slot, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	wget_bar_vprintf(bar, slot, fmt, args);
	va_end(args);
}

/**
 * Call this function when a resize of the screen / console has been detected.
 */
void wget_bar_screen_resized(void)
{
	winsize_changed = 1;
}

/**
 *
 * \param[in] bar Pointer to \p wget_bar_t
 * @param buf Pointer to buffer to be displayed
 * @param len Number of bytes to be displayed
 *
 * Write 'above' the progress bar area, scrolls screen one line up
 * if needed. Currently used by Wget2 to display error messages in
 * color red.
 *
 * This function needs a redesign to be useful for general purposes.
 */
void wget_bar_write_line(wget_bar_t *bar, const char *buf, size_t len)
{
	wget_thread_mutex_lock(bar->mutex);
	// ESC 7:    Save cursor
	// CSI <n>S: Scroll up whole screen
	// CSI <n>A: Cursor up
	// CSI <n>G: Cursor horizontal absolute
	// CSI 0J:   Clear from cursor to end of screen
	// CSI 31m:  Red text color
	wget_fprintf(stdout, "\0337\033[1S\033[%dA\033[1G\033[0J\033[31m", bar->nslots + 1);
	fwrite(buf, 1, len, stdout);
	fputs("\033[m", stdout); // reset text color
	_restore_cursor_position();

	_bar_update(bar);
	wget_thread_mutex_unlock(bar->mutex);
}

/**
 * @param type Report speed type
 *
 * Set the progress bar report speed type to WGET_REPORT_SPEED_BYTES
 * or WGET_REPORT_SPEED_BITS.
 *
 * Default is WGET_REPORT_SPEED_BYTES.
 */
void wget_bar_set_speed_type(char type)
{
	report_speed_type = type;
	if (type == WGET_REPORT_SPEED_BITS) {
		report_speed_type_char = 'b';
		speed_modifier = 8;
	}

}
/** @}*/
