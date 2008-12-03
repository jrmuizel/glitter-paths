#include <assert.h>
#include "glitter-paths.c"

struct point {
        double x, y;
        int valid;
};

struct context {
        /* A8 pixels. */
        unsigned char *pixels;
        size_t stride;
        unsigned width, height;

        /* Path state. */
        struct point current_point;
        struct point first_point;

        /* Render state */
        glitter_scan_converter_t *converter;
        int nonzero_fill;
};

struct context *
cx_create()
{
        struct context *cx = calloc(1, sizeof(struct context));

        cx->pixels = NULL;
        cx->stride = 0;
        cx->width = 0;
        cx->height = 0;

        cx->current_point.x = 0;
        cx->current_point.y = 0;
        cx->current_point.valid = 0;
        cx->first_point = cx->current_point;

        cx->converter = glitter_scan_converter_create();
        cx->nonzero_fill = 1;

        return cx;
}

void
cx_reset_clip(
        struct context *cx,
        int xmin, int ymin,
        int xmax, int ymax)
{
        glitter_scan_converter_reset(
                cx->converter,
                xmin, ymin,
                xmax, ymax);
}

void
cx_destroy(struct context *cx)
{
        if (cx) {
                free(cx->pixels);
                glitter_scan_converter_destroy(cx->converter);
                memset(cx, 0, sizeof(struct context));
                free(cx);
        }
}

void
cx_resize(struct context *cx,
          unsigned width, unsigned height)
{
        cx->pixels = realloc(cx->pixels, width*height);
        if (0==width*height)
                cx->pixels = NULL;
        memset(cx->pixels, 0, width*height);
        cx->width = width;
        cx->stride = width;
        cx->height = height;

        cx_reset_clip(cx, 0,0, width, height);
}

void
cx_clear(struct context *cx)
{
        memset(cx->pixels, 0, cx->width*cx->height);
}

void
cx_moveto(struct context *cx,
          double x, double y)
{
        cx->current_point.x = x;
        cx->current_point.y = y;
        cx->current_point.valid = 1;
        cx->first_point = cx->current_point;
}

void
cx_lineto(struct context *cx,
          double x, double y)
{
        if (cx->current_point.valid) {
                glitter_input_scaled_t x1 = cx->current_point.x * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t y1 = cx->current_point.y * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t x2 = x * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t y2 = y * GLITTER_INPUT_SCALE;

                glitter_scan_converter_add_edge(
                        cx->converter,
                        x1, y1,
                        x2, y2,
                        +1);

                cx->current_point.x = x;
                cx->current_point.y = y;
                cx->current_point.valid = 1;
        }
        else {
                cx_moveto(cx, x, y);
        }
}

void
cx_closepath(struct context *cx)
{
        if (cx->first_point.valid) {
                cx_lineto(cx, cx->first_point.x, cx->first_point.y);
        }
}

void
cx_fill(struct context *cx)
{
        cx_closepath(cx);

        glitter_scan_converter_render(
                cx->converter,
                cx->nonzero_fill,
                cx->pixels,
                cx->stride);

        cx->current_point.valid = 0;
        cx->first_point.valid = 0;
}

void
cx_set_fill_rule(struct context *cx, int nonzero_fill)
{
        cx->nonzero_fill = nonzero_fill;
}

void
cx_get_pixels(
        struct context *cx,
        unsigned char **OUT_pixels,
        size_t *OUT_stride,
        unsigned *OUT_width,
        unsigned *OUT_height)
{
        *OUT_pixels = cx->pixels;
        *OUT_stride = cx->stride;
        *OUT_width = cx->width;
        *OUT_height = cx->height;
}
