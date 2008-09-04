#include <stddef.h>
struct context {};
struct context CX;

struct context *cx_create() { return &CX; }

void cx_resize(struct context *cx, unsigned width, unsigned height) {}
void cx_clear(struct context *cx) {}
void cx_destroy(struct context *cx) {}
void cx_reset_clip(struct context *cx, int xmin, int ymin, int xmax, int ymax) {}
void cx_moveto(struct context *cx, double x, double y) {}
void cx_lineto(struct context *cx, double x, double y) {}
void cx_closepath(struct context *cx) {}
void cx_fill(struct context *cx) {}
void cx_set_fill_rule(struct context *cx, int nonzero_fill) {}

void cx_get_pixels(
        struct context *cx,
        unsigned char **OUT_pixels,
        size_t *OUT_stride,
        unsigned *OUT_width,
        unsigned *OUT_height)
{
        static unsigned char pixel = 0;
        *OUT_pixels = &pixel;
        *OUT_stride = 1;
        *OUT_width = 1;
        *OUT_height = 1;
}
