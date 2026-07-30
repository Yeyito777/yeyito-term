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
void mac_renderer_end(void);

#endif
