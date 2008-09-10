#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <sys/time.h>
#include <time.h>

static double
get_current_ms()
{
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return tv.tv_sec*1000.0 + tv.tv_usec/1000.0;
}

static void
save_data_as_pgm_to_stream(
        unsigned char *pixels,
        size_t stride,
        unsigned width, unsigned height,
        FILE *fp)
{
        unsigned x, y;
        fprintf(fp, "P2\n%u %u\n255\n",
                width, height);
        for (y=0; y<height; y++) {
                for (x=0; x<width; x++) {
                        fprintf(fp, "%d ", pixels[x + y*stride]);
                }
                fprintf(fp, "\n");
        }
}

/*
 * Generic rendering context
 *
 *  The backend under test defines these.
 */

struct context;

struct context *cx_create();
void cx_resize(struct context *cx, unsigned width, unsigned height);
void cx_clear(struct context *cx);
void cx_destroy(struct context *cx);
void cx_reset_clip(struct context *cx, int xmin, int ymin, int xmax, int ymax);
void cx_moveto(struct context *cx, double x, double y);
void cx_lineto(struct context *cx, double x, double y);
void cx_closepath(struct context *cx);
void cx_fill(struct context *cx);
void cx_set_fill_rule(struct context *cx, int nonzero_fill);
void cx_get_pixels(
        struct context *cx,
        unsigned char **OUT_pixels,
        size_t *OUT_stride,
        unsigned *OUT_width,
        unsigned *OUT_height);

