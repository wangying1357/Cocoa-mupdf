#include "gl-app.h"

#include "mupdf/pdf.h" /* for pdf specifics and forms */
#include "tinyfiledialogs.h"

enum {
    /* Screen furniture: aggregate size of unusable space from title bars, task bars, window borders, etc */
    SCREEN_FURNITURE_W = 20,
    SCREEN_FURNITURE_H = 40,

    /* Default EPUB/HTML layout dimensions */
    DEFAULT_LAYOUT_W = 450,
    DEFAULT_LAYOUT_H = 600,
    DEFAULT_LAYOUT_EM = 12,

    /* Default UI sizes */
    DEFAULT_UI_FONTSIZE = 15,
    DEFAULT_UI_BASELINE = 14,
    DEFAULT_UI_LINEHEIGHT = 18,
};

#define DEFAULT_WINDOW_W (612 * currentzoom / 72)
#define DEFAULT_WINDOW_H (792 * currentzoom / 72)

struct ui ui;
fz_context* ctx = NULL;
GLFWwindow* window = NULL;

static const int zoom_list[] = { 18, 24, 36, 48, 54, 64, 72, 84, 96, 108, 120, 132, 144, 156, 168, 180, 192, 204, 216, 228, 240, 252, 264, 276, 288, 300, 312, 324, 336, 348, 360, 372, 384, 396, 408, 420, 432, 446, 464, 512 };

#define MINRES (zoom_list[0])
#define MAXRES (zoom_list[nelem(zoom_list) - 1])
#define DEFRES 96

static const char* title = "MuPDF/GL";
static fz_document* doc = NULL;
static fz_page* page = NULL;
static pdf_document* pdf = NULL;
static fz_outline* outline = NULL;
static fz_link* links = NULL;

static int number = 0;

static struct texture page_tex = { 0 };
static int scroll_x = 0, scroll_y = 0;
static int canvas_x = 0, canvas_w = 100;
static int canvas_y = 0, canvas_h = 100;

static struct texture annot_tex[256];
static int annot_count = 0;

static int screen_w = 1280, screen_h = 720;
static int window_w = 1, window_h = 1;

static int oldpage = 0, currentpage = 0;
static float oldzoom = DEFRES, currentzoom = DEFRES;
static float oldrotate = 0, currentrotate = 0;
static fz_matrix page_ctm, page_inv_ctm;

static int isfullscreen = 0;
static int g_oldinvertcolor = 0, g_isinvertcolor = 0;
static int g_old_x_shrink = 0, g_old_y_shrink = 0, g_x_shrink = 0, g_y_shrink = 0;
static int showoutline = 0;
static int showlinks = 0;
static int showsearch = 0;
static int showinfo = 0;

static int history_count = 0;
static int history[256];
static int future_count = 0;
static int future[256];
static int marks[10];
static const int MAX_RECENT_FILES = 50; // histroy file list

static int search_active = 0;
static struct input search_input = { { 0 }, 0 };
static char* search_needle = 0;
static int search_dir = 1;
static int search_page = -1;
static int search_hit_page = -1;
static int search_hit_count = 0;
static fz_rect search_hit_bbox[500];

static unsigned int next_power_of_two(unsigned int n)
{
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return ++n;
}

/* OpenGL capabilities */
static int has_ARB_texture_non_power_of_two = 1;
static GLint max_texture_size = 8192;

static int ui_needs_update = 0;
/* backgroud color */
static GLuint g_backcolor = 0xffffff;

static GLuint get_random_backcolor(void)
{
    static const GLuint sa_bkcolor_list[] = {
        0xFFFFFF, //white
        0xEEE8D5, //base2
        0xFDF6E3, //base3
        0xE5DED6, //newsprint
        0xEFEFEF, //palegray
        0xe6ebee, //elegant
        0xf3f6f4, //delight
        0xfff6da, //sandbox
        0xe6e6e6, //greyscale
        0xf1feee, //sprout
    };
    srand(time(0)); //use current time as seed for random generator
    int random_variable = rand() % nelem(sa_bkcolor_list);

    return sa_bkcolor_list[random_variable];
    /*return (GLuint)random_variable;*/
}

static void ui_begin(void)
{
    ui_needs_update = 0;
    ui.hot = NULL;
}

static void ui_end(void)
{
    if (!ui.down && !ui.middle && !ui.right)
        ui.active = NULL;
    if (ui_needs_update)
        glfwPostEmptyEvent();
}

static void open_browser(const char* uri)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", uri, 0, 0, SW_SHOWNORMAL);
#else
    const char* browser = getenv("BROWSER");
    if (!browser) {
#ifdef __APPLE__
        browser = "open";
#else
        browser = "xdg-open";
#endif
    }
    if (fork() == 0) {
        execlp(browser, browser, uri, (char*)0);
        fprintf(stderr, "cannot exec '%s'\n", browser);
        exit(0);
    }
#endif
}

const char* ogl_error_string(GLenum code)
{
#define CASE(E)    \
    case E:        \
        return #E; \
        break
    switch (code) {
        /* glGetError */
        CASE(GL_NO_ERROR);
        CASE(GL_INVALID_ENUM);
        CASE(GL_INVALID_VALUE);
        CASE(GL_INVALID_OPERATION);
        CASE(GL_OUT_OF_MEMORY);
        CASE(GL_STACK_UNDERFLOW);
        CASE(GL_STACK_OVERFLOW);
    default:
        return "(unknown)";
    }
#undef CASE
}

void ogl_assert(fz_context* ctx, const char* msg)
{
    int code = glGetError();
    if (code != GL_NO_ERROR) {
        fz_warn(ctx, "glGetError(%s): %s", msg, ogl_error_string(code));
    }
}

