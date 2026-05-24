/* GPU renderer implementation.
 *
 * This file is intentionally included by ../x.c instead of compiled as a
 * separate translation unit.  The renderer is tightly coupled to st/x.c
 * private state (window geometry, Xft colors/fonts, and config globals);
 * keeping it in the same translation unit preserves that encapsulation while
 * moving the renderer-specific code out of the event/input/Xft sections.
 */

static float gpupal[512][3];
static int gpupalvalid;

typedef struct {
	Rune rune;
	int flags;
	int color;
	int x, y, w, h, ow, oh;
	int left, top, advance;
	int valid;
} GpuGlyph;

typedef struct {
	FT_Face face;
	int flags;
	int color;
	char *file;
	int index;
} GpuFallbackFace;

typedef struct {
	GLfloat x, y;
	GLfloat u, v;
	GLfloat r, g, b, a;
} GpuVertex;

typedef struct {
	GpuVertex *v;
	int len, cap;
} GpuBatch;

typedef struct {
	int active, doublebuf;
	int bufferage;
	int needclear;
	unsigned int backage;
	int damageidx, damagerows;
	uchar *damage[GPU_DAMAGE_HISTORY];
	int vw, vh;
	GLXContext ctx;
	FT_Library ft;
	FT_Face face[4];
	double fontpx;
	int ascent, descent;
	GLuint atlas, catlas;
	int catlasready;
	int atlasw, atlash, penx, peny, rowh;
	GpuGlyph *glyphs;
	int glyphlen, glyphcap;
	int ascii[4][128];
	int glyphhash[GPU_GLYPH_HASH];
	GpuFallbackFace *fallbacks;
	int fallbacklen, fallbackcap;
	GpuBatch bg, text, ctext, deco, obg, otext, octext, odeco;
} Gpu;

static Gpu gpu;

static double
gpuxscale(void)
{
	return win.tw > 0 ? (double)(win.w - 2 * borderpx) / win.tw : 1.0;
}

static double
gpuyscale(void)
{
	return win.th > 0 ? (double)(win.h - 2 * borderpx) / win.th : 1.0;
}

static double
gpucellw(void)
{
	return win.cw * gpuxscale();
}

static double
gpucellh(void)
{
	return win.ch * gpuyscale();
}

static int
gpuround(double v)
{
	return (int)floor(v + 0.5);
}

static int
gpucellx(int x)
{
	return borderpx + gpuround(x * gpucellw());
}

static int
gpucelly(int y)
{
	return borderpx + gpuround(y * gpucellh());
}

static int
gpucellright(int x, int wide)
{
	return gpucellx(x + (wide ? 2 : 1));
}

static int
gpurowbottom(int y)
{
	return gpucelly(y + 1);
}

static int
gpubaseline(int y)
{
	int top = gpucelly(y), bottom = gpurowbottom(y);
	int lineh = gpu.ascent + gpu.descent;
	int extra = MAX(0, bottom - top - lineh);
	return top + (extra + 1) / 2 + gpu.ascent;
}

static int
gpuisemoji(Rune r)
{
	return BETWEEN(r, 0x1f000, 0x1faff) || BETWEEN(r, 0x2600, 0x27bf);
}

static unsigned char
gpualpha(unsigned char a)
{
	/* Xft's LCD/filtering path looks punchier than a raw grayscale FreeType
	 * mask.  Drop near-invisible fringe pixels and slightly boost coverage so
	 * GPU text does not look washed out or out-of-focus. */
	if (a < 16)
		return 0;
	return MIN(255, ((int)a - 16) * 280 / 239);
}

static void
gpusetfontsize(FT_Face face, int color)
{
	if (!face)
		return;
	if (color && face->num_fixed_sizes > 0) {
		FT_Select_Size(face, 0);
		return;
	}
	FT_Set_Pixel_Sizes(face, 0, MAX(1, gpuround(gpu.fontpx)));
}

static void
gpucolor(uint32_t c, float out[3])
{
	int i, n;

	if (IS_TRUECOL(c)) {
		out[0] = ((c >> 16) & 0xff) / 255.0f;
		out[1] = ((c >> 8) & 0xff) / 255.0f;
		out[2] = (c & 0xff) / 255.0f;
		return;
	}
	n = MIN(dc.collen, LEN(gpupal));
	if (!gpupalvalid) {
		for (i = 0; i < n; i++) {
			gpupal[i][0] = dc.col[i].color.red / 65535.0f;
			gpupal[i][1] = dc.col[i].color.green / 65535.0f;
			gpupal[i][2] = dc.col[i].color.blue / 65535.0f;
		}
		gpupalvalid = 1;
	}
	c = MIN(c, n - 1);
	out[0] = gpupal[c][0];
	out[1] = gpupal[c][1];
	out[2] = gpupal[c][2];
}

