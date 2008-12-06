/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* glitter-paths - polygon scan converter
 *
 * Copyright (c) 2008  M Joonas Pihlaja
 * Copyright (c) 2007  David Turner
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef GLITTER_PATHS_H
#define GLITTER_PATHS_H

/* "Input scaled" numbers are fixed precision reals with multiplier
 * 2**GLITTER_INPUT_BITS.  Input coordinates are given to glitter as
 * pixel scaled numbers.  These get converted to the internal grid
 * scaled numbers as soon as possible. Internal overflow is possible
 * if GRID_X/Y inside glitter-paths.c is larger than
 * 1<<GLITTER_INPUT_BITS. */
#ifndef GLITTER_INPUT_BITS
#  define GLITTER_INPUT_BITS 8
#endif
#define GLITTER_INPUT_SCALE (1<<GLITTER_INPUT_BITS)
typedef int glitter_input_scaled_t;

#if !GLITTER_HAVE_STATUS_T
typedef enum {
    GLITTER_STATUS_SUCCESS = 0,
    GLITTER_STATUS_NO_MEMORY
} glitter_status_t;
#endif

#ifndef I
# define I /*static*/
#endif

/* Opaque type for scan converting. */
typedef struct glitter_scan_converter glitter_scan_converter_t;

/* Make a new scan converter.  Return NULL on malloc failure. */
I glitter_scan_converter_t *
glitter_scan_converter_create(void);

/* Destroy a scan converter. */
I void
glitter_scan_converter_destroy(
    glitter_scan_converter_t *converter);

/* Reset a scan converter to accept polygon edges and set the clip box
 * in pixels.  Allocates O(ymax-ymin) bytes of memory.	The clip box
 * is set to integer pixel coordinates xmin <= x < xmax, ymin <= y <
 * ymax. */
I glitter_status_t
glitter_scan_converter_reset(
    glitter_scan_converter_t *converter,
    int xmin, int ymin,
    int xmax, int ymax);

/* Add a new polygon edge from pixel (x1,y1) to (x2,y2) to the scan
 * converter.  The coordinates represent pixel positions scaled by
 * 2**GLITTER_PIXEL_BITS.  If this function fails then the scan
 * converter should be reset or destroyed.  Dir must be +1 or -1,
 * with the latter reversing the orientation of the edge. */
I glitter_status_t
glitter_scan_converter_add_edge(
    glitter_scan_converter_t *converter,
    glitter_input_scaled_t x1, glitter_input_scaled_t y1,
    glitter_input_scaled_t x2, glitter_input_scaled_t y2,
    int dir);

/* Render the polygon in the scan converter to the given A8 format
 * image raster.  Only the pixels accessible as pixels[y*stride+x] for
 * x,y inside the clip box are written to, where xmin <= x < xmax,
 * ymin <= y < ymax.  The image is assumed to be clear on input.
 *
 * If nonzero_fill is true then the interior of the polygon is
 * computed with the non-zero fill rule.  Otherwise the even-odd fill
 * rule is used.
 *
 * The scan converter must be reset or destroyed after this call. */
#ifndef GLITTER_BLIT_COVERAGES_ARGS
# define GLITTER_BLIT_COVERAGES_ARGS unsigned char *raster_pixels, long raster_stride
#endif
I glitter_status_t
glitter_scan_converter_render(
    glitter_scan_converter_t *converter,
    int nonzero_fill,
    GLITTER_BLIT_COVERAGES_ARGS);

#endif /* GLITTER_PATHS_H */
