#define BIN_SH /*
CFLAGS="-O3 -funroll-all-loops"
#CFLAGS="-O0"
gcc $CFLAGS -g -Wall -W -o `basename $0 .c` $0
exit $?
*/
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <time.h>

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

typedef enum {
        CMD_MOVETO,
        CMD_LINETO,
        CMD_CLOSEPATH,
        CMD_NONZERO_FILL_RULE,
        CMD_EVENODD_FILL_RULE,
        CMD_FILL,
        CMD_RESET_CLIP,
        CMD_RESIZE
} cmd_opcode_t;

union mem {
        cmd_opcode_t op;
        int w;
        int h;
        int xmin;
        int ymin;
        int xmax;
        int ymax;
        double x;
        double y;
        double double_;
        int int_;
};

struct program {
        union mem *mem;
        size_t size;
        size_t cap;
};


static void
image_clear(struct image *im)
{
        unsigned y;
        unsigned h = im->height;
        unsigned w = im->width;
        size_t stride = im->stride;
        unsigned char *p = im->pixels;
        for (y=0; y<h; y++) {
                memset(p, 0, w);
                p += stride;
        }
}

static void
image_fini(struct image *im)
{
        free(im->pixels);
        memset(im, 0, sizeof(struct image));
}

static void
image_init(
        struct image *im,
        unsigned width, unsigned height)
{
        im->pixels = malloc(width*height);
        assert(im->pixels);
        im->stride = width;
        im->width = width;
        im->height = height;
        image_clear(im);
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

static void
program_init(struct program *p)
{
        p->size = 0;
        p->cap = 0;
        p->mem = NULL;
}

static void
program_fini(struct program *p)
{
        if (!p) return;
        free(p->mem);
        program_init(p);
}

static void
program_emit(struct program *p, union mem c)
{
        if (p->size == p->cap) {
                p->cap = 2*p->cap + 1;
                p->mem = realloc(p->mem, p->cap*sizeof(union mem));
                assert(p->mem);
        }
        p->mem[p->size++] = c;
}

static void
program_emit_op(struct program *p, cmd_opcode_t op)
{
        union mem c;
        c.op = op;
        program_emit(p, c);
}

static void
program_emit_int(struct program *p, int i)
{
        union mem c;
        c.int_ = i;
        program_emit(p, c);
}

static void
program_emit_double(struct program *p, int x)
{
        union mem c;
        c.double_ = x;
        program_emit(p, c);
}

static void
program_emit_point(struct program *p, double x, double y)
{
        program_emit_double(p, x);
        program_emit_double(p, y);
}

static void
program_emit_moveto(struct program *p, double x, double y)
{
        program_emit_op(p, CMD_MOVETO);
        program_emit_point(p, x, y);
}

static void
program_emit_lineto(struct program *p, double x, double y)
{
        program_emit_op(p, CMD_LINETO);
        program_emit_point(p, x, y);
}

static void
program_emit_closepath(struct program *p)
{
        program_emit_op(p, CMD_CLOSEPATH);
}


static void
program_emit_nonzero_fill_rule(struct program *p)
{
        program_emit_op(p, CMD_NONZERO_FILL_RULE);
}

static void
program_emit_evenodd_fill_rule(struct program *p)
{
        program_emit_op(p, CMD_EVENODD_FILL_RULE);
}

static void
program_emit_fill(struct program *p)
{
        program_emit_op(p, CMD_FILL);
}

static void
program_emit_reset_clip(struct program *p, int xmin, int ymin, int xmax, int ymax)
{
        program_emit_op(p, CMD_RESET_CLIP);
        program_emit_int(p, xmin);
        program_emit_int(p, ymin);
        program_emit_int(p, xmax);
        program_emit_int(p, ymax);
}

static void
program_emit_resize(struct program *p, int w, int h)
{
        program_emit_op(p, CMD_RESIZE);
        program_emit_int(p, w);
        program_emit_int(p, h);
}

static int
program_parse_stream(struct program *pgm, FILE *fp)
{
        struct point cp;
        cp.x = 0;
        cp.y = 0;
        cp.valid = 0;

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
                if (1 == fscanf(fp, " %c", &cmd))
                        c = (unsigned char)cmd;
                switch (c) {
                case 'M':       /* move */
                        get_double_arg(x);
                        get_double_arg(y);
                        cp.x = *x; cp.y = *y;
                        cp.valid = 1;
                        program_emit_moveto(pgm, cp.x, cp.y);
                        break;
                case 'm':       /* move, relative */
                        get_double_arg(x);
                        get_double_arg(y);
                        if (cp.valid) {
                                cp.x += *x; cp.y += *y;
                                program_emit_moveto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'L':       /* line */
                        get_double_arg(x);
                        get_double_arg(y);
                        cp.x = *x; cp.y = *y;
                        cp.valid = 1;
                        program_emit_lineto(pgm, cp.x, cp.y);
                        break;
                case 'l':       /* line, relative */
                        get_double_arg(x);
                        get_double_arg(y);
                        if (cp.valid) {
                                cp.x += *x; cp.y += *y;
                                program_emit_lineto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'H':       /* horizontal line */
                        get_double_arg(x);
                        if (cp.valid) {
                                cp.x = *x;
                                program_emit_lineto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'h':       /* horizontal line, relative */
                        get_double_arg(x);
                        if (cp.valid) {
                                cp.x += *x;
                                program_emit_lineto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'V':       /* vertical line */
                        get_double_arg(y);
                        if (cp.valid) {
                                cp.y = *y;
                                program_emit_lineto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'v':       /* vertical line, relative*/
                        get_double_arg(y);
                        if (cp.valid) {
                                cp.y += *y;
                                program_emit_lineto(pgm, cp.x, cp.y);
                        }
                        break;
                case 'z':
                case 'Z':       /* close path */
                        program_emit_closepath(pgm);
                        cp.valid = 0;
                        break;
                case 'N':       /* non-zero winding number fill rule */
                        program_emit_nonzero_fill_rule(pgm);
                        break;
                case 'E':       /* even-odd fill rule */
                        program_emit_evenodd_fill_rule(pgm);
                        break;
                case 'F':       /* fill */
                        program_emit_fill(pgm);
                        cp.valid = 0;
                        break;
                case '#':       /* eol comment */
                        while ((c = fgetc(fp)) && EOF != c && '\n' != c) {}
                        break;
                case 'B':       /* B: xmin ymin xmax ymax; set clip */
                        get_double_arg(x+0);
                        get_double_arg(y+0);
                        get_double_arg(x+1);
                        get_double_arg(y+1);
                        program_emit_reset_clip(pgm, x[0], y[0], x[1], y[1]);
                        cp.valid = 0;
                        break;
                case 'I':       /* reinitialise context; width height */
                        get_double_arg(x);
                        get_double_arg(y);
                        program_emit_resize(pgm, *x, *y);
                        program_emit_reset_clip(pgm, 0, 0, *x, *y);
                        cp.valid = 0;
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
        program_emit_fill(pgm);
        return 0;
#undef get_double_arg
}

static void
program_interpret(
        struct program *pgm,
        struct context *cx)
{
        size_t pc = 0;
        size_t size = pgm->size;
        union mem *mem = pgm->mem;

        while (pc < size) {
                switch (mem[pc].op) {
                case CMD_LINETO:
                        cx_lineto(cx, mem[pc+1].x, mem[pc+2].y);
                        pc += 3;
                        break;
                case CMD_MOVETO:
                        cx_moveto(cx, mem[pc+1].x, mem[pc+2].y);
                        pc += 3;
                        break;
                case CMD_CLOSEPATH:
                        cx_closepath(cx);
                        pc += 1;
                        break;
                case CMD_FILL:
                        cx_fill(cx);
                        pc += 1;
                        break;
                case CMD_NONZERO_FILL_RULE:
                        cx->nonzero_fill = 1;
                        pc += 1;
                        break;
                case CMD_EVENODD_FILL_RULE:
                        cx->nonzero_fill = 0;
                        pc += 1;
                        break;
                case CMD_RESET_CLIP:
                        cx_reset_clip(cx,
                                      mem[pc+1].xmin,
                                      mem[pc+2].ymin,
                                      mem[pc+3].xmax,
                                      mem[pc+4].ymax);
                        pc += 5;
                        break;
                case CMD_RESIZE:
                        cx_resize(cx, mem[pc+1].w, mem[pc+2].h);
                        pc += 3;
                        break;
                default:
                        assert(0 && "illegal opcode");
                }
        }
        assert(pc == size);
}

static double
get_current_ms()
{
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return tv.tv_sec*1000.0 + tv.tv_usec/1000.0;
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
        struct program pgm[1];
        char const *filename = NULL;
        int width = 512, have_width = 0;
        int height = 512, have_height = 0;
        FILE *fp;
        int err=0;
        int i;
        int niter = 1;
        int nonzero_fill = 1;
        int no_pgm = 0;
        int do_timer = 0;
        int do_clear = 0;

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
                else if (0==strcmp("--timer", argv[i])) {
                        do_timer = 1;
                }
                else if ((arg = prefix(argv[i], "--niter="))) {
                        niter = atoi(arg);
                }
                else if (0==strcmp("--no-pgm", argv[i])) {
                        no_pgm = 1;
                }
                else if (0==strcmp("--clear", argv[i])) {
                        do_clear = 1;
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
                                "[--niter=<n>] "
                                "[--timer] "
                                "[--no-pgm] "
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
        cx_reset_clip(cx, 0, 0, width, height);
        program_init(pgm);

        err = program_parse_stream(pgm, fp);
        if (!err) {
                double ms = get_current_ms();
                for (i=1; i<=niter; i++) {
                        if (do_clear) image_clear(cx->image);
                        cx_reset_clip(cx, 0,0, width, height);
                        program_interpret(pgm, cx);
                }
                ms = get_current_ms() - ms;
                if (do_timer) {
                        fprintf(stderr,
                                "%d iterations took %f ms at %f ms / iter and %f iter / sec\n",
                                niter, ms, ms / (niter*1.0),
                                niter / ms * 1000.0);
                }
        }
        if (!err && !no_pgm) {
                image_save_as_pgm_to_stream(cx->image, stdout);
        }

        program_fini(pgm);
        cx_fini(cx);

        if (fp != stdin)
                fclose(fp);
        return err ? 1 : 0;
}
