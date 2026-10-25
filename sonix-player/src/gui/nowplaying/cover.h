#ifndef COVER_H
#define COVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// A cover picture already decoded and scaled to the size it is drawn at, in
// the display's own pixel format, so drawing it costs nothing more than a blit.
//
// The player's cover and an album page's head also carry the picture's colour,
// read from the thumbnail database or worked out once on the loader thread and
// written there (see cover_load_player_images, cover_load_shared), so the page
// that shows the picture does not work it out again every time it opens.
typedef struct {
	lv_image_dsc_t dsc;	 // hand this straight to lv_image_set_src()
	uint8_t *pixels;	 // pixel buffer referenced by dsc.data
	bool has_tone;		 // the three below are filled in
	uint32_t tone;		 // cover_page_tone() at COVER_PAGE_LIGHT_FROM: the sleeve's colour,
	int16_t tone_luma;	 // its brightness as a whole,
	bool tone_light;	 // and whether a page in it is a light one
} cover_image_t;

// How bright a sleeve has to be for a page in its colour to be a light one,
// written in dark ink. The colour kept with a picture is worked out at this
// threshold.
#define COVER_PAGE_LIGHT_FROM 150

// Where the on-disk thumbnail cache lives -- normally a hidden folder on the
// card, which is what the stock player does too (it keeps its own JPEGs under
// .hiby/.../cache/). Call this once at startup, before the browser opens.
// Passing NULL, or a directory that cannot be written to, falls back to /tmp.
void cover_set_cache_dir(const char *sd_root);

// Opens the cache again in the same place when its file was deleted or
// replaced under the open handle; covers cached in memory stay until they are
// recycled.
void cover_cache_reopen_if_replaced(void);

// How a picture is mapped onto its box.
typedef enum {
	COVER_FIT_CONTAIN, // whole picture visible, letterboxed, never upscaled
	COVER_FIT_COVER,   // fills the box edge to edge, centre-cropping the rest
} cover_fit_t;

// Decodes the cover art of a track (embedded picture first, then a cover file
// in its folder) into a box_w x box_h box.
bool cover_load_for_file(const char *filepath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// Same for a folder: only looks at cover.jpg / folder.png / ... inside it.
bool cover_load_for_dir(const char *dirpath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// A picture given by path, with no track and no folder to search: the file
// itself is the artwork. AirPlay is the caller -- shairport writes what the
// phone sent to /tmp/cover-<md5>.jpg and hands over that path.
bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// The same from bytes already in memory, for a picture that has no path of its
// own: the cover of an EPUB, which lives inside the book's ZIP and would have
// to be written to the card first for the call above to reach it.
bool cover_load_image_memory(const void *data, size_t size, int box_w, int box_h, cover_fit_t fit,
							 cover_image_t *out);

// What the artwork for this file IS, as one number, without decoding it.
//
// The player asks this before it throws a cover away: two tracks of the same
// album carry the same picture, and there is no reason to decode it, resample
// it and blur it again for the second one -- which is a second of work on this
// processor and a cover that blinks out and back for no change at all.
//
// The answer is about the BYTES and not about the folder or the album tag, so
// it is right in the cases a tag cannot settle: a picture embedded identically
// in every track of a record, a cover.jpg that all the tracks of a folder fall
// back to, and a compilation whose tracks really do carry different pictures.
// Whichever source would answer for the artwork is the one hashed, so a
// picture that cannot be decoded compares as itself and both tracks fall back
// to the same place.
//
// 0 means there is no artwork at all. Cheap: it reads the picture but never
// decodes it, and hashes its size with a slice of each end. For a track whose
// picture is embedded the answer is kept in the thumbnail database, keyed by
// the file's path, size and mtime, and the picture is not read again.
uint64_t cover_source_id(const char *filepath);

// True when the player will not need the picture embedded in `filepath` at
// all: its id is kept (see above), and so is a cover made from it. Reads
// neither the picture nor the cover, only the two tables. Any thread.
bool cover_picture_known(const char *filepath);

// Builds both images the player screen needs: the artwork itself, and the
// upside-down blurred copy drawn behind the controls. A picture already shown
// at this size comes out of the thumbnail database (one row per picture, shared
// by the tracks that carry it) and the backdrop is made from it; otherwise the
// artwork is decoded once for both and the cover stored. Either output may be
// skipped by passing NULL for its size. Returns false if the track has no
// artwork at all, leaving both outputs zeroed.
bool cover_load_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out);

// Storing the cover in the thumbnail database means encoding it as a JPEG, the
// slowest step after the decode, and nobody waits for it. Between
// cover_defer_player_store(true) and (false), this thread's loaders keep a copy
// instead, and cover_store_deferred() writes it once the picture has been
// handed on.
void cover_defer_player_store(bool on);
void cover_store_deferred(void);

// Puts the player's pictures for `filepath` in the on-disk cache without
// handing them to anybody: the next track's, decoded while this one plays, so
// that skipping to it reads them back instead of decoding. Does nothing when
// they are already cached or the track has no artwork. Always returns false
// with both outputs zeroed (the coverloader's dual-job signature).
bool cover_warm_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out);