void ui_draw_image(struct texture* tex, float x, float y)
{
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLE_STRIP);
    {
        glColor4ub(
            (g_backcolor >> 16) & 0xFF,
            (g_backcolor >> 8) & 0xFF,
            g_backcolor & 0xFF,
            255);
        /*float x_pos = 100 * page_ctm.a;*/

        glTexCoord2f(0, tex->t);
        glVertex2f(x + tex->x, y + tex->y + tex->h);
        glTexCoord2f(0, 0);
        glVertex2f(x + tex->x, y + tex->y);
        glTexCoord2f(tex->s, tex->t);
        glVertex2f(x + tex->x + tex->w, y + tex->y + tex->h);
        glTexCoord2f(tex->s, 0);
        glVertex2f(x + tex->x + tex->w, y + tex->y);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static int zoom_in(int oldres)
{
    int i;
    for (i = 0; i < nelem(zoom_list) - 1; ++i)
        if (zoom_list[i] <= oldres && zoom_list[i + 1] > oldres)
            return zoom_list[i + 1];
    return zoom_list[i];
}

static int zoom_out(int oldres)
{
    int i;
    for (i = 0; i < nelem(zoom_list) - 1; ++i)
        if (zoom_list[i] < oldres && zoom_list[i + 1] >= oldres)
            return zoom_list[i];
    return zoom_list[0];
}

static void update_title(void)
{
    static char buf[1024];
    /*
	size_t n = strlen(title);

	// 大于15个汉字就会出问题，需要用utf-8的字符处理函数，现在临时将buf加大，并且不裁剪
	if (n > 50)
		sprintf(buf, "...%s - %d / %d", title + n - 50, currentpage + 1, fz_count_pages(ctx, doc));
	else
		sprintf(buf, "%s - %d / %d", title, currentpage + 1, fz_count_pages(ctx, doc));
	*/

    snprintf(buf, sizeof(buf), "%s - %d / %d", title, currentpage + 1, fz_count_pages(ctx, doc));
    glfwSetWindowTitle(window, buf);
}

void texture_from_pixmap(struct texture* tex, fz_pixmap* pix)
{
    if (g_isinvertcolor)
        fz_invert_pixmap(NULL, pix);

    if (!tex->id)
        glGenTextures(1, &tex->id);
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    tex->x = pix->x;
    tex->y = pix->y;
    tex->w = pix->w;
    tex->h = pix->h;

    if (has_ARB_texture_non_power_of_two) {
        if (tex->w > max_texture_size || tex->h > max_texture_size)
            fz_warn(ctx, "texture size (%d x %d) exceeds implementation limit (%d)", tex->w, tex->h, max_texture_size);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->w, tex->h, 0, pix->n == 4 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, pix->samples);
        tex->s = 1;
        tex->t = 1;
    } else {
        int w2 = next_power_of_two(tex->w);
        int h2 = next_power_of_two(tex->h);
        if (w2 > max_texture_size || h2 > max_texture_size)
            fz_warn(ctx, "texture size (%d x %d) exceeds implementation limit (%d)", w2, h2, max_texture_size);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w2, h2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex->w, tex->h, pix->n == 4 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, pix->samples);
        tex->s = (float)tex->w / w2;
        tex->t = (float)tex->h / h2;
    }
}

void render_page(void)
{
    fz_annot* annot;
    fz_pixmap* pix;

    fz_scale(&page_ctm, currentzoom / 72, currentzoom / 72);
    fz_pre_rotate(&page_ctm, -currentrotate);
    fz_invert_matrix(&page_inv_ctm, &page_ctm);

    fz_drop_page(ctx, page);

    page = fz_load_page(ctx, doc, currentpage);

    fz_drop_link(ctx, links);
    links = NULL;
    links = fz_load_links(ctx, page);

    pix = fz_new_pixmap_from_page_contents(ctx, page, &page_ctm, fz_device_rgb(ctx), 0);

    if (g_x_shrink || g_y_shrink) {
        /* crop page white margin */
        fz_irect nsize;
        fz_pixmap_bbox_no_ctx(pix, &nsize);
        nsize.x0 += g_x_shrink * currentzoom / 72;
        nsize.y0 += g_y_shrink * currentzoom / 72;
        nsize.x1 -= g_x_shrink * currentzoom / 72;
        nsize.y1 -= g_y_shrink * currentzoom / 72;
        fz_pixmap* shrink_pix = fz_new_pixmap_with_bbox(ctx, pix->colorspace, &nsize, 0);
        fz_copy_pixmap_rect(ctx, shrink_pix, pix, &nsize);

        texture_from_pixmap(&page_tex, shrink_pix);
        fz_drop_pixmap(ctx, shrink_pix);
    } else {
        texture_from_pixmap(&page_tex, pix);
    }
    fz_drop_pixmap(ctx, pix);

    annot_count = 0;
    for (annot = fz_first_annot(ctx, page); annot; annot = fz_next_annot(ctx, annot)) {
        pix = fz_new_pixmap_from_annot(ctx, annot, &page_ctm, fz_device_rgb(ctx), 1);
        texture_from_pixmap(&annot_tex[annot_count++], pix);
        fz_drop_pixmap(ctx, pix);
        if (annot_count >= nelem(annot_tex)) {
            fz_warn(ctx, "too many annotations to display!");
            break;
        }
    }
}

static void push_history(void)
{
    if (history_count + 1 >= nelem(history)) {
        memmove(history, history + 1, sizeof *history * (nelem(history) - 1));
        history[history_count] = currentpage;
    } else {
        history[history_count++] = currentpage;
    }
}

static void push_future(void)
{
    if (future_count + 1 >= nelem(future)) {
        memmove(future, future + 1, sizeof *future * (nelem(future) - 1));
        future[future_count] = currentpage;
    } else {
        future[future_count++] = currentpage;
    }
}

static void clear_future(void)
{
    future_count = 0;
}

static void jump_to_page(int newpage)
{
    newpage = fz_clampi(newpage, 0, fz_count_pages(ctx, doc) - 1);
    clear_future();
    push_history();
    currentpage = newpage;
    push_history();
}

static void pop_history(void)
{
    int here = currentpage;
    push_future();
    while (history_count > 0 && currentpage == here)
        currentpage = history[--history_count];
}

static void pop_future(void)
{
    int here = currentpage;
    push_history();
    while (future_count > 0 && currentpage == here)
        currentpage = future[--future_count];
    push_history();
}

static void do_copy_region(fz_rect* screen_sel, int xofs, int yofs)
{
    fz_buffer* buf;
    fz_rect page_sel;

    xofs -= page_tex.x;
    yofs -= page_tex.y;

    page_sel.x0 = screen_sel->x0 - xofs;
    page_sel.y0 = screen_sel->y0 - yofs;
    page_sel.x1 = screen_sel->x1 - xofs;
    page_sel.y1 = screen_sel->y1 - yofs;

    fz_transform_rect(&page_sel, &page_inv_ctm);

#ifdef _WIN32
    buf = fz_new_buffer_from_page(ctx, page, &page_sel, 1);
#else
    buf = fz_new_buffer_from_page(ctx, page, &page_sel, 0);
#endif
    fz_write_buffer_rune(ctx, buf, 0);
    glfwSetClipboardString(window, (char*)buf->data);
    fz_drop_buffer(ctx, buf);
}