static void
gpuresolve(Glyph g, int x, int y, float fg[3], float bg[3])
{
	uint32_t f = g.fg, b = g.bg, t;

	if (y == vimnav_curline_y() && b == defaultbg)
		b = vimnav_curline_bg;
	if (debug_mode && b == defaultbg) {
		int ps, pe;
		vimnav_prompt_line_range(&ps, &pe);
		if (ps >= 0 && y >= ps && y <= pe)
			b = debug_prompt_bg;
	}
	if (g.mode & ATTR_SELECTED)
		b = selectionbg;
	if (g.mode & ATTR_MATCH) {
		f = b;
		b = search_match_bg;
	}
	if ((g.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(f, 0, 7))
		f += 8;
	if (IS_SET(MODE_REVERSE)) {
		if (f == defaultfg)
			f = defaultbg;
		if (b == defaultbg)
			b = defaultfg;
	}
	if (g.mode & ATTR_REVERSE) {
		t = f;
		f = b;
		b = t;
	}
	if (g.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		f = b;
	if (g.mode & ATTR_INVISIBLE)
		f = b;

	gpucolor(f, fg);
	gpucolor(b, bg);
	if ((g.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		fg[0] *= 0.5f;
		fg[1] *= 0.5f;
		fg[2] *= 0.5f;
	}
}

static FT_Face
gpuloadface(Font *font)
{
	FcChar8 *file = NULL;
	int index = 0;
	FT_Face face = NULL;

	if (!font->match)
		return NULL;
	if (FcPatternGetString(font->match->pattern, FC_FILE, 0, &file) != FcResultMatch)
		return NULL;
	FcPatternGetInteger(font->match->pattern, FC_INDEX, 0, &index);
	if (FT_New_Face(gpu.ft, (const char *)file, index, &face))
		return NULL;
	return face;
}

static int
gpufaceidx(int mode)
{
	int idx = FRC_NORMAL;

	if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD))
		idx = FRC_ITALICBOLD;
	else if (mode & ATTR_ITALIC)
		idx = FRC_ITALIC;
	else if (mode & ATTR_BOLD)
		idx = FRC_BOLD;
	if (idx != FRC_NORMAL && !gpu.face[idx]) {
		if (!xloadstylefont(idx))
			gpu.face[idx] = gpuloadface(idx == FRC_BOLD ? &dc.bfont :
			                           idx == FRC_ITALIC ? &dc.ifont : &dc.ibfont);
		if (gpu.face[idx]) {
			gpusetfontsize(gpu.face[idx], 0);
			return idx;
		}
	}
	return gpu.face[idx] ? idx : FRC_NORMAL;
}

static void
gpuatlasreset(void)
{
	int i;

	if (!gpu.active)
		return;
	gpu.penx = gpu.peny = 1;
	gpu.rowh = 0;
	gpu.glyphlen = 0;
	memset(gpu.ascii, 0xff, sizeof gpu.ascii);
	memset(gpu.glyphhash, 0, sizeof gpu.glyphhash);
	for (i = 0; i < 4; i++)
		if (gpu.face[i])
			gpusetfontsize(gpu.face[i], 0);
	glBindTexture(GL_TEXTURE_2D, gpu.atlas);
	/* Allocate atlas storage only.  Cleared contents are unnecessary because
	 * batches only sample freshly assigned glyph rectangles.  Avoiding a full
	 * 16 MiB memset/upload makes first GPU draw and zoom much cheaper. */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, gpu.atlasw, gpu.atlash, 0,
	             GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
	gpu.catlasready = 0;
	if (gpu.face[FRC_NORMAL]) {
		gpu.ascent = gpu.face[FRC_NORMAL]->size->metrics.ascender >> 6;
		gpu.descent = -(gpu.face[FRC_NORMAL]->size->metrics.descender >> 6);
	}
	for (i = 0; i < gpu.fallbacklen; i++)
		if (gpu.fallbacks[i].face)
			gpusetfontsize(gpu.fallbacks[i].face, gpu.fallbacks[i].color);
}

static void
gpuresize(void)
{
	double px;

	if (!gpu.active)
		return;
	px = MAX(1.0, usedfontsize * gpuyscale());
	if (fabs(px - gpu.fontpx) > 0.10) {
		gpu.fontpx = px;
		gpuatlasreset();
		gpu.needclear = 1;
	}
	if (gpu.vw != win.w || gpu.vh != win.h) {
		glXMakeCurrent(xw.dpy, xw.win, gpu.ctx);
		gpu.vw = win.w;
		gpu.vh = win.h;
		gpu.needclear = 1;
		glViewport(0, 0, win.w, win.h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, win.w, win.h, 0, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
	}
}

static void
gpudamageensure(void)
{
	int i, rows = trow();

	if (!gpu.doublebuf || rows <= 0)
		return;
	if (gpu.damagerows == rows && gpu.damage[0])
		return;
	for (i = 0; i < GPU_DAMAGE_HISTORY; i++) {
		gpu.damage[i] = xrealloc(gpu.damage[i], rows * sizeof(*gpu.damage[i]));
		memset(gpu.damage[i], 0, rows * sizeof(*gpu.damage[i]));
	}
	gpu.damagerows = rows;
	gpu.needclear = 1;
}

static void
gpudisablesync(void)
{
	typedef void (*SwapIntervalEXT)(Display *, GLXDrawable, int);
	typedef int (*SwapIntervalMESA)(unsigned int);
	typedef int (*SwapIntervalSGI)(int);
	SwapIntervalEXT ext;
	SwapIntervalMESA mesa;
	SwapIntervalSGI sgi;

	/* Terminal throughput should not be capped by compositor/Xephyr vsync. */
	ext = (SwapIntervalEXT)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
	if (ext) {
		ext(xw.dpy, xw.win, 0);
		return;
	}
	mesa = (SwapIntervalMESA)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
	if (mesa && mesa(0) == 0)
		return;
	sgi = (SwapIntervalSGI)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalSGI");
	if (sgi)
		sgi(0);
}

static int
gpucoloreq(const float a[3], const float b[3])
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static void
gpuinit(void)
{
	XVisualInfo templ, *vi;
	int nvi = 0;
	long vid;
	int usegl = 0;

	if (!gpudraw)
		return;
	vid = XVisualIDFromVisual(xw.vis);
	templ.visualid = vid;
	vi = XGetVisualInfo(xw.dpy, VisualIDMask, &templ, &nvi);
	if (!vi)
		return;
	if (glXGetConfig(xw.dpy, vi, GLX_USE_GL, &usegl) || !usegl) {
		XFree(vi);
		return;
	}
	glXGetConfig(xw.dpy, vi, GLX_DOUBLEBUFFER, &gpu.doublebuf);
	{
		const char *ext = glXQueryExtensionsString(xw.dpy, xw.scr);
		gpu.bufferage = ext && strstr(ext, "GLX_EXT_buffer_age");
	}
	gpu.ctx = glXCreateContext(xw.dpy, vi, NULL, True);
	XFree(vi);
	if (!gpu.ctx)
		return;
	/* GLX swaps can clobber the X event mask on some servers/Xephyr; restore
	 * the st mask after creating the context so keyboard/mouse input stays live. */
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
	if (!glXMakeCurrent(xw.dpy, xw.win, gpu.ctx)) {
		glXDestroyContext(xw.dpy, gpu.ctx);
		memset(&gpu, 0, sizeof gpu);
		return;
	}
	if (FT_Init_FreeType(&gpu.ft)) {
		glXDestroyContext(xw.dpy, gpu.ctx);
		memset(&gpu, 0, sizeof gpu);
		return;
	}
	gpu.face[FRC_NORMAL] = gpuloadface(&dc.font);
	if (!gpu.face[FRC_NORMAL]) {
		gpudestroy();
		return;
	}
	gpu.active = 1;
	gpu.needclear = 1;
	gpu.fontpx = usedfontsize;
		gpu.atlasw = 1024;
		gpu.atlash = 1024;
	glGenTextures(1, &gpu.atlas);
	glBindTexture(GL_TEXTURE_2D, gpu.atlas);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &gpu.catlas);
	glBindTexture(GL_TEXTURE_2D, gpu.catlas);
	/* Color emoji fonts here are bitmap strikes (Twemoji is a 71px strike).
	 * Xft/XRender scales those to terminal size with simple linear filtering;
	 * mipmaps reduced jaggies but measured/appeared too blurry at this scale. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gpudisablesync();
	gpuatlasreset();
	gpuresize();
}

static void
gpudestroy(void)
{
	int i;

	if (gpu.atlas)
		glDeleteTextures(1, &gpu.atlas);
	if (gpu.catlas)
		glDeleteTextures(1, &gpu.catlas);
	for (i = 0; i < GPU_DAMAGE_HISTORY; i++)
		free(gpu.damage[i]);
	for (i = 0; i < 4; i++)
		if (gpu.face[i])
			FT_Done_Face(gpu.face[i]);
	for (i = 0; i < gpu.fallbacklen; i++) {
		if (gpu.fallbacks[i].face)
			FT_Done_Face(gpu.fallbacks[i].face);
		free(gpu.fallbacks[i].file);
	}
	if (gpu.ft)
		FT_Done_FreeType(gpu.ft);
	if (gpu.ctx) {
		glXMakeCurrent(xw.dpy, None, NULL);
		glXDestroyContext(xw.dpy, gpu.ctx);
	}
	free(gpu.glyphs);
	free(gpu.fallbacks);
	free(gpu.bg.v);
	free(gpu.text.v);
	free(gpu.ctext.v);
	free(gpu.deco.v);
	free(gpu.obg.v);
	free(gpu.otext.v);
	free(gpu.octext.v);
	free(gpu.odeco.v);
	memset(&gpu, 0, sizeof gpu);
}

static FT_Face
gpufallbackface(Rune rune, int flags, int color)
{
	FcPattern *pat, *match;
	FcCharSet *charset;
	FcResult result;
	FcChar8 *file = NULL;
	char *filedup;
	int index = 0, i;
	FT_Face face = NULL;

	for (i = 0; i < gpu.fallbacklen; i++)
		if (gpu.fallbacks[i].flags == flags &&
		    gpu.fallbacks[i].color == color &&
		    FT_Get_Char_Index(gpu.fallbacks[i].face, rune))
			return gpu.fallbacks[i].face;

	pat = FcPatternCreate();
	charset = FcCharSetCreate();
	if (!pat || !charset)
		goto cleanup;
	FcCharSetAddChar(charset, rune);
	FcPatternAddCharSet(pat, FC_CHARSET, charset);
	FcPatternAddBool(pat, FC_SCALABLE, 1);
	FcPatternAddBool(pat, FC_COLOR, color ? 1 : 0);
	if (flags == FRC_BOLD || flags == FRC_ITALICBOLD)
		FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (flags == FRC_ITALIC || flags == FRC_ITALICBOLD)
		FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	match = FcFontMatch(NULL, pat, &result);
	if (!match)
		goto cleanup;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
		FcPatternDestroy(match);
		goto cleanup;
	}
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	filedup = xstrdup((const char *)file);
	for (i = 0; i < gpu.fallbacklen; i++)
		if (gpu.fallbacks[i].flags == flags &&
		    gpu.fallbacks[i].color == color &&
		    gpu.fallbacks[i].index == index &&
		    !strcmp(gpu.fallbacks[i].file, filedup)) {
			free(filedup);
			face = gpu.fallbacks[i].face;
			FcPatternDestroy(match);
			goto cleanup;
		}
	if (FT_New_Face(gpu.ft, filedup, index, &face)) {
		free(filedup);
		FcPatternDestroy(match);
		goto cleanup;
	}
	gpusetfontsize(face, color);
	if (gpu.fallbacklen >= gpu.fallbackcap) {
		gpu.fallbackcap += 16;
		gpu.fallbacks = xrealloc(gpu.fallbacks,
		                         gpu.fallbackcap * sizeof *gpu.fallbacks);
	}
	gpu.fallbacks[gpu.fallbacklen++] = (GpuFallbackFace){
		.face = face, .flags = flags, .color = color, .file = filedup, .index = index
	};
	FcPatternDestroy(match);

cleanup:
	if (charset)
		FcCharSetDestroy(charset);
	if (pat)
		FcPatternDestroy(pat);
	return face;
}

static GpuGlyph *
gpuglyph(Rune rune, int mode)
{
	GpuGlyph *g;
	FT_Face face;
	FT_Bitmap *bm;
	uint h = 0, slot = GPU_GLYPH_HASH;
	int i, probes, flags = gpufaceidx(mode), wantcolor = gpuisemoji(rune);
	int cropx = 0, cropy = 0;

	if (rune < 128 && gpu.ascii[flags][rune] >= 0)
		return &gpu.glyphs[gpu.ascii[flags][rune]];
	if (rune >= 128) {
		h = (rune * 2654435761u + flags * 97u) & (GPU_GLYPH_HASH - 1);
		for (probes = 0, slot = h; probes < GPU_GLYPH_HASH && gpu.glyphhash[slot];
		     probes++, slot = (slot + 1) & (GPU_GLYPH_HASH - 1)) {
			i = gpu.glyphhash[slot] - 1;
			if (gpu.glyphs[i].rune == rune && gpu.glyphs[i].flags == flags)
				return &gpu.glyphs[i];
		}
		if (probes == GPU_GLYPH_HASH)
			slot = GPU_GLYPH_HASH;
	} else {
		for (i = 0; i < gpu.glyphlen; i++)
			if (gpu.glyphs[i].rune == rune && gpu.glyphs[i].flags == flags) {
				gpu.ascii[flags][rune] = i;
				return &gpu.glyphs[i];
			}
	}
	if (gpu.glyphlen >= gpu.glyphcap) {
		gpu.glyphcap += 256;
		gpu.glyphs = xrealloc(gpu.glyphs, gpu.glyphcap * sizeof *gpu.glyphs);
	}
	g = &gpu.glyphs[gpu.glyphlen++];
	if (rune < 128)
		gpu.ascii[flags][rune] = gpu.glyphlen - 1;
	else if (slot < GPU_GLYPH_HASH)
		gpu.glyphhash[slot] = gpu.glyphlen;
	memset(g, 0, sizeof *g);
	g->rune = rune;
	g->flags = flags;
	face = gpu.face[flags] ? gpu.face[flags] : gpu.face[FRC_NORMAL];
	if (wantcolor) {
		FT_Face fallback = gpufallbackface(rune, flags, 1);
		if (fallback)
			face = fallback;
	}
	if (!FT_Get_Char_Index(face, rune)) {
		FT_Face fallback = gpufallbackface(rune, flags, 0);
		if (fallback)
			face = fallback;
	}
	if (FT_Load_Char(face, rune, FT_LOAD_RENDER |
	                (wantcolor ? FT_LOAD_COLOR : FT_LOAD_TARGET_NORMAL))) {
		face = gpu.face[FRC_NORMAL];
		FT_Load_Char(face, 0xfffd, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
	}
	bm = &face->glyph->bitmap;
	g->color = bm->pixel_mode == FT_PIXEL_MODE_BGRA;
	g->w = bm->width;
	g->h = bm->rows;
	g->ow = bm->width;
	g->oh = bm->rows;
	if (g->color) {
		int x, y, minx = bm->width, miny = bm->rows, maxx = -1, maxy = -1;
		for (y = 0; y < bm->rows; y++) {
			const unsigned char *row = bm->buffer + y * bm->pitch;
			if (bm->pitch < 0)
				row = bm->buffer + (bm->rows - 1 - y) * -bm->pitch;
			for (x = 0; x < bm->width; x++) {
				if (row[x * 4 + 3]) {
					minx = MIN(minx, x);
					miny = MIN(miny, y);
					maxx = MAX(maxx, x);
					maxy = MAX(maxy, y);
				}
			}
		}
		if (maxx >= minx && maxy >= miny) {
			cropx = minx;
			cropy = miny;
			g->w = maxx - minx + 1;
			g->h = maxy - miny + 1;
		}
	}
	g->left = face->glyph->bitmap_left;
	g->top = face->glyph->bitmap_top;
	g->advance = face->glyph->advance.x >> 6;
	g->valid = 1;
	if (g->w <= 0 || g->h <= 0)
		return g;
	if (gpu.penx + g->w + 1 >= gpu.atlasw) {
		gpu.penx = 1;
		gpu.peny += gpu.rowh + 1;
		gpu.rowh = 0;
	}
	if (gpu.peny + g->h + 1 >= gpu.atlash) {
		gpuatlasreset();
		return gpuglyph(rune, mode);
	}
	g->x = gpu.penx;
	g->y = gpu.peny;
	glBindTexture(GL_TEXTURE_2D, g->color ? gpu.catlas : gpu.atlas);
	if (g->color) {
		unsigned char *tight = xmalloc(g->w * g->h * 4), *dst = tight;
		int row;
		if (!gpu.catlasready) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gpu.atlasw, gpu.atlash, 0,
			             GL_BGRA, GL_UNSIGNED_BYTE, NULL);
			gpu.catlasready = 1;
		}
		for (row = 0; row < g->h; row++) {
			const unsigned char *src = bm->buffer + (row + cropy) * bm->pitch;
			if (bm->pitch < 0)
				src = bm->buffer + (bm->rows - 1 - (row + cropy)) * -bm->pitch;
			src += cropx * 4;
			memcpy(dst, src, g->w * 4);
			dst += g->w * 4;
		}
		glTexSubImage2D(GL_TEXTURE_2D, 0, g->x, g->y, g->w, g->h,
		                GL_BGRA, GL_UNSIGNED_BYTE, tight);
		free(tight);
	} else {
		unsigned char *tight = xmalloc(g->w * g->h), *dst = tight;
		int row, col;
		for (row = 0; row < g->h; row++) {
			const unsigned char *src = bm->buffer + row * bm->pitch;
			if (bm->pitch < 0)
				src = bm->buffer + (g->h - 1 - row) * -bm->pitch;
			if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
				for (col = 0; col < g->w; col++)
					dst[col] = (src[col >> 3] & (0x80 >> (col & 7))) ? 255 : 0;
			} else if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
				for (col = 0; col < g->w; col++)
					dst[col] = gpualpha(src[col]);
			} else {
				memset(dst, 0, g->w);
			}
			dst += g->w;
		}
		glTexSubImage2D(GL_TEXTURE_2D, 0, g->x, g->y, g->w, g->h,
		                GL_ALPHA, GL_UNSIGNED_BYTE, tight);
		free(tight);
	}
	gpu.penx += g->w + 1;
	gpu.rowh = MAX(gpu.rowh, g->h);
	return g;
}

static void
gpubatchclear(GpuBatch *b)
{
	b->len = 0;
}

static void
gpubatchreset(void)
{
	gpubatchclear(&gpu.bg);
	gpubatchclear(&gpu.text);
	gpubatchclear(&gpu.ctext);
	gpubatchclear(&gpu.deco);
	gpubatchclear(&gpu.obg);
	gpubatchclear(&gpu.otext);
	gpubatchclear(&gpu.octext);
	gpubatchclear(&gpu.odeco);
}

static GpuVertex *
gpubatchalloc(GpuBatch *b, int n)
{
	if (b->len + n > b->cap) {
		b->cap = MAX(b->cap * 2, b->len + n + 2048);
		b->v = xrealloc(b->v, b->cap * sizeof *b->v);
	}
	GpuVertex *v = &b->v[b->len];
	b->len += n;
	return v;
}

static void
gpubatchquad(GpuBatch *b, double x, double y, double w, double h,
             double u1, double v1, double u2, double v2, float c[3])
{
	GpuVertex *v = gpubatchalloc(b, 6);
	GpuVertex a = { x,     y,     u1, v1, c[0], c[1], c[2], 1.0f };
	GpuVertex b0 = { x + w, y,     u2, v1, c[0], c[1], c[2], 1.0f };
	GpuVertex c0 = { x + w, y + h, u2, v2, c[0], c[1], c[2], 1.0f };
	GpuVertex d = { x,     y + h, u1, v2, c[0], c[1], c[2], 1.0f };
	v[0] = a;
	v[1] = b0;
	v[2] = c0;
	v[3] = a;
	v[4] = c0;
	v[5] = d;
}

static void
gpubatchrect(GpuBatch *b, double x, double y, double w, double h, float c[3])
{
	gpubatchquad(b, x, y, w, h, 0, 0, 0, 0, c);
}

static void
gpubatchglyph(GpuBatch *b, double x, double y, double w, double h,
              GpuGlyph *g, float c[3])
{
	double tx1 = (double)(g->x + 0.5) / gpu.atlasw;
	double ty1 = (double)(g->y + 0.5) / gpu.atlash;
	double tx2 = (double)(g->x + g->w - 0.5) / gpu.atlasw;
	double ty2 = (double)(g->y + g->h - 0.5) / gpu.atlash;
	gpubatchquad(b, x, y, w, h, tx1, ty1, tx2, ty2, c);
}

static void
gpudrawbatch(GpuBatch *b, int textured)
{
	const void *voff, *toff, *coff;

	if (!b->len)
		return;
	voff = &b->v[0].x;
	toff = &b->v[0].u;
	coff = &b->v[0].r;
	if (textured) {
		glEnable(GL_BLEND);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, textured == 2 ? gpu.catlas : gpu.atlas);
		glBlendFunc(textured == 2 ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), toff);
	} else {
		glDisable(GL_BLEND);
		glDisable(GL_TEXTURE_2D);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(2, GL_FLOAT, sizeof(GpuVertex), voff);
	glColorPointer(4, GL_FLOAT, sizeof(GpuVertex), coff);
	glDrawArrays(GL_TRIANGLES, 0, b->len);
	if (textured)
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void
gpudrawline(Line line, int x1, int y, int x2)
{
	int x, haverun = 0, searchactive = search_active(), selactive = selection_active();
	int basey = gpucelly(y), rowh = gpurowbottom(y) - basey;
	int baseline = gpubaseline(y), runx = 0, runw = 0;
	int vimline = vimnav_curline_y();
	Glyph g;
	float fg[3], bg[3], dfg[3], dbg[3], runbg[3], white[3] = {1.0f, 1.0f, 1.0f};
	GpuGlyph *gg;

	if (gpu.doublebuf && gpu.damage[0] && BETWEEN(y, 0, gpu.damagerows - 1))
		gpu.damage[gpu.damageidx][y] = 1;

	gpucolor(defaultfg, dfg);
	gpucolor(defaultbg, dbg);
	for (x = x1; x < x2; x++) {
		g = line[x];
		if (g.mode == ATTR_WDUMMY)
			continue;
		if (selactive && selected(x, y))
			g.mode |= ATTR_SELECTED;
		if (searchactive && search_matched(x, y))
			g.mode |= ATTR_MATCH;
		if (g.bg == defaultbg &&
		    !(g.mode & (ATTR_SELECTED|ATTR_MATCH|ATTR_BOLD_FAINT|ATTR_REVERSE|ATTR_BLINK|ATTR_INVISIBLE)) &&
		    !IS_SET(MODE_REVERSE) && !debug_mode && y != vimline) {
			if (g.fg == defaultfg)
				memcpy(fg, dfg, sizeof fg);
			else
				gpucolor(g.fg, fg);
			memcpy(bg, dbg, sizeof bg);
		} else {
			gpuresolve(g, x, y, fg, bg);
		}
		int cellx = gpucellx(x);
		int cellw = gpucellright(x, g.mode & ATTR_WIDE) - cellx;
		if (!haverun) {
			haverun = 1;
			runx = cellx;
			runw = cellw;
			memcpy(runbg, bg, sizeof runbg);
		} else if (gpucoloreq(runbg, bg)) {
			runw += cellw;
		} else {
			gpubatchrect(&gpu.bg, runx, basey, runw, rowh, runbg);
			runx = cellx;
			runw = cellw;
			memcpy(runbg, bg, sizeof runbg);
		}
		if (g.u != ' ') {
			gg = gpuglyph(g.u, g.mode);
			if (gg && gg->valid && gg->w > 0 && gg->h > 0) {
				if (gg->color) {
					double fitw = MAX(cellw, rowh);
					double scale = MIN(fitw / MAX(1, gg->ow), (double)rowh / MAX(1, gg->oh));
					scale *= 0.94;
					int dw = MAX(1, gpuround(gg->w * scale));
					int dh = MAX(1, gpuround(gg->h * scale));
					int dx = cellx + (cellw - dw) / 2;
					int dy = basey + (rowh - dh) / 2;
					gpubatchglyph(&gpu.ctext, dx, dy, dw, dh, gg, white);
				} else {
					int dx = cellx + gg->left;
					int dy = baseline - gg->top;
					gpubatchglyph(&gpu.text, dx, dy, gg->w, gg->h, gg, fg);
				}
			}
		}
		if (g.mode & ATTR_UNDERLINE)
			gpubatchrect(&gpu.deco, cellx, baseline + 1,
			             gpucellright(x, 0) - cellx, 1, fg);
		if (g.mode & ATTR_STRUCK)
			gpubatchrect(&gpu.deco, cellx, basey + rowh / 2,
			             gpucellright(x, 0) - cellx, 1, fg);
	}
	if (haverun)
		gpubatchrect(&gpu.bg, runx, basey, runw, rowh, runbg);
}

static void
gpudrawcell(Glyph g, int x, int y, int overlay)
{
	int cellx = gpucellx(x), celly = gpucelly(y);
	int cellw, cellh = gpurowbottom(y) - celly;
	int baseline = gpubaseline(y);
	float fg[3], bg[3], white[3] = {1.0f, 1.0f, 1.0f};
	GpuGlyph *gg;
	GpuBatch *bb = overlay ? &gpu.obg : &gpu.bg;
	GpuBatch *tb = overlay ? &gpu.otext : &gpu.text;
	GpuBatch *ctb = overlay ? &gpu.octext : &gpu.ctext;
	GpuBatch *db = overlay ? &gpu.odeco : &gpu.deco;
	int selactive = selection_active(), searchactive = search_active();

	if (g.mode == ATTR_WDUMMY)
		return;
	if (selactive && selected(x, y))
		g.mode |= ATTR_SELECTED;
	if (searchactive && search_matched(x, y))
		g.mode |= ATTR_MATCH;
	gpuresolve(g, x, y, fg, bg);
	cellw = gpucellright(x, g.mode & ATTR_WIDE) - cellx;
	gpubatchrect(bb, cellx, celly, cellw, cellh, bg);
	if (g.u != ' ') {
		gg = gpuglyph(g.u, g.mode);
		if (gg && gg->valid && gg->w > 0 && gg->h > 0) {
			if (gg->color) {
				double fitw = MAX(cellw, cellh);
				double scale = MIN(fitw / MAX(1, gg->ow), (double)cellh / MAX(1, gg->oh));
				scale *= 0.94;
				int dw = MAX(1, gpuround(gg->w * scale));
				int dh = MAX(1, gpuround(gg->h * scale));
				int dx = cellx + (cellw - dw) / 2;
				int dy = celly + (cellh - dh) / 2;
				gpubatchglyph(ctb, dx, dy, dw, dh, gg, white);
			} else {
				int dx = cellx + gg->left;
				int dy = baseline - gg->top;
				gpubatchglyph(tb, dx, dy, gg->w, gg->h, gg, fg);
			}
		}
	}
	if (g.mode & ATTR_UNDERLINE)
		gpubatchrect(db, cellx, baseline + 1, gpucellright(x, 0) - cellx, 1, fg);
	if (g.mode & ATTR_STRUCK)
		gpubatchrect(db, cellx, celly + cellh / 2, gpucellright(x, 0) - cellx, 1, fg);
}

static void
gpudrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	int cellx = gpucellx(cx), celly = gpucelly(cy);
	int cellw = gpucellright(cx, 0) - cellx, cellh = gpurowbottom(cy) - celly;
	int selactive = selection_active(), searchactive = search_active();
	float col[3];
	Glyph cg = g;

	if (selactive && selected(ox, oy))
		og.mode |= ATTR_SELECTED;
	if (searchactive && search_matched(ox, oy))
		og.mode |= ATTR_MATCH;
	gpudrawcell(og, ox, oy, 1);
	if ((IS_SET(MODE_HIDE) && !vimnav.forced) || cmdline_active())
		return;
	cg.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE;
	if (vimnav.forced) {
		cg.fg = defaultbg;
		cg.bg = TRUECOLOR(0xff, 0x6b, 0x6b);
		gpucolor(cg.bg, col);
	} else if (IS_SET(MODE_REVERSE)) {
		cg.mode |= ATTR_REVERSE;
		cg.bg = defaultfg;
		if (selactive && selected(cx, cy)) {
			cg.fg = defaultrcs;
			gpucolor(defaultcs, col);
		} else {
			cg.fg = defaultcs;
			gpucolor(defaultrcs, col);
		}
	} else {
		cg.fg = defaultbg;
		cg.bg = defaultcs;
		gpucolor(cg.bg, col);
	}
	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7:
			cg.u = 0x2603;
			/* FALLTHROUGH */
		case 0: case 1: case 2:
			gpudrawcell(cg, cx, cy, 1);
			break;
		case 3: case 4:
			gpubatchrect(&gpu.odeco, cellx,
			             celly + cellh - cursorthickness,
			             cellw, cursorthickness, col);
			break;
		case 5: case 6:
			gpubatchrect(&gpu.odeco, cellx, celly, cursorthickness, cellh, col);
			break;
		}
	} else {
		gpubatchrect(&gpu.odeco, cellx, celly, cellw, 1, col);
		gpubatchrect(&gpu.odeco, cellx, celly, 1, cellh, col);
		gpubatchrect(&gpu.odeco, cellx + cellw - 1, celly, 1, cellh, col);
		gpubatchrect(&gpu.odeco, cellx, celly + cellh - 1, cellw, 1, col);
	}
}