// Makes the player's cover of `filepath` at cover_w x cover_h and keeps it in
// the thumbnail database, as cover_load_player_images() does, so that the
// track's cover later comes out of the cache. Does nothing when the track has no
// artwork or its cover is already kept at that size. Loader thread only.
void cover_prefetch_player(const char *filepath, int cover_w, int cover_h);

// The two pictures the screensaver needs from one photograph: the picture
// itself filling the screen, and the blurred block along the bottom that the
// clock and the track name sit on.
//
// Not cover_load_player_images() with different numbers: there the blurred
// block sits BELOW the artwork and is a mirrored centre crop of it, while here
// it sits ON TOP of a picture that fills the whole screen, so it has to be the
// bottom band of that same picture, blurred and the right way up. Anything
// else reads as a second photograph laid over the first.
//
// One decode feeds both. False when the file cannot be read or decoded, with
// both outputs zeroed.
bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *image_out, int strip_w,
								   int strip_h, cover_image_t *strip_out);

// A blurred, dimmed copy of a picture already scaled and packed, the same size
// as the original. What the sleeve turns into when the VU meters are opened
// over it, treated exactly like the block behind the controls so the two read
// as one surface. The caller owns `out` and must cover_free() it.
bool cover_blur_copy(const cover_image_t *src, cover_image_t *out);

// The same treatment at another size: the picture centre-cropped to the shape
// of out_w x out_h, then blurred and dimmed at that size. What the lyrics put
// behind the whole screen in the arrangements whose own blurred copy only
// covers the controls. The caller owns `out` and must cover_free() it.
bool cover_blur_fill(const cover_image_t *src, int out_w, int out_h, cover_image_t *out);

// The one colour that stands for a sleeve, as 0xRRGGBB. Cover Flow throws it
// under the record in the middle; the player's alternative layout tints the
// title pill, the waveform and the star with it. A grey sleeve comes back a
// warm white rather than a colour squeezed out of rounding noise. Zero when
// there is nothing to look at.
uint32_t cover_dominant_tone(const cover_image_t *cover);

// How bright a sleeve looks as a whole, 0..255 (Rec. 601 luma of its average),
// or -1 when there is nothing to look at. cover_dominant_tone() says what colour
// a sleeve is and always answers at full strength; this says whether it is a
// dark record or a light one, for a page that takes the sleeve's colour and
// has to choose the ink to write on it in.
int cover_mean_luma(const cover_image_t *cover);

// The colour a page painted in the sleeve's colours takes, as 0xRRGGBB in
// *tone, without the push to full strength that a light needs and a painted
// surface does not. One family of the sleeve's hues rather than an average of
// all of them, so a two-colour sleeve gives one of its two colours and not the
// mud between them. *luma is cover_mean_luma(), and *light says whether the
// page should be a light one: a sleeve at least light_from bright, or one
// mostly of a bright yellow, which only darkens to olive. False when there is
// nothing to look at.
bool cover_page_tone(const cover_image_t *cover, int light_from, uint32_t *tone, int *luma, bool *light);