static void ui_label_draw(int x0, int y0, int x1, int y1, const char* text)
{
    glColor4f(1, 1, 1, 1);
    glRectf(x0, y0, x1, y1);
    glColor4f(0, 0, 0, 1);
    ui_draw_string(ctx, x0 + 2, y0 + 2 + ui.baseline, text);
}

static void ui_scrollbar(int x0, int y0, int x1, int y1, int* value, int page_size, int max)
{
    static float saved_top = 0;
    static int saved_ui_y = 0;
    float top;

    int total_h = y1 - y0;
    int thumb_h = fz_maxi(x1 - x0, total_h * page_size / max);
    int avail_h = total_h - thumb_h;

    max -= page_size;

    if (max <= 0) {
        *value = 0;
        glColor4f(0.6f, 0.6f, 0.6f, 1.0f);
        glRectf(x0, y0, x1, y1);
        return;
    }

    top = (float)*value * avail_h / max;

    if (ui.down && !ui.active) {
        if (ui.x >= x0 && ui.x < x1 && ui.y >= y0 && ui.y < y1) {
            if (ui.y < top) {
                ui.active = "pgdn";
                *value -= page_size;
            } else if (ui.y >= top + thumb_h) {
                ui.active = "pgup";
                *value += page_size;
            } else {
                ui.hot = value;
                ui.active = value;
                saved_top = top;
                saved_ui_y = ui.y;
            }
        }
    }

    if (ui.active == value) {
        *value = (saved_top + ui.y - saved_ui_y) * max / avail_h;
    }

    if (*value < 0)
        *value = 0;
    else if (*value > max)
        *value = max;

    top = (float)*value * avail_h / max;

    glColor4f(0.6f, 0.6f, 0.6f, 1.0f);
    glRectf(x0, y0, x1, y1);
    glColor4f(0.8f, 0.8f, 0.8f, 1.0f);
    glRectf(x0, top, x1, top + thumb_h);
}

static int measure_outline_height(fz_outline* node)
{
    int h = 0;
    while (node) {
        h += ui.lineheight;
        if (node->down)
            h += measure_outline_height(node->down);
        node = node->next;
    }
    return h;
}

static int do_outline_imp(fz_outline* node, int end, int x0, int x1, int x, int y)
{
    int h = 0;
    int p = currentpage;
    int n = end;

    while (node) {
        if (node->dest.kind == FZ_LINK_GOTO) {
            p = node->dest.ld.gotor.page;

            if (ui.x >= x0 && ui.x < x1 && ui.y >= y + h && ui.y < y + h + ui.lineheight) {
                ui.hot = node;
                if (!ui.active && ui.down) {
                    ui.active = node;
                    jump_to_page(p);
                    ui_needs_update = 1; /* we changed the current page, so force a redraw */
                }
            }

            n = end;
            if (node->next && node->next->dest.kind == FZ_LINK_GOTO) {
                n = node->next->dest.ld.gotor.page;
            }
            if (currentpage == p || (currentpage > p && currentpage < n)) {
                glColor4f(0.9f, 0.9f, 0.9f, 1.0f);
                glRectf(x0, y + h, x1, y + h + ui.lineheight);
            }
        }

        glColor4f(0, 0, 0, 1);
        ui_draw_string(ctx, x, y + h + ui.baseline, node->title);
        h += ui.lineheight;
        if (node->down)
            h += do_outline_imp(node->down, n, x0, x1, x + ui.lineheight, y + h);

        node = node->next;
    }
    return h;
}

static void do_outline(fz_outline* node, int outline_w)
{
    static char* id = "outline";
    static int outline_scroll_y = 0;
    static int saved_outline_scroll_y = 0;
    static int saved_ui_y = 0;

    int outline_h;
    int total_h;

    outline_w -= ui.lineheight;
    outline_h = window_h;
    total_h = measure_outline_height(outline);

    if (ui.x >= 0 && ui.x < outline_w && ui.y >= 0 && ui.y < outline_h) {
        ui.hot = id;
        if (!ui.active && ui.middle) {
            ui.active = id;
            saved_ui_y = ui.y;
            saved_outline_scroll_y = outline_scroll_y;
        }
    }

    if (ui.active == id)
        outline_scroll_y = saved_outline_scroll_y + (saved_ui_y - ui.y) * 5;

    if (ui.hot == id)
        outline_scroll_y -= ui.scroll_y * ui.lineheight * 3;

    ui_scrollbar(outline_w, 0, outline_w + ui.lineheight, outline_h, &outline_scroll_y, outline_h, total_h);

    glScissor(0, 0, outline_w, outline_h);
    glEnable(GL_SCISSOR_TEST);

    glColor4f(1, 1, 1, 1);
    glRectf(0, 0, outline_w, outline_h);

    do_outline_imp(outline, fz_count_pages(ctx, doc), 0, outline_w, 10, -outline_scroll_y);

    glDisable(GL_SCISSOR_TEST);
}

static void do_links(fz_link* link, int xofs, int yofs)
{
    fz_rect r;
    float x, y;

    x = ui.x;
    y = ui.y;

    xofs -= page_tex.x;
    yofs -= page_tex.y;

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);

    while (link) {
        r = link->rect;
        fz_transform_rect(&r, &page_ctm);

        if (x >= xofs + r.x0 && x < xofs + r.x1 && y >= yofs + r.y0 && y < yofs + r.y1) {
            ui.hot = link;
            if (!ui.active && ui.down)
                ui.active = link;
        }

        if (ui.hot == link || showlinks) {
            if (ui.active == link && ui.hot == link)
                glColor4f(0, 0, 1, 0.4f);
            else if (ui.hot == link)
                glColor4f(0, 0, 1, 0.2f);
            else
                glColor4f(0, 0, 1, 0.1f);
            glRectf(xofs + r.x0, yofs + r.y0, xofs + r.x1, yofs + r.y1);
        }

        if (ui.active == link && !ui.down) {
            if (ui.hot == link) {
                if (link->dest.kind == FZ_LINK_GOTO)
                    jump_to_page(link->dest.ld.gotor.page);
                else if (link->dest.kind == FZ_LINK_URI)
                    open_browser(link->dest.ld.uri.uri);
            }
            ui_needs_update = 1;
        }

        link = link->next;
    }

    glDisable(GL_BLEND);
}

