#ifndef ST_MACOS_RENDERER_H
#define ST_MACOS_RENDERER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	float r, g, b, a;
} MacColor;

enum MacRenderLayer {
	MAC_LAYER_BACKGROUND = 0,
	MAC_LAYER_TEXT,
	MAC_LAYER_DECORATION,
	/* Per-placement changed cells: above their image, below higher images. */
	MAC_LAYER_IMAGE_BACKGROUND,
	MAC_LAYER_IMAGE_TEXT,
	MAC_LAYER_IMAGE_DECORATION,
	MAC_LAYER_OVERLAY_BACKGROUND,
	MAC_LAYER_OVERLAY_TEXT,
	MAC_LAYER_OVERLAY_DECORATION,
	MAC_LAYER_COUNT
};

enum MacFontStyle {
	MAC_FONT_BOLD = 1 << 0,
	MAC_FONT_ITALIC = 1 << 1,
	MAC_FONT_EMOJI = 1 << 2,
};

int mac_renderer_init(void *mtkview, const char *font_name,
		double font_size);
void mac_renderer_destroy(void);
void mac_renderer_set_font(const char *font_name, double font_size);
void mac_renderer_set_scale(double backing_scale);
double mac_renderer_scale(void);
double mac_renderer_font_size(void);
double mac_renderer_ascent(void);
double mac_renderer_descent(void);
double mac_renderer_leading(void);
double mac_renderer_cell_width(void);
double mac_renderer_cell_height(void);
double mac_renderer_text_width(const char *text, size_t len,
		double font_scale);

int mac_renderer_begin(MacColor clear_color);
void mac_renderer_rect(enum MacRenderLayer layer, double x, double y,
		double width, double height, MacColor color);
void mac_renderer_rune(enum MacRenderLayer layer, uint32_t rune,
		unsigned int style, double x, double top, double baseline,
		double max_width, double max_height, MacColor color);
void mac_renderer_text(enum MacRenderLayer layer, const char *text,
		size_t len, double x, double baseline, double font_scale,
		MacColor color);
void mac_renderer_image(int stage, uint64_t serial, const uint8_t *rgba,
		int image_width, int image_height, int source_x, int source_y,
		int source_width, int source_height, double x, double y,
		double width, double height);
void mac_renderer_set_image_clip(double x, double y, double width,
		double height);
/* Smooth scrolling: quads emitted until the next call (cells, decorations,
 * images) move down by `dy` points and, if `clip`, are clipped to the
 * rectangle [clip_left, clip_right) x [clip_top, clip_bottom) points.
 * mac_renderer_set_row(0, 0, 0, 0, 0, 0) resets. */
void mac_renderer_set_row(double dy, int clip, double clip_left,
		double clip_top, double clip_right, double clip_bottom);
void mac_renderer_remove_image(uint64_t serial);
/* st video: draw image `serial` from `texture` (a bridged id<MTLTexture>)
 * instead of uploading pixels; mac_renderer_image then accepts NULL rgba. */
void mac_renderer_set_image_texture(uint64_t serial, void *texture);
/* CFRelease `object` once the GPU has finished the frame being built (or
 * the next one committed): a texture an earlier frame may still sample. */
void mac_renderer_release_after_frame(const void *object);
void mac_renderer_end(void);
/* Launch diagnostics: frames committed so far, and a one-shot callback, on
 * the main queue, with the next committed frame's presentation time
 * (CACurrentMediaTime's clock; 0 if it was never shown). */
unsigned long mac_renderer_frames(void);
void mac_renderer_on_next_present(void (*callback)(double presented));
/* Frame timing log: seconds the last mac_renderer_begin() waited for a
 * drawable, and a hook called on the main queue with each committed
 * frame's number (mac_renderer_frames() - 1 right after it) and the time
 * it was presented (CACurrentMediaTime's clock; 0 if it never was, -1
 * when drawing offscreen). */
double mac_renderer_drawable_wait(void);
void mac_renderer_set_present_hook(void (*hook)(unsigned long frame,
		double presented));
/* tests/macos_headless: frames draw into the texture (a bridged
 * id<MTLTexture>) acquire() returns instead of the view's drawable, and
 * done(texture) runs on a Metal thread once the GPU has finished one.
 * acquire() may block, as nextDrawable does; NULL skips the frame. */
void mac_renderer_set_offscreen(void *(*acquire)(void),
		void (*done)(void *texture));

#endif
