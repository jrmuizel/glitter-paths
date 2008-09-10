#include <stdio.h>
#include <stddef.h>
struct context {};
struct context CX;

struct context *cx_create() { puts("# create"); return &CX; }

void cx_destroy(struct context *cx) { puts("# destroy"); }
void cx_clear(struct context *cx) { puts("# clear"); }

void cx_resize(struct context *cx, unsigned width, unsigned height) { printf("I %u %u\n", width, height); }
void cx_reset_clip(struct context *cx, int xmin, int ymin, int xmax, int ymax) { printf("B %d %d %d %d\n", xmin, ymin, xmax, ymax); }
void cx_moveto(struct context *cx, double x, double y) { printf("M %f %f\n", x, y); }
void cx_lineto(struct context *cx, double x, double y) { printf("L %f %f\n", x, y); }
void cx_closepath(struct context *cx) { puts("Z"); }
void cx_fill(struct context *cx) { puts("F"); }
void cx_set_fill_rule(struct context *cx, int nonzero_fill) { puts(nonzero_fill ? "N" : "E"); }

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