static void do_page_selection(int x0, int y0, int x1, int y1)
{
    static fz_rect sel;

    if (ui.x >= x0 && ui.x < x1 && ui.y >= y0 && ui.y < y1) {
        ui.hot = &sel;
        if (!ui.active && ui.right) {
            ui.active = &sel;
            sel.x0 = sel.x1 = ui.x;
            sel.y0 = sel.y1 = ui.y;
        }
    }

    if (ui.active == &sel) {
        sel.x1 = ui.x;
        sel.y1 = ui.y;

        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO); /* invert destination color */
        glEnable(GL_BLEND);

        glColor4f(1, 1, 1, 1);
        glRectf(sel.x0, sel.y0, sel.x1 + 1, sel.y1 + 1);

        glDisable(GL_BLEND);
    }

    if (ui.active == &sel && !ui.right) {
        do_copy_region(&sel, x0, y0);
        ui_needs_update = 1;
    }
}

static void do_search_hits(int xofs, int yofs)
{
    fz_rect r;
    int i;

    xofs -= page_tex.x;
    yofs -= page_tex.y;

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);

    for (i = 0; i < search_hit_count; ++i) {
        r = search_hit_bbox[i];

        fz_transform_rect(&r, &page_ctm);

        glColor4f(1, 0, 0, 0.4f);
        glRectf(xofs + r.x0, yofs + r.y0, xofs + r.x1, yofs + r.y1);
    }

    glDisable(GL_BLEND);
}

static void do_forms(float xofs, float yofs)
{
    pdf_ui_event event;
    fz_point p;
    int i;

    for (i = 0; i < annot_count; ++i)
        ui_draw_image(&annot_tex[i], xofs - page_tex.x, yofs - page_tex.y);

    if (!pdf || search_active)
        return;

    p.x = xofs - page_tex.x + ui.x;
    p.y = xofs - page_tex.x + ui.y;
    fz_transform_point(&p, &page_inv_ctm);

    if (ui.down && !ui.active) {
        event.etype = PDF_EVENT_TYPE_POINTER;
        event.event.pointer.pt = p;
        event.event.pointer.ptype = PDF_POINTER_DOWN;
        if (pdf_pass_event(ctx, pdf, (pdf_page*)page, &event)) {
            if (pdf->focus)
                ui.active = do_forms;
            pdf_update_page(ctx, pdf, (pdf_page*)page);
            render_page();
            ui_needs_update = 1;
        }
    } else if (ui.active == do_forms && !ui.down) {
        ui.active = NULL;
        event.etype = PDF_EVENT_TYPE_POINTER;
        event.event.pointer.pt = p;
        event.event.pointer.ptype = PDF_POINTER_UP;
        if (pdf_pass_event(ctx, pdf, (pdf_page*)page, &event)) {
            pdf_update_page(ctx, pdf, (pdf_page*)page);
            render_page();
            ui_needs_update = 1;
        }
    }
}

static void toggle_fullscreen(void)
{
#if 0
	static int oldw = 100, oldh = 100, oldx = 0, oldy = 0;

	if (!isfullscreen)
	{
		oldw = glutGet(GLUT_WINDOW_WIDTH);
		oldh = glutGet(GLUT_WINDOW_HEIGHT);
		oldx = glutGet(GLUT_WINDOW_X);
		oldy = glutGet(GLUT_WINDOW_Y);
		glutFullScreen();
		isfullscreen = 1;
	}
	else
	{
		glutPositionWindow(oldx, oldy);
		glutReshapeWindow(oldw, oldh);
		isfullscreen = 0;
	}
#endif
}

static void shrinkwrap(void)
{
    int w = fz_mini(page_tex.w + canvas_x, screen_w - SCREEN_FURNITURE_W);
    int h = fz_mini(page_tex.h + canvas_y, screen_h - SCREEN_FURNITURE_H);
    if (isfullscreen)
        toggle_fullscreen();
    glfwSetWindowSize(window, w, h);
}

static void toggle_outline(void)
{
    if (outline) {
        showoutline = !showoutline;
        if (showoutline)
            canvas_x = ui.lineheight * 16;
        else
            canvas_x = 0;
        if (canvas_w == page_tex.w && canvas_h == page_tex.h)
            shrinkwrap();
    }
}

static void auto_zoom_w(void)
{
    currentzoom = fz_clamp(currentzoom * canvas_w / (float)page_tex.w, MINRES, MAXRES);
}

static void auto_zoom_h(void)
{
    currentzoom = fz_clamp(currentzoom * canvas_h / (float)page_tex.h, MINRES, MAXRES);
}

static void auto_zoom(void)
{
    float page_a = (float)page_tex.w / page_tex.h;
    float screen_a = (float)canvas_w / canvas_h;
    if (page_a > screen_a)
        auto_zoom_w();
    else
        auto_zoom_h();
}

static void move_backward(int row_sz)
{
    if (scroll_y <= 0) {
        if (currentpage > 0) {
            scroll_y = page_tex.h;
            currentpage -= 1;
        }
    } else {
        scroll_y -= row_sz;
    }
}

static void smart_move_backward(void)
{
    if (scroll_y <= 0) {
        if (scroll_x <= 0) {
            if (currentpage - 1 >= 0) {
                scroll_x = page_tex.w;
                scroll_y = page_tex.h;
                currentpage -= 1;
            }
        } else {
            scroll_y = page_tex.h;
            scroll_x -= canvas_w * 9 / 10;
        }
    } else {
        scroll_y -= canvas_h * 9 / 10;
    }
}

static void move_forward(int row_sz)
{
    if (scroll_y + canvas_h >= page_tex.h) {
        if (currentpage + 1 < fz_count_pages(ctx, doc)) {
            scroll_y = 0;
            currentpage += 1;
        }
    } else {
        scroll_y += row_sz;
    }
}