// A colour from cover_page_tone() at full strength, the way
// cover_dominant_tone() answers: for a mark drawn over a dark backdrop, where
// the colour of a painted page would be too faint to read. A colour that is
// hardly a colour at all comes back the same warm white.
uint32_t cover_tone_stretched(uint32_t tone);

// A square picture of `size` made from the player's own cover of `filepath`
// (player_w x player_h, the size the now-playing page draws it at). The player's
// table of covers is asked first, so a record already played comes back
// without its file being read; otherwise the cover is decoded at the player's
// size and kept there, so the player finds it the first time the record plays.
// What an album's page puts at its head. Runs on the loader thread.
bool cover_load_shared(const char *filepath, int player_w, int player_h, int size, cover_image_t *out);

// A copy of `src` scaled to fill a square of `size`, with its colour kept. For
// a picture already on screen that another page wants smaller, on the spot:
// a scale, no decode.
bool cover_scaled_copy(const cover_image_t *src, int size, cover_image_t *out);

// The id cover_source_id() keeps for `filepath`, when it keeps one: from the
// table only, nothing is read. False for a track not seen yet.
bool cover_known_id(const char *filepath, uint64_t *id);
// The colour a page in this file's picture wears, when it has been worked out
// before (cover_colours): two keyed lookups, no picture read. False otherwise.
bool cover_tone_known(const char *filepath, uint32_t *tone, int *luma, bool *light);

void cover_free(cover_image_t *img);

// How the blurred backdrop is toned down so text stays readable on top of it:
// a dark UI darkens the picture, a light one washes it out towards white.
// Takes effect the next time a backdrop is built.
void cover_set_backdrop_light(bool light);

// Whether the blurred backdrop is built the right way up.
//
// Behind the controls it is turned over: it stands below the sleeve and reads
// as its reflection. Filling the screen with the sleeve shown on top of it, it
// has to face the same way the sleeve does. Takes effect the next time a
// backdrop is built; the screensaver's bottom band is never flipped either way.
void cover_set_backdrop_upright(bool upright);

// ---------------------------------------------------------------------------
// Browser thumbnails
//
// The browser owns each thumbnail and frees it the moment its row leaves the
// screen, so the memory held by artwork is proportional to what is visible and
// not to how far the user has scrolled. Reloading is cheap because the scaled
// thumbnail is kept on the card (see cover_set_cache_dir): a row coming back
// into view is a few-kilobyte read, not a decode.
//
// For a track, the folder's artwork is looked up first -- every file in an
// album resolves to the same picture, so they all share one cache entry.
// ---------------------------------------------------------------------------

// Loads a square thumbnail. The caller owns `out` and must cover_free() it.
// Returns false when there is no artwork for this path.
bool cover_thumb_load(const char *path, bool is_dir, int size, cover_image_t *out);

// Like cover_thumb_load, but only looks in the on-disk thumbnail cache --
// never decodes. For pages that want covers instantly or not at all: the queue
// list builds a hundred rows in one go, where a hundred small cache reads are
// affordable and a hundred decodes are not.
bool cover_thumb_cached(const char *path, int size, cover_image_t *out);

// ---------------------------------------------------------------------------
// Book covers
//
// An EPUB's cover is inside the book: reaching it means opening the ZIP and
// reading the OPF, then decoding the picture. The shelf keeps the scaled cover
// and the book's title in the thumbnail cache instead, keyed by the book's
// path, size and modification time and by the box it is drawn in, so a book
// is opened once and every later visit is a read. What a page shows is still
// its own and is freed with the page.
// ---------------------------------------------------------------------------

// A cached cover for `path` at `box_w` x `box_h`. True on a hit: then
// `*has_cover` says whether the book has a cover at all, `out` holds it when it
// does (the caller cover_free()s it), and `title` what the book calls itself,
// empty when it does not say.
bool cover_book_cached(const char *path, int box_w, int box_h, cover_image_t *out, bool *has_cover, char *title,
					   size_t title_size);

// Keeps what was read from the book: its cover, or NULL for a book without
// one, and its title.
void cover_book_store(const char *path, int box_w, int box_h, const cover_image_t *cover, const char *title);

#endif // COVER_H