/*
 * Path program
 */
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
program_emit_double(struct program *p, double x)
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
program_emit_flattened_curveto(
        struct program *p,
        double x1, double y1,   /* current point */
        double x2, double y2,
        double x3, double y3,
        double x4, double y4)
{
        /* do something clever later. */
        int i;
        double px = x1;
        double py = y1;
        int n = 10;
        for (i=0; i<n; i++) {
                double t = 1.0 * i / n;
                double c1 = (1-t)*(1-t)*(1-t);
                double c2 = 3*(1-t)*(1-t)*t;
                double c3 = 3*(1-t)*t*t;
                double c4 = t*t*t;
                double x = x1*c1 + x2*c2 + x3*c3 + x4*c4;
                double y = y1*c1 + y2*c2 + y3*c3 + y4*c4;
                if (hypot(x-px, y-py) > 0.1) {
                        program_emit_lineto(p, x, y);
                        px = x;
                        px = y;
                }
        }
        program_emit_lineto(p, x4, y4);
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
        struct {
                double x, y;
                int valid;
        } cp;
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
                double x[4], y[4];
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
                case 'C':       /* bx by cx cy dx dy */
                        get_double_arg(x+0);
                        get_double_arg(y+0);
                        get_double_arg(x+1);
                        get_double_arg(y+1);
                        get_double_arg(x+2);
                        get_double_arg(y+2);
                        if (cp.valid) {
                                program_emit_flattened_curveto(
                                        pgm,
                                        cp.x, cp.y,
                                        x[0], y[0],
                                        x[1], y[1],
                                        x[2], y[2]);
                                cp.x = x[2];
                                cp.y = y[2];
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

struct extents {
        double xmin, ymin, xmax, ymax;
};

static void
extents_init_empty(struct extents *e)
{
        e->xmin = 1e300;
        e->ymin = 1e300;
        e->xmax = -1e300;
        e->ymax = -1e300;
}

static void
extents_init_full(struct extents *e)
{
        e->xmin = -1e300;
        e->ymin = -1e300;
        e->xmax = 1e300;
        e->ymax = 1e300;
}

static void
extents_update(struct extents *e, double x, double y)
{
        if (x < e->xmin) e->xmin = x;
        if (x > e->xmax) e->xmax = x;
        if (y < e->ymin) e->ymin = y;
        if (y > e->ymax) e->ymax = y;
}

static void
extents_clip(struct extents *e, double *x, double *y)
{
        if (*x < e->xmin) *x = e->xmin;
        if (*x > e->xmax) *x = e->xmax;
        if (*y < e->ymin) *y = e->ymin;
        if (*y > e->ymax) *y = e->ymax;
}

static struct extents
program_extents(struct program *pgm)
{
        size_t pc = 0;
        size_t size = pgm->size;
        union mem *mem = pgm->mem;
        struct extents extents[1];
        struct extents clip[1];
        double x, y;

        extents_init_empty(extents);
        extents_init_full(clip);

        while (pc < size) {
                switch (mem[pc].op) {
                case CMD_LINETO:
                case CMD_MOVETO:
                        x = mem[pc+1].x;
                        y = mem[pc+2].y;
                        extents_clip(clip, &x, &y);
                        extents_update(extents, x, y);
                        pc += 3;
                        break;
                case CMD_CLOSEPATH:
                        pc += 1;
                        break;
                case CMD_FILL:
                        pc += 1;
                        break;
                case CMD_NONZERO_FILL_RULE:
                        pc += 1;
                        break;
                case CMD_EVENODD_FILL_RULE:
                        pc += 1;
                        break;
                case CMD_RESET_CLIP:
                        clip->xmin = mem[pc+1].xmin;
                        clip->ymin = mem[pc+2].ymin;
                        clip->xmax = mem[pc+3].xmax;
                        clip->ymax = mem[pc+4].ymax;
                        pc += 5;
                        break;
                case CMD_RESIZE:
                        clip->xmin = 0;
                        clip->ymin = 0;
                        clip->xmax = mem[pc+1].w;
                        clip->ymax = mem[pc+2].h;
                        pc += 3;
                        break;
                default:
                        assert(0 && "illegal opcode");
                }
        }
        assert(pc == size);
        return *extents;
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
                        cx_set_fill_rule(cx, 1);
                        pc += 1;
                        break;
                case CMD_EVENODD_FILL_RULE:
                        cx_set_fill_rule(cx, 0);
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

static void
program_translate(
        struct program *pgm,
        double dx, double dy)
{
        size_t pc = 0;
        size_t size = pgm->size;
        union mem *mem = pgm->mem;

        while (pc < size) {
                switch (mem[pc].op) {
                case CMD_LINETO:
                case CMD_MOVETO:
                        mem[pc+1].x += dx;
                        mem[pc+2].y += dy;
                        pc += 3;
                        break;
                case CMD_CLOSEPATH:
                        pc += 1;
                        break;
                case CMD_FILL:
                        pc += 1;
                        break;
                case CMD_NONZERO_FILL_RULE:
                        pc += 1;
                        break;
                case CMD_EVENODD_FILL_RULE:
                        pc += 1;
                        break;
                case CMD_RESET_CLIP:
                        mem[pc+1].xmin += dx;
                        mem[pc+2].ymin += dy;
                        mem[pc+3].xmax += dx;
                        mem[pc+4].ymax += dy;
                        pc += 5;
                        break;
                case CMD_RESIZE:
                        dx = 0.0;
                        dy = 0.0;
                        pc += 3;
                        break;
                default:
                        assert(0 && "illegal opcode");
                }
        }
        assert(pc == size);
}

/*
 * Arg parsing and main.
 */

static char *
prefix(char const *s, char const *pref)
{
        size_t len = strlen(pref);
        if (0 == strncmp(s, pref, len)) {
                return (char *)(s + len);
        }
        return NULL;
}

struct args {
        char const *filename;
        char const *fillrulename;
        int nonzero_fill;
        int niter, timer, clear, no_pgm;
        int width, height;
};

static struct args
parse_args(int argc, char **argv)
{
        struct args args = {
                NULL,           /* <filename> */
                NULL,           /* --fillrule=<name> */
                1,              /*    nonzero_fill */
                1,              /* --niter=<num iters> */
                0,              /* --timer: do we show it? */
                0,              /* --clear (frames between iters) */
                0,              /* --no-pgm (at end of run) */
                0,              /* {width] */
                0               /* [height] */
        };
        int i;

        for (i=1; i<argc; i++) {
                int usage = 0;
                char *arg;
                if ((arg = prefix(argv[i], "--fill-rule="))) {
                        args.fillrulename = arg;
                }
                else if (0==strcmp("--help", argv[i])) {
                        usage = 1;
                }
                else if (0==strcmp("--timer", argv[i])) {
                        args.timer = 1;
                }
                else if ((arg = prefix(argv[i], "--niter="))) {
                        args.niter = atoi(arg);
                        if (args.niter <= 0) {
                                fprintf(stderr,
                                        "bad --niter %s\n", arg);
                                exit(1);
                        }
                }
                else if (0==strcmp("--no-pgm", argv[i])) {
                        args.no_pgm = 1;
                }
                else if (0==strcmp("--clear", argv[i])) {
                        args.clear = 1;
                }
                else if (!args.filename) {
                        args.filename = argv[i];
                }
                else if (args.width <= 0) {
                        args.width = atoi(argv[i]);
                        if (args.width <= 0) {
                                fprintf(stderr,
                                        "bad width %s\n", argv[i]);
                                usage = 1;
                        }
                }
                else if (args.height <= 0) {
                        args.height = atoi(argv[i]);
                        if (args.height <= 0) {
                                fprintf(stderr,
                                        "bad height %s\n", argv[i]);
                                usage = 1;
                        }
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
                                "[--clear] "
                                "[--no-pgm] "
                                "[filename|-] [width] [height]\n");
                        exit(1);
                }
        }

        if (!args.filename) {
                args.filename = "-";
        }

        if (args.fillrulename) {
                char const *name = args.fillrulename;
                if (prefix(name, "even-odd")) {
                        args.nonzero_fill = 0;
                }
                else if (prefix(name, "nonzero")) {
                        args.nonzero_fill = 1;
                }
                else {
                        fprintf(stderr, "unknown fill rule name '%s'\n",
                                name);
                        exit(1);
                }
        }

        return args;
}

int
main(int argc, char **argv)
{
        struct context *cx;
        struct program pgm[1];
        struct args args;

        FILE *fp;
        int err=0;
        struct extents extents;
        double dx = 0.0;
        double dy = 0.0;
        double ms;
        int i;

        /* Parse args */
        args = parse_args(argc, argv);

        /* Parse the path into a program. */
        fp = strcmp("-", args.filename) ? fopen(args.filename, "rb") : stdin;
        if (NULL == fp) {
                fprintf(stderr, "can't open file '%s': %s\n",
                        args.filename,
                        strerror(errno));
                exit(1);
        }

        program_init(pgm);
        if (args.nonzero_fill)
                program_emit_nonzero_fill_rule(pgm);
        else
                program_emit_evenodd_fill_rule(pgm);

        err = program_parse_stream(pgm, fp);
        if (err) {
                fprintf(stderr, "parse error\n");
                exit(1);
        }

        /* Crop the context if we don't have an explicit width,
         * height. */
        extents = program_extents(pgm);
        if (args.width <= 0) {
                args.width = 1;
                if (extents.xmin <= extents.xmax) {
                        args.width = ceil(extents.xmax - extents.xmin);
                        dx = -extents.xmin;
                }
        }
        if (args.height <= 0) {
                args.height = 1;
                if (extents.ymin <= extents.ymax) {
                        args.height = ceil(extents.ymax - extents.ymin);
                        dy = -extents.ymin;
                }
        }
        program_translate(pgm, dx, dy);

        /* Loop rendering! */
        cx = cx_create();
        cx_resize(cx, args.width, args.height);

        ms = get_current_ms();
        for (i=1; i<=args.niter; i++) {
                if (args.clear) cx_clear(cx);
                cx_reset_clip(cx, 0, 0, args.width, args.height);
                program_interpret(pgm, cx);
        }

        /* Dump output and clean up. */
        ms = get_current_ms() - ms;
        if (args.timer) {
                fprintf(stderr,
                        "%d iterations took %f ms at %f ms / iter and %f iter / sec\n",
                        args.niter, ms, ms / (args.niter*1.0),
                        args.niter / ms * 1000.0);
        }

        if (!args.no_pgm) {
                unsigned char *pixels;
                size_t stride;
                unsigned width, height;
                cx_get_pixels(cx, &pixels, &stride, &width, &height);
                save_data_as_pgm_to_stream(
                        pixels, stride, width, height, stdout);
        }

        program_fini(pgm);
        cx_destroy(cx);

        if (fp != stdin)
                fclose(fp);
        return err ? 1 : 0;
}