static void smart_move_forward(void)
{
    if (scroll_y + canvas_h >= page_tex.h) {
        if (scroll_x + canvas_w >= page_tex.w) {
            if (currentpage + 1 < fz_count_pages(ctx, doc)) {
                scroll_x = 0;
                scroll_y = 0;
                currentpage += 1;
            }
        } else {
            scroll_y = 0;
            scroll_x += canvas_w * 9 / 10;
        }
    } else {
        scroll_y += canvas_h * 9 / 10;
    }
}

static void quit(void)
{
    glfwSetWindowShouldClose(window, 1);
}

static void do_app(void)
{
    if (ui.key == KEY_F4 && ui.mod == GLFW_MOD_ALT)
        quit();

    if (ui.down || ui.middle || ui.right || ui.key)
        showinfo = 0;

    if (!ui.focus && ui.key) {
        switch (ui.key) {
        case 'q':
            quit();
            break;
        case 'm':
            if (number == 0)
                push_history();
            else if (number > 0 && number < nelem(marks))
                marks[number] = currentpage;
            break;
        case 't':
            if (number == 0) {
                if (history_count > 0)
                    pop_history();
            } else if (number > 0 && number < nelem(marks)) {
                jump_to_page(marks[number]);
            }
            break;
        case 'T':
            if (number == 0) {
                if (future_count > 0)
                    pop_future();
            }
            break;
        case 'N':
            search_dir = -1;
            if (search_hit_page == currentpage)
                search_page = currentpage + search_dir;
            else
                search_page = currentpage;
            if (search_page >= 0 && search_page < fz_count_pages(ctx, doc)) {
                search_hit_page = -1;
                if (search_needle)
                    search_active = 1;
            }
            break;
        case 'n':
            search_dir = 1;
            if (search_hit_page == currentpage)
                search_page = currentpage + search_dir;
            else
                search_page = currentpage;
            if (search_page >= 0 && search_page < fz_count_pages(ctx, doc)) {
                search_hit_page = -1;
                if (search_needle)
                    search_active = 1;
            }
            break;
        case 'f':
            toggle_fullscreen();
            break;
        case 'w':
            shrinkwrap();
            break;
        case 'o':
            toggle_outline();
            break;
        case 'W':
            auto_zoom_w();
            break;
        case 'H':
            auto_zoom_h();
            break;
        case 'Z':
            auto_zoom();
            break;
        case 'z':
            currentzoom = number > 0 ? number : DEFRES;
            break;
        case 'x':
            g_x_shrink = fz_mini(page_tex.w / 2, g_x_shrink + 5);
            break;
        case 'X':
            g_x_shrink = fz_maxi(0, (g_x_shrink - 5));
            break;
        case 'y':
            g_y_shrink = fz_mini(page_tex.h / 2, g_y_shrink + 5);
            break;
        case 'Y':
            g_y_shrink = fz_maxi(0, (g_y_shrink - 5));
            break;
        case 'r':
            g_x_shrink = g_y_shrink = 0;
            g_backcolor = 0xFFFFFF;
            currentrotate = 0;
            auto_zoom();
            break;
        case 'c':
            g_backcolor = get_random_backcolor();
            break;
        case '<':
            currentpage -= 10 * fz_maxi(number, 1);
            break;
        case '>':
            currentpage += 10 * fz_maxi(number, 1);
            break;
        case ',':
        case KEY_PAGE_UP:
            currentpage -= fz_maxi(number, 1);
            break;
        case '.':
        case KEY_PAGE_DOWN:
            currentpage += fz_maxi(number, 1);
            break;
        case 'b':
            number = fz_maxi(number, 1);
            while (number--)
                smart_move_backward();
            break;
        case ' ':
            number = fz_maxi(number, 1);
            while (number--)
                smart_move_forward();
            break;
        case 'u':
            move_backward(canvas_h * 9 / 10);
            break;
        case 'd':
            move_forward(canvas_h * 9 / 10);
            break;
        case 'g':
            if (number > 0)
                jump_to_page(number - 1);
            break;
        case 'G':
            jump_to_page(fz_count_pages(ctx, doc) - 1);
            break;
        case '+':
        case '=':
            currentzoom = zoom_in(currentzoom);
            break;
        case '-':
            currentzoom = zoom_out(currentzoom);
            break;
        case '[':
            currentrotate += 0.1;
            break;
        case ']':
            currentrotate -= 0.1;
            break;
        case '{':
            currentrotate += 90;
            break;
        case '}':
            currentrotate -= 90;
            break;
        case 'L':
            showlinks = !showlinks;
            break;
        case 'i':
            showinfo = !showinfo;
            break;
        case 'v':
            g_isinvertcolor = !g_isinvertcolor;
            break;
        case '/':
            search_dir = 1;
            showsearch = 1;
            search_input.p = search_input.text;
            search_input.q = search_input.end;
            break;
        case '?':
            search_dir = -1;
            showsearch = 1;
            search_input.p = search_input.text;
            search_input.q = search_input.end;
            break;
        case 'k':
            move_backward(canvas_h / 7);
            break;
        case 'j':
            move_forward(canvas_h / 7);
            break;
        case 'h':
            scroll_x -= canvas_w / 10;
            break;
        case 'l':
            scroll_x += canvas_w / 10;
            break;
        case KEY_UP:
            scroll_y -= 10;
            break;
        case KEY_DOWN:
            scroll_y += 10;
            break;
        case KEY_LEFT:
            scroll_x -= 10;
            break;
        case KEY_RIGHT:
            scroll_x += 10;
            break;
        }

        if (ui.key >= '0' && ui.key <= '9')
            number = number * 10 + ui.key - '0';
        else
            number = 0;

        currentpage = fz_clampi(currentpage, 0, fz_count_pages(ctx, doc) - 1);
        currentzoom = fz_clamp(currentzoom, MINRES, MAXRES);
        while (currentrotate < 0)
            currentrotate += 360;
        while (currentrotate >= 360)
            currentrotate -= 360;

        if (search_hit_page != currentpage)
            search_hit_page = -1; /* clear highlights when navigating */

        ui_needs_update = 1;

        ui.key = 0; /* we ate the key event, so zap it */
    }
}

static int do_info_line(int x, int y, char* label, char* text)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s: %s", label, text);
    ui_draw_string(ctx, x, y, buf);
    return y + ui.lineheight;
}

