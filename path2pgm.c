#define BIN_SH /*
gcc -O3 -funroll-all-loops -g -Wall -W -o `basename $0 .c` $0
exit $?
*/
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glitter-paths.c"

struct point {
        double x, y;
        int valid;
};

struct image {
        unsigned char *pixels;
        unsigned width;
        unsigned height;
        size_t stride;
};

struct context {
        glitter_scan_converter_t *converter;
        int nonzero_fill;

        /* Clip box in pixels. */
        int xmin, ymin, xmax, ymax;

        /* Path state. */
        struct point current_point;
        struct point first_point;

        struct image image[1];
};

static void
image_init(
        struct image *im,
        unsigned width, unsigned height)
{
        im->pixels = calloc(width,height);
        assert(im->pixels);
        im->stride = width;
        im->width = width;
        im->height = height;
}

static void
image_fini(struct image *im)
{
        free(im->pixels);
        memset(im, 0, sizeof(struct image));
}

static void
image_save_as_pgm_to_stream(struct image *im, FILE *fp)
{
        unsigned x, y;
        fprintf(fp, "P2\n%u %u\n255\n",
                im->width, im->height);
        for (y=0; y<im->height; y++) {
                for (x=0; x<im->width; x++) {
                        fprintf(fp, "%d ", im->pixels[x + y*im->stride]);
                }
                fprintf(fp, "\n");
        }
}

static void
cx_resize(struct context *cx, unsigned width, unsigned height)
{
        image_fini(cx->image);
        image_init(cx->image, width, height);
}

static void
cx_init(struct context *cx, unsigned width, unsigned height, int nonzero_fill)
{
        memset(cx, 0, sizeof(struct context));
        cx->converter = glitter_scan_converter_create();
        cx->nonzero_fill = nonzero_fill;
        assert(cx->converter);
        image_init(cx->image, width, height);
}

static void
cx_fini(struct context *cx)
{
        glitter_scan_converter_destroy(cx->converter);
        image_fini(cx->image);
        memset(cx, 0, sizeof(struct context));
}

static void
cx_reset_clip(
        struct context *cx,
        int xmin, int ymin,
        int xmax, int ymax)
{
        cx->xmin = xmin;
        cx->ymin = ymin;
        cx->xmax = xmax;
        cx->ymax = ymax;
        assert(!glitter_scan_converter_reset(
                       cx->converter,
                       xmin, ymin,
                       xmax, ymax));
}

static void
cx_moveto(struct context *cx,
          double x, double y)
{
        cx->current_point.x = x;
        cx->current_point.y = y;
        cx->current_point.valid = 1;
        cx->first_point = cx->current_point;
}

static void
cx_lineto(struct context *cx,
          double x, double y)
{
        if (cx->current_point.valid) {
                glitter_input_scaled_t x1 = cx->current_point.x * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t y1 = cx->current_point.y * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t x2 = x * GLITTER_INPUT_SCALE;
                glitter_input_scaled_t y2 = y * GLITTER_INPUT_SCALE;

                assert(!glitter_scan_converter_add_edge(
                               cx->converter, x1, y1, x2, y2, +1));

                cx->current_point.x = x;
                cx->current_point.y = y;
                cx->current_point.valid = 1;
        }
        else {
                cx_moveto(cx, x, y);
        }
}

static void
cx_closepath(struct context *cx)
{
        if (cx->first_point.valid) {
                cx_lineto(cx, cx->first_point.x, cx->first_point.y);
        }
}

static void
cx_fill(struct context *cx)
{
        struct image *im = cx->image;
        if (cx->current_point.valid) {
                glitter_status_t err;
                err = glitter_scan_converter_render(
                        cx->converter,
                        im->pixels,
                        im->stride,
                        cx->nonzero_fill);
                assert(!err);
        }
        cx->current_point.valid = 0;
        cx->first_point.valid = 0;
}

