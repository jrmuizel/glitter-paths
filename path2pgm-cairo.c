#include <stdlib.h>
#include <cairo.h>

struct context {
        cairo_t *cr;
        cairo_format_t format;
        void *data;
};

struct context *
cx_create()
{
        struct context *cx = calloc(1, sizeof(struct context));
        cx->cr = cairo_create(NULL);
        cx->format = CAIRO_FORMAT_ARGB32;
        cx->data = NULL;
        return cx;
}

void
cx_destroy(struct context *cx)
{
        cairo_destroy(cx->cr);
        free(cx->data);
        free(cx);
}

void
cx_reset_clip(struct context *cx,
              int xmin, int ymin, int xmax, int ymax)
{
        cairo_t *cr = cx->cr;
        cairo_reset_clip(cr);
        cairo_rectangle(cr, xmin, ymin, xmax-xmin, ymax-ymin);
        cairo_clip(cr);
}

void
cx_resize(struct context *cx, unsigned width, unsigned height)
{
        cairo_surface_t *surf = cairo_image_surface_create(cx->format, width, height);
        cairo_destroy(cx->cr);
        cx->cr = cairo_create(surf);
        cairo_surface_destroy(surf);
        cairo_set_source_rgb(cx->cr, 1,1,1);
}

void
cx_clear(struct context *cx)
{
        cairo_t *cr = cx->cr;
        cairo_save(cr); {
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
                cairo_reset_clip(cr);
                cairo_paint(cr);
                cairo_restore(cr);
        }
}

void
cx_set_fill_rule(struct context *cx, int nonzero_fill)
{
        cairo_set_fill_rule(cx->cr,
                            nonzero_fill
                            ? CAIRO_FILL_RULE_WINDING
                            : CAIRO_FILL_RULE_EVEN_ODD);
}

void
cx_moveto(struct context *cx, double x, double y)
{
        cairo_move_to(cx->cr, x, y);
}

void
cx_lineto(struct context *cx, double x, double y)
{
        cairo_line_to(cx->cr, x, y);
}

void
cx_closepath(struct context *cx)
{
        cairo_close_path(cx->cr);
}

void
cx_fill(struct context *cx)
{
        cairo_fill(cx->cr);
}

void
cx_get_pixels(
        struct context *cx,
        unsigned char **OUT_pixels,
        size_t *OUT_stride,
        unsigned *OUT_width,
        unsigned *OUT_height)
{
        cairo_surface_t *target = cairo_get_target(cx->cr);
        unsigned w = cairo_image_surface_get_width(target);
        unsigned h = cairo_image_surface_get_height(target);
        size_t stride = w + ((-w)&3U);
        cairo_surface_t *mask;
        cairo_t *mask_cr;

        free(cx->data);
        *OUT_pixels = calloc(h, stride);
        *OUT_stride = stride;
        *OUT_width = w;
        *OUT_height = h;
        cx->data = *OUT_pixels;

        mask = cairo_image_surface_create_for_data(
                *OUT_pixels, CAIRO_FORMAT_A8, w, h, stride);
        mask_cr = cairo_create(mask);
        cairo_surface_destroy(mask);

        cairo_set_source_surface(mask_cr, target, 0, 0);
        cairo_paint(mask_cr);
        cairo_destroy(mask_cr);
}