static void do_info(void)
{
    char buf[256];

    int x = canvas_x + 4 * ui.lineheight;
    int y = canvas_y + 4 * ui.lineheight;
    int w = canvas_w - 8 * ui.lineheight;
    int h = 9 * ui.lineheight;

    glBegin(GL_TRIANGLE_STRIP);
    {
        glColor4f(0.9f, 0.9f, 0.9f, 1.0f);
        glVertex2f(x, y);
        glVertex2f(x, y + h);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
    }
    glEnd();

    x += ui.lineheight;
    y += ui.lineheight + ui.baseline;

    glColor4f(0, 0, 0, 1);
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, buf, sizeof buf) > 0)
        y = do_info_line(x, y, "Title", buf);
    if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_AUTHOR, buf, sizeof buf) > 0)
        y = do_info_line(x, y, "Author", buf);
    if (fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, buf, sizeof buf) > 0)
        y = do_info_line(x, y, "Format", buf);
    if (fz_lookup_metadata(ctx, doc, FZ_META_ENCRYPTION, buf, sizeof buf) > 0)
        y = do_info_line(x, y, "Encryption", buf);
    if (pdf_specifics(ctx, doc)) {
        if (fz_lookup_metadata(ctx, doc, "info:Creator", buf, sizeof buf) > 0)
            y = do_info_line(x, y, "PDF Creator", buf);
        if (fz_lookup_metadata(ctx, doc, "info:Producer", buf, sizeof buf) > 0)
            y = do_info_line(x, y, "PDF Producer", buf);
        buf[0] = 0;
        if (fz_has_permission(ctx, doc, FZ_PERMISSION_PRINT))
            fz_strlcat(buf, "print, ", sizeof buf);
        if (fz_has_permission(ctx, doc, FZ_PERMISSION_COPY))
            fz_strlcat(buf, "copy, ", sizeof buf);
        if (fz_has_permission(ctx, doc, FZ_PERMISSION_EDIT))
            fz_strlcat(buf, "edit, ", sizeof buf);
        if (fz_has_permission(ctx, doc, FZ_PERMISSION_ANNOTATE))
            fz_strlcat(buf, "annotate, ", sizeof buf);
        if (strlen(buf) > 2)
            buf[strlen(buf) - 2] = 0;
        else
            fz_strlcat(buf, "none", sizeof buf);
        y = do_info_line(x, y, "Permissions", buf);
    }
}

static void do_canvas(void)
{
    static int saved_scroll_x = 0;
    static int saved_scroll_y = 0;
    static int saved_ui_x = 0;
    static int saved_ui_y = 0;

    float x, y;

    if (oldpage != currentpage || oldzoom != currentzoom || oldrotate != currentrotate || g_oldinvertcolor != g_isinvertcolor || g_old_x_shrink != g_x_shrink || g_old_y_shrink != g_y_shrink) {
        render_page();
        update_title();
        oldpage = currentpage;
        oldzoom = currentzoom;
        oldrotate = currentrotate;
        g_oldinvertcolor = g_isinvertcolor;
        g_old_x_shrink = g_x_shrink;
        g_old_y_shrink = g_y_shrink;
    }

    if (ui.x >= canvas_x && ui.x < canvas_x + canvas_w && ui.y >= canvas_y && ui.y < canvas_y + canvas_h) {
        ui.hot = doc;
        if (!ui.active && ui.middle) {
            ui.active = doc;
            saved_scroll_x = scroll_x;
            saved_scroll_y = scroll_y;
            saved_ui_x = ui.x;
            saved_ui_y = ui.y;
        }
    }

    if (ui.hot == doc) {
        scroll_x -= ui.scroll_x * ui.lineheight * 3;
        scroll_y -= ui.scroll_y * ui.lineheight * 3;
    }

    if (ui.active == doc) {
        scroll_x = saved_scroll_x + saved_ui_x - ui.x;
        scroll_y = saved_scroll_y + saved_ui_y - ui.y;
    }

    if (page_tex.w <= canvas_w) {
        scroll_x = 0;
        x = canvas_x + (canvas_w - page_tex.w) / 2;
    } else {
        scroll_x = fz_clamp(scroll_x, 0, page_tex.w - canvas_w);
        x = canvas_x - scroll_x;
    }

    if (page_tex.h <= canvas_h) {
        scroll_y = 0;
        y = canvas_y + (canvas_h - page_tex.h) / 2;
    } else {
        scroll_y = fz_clamp(scroll_y, 0, page_tex.h - canvas_h);
        y = canvas_y - scroll_y;
    }

    ui_draw_image(&page_tex, x - page_tex.x, y - page_tex.y);

    do_forms(x, y);

    if (!search_active) {
        do_links(links, x, y);
        do_page_selection(x, y, x + page_tex.w, y + page_tex.h);
        if (search_hit_page == currentpage && search_hit_count > 0)
            do_search_hits(x, y);
    }
}

static void run_main_loop(void)
{
    glViewport(0, 0, window_w, window_h);
    glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, window_w, window_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    ui_begin();

    if (search_active) {
        float start_time = glfwGetTime();

        if (ui.key == KEY_ESCAPE)
            search_active = 0;

        /* ignore events during search */
        ui.key = ui.mod = 0;
        ui.down = ui.middle = ui.right = 0;

        while (glfwGetTime() < start_time + 0.2) {
            search_hit_count = fz_search_page_number(ctx, doc, search_page, search_needle,
                search_hit_bbox, nelem(search_hit_bbox));
            if (search_hit_count) {
                search_active = 0;
                search_hit_page = search_page;
                jump_to_page(search_hit_page);
                break;
            } else {
                search_page += search_dir;
                if (search_page < 0 || search_page == fz_count_pages(ctx, doc)) {
                    search_active = 0;
                    break;
                }
            }
        }

        /* keep searching later */
        if (search_active)
            ui_needs_update = 1;
    }

    do_app();

    canvas_w = window_w - canvas_x;
    canvas_h = window_h - canvas_y;

    do_canvas();

    if (showinfo)
        do_info();

    if (showoutline)
        do_outline(outline, canvas_x);

    if (showsearch) {
        int state = ui_input(canvas_x, 0, canvas_x + canvas_w, ui.lineheight + 4, &search_input);
        if (state == -1) {
            ui.focus = NULL;
            showsearch = 0;
        } else if (state == 1) {
            ui.focus = NULL;
            showsearch = 0;
            search_page = -1;
            if (search_needle) {
                fz_free(ctx, search_needle);
                search_needle = NULL;
            }
            if (search_input.end > search_input.text) {
                search_needle = fz_strdup(ctx, search_input.text);
                search_active = 1;
                search_page = currentpage;
            }
        }
        ui_needs_update = 1;
    }

    if (search_active) {
        char buf[256];
        sprintf(buf, "Searching page %d of %d.", search_page + 1, fz_count_pages(ctx, doc));
        ui_label_draw(canvas_x, 0, canvas_x + canvas_w, ui.lineheight + 4, buf);
    }

    ui_end();

    glfwSwapBuffers(window);

    ogl_assert(ctx, "swap buffers");
}