static int
cx_interpret_stream(struct context *cx, FILE *fp)
{
#define get_double_arg(arg) do {\
	while ((c = fgetc(fp)) && EOF != c && (isspace(c) || ',' == c)) {}\
	if (EOF != c) ungetc(c, fp);\
        if (1 != fscanf(fp, "%lf", (arg))) {\
                fprintf(stderr, "failed to read a numeric argument\n");\
                return -1;\
        }\
} while (0)

        while (!feof(fp) && !ferror(fp)) {
                double x[2], y[2];
                int c = EOF;
                char cmd;
                struct point cp = cx->current_point;
                if (1 == fscanf(fp, " %c", &cmd))
                        c = (unsigned char)cmd;
                switch (c) {
                case 'M':       /* move */
                        get_double_arg(x);
                        get_double_arg(y);
                        cx_moveto(cx, *x, *y);
                        break;
                case 'm':       /* move, relative */
                        get_double_arg(x);
                        get_double_arg(y);
                        if (cp.valid)
                                cx_moveto(cx, cp.x + *x, cp.y + *y);
                        break;
                case 'L':       /* line */
                        get_double_arg(x);
                        get_double_arg(y);
                        cx_lineto(cx, *x, *y);
                        break;
                case 'l':       /* line, relative */
                        get_double_arg(x);
                        get_double_arg(y);
                        if (cp.valid)
                                cx_lineto(cx, cp.x + *x, cp.y + *y);
                        break;
                case 'H':       /* horizontal line */
                        get_double_arg(x);
                        if (cp.valid)
                                cx_lineto(cx, *x, cp.y);
                        break;
                case 'h':       /* horizontal line, relative */
                        get_double_arg(x);
                        if (cp.valid)
                                cx_lineto(cx, cp.x + *x, cp.y);
                        break;
                case 'V':       /* vertical line */
                        get_double_arg(y);
                        if (cp.valid)
                                cx_lineto(cx, cp.x, *y);
                        break;
                case 'v':       /* vertical line, relative*/
                        get_double_arg(y);
                        if (cp.valid)
                                cx_lineto(cx, cp.x, cp.y + *y);
                        break;
                case 'z':
                case 'Z':       /* close path */
                        cx_closepath(cx);
                        break;
                case 'N':       /* non-zero winding number fill rule */
                        cx->nonzero_fill = 1;
                        break;
                case 'E':       /* even-odd fill rule */
                        cx->nonzero_fill = 0;
                        break;
                case 'F':       /* fill */
                        cx_fill(cx);
                        break;
                case '#':       /* eol comment */
                        while ((c = fgetc(fp)) && EOF != c && '\n' != c) {}
                        break;
                case 'B':       /* B: xmin ymin xmax ymax; set clip */
                        get_double_arg(x+0);
                        get_double_arg(y+0);
                        get_double_arg(x+1);
                        get_double_arg(y+1);
                        cx_reset_clip(cx, x[0], y[0], x[1], y[1]);
                        break;
                case 'I':       /* reinitialise context; width height */
                        get_double_arg(x);
                        get_double_arg(y);
                        cx_resize(cx, *x, *y);
                        cx_reset_clip(cx, 0, 0, *x, *y);
                        break;
                case EOF:
                        break;
                default:
                        fprintf(stderr,
                                "unknown command character '%c' in input\n",
                                c);
                        return -1;
                }
        }
        cx_fill(cx);
        return 0;
#undef get_double_arg
}

static char *
prefix(char *s, char const *pref)
{
        size_t len = strlen(pref);
        if (0 == strncmp(s, pref, len)) {
                return s + len;
        }
        return NULL;
}

int
main(int argc, char **argv)
{
        struct context cx[1];
        char const *filename = NULL;
        int width = 512, have_width = 0;
        int height = 512, have_height = 0;
        FILE *fp;
        int err=0;
        int i;
        int niter = 1;
        int nonzero_fill = 1;

        for (i=1; i<argc; i++) {
                int usage = 0;
                char *arg;
                if (0==strcmp("--fill-rule=even-odd", argv[i])) {
                        nonzero_fill = 0;
                }
                else if (0==strcmp("--fill-rule=winding", argv[i])) {
                        nonzero_fill = 1;
                }
                else if (0==strcmp("--help", argv[i])) {
                        usage = 1;
                }
                else if ((arg = prefix(argv[i], "--niter="))) {
                        niter = atoi(arg);
                }
                else if (!filename) {
                        filename = argv[i];
                }
                else if (!have_width) {
                        have_width = 1;
                        width = atoi(argv[i]);
                        assert(width>0);
                }
                else if (!have_height) {
                        have_height = 1;
                        height = atoi(argv[i]);
                        assert(height>0);
                }
                else {
                        usage = 1;
                }
                if (usage) {
                        fprintf(stderr,
                                "usage: "
                                "[--fill-rule=even-odd|winding] "
                                "[filename|-] [width] [height]\n");
                        exit(1);
                }
        }
        filename = filename ? filename : "-";

        fp = strcmp("-", filename) ? fopen(filename, "rb") : stdin;
        if (NULL == fp) {
                fprintf(stderr, "can't open file '%s': %s\n",
                        filename,
                        strerror(errno));
                exit(1);
        }


        cx_init(cx, width, height, nonzero_fill);
        for (i=1; !err && i<=niter; i++) {
                cx_reset_clip(cx, 0, 0, width, height);

                err = cx_interpret_stream(cx, fp);
                if (err) break;

                if (niter > 1) {
                        rewind(fp);
                }
        }

        if (!err) {
                image_save_as_pgm_to_stream(cx->image, stdout);
        }

        cx_fini(cx);

        if (fp != stdin)
                fclose(fp);
        return err ? 1 : 0;
}