static void on_char(GLFWwindow* window, unsigned int key, int mod)
{
    ui.key = key;
    ui.mod = mod;

    /* Let Shift + Space == PageUp */
    if (GLFW_MOD_SHIFT == mod && ' ' == key) {
        ui.key = 'b';
    }

    run_main_loop();
    ui.key = ui.mod = 0;
}

static void on_key(GLFWwindow* window, int special, int scan, int action, int mod)
{
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        ui.key = 0;
        switch (special) {
#ifndef GLFW_MUPDF_FIXES
        /* regular control characters: ^A, ^B, etc. */
        default:
            if (special >= 'A' && special <= 'Z' && mod == GLFW_MOD_CONTROL)
                ui.key = KEY_CTL_A + special - 'A';
            break;

        /* regular control characters: escape, enter, backspace, tab */
        case GLFW_KEY_ESCAPE:
            ui.key = KEY_ESCAPE;
            break;
        case GLFW_KEY_ENTER:
            ui.key = KEY_ENTER;
            break;
        case GLFW_KEY_BACKSPACE:
            ui.key = KEY_BACKSPACE;
            break;
        case GLFW_KEY_TAB:
            ui.key = KEY_TAB;
            break;
#endif
        case GLFW_KEY_INSERT:
            ui.key = KEY_INSERT;
            break;
        case GLFW_KEY_DELETE:
            ui.key = KEY_DELETE;
            break;
        case GLFW_KEY_RIGHT:
            ui.key = KEY_RIGHT;
            break;
        case GLFW_KEY_LEFT:
            ui.key = KEY_LEFT;
            break;
        case GLFW_KEY_DOWN:
            ui.key = KEY_DOWN;
            break;
        case GLFW_KEY_UP:
            ui.key = KEY_UP;
            break;
        case GLFW_KEY_PAGE_UP:
            ui.key = KEY_PAGE_UP;
            break;
        case GLFW_KEY_PAGE_DOWN:
            ui.key = KEY_PAGE_DOWN;
            break;
        case GLFW_KEY_HOME:
            ui.key = KEY_HOME;
            break;
        case GLFW_KEY_END:
            ui.key = KEY_END;
            break;
        case GLFW_KEY_F1:
            ui.key = KEY_F1;
            break;
        case GLFW_KEY_F2:
            ui.key = KEY_F2;
            break;
        case GLFW_KEY_F3:
            ui.key = KEY_F3;
            break;
        case GLFW_KEY_F4:
            ui.key = KEY_F4;
            break;
        case GLFW_KEY_F5:
            ui.key = KEY_F5;
            break;
        case GLFW_KEY_F6:
            ui.key = KEY_F6;
            break;
        case GLFW_KEY_F7:
            ui.key = KEY_F7;
            break;
        case GLFW_KEY_F8:
            ui.key = KEY_F8;
            break;
        case GLFW_KEY_F9:
            ui.key = KEY_F9;
            break;
        case GLFW_KEY_F10:
            ui.key = KEY_F10;
            break;
        case GLFW_KEY_F11:
            ui.key = KEY_F11;
            break;
        case GLFW_KEY_F12:
            ui.key = KEY_F12;
            break;
        }
        if (ui.key) {
            ui.mod = mod;
            run_main_loop();
            ui.key = ui.mod = 0;
        }
    }
}

static void on_mouse_button(GLFWwindow* window, int button, int action, int mod)
{
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        ui.down = (action == GLFW_PRESS);
        break;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        ui.middle = (action == GLFW_PRESS);
        break;
    case GLFW_MOUSE_BUTTON_RIGHT:
        ui.right = (action == GLFW_PRESS);
        break;
    }

    run_main_loop();
}

static void on_mouse_motion(GLFWwindow* window, double x, double y)
{
    ui.x = x;
    ui.y = y;
    ui_needs_update = 1;
}

static void on_scroll(GLFWwindow* window, double x, double y)
{
    ui.scroll_x = x;
    ui.scroll_y = y;
    run_main_loop();
    ui.scroll_x = ui.scroll_y = 0;
}

static void on_reshape(GLFWwindow* window, int w, int h)
{
    showinfo = 0;
    window_w = w;
    window_h = h;
    ui_needs_update = 1;
}

static void on_display(GLFWwindow* window)
{
    ui_needs_update = 1;
}

static void on_error(int error, const char* msg)
{
    fprintf(stderr, "gl error %d: %s\n", error, msg);
}

static void usage(const char* argv0)
{
    fprintf(stderr, "mupdf-gl version %s\n", FZ_VERSION);
    fprintf(stderr, "usage: %s [options] document [page]\n", argv0);
    fprintf(stderr, "\t-p -\tpassword\n");
    fprintf(stderr, "\t-r -\tresolution\n");
    fprintf(stderr, "\t-W -\tpage width for EPUB layout\n");
    fprintf(stderr, "\t-H -\tpage height for EPUB layout\n");
    fprintf(stderr, "\t-S -\tfont size for EPUB layout\n");
    fprintf(stderr, "\t-U -\tuser style sheet for EPUB layout\n");
    fprintf(stderr, "\t-C -\tset backgroud color, E.g 0xfdf6e3\n");
    exit(1);
}

static int load_reading_progress(const char* filename)
{
    int pages = 1;
    char line[1024];
    char buf[512];

    const char* home = getenv("HOME");
    if (!home)
        return 1;

    sprintf(buf, "%s/.mupdf.history", home);
    FILE* fp = fopen(buf, "r");
    if (fp != NULL) {
        const int len = strlen(filename);

        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, filename, len) == 0) {
                const char* pos = line + len + 1;
                pages = fz_atoi(pos);
                break;
            }
        }
        fclose(fp);
    }

    return pages;
}

static void save_reading_progress(const char* filename, int pages)
{
    char line[1024];
    char temp_file[512];
    char original_file[512];

    const char* home = getenv("HOME");
    if (!home)
        return;

    sprintf(temp_file, "%s/.mupdf.history.tmp", home);
    FILE* tmpfp = fopen(temp_file, "w");
    if (!tmpfp)
        return;
    fprintf(tmpfp, "%s:%d\n", filename, pages + 1); /* save current file record */

    sprintf(original_file, "%s/.mupdf.history", home);
    FILE* fp = fopen(original_file, "r");
    if (fp != NULL) {
        const int len = strlen(filename);
        int record = MAX_RECENT_FILES;

        while (--record > 0 && fgets(line, sizeof(line), fp)) {
            if (strncmp(line, filename, len) != 0) {
                fprintf(tmpfp, "%s", line);
            }
        }
        fclose(fp);
        remove(original_file);
    }

    fclose(tmpfp);
    rename(temp_file, original_file);
}

#ifdef _MSC_VER
int main_utf8(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
    const GLFWvidmode* video_mode;
    char filename[2048];
    char* password = "";
    float layout_w = DEFAULT_LAYOUT_W;
    float layout_h = DEFAULT_LAYOUT_H;
    float layout_em = DEFAULT_LAYOUT_EM;
    char* layout_css = NULL;
    int pageno = 1;
    int c;

    while ((c = fz_getopt(argc, argv, "p:r:W:H:S:U:C:")) != -1) {
        switch (c) {
        default:
            usage(argv[0]);
            break;
        case 'p':
            password = fz_optarg;
            break;
        case 'r':
            currentzoom = fz_atof(fz_optarg);
            break;
        case 'W':
            layout_w = fz_atof(fz_optarg);
            break;
        case 'H':
            layout_h = fz_atof(fz_optarg);
            break;
        case 'S':
            layout_em = fz_atof(fz_optarg);
            break;
        case 'U':
            layout_css = fz_optarg;
            break;
        case 'C':
            g_backcolor = strtol(fz_optarg, NULL, 16);
            break;
        }
    }

    if (fz_optind < argc) {
        fz_strlcpy(filename, argv[fz_optind++], sizeof filename);
    } else {
#ifdef _WIN32
        win_install();
        if (!win_open_file(filename, sizeof filename))
            exit(0);
#else
        char const* lTheOpenFileName;
        char const* lFilterPatterns[] = { "*.pdf" };

        lTheOpenFileName = tinyfd_openFileDialog(
            "Please choose a pdf file",
            "",
            nelem(lFilterPatterns),
            lFilterPatterns,
            NULL,
            0);
        if (lTheOpenFileName) {
            fz_strlcpy(filename, lTheOpenFileName, sizeof(filename));
        } else {
            usage(argv[0]);
        }
#endif
    }

    title = strrchr(filename, '/');
    if (!title)
        title = strrchr(filename, '\\');
    if (title)
        ++title;
    else
        title = filename;

    if (argc - fz_optind > 0)
        pageno = atoi(argv[fz_optind++]);
    else {
        pageno = load_reading_progress(title);
    }

    memset(&ui, 0, sizeof ui);

    search_input.p = search_input.text;
    search_input.q = search_input.p;
    search_input.end = search_input.p;

    if (!glfwInit()) {
        fprintf(stderr, "cannot initialize glfw\n");
        exit(1);
    }

    video_mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    screen_w = video_mode->width;
    screen_h = video_mode->height;

    glfwSetErrorCallback(on_error);

    window = glfwCreateWindow(DEFAULT_WINDOW_W, DEFAULT_WINDOW_H, filename, NULL, NULL);
    if (!window) {
        fprintf(stderr, "cannot create glfw window\n");
        exit(1);
    }

    glfwMakeContextCurrent(window);

    ctx = fz_new_context(NULL, NULL, 0);
    fz_register_document_handlers(ctx);

    if (layout_css) {
        fz_buffer* buf = fz_read_file(ctx, layout_css);
        fz_write_buffer_byte(ctx, buf, 0);
        fz_set_user_css(ctx, (char*)buf->data);
        fz_drop_buffer(ctx, buf);
    }

    has_ARB_texture_non_power_of_two = glfwExtensionSupported("GL_ARB_texture_non_power_of_two");
    if (!has_ARB_texture_non_power_of_two)
        fz_warn(ctx, "OpenGL implementation does not support non-power of two texture sizes");

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);

    ui.fontsize = DEFAULT_UI_FONTSIZE;
    ui.baseline = DEFAULT_UI_BASELINE;
    ui.lineheight = DEFAULT_UI_LINEHEIGHT;

    ui_init_fonts(ctx, ui.fontsize);

    doc = fz_open_document(ctx, filename);
    if (fz_needs_password(ctx, doc)) {
        if (!fz_authenticate_password(ctx, doc, password)) {
            fprintf(stderr, "Invalid password.\n");
            exit(1);
        }
    }

    outline = fz_load_outline(ctx, doc);
    pdf = pdf_specifics(ctx, doc);
    if (pdf)
        pdf_enable_js(ctx, pdf);

    fz_layout_document(ctx, doc, layout_w, layout_h, layout_em);

    /* Go to page number at startup*/
    if (pageno > 1) {
        pageno = fz_min(pageno, fz_count_pages(ctx, doc));
        jump_to_page(pageno - 1);
    }

    render_page();
    update_title();
    shrinkwrap();

    glfwSetFramebufferSizeCallback(window, on_reshape);
    glfwSetCursorPosCallback(window, on_mouse_motion);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetCharModsCallback(window, on_char);
    glfwSetKeyCallback(window, on_key);
    glfwSetWindowRefreshCallback(window, on_display);

    glfwGetFramebufferSize(window, &window_w, &window_h);

    ui_needs_update = 1;

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEvents();
        if (ui_needs_update)
            run_main_loop();
    }

    ui_finish_fonts(ctx);

    fz_drop_link(ctx, links);
    fz_drop_page(ctx, page);
    fz_drop_outline(ctx, outline);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

    glfwTerminate();

    // save reading progress.
    save_reading_progress(title, currentpage);

    return 0;
}

#ifdef _MSC_VER
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    int argc;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char** argv = fz_argv_from_wargv(argc, wargv);
    int ret = main_utf8(argc, argv);
    fz_free_argv(argc, argv);
    return ret;
}
#endif
