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
/* Glitter-paths is a stand alone polygon rasteriser derived from
 * David Turner's reimplementation of Tor Anderssons's 15x17
 * supersampling rasteriser from the Apparition graphics library.  The
 * main new feature here is cheaply choosing per-scan line between
 * doing fully analytical coverage computation for an entire row at a
 * time vs. using the supersampling approach.
 *
 * David Turner's code can be found at
 *
 *   http://david.freetype.org/rasterizer-shootout/raster-comparison-20070813.tar.bz2
 *
 * In particular this file incorporates large parts of ftgrays_tor10.h
 * from raster-comparison-20070813.tar.bz2
 */

#include "glitter-paths.h"

/*-------------------------------------------------------------------------
 * glitter-paths.c: Implementation internal types
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* All polygon coordinates are snapped onto a subsample grid. "Grid
 * scaled" numbers are fixed precision reals with multiplier GRID_X or
 * GRID_Y. */
typedef int grid_scaled_t;
typedef int grid_scaled_x_t;
typedef int grid_scaled_y_t;

/* Default x/y scale factors.
 *  You can either define GRID_X/Y_BITS to get a power-of-two scale
 *  or define GRID_X/Y separately. */
#if !defined(GRID_X) && !defined(GRID_X_BITS)
#  define GRID_X_BITS 8
#endif
#if !defined(GRID_Y) && !defined(GRID_Y_BITS)
#  define GRID_Y 15
#endif

/* Use GRID_X/Y_BITS to define GRID_X/Y if they're availale. */
#ifdef GRID_X_BITS
#  define GRID_X (1 << GRID_X_BITS)
#endif
#ifdef GRID_Y_BITS
#  define GRID_Y (1 << GRID_Y_BITS)
#endif

/* The SPLIT_X macro splits a grid scaled coordinate into integer
 * and fractional parts. The integer part should be floored. */
#if defined(SPLIT_X)
  /* do nothing */
#elif defined(GRID_X_BITS)
#  define SPLIT_X(t, i, f)  SPLIT_bits(t, i, f, GRID_X_BITS)
#else
#  define SPLIT_X(t, i, f) SPLIT_general(t, i, f, GRID_X)
#endif

#define SPLIT_general(t, i, f, m) do {	\
    (i) = (t) / (m);			\
    (f) = (t) % (m);			\
    if ((f) < 0) {			\
	--(i);				\
	(f) += (m);			\
    }					\
} while (0)

#define SPLIT_bits(t, i, f, b) do {	\
    (f) = (t) & ((1 << (b)) - 1);	\
    (i) = (t) >> (b);			\
} while (0)

/* A grid area is a real in [0,1] scaled by 2*GRID_X*GRID_Y.  We want
 * to be able to represent exactly areas of subpixel trapezoids whose
 * vertices are given in grid scaled coordinates.  The scale factor
 * comes from needing to accurately represent the area 0.5*dx*dy of a
 * triangle with base dx and height dy in grid scaled numbers. */
typedef int grid_area_t;
#define GRID_XY (2*GRID_X*GRID_Y) /* Unit area on the grid. */

/* GRID_AREA_TO_ALPHA(area): map [0,GRID_XY] to [0,255]. */
#if GRID_XY == 510
#  define GRID_AREA_TO_ALPHA(c)	  (((c)+1) >> 1)
#elif GRID_XY == 255
#  define  GRID_AREA_TO_ALPHA(c)  (c)
#elif GRID_XY == 64
#  define  GRID_AREA_TO_ALPHA(c)  (((c) << 2) | -(((c) & 0x40) >> 6))
#elif GRID_XY == 128
#  define  GRID_AREA_TO_ALPHA(c)  ((((c) << 1) | -((c) >> 7)) & 255)
#elif GRID_XY == 256
#  define  GRID_AREA_TO_ALPHA(c)  (((c) | -((c) >> 8)) & 255)
#elif GRID_XY == 15
#  define  GRID_AREA_TO_ALPHA(c)  (((c) << 4) + (c))
#elif GRID_XY == 2*256*15
#  define  GRID_AREA_TO_ALPHA(c)  (((c) + ((c)<<4)) >> 9)
#else
#  define  GRID_AREA_TO_ALPHA(c)  ((c)*255 / GRID_XY) /* tweak me for rounding */
#endif

#define UNROLL3(x) x x x

/* A quotient and remainder of a division.  Mostly used to represent
 * rational x-coordinates along an edge. */
struct quorem {
    int quo;
    int rem;
};

/* Header for a chunk of memory in a memory pool. */
struct _pool_chunk {
    /* # bytes used in this chunk. */
    size_t size;

    /* # bytes total in this chunk */
    size_t capacity;

    /* Pointer to the previous used chunk, or NULL if this is the last
     * sentinel chunk.	*/
    struct _pool_chunk *prev_chunk; /* NULL if a sentinels. */

    /* Actual data starts here.	 Well aligned for pointers. */
    unsigned char data[0];
};

/* A memory pool.  This is supposed to be embedded on the stack or
 * within some other structure.	 It may optionally be followed by an
 * embedded array from which requests are fulfilled until
 * malloc needs to be called to allocate a first real chunk. */
struct pool {
    /* Chunk we're allocating from. */
    struct _pool_chunk *current;

    /* Free list of previously allocated chunks.  All have >= default
     * capacity. */
    struct _pool_chunk *first_free;

    /* The default capacity of a chunk. */
    size_t default_capacity;

    /* Header for the sentinel chunk.  Directly following the pool
     * struct should be some space for embedded elements from which
     * the sentinel chunk allocates from. */
    struct _pool_chunk sentinel[1];
};

/* A polygon edge. */
struct edge {
    /* Next in y-bucket or active list. */
    struct edge *next;

    /* Current x coordinate. Initialised to the x coordinate of the
     * top of the edge. The quotient is in grid_scaled_x_t units and
     * the remainder is mod dy in grid_scaled_y_t units.*/
    struct quorem x;

    /* Advance of the current x when moving down a subsample line. */
    struct quorem dxdy;

    /* Advance of the current x when moving down a full pixel
     * row. Only initialised when the height of the edge is large
     * enough that there's a chance the edge could be stepped by a
     * full row's worth of subsample rows at a time. */
    struct quorem dxdy_full;

    /* The clipped y of the top of the edge. */
    grid_scaled_y_t ytop;

    /* y2-y1 after orienting the edge downwards.  */
    grid_scaled_y_t dy;

    /* Number of subsample rows remaining to scan convert of this
     * edge. */
    grid_scaled_y_t h;

    /* Original sign of the edge: +1 for downwards (towards increasing
     * y), -1 for upwards edges.  */
    int dir;
};

/* Number of subsample rows per y-bucket. Should be >= 1 and <=
 * GRID_Y. */
#define EDGE_Y_BUCKET_HEIGHT GRID_Y

#define EDGE_Y_BUCKET_INDEX(y, ymin) (((y) - (ymin))/EDGE_Y_BUCKET_HEIGHT)

/* A collection of sorted and vertically clipped edges of the polygon.
 * Edges are moved from the polygon to an active list while scan
 * converting. */
struct polygon {
    /* The vertical clip extents. */
    grid_scaled_y_t ymin, ymax;

    /* Array of edges all starting in the same bucket.	An edge is put
     * into bucket EDGE_BUCKET_INDEX(edge->ytop, polygon->ymin) when
     * it is added to the polygon. */
    struct edge **y_buckets;

    struct {
	struct pool base[1];
	struct edge embedded[32];
    } edge_pool;
};

/* A cell represents an edge pixel of a polygon on the current scan
 * line.
 *
 * For a single edge the cell->area is the area within the pixel to
 * the left of the edge, and the cell->cover is the area within the
 * pixel to the left and right of the edge.  e.g. in the figure below
 * denoting an edge starting within the pixel, the cell->area is the
 * area of the # covered part and the cell->cover is sum of the area
 * of the # and % covered parts.
 *
 * The cell->areas and cell->covers are actually signed areas, with
 * the sign of the contribution of an edge depending on the which side
 * of the edge is inside the polygon (inside is = +1).
 *
 *  +-----------+
 *  |		|
 *  |		|
 *  |####\%%%%%%|
 *  |#####\%%%%%|
 *  |######\%%%%|
 *  +-----------+
 */
struct cell {
    struct cell *next;
    int x;
    grid_area_t area;
    grid_area_t cover;
};

/* A cell list represents the current scan line mid-conversion as a
 * list of cells ordered by ascending x.  The cell list is geared
 * towards iterating through the cells in the list in order. */
struct cell_list {
    /* Points to the left-most cell in the scan line. */
    struct cell *head;

    /* Cursor state for iterating through the cell list.  Points to
     * a pointer to the current cell (either &cell_list->head or the next
     * field of the previous cell. */
    struct cell **tailpred;

    struct {
	struct pool base[1];
	struct cell embedded[32];
    } cell_pool;
};

struct cell_pair {
    struct cell *cell1;
    struct cell *cell2;
};

/* The active list contains edges in the current (sub)scan line
 * ordered by the x-coordinate of the intercept of the edge and the
 * scan line. */
struct active_list {
    /* Leftmost edge on the current scan line. */
    struct edge *head;

    /* A lower bound on the height of the active edges is used to
     * estimate how soon some active edge ends.	 We can't advance the
     * scan conversion by a full pixel row if an edge ends somewhere
     * in it. */
    grid_scaled_y_t min_h;
};

struct glitter_scan_converter {
    struct polygon	polygon[1];
    struct active_list	active[1];
    struct cell_list	coverages[1];

    /* Clip box. */
    grid_scaled_x_t xmin, xmax;
    grid_scaled_y_t ymin, ymax;
};

/* Compute the floored division a/b. Assumes / and % perform symmetric
 * division. */
inline static struct quorem
floored_divrem(int a, int b)
{
    struct quorem qr;
    qr.quo = a/b;
    qr.rem = a%b;
    if ((a^b)<0 && qr.rem) {
	qr.quo -= 1;
	qr.rem += b;
    }
    return qr;
}

/* Compute the floored division (x*a)/b. Assumes / and % perform symmetric
 * division. */
inline static struct quorem
floored_muldivrem(int x, int a, int b)
{
    struct quorem qr;
    long long xa = (long long)x*a;
    qr.quo = xa/b;
    qr.rem = xa%b;
    if ((xa>=0) != (b>=0) && qr.rem) {
	qr.quo -= 1;
	qr.rem += b;
    }
    return qr;
}

static void
_pool_chunk_init(
    struct _pool_chunk *p,
    struct _pool_chunk *prev_chunk,
    size_t capacity)
{
    p->prev_chunk = prev_chunk;
    p->size = 0;
    p->capacity = capacity;
}

static struct _pool_chunk *
_pool_chunk_create(
    struct _pool_chunk *prev_chunk,
    size_t size)
{
    struct _pool_chunk *p;
    size_t size_with_head = size + sizeof(struct _pool_chunk);
    if (size_with_head < size)
	return NULL;
    p = malloc(size_with_head);
    if (p)
	_pool_chunk_init(p, prev_chunk, size);
    return p;
}

/* Initialise a new pool.  The pool will malloc chunks from the system
 * in chunks of at least default_capacity bytes after fulfilling the
 * first embedded_capacity bytes requested from a byte array following
 * the pool struct. */
static void
pool_init(
    struct pool *pool,
    size_t default_capacity,
    size_t embedded_capacity)
{
    pool->current = pool->sentinel;
    pool->first_free = NULL;
    pool->default_capacity = default_capacity;
    _pool_chunk_init(pool->sentinel, NULL, embedded_capacity);
}

/* Deallocate all pooled memory returning it to the OS and empty the
 * pool. */
static void
pool_fini(struct pool *pool)
{
    struct _pool_chunk *p = pool->current;
    do {
	while (NULL != p) {
	    struct _pool_chunk *prev = p->prev_chunk;
	    if (p != pool->sentinel)
		free(p);
	    p = prev;
	}
	p = pool->first_free;
	pool->first_free = NULL;
    } while (NULL != p);
    pool_init(pool, 0, 0);
}

/* Satisfy an allocation by first allocating a new large enough chunk
 * and adding it to the head of the pool's chunk list.	This is called
 * as a fallback if pool_alloc() ran out of memory from the head chunk
 * in the pool. */
static void *
_pool_alloc_from_new_chunk(
    struct pool *pool,
    size_t size)
{
    struct _pool_chunk *chunk;
    void *obj;
    size_t capacity;

    /* If the allocation is smaller than the default chunk size then
     * try getting a chunk off the free list.  Force alloc of a new
     * chunk for large requests. */
    capacity = size;
    chunk = NULL;
    if (size < pool->default_capacity) {
	capacity = pool->default_capacity;
	chunk = pool->first_free;
	if (chunk) {
	    pool->first_free = chunk->prev_chunk;
	    _pool_chunk_init(chunk, pool->current, chunk->capacity);
	}
    }

    if (NULL == chunk) {
	chunk = _pool_chunk_create(
	    pool->current,
	    capacity);
	if (NULL == chunk)
	    return NULL;
    }
    pool->current = chunk;

    obj = &chunk->data[chunk->size];
    chunk->size += size;
    return obj;
}

/* Allocate size bytes from the pool.  The first allocated address
 * returned from a pool is aligned to sizeof(void*).  Subsequent
 * addresses will maintain alignment as long as multiples of void* are
 * allocated.  Returns the address of a new memory area or NULL on
 * allocation failures.	 The pool retains ownership of the returned
 * memory. */
inline static void *
pool_alloc(
    struct pool *pool,
    size_t size)
{
    struct _pool_chunk *chunk = pool->current;

    /* Try the current chunk. */
    if (size <= chunk->capacity - chunk->size) {
	void *obj = &chunk->data[chunk->size];
	chunk->size += size;
	return obj;
    }
    else {
	return _pool_alloc_from_new_chunk(pool, size);
    }
}
/* Relinquish all pool_alloced memory back to the pool. */
static void
pool_reset(struct pool *pool)
{
    /* Transfer all used chunks to the chunk free list.	 Note how
     * this leaves the most recently used chunks at the top of the
     * free list. */
    struct _pool_chunk *chunk = pool->current;
    if (chunk != pool->sentinel) {
	while (chunk->prev_chunk != pool->sentinel) {
	    chunk = chunk->prev_chunk;
	}
	chunk->prev_chunk = pool->first_free;
	pool->first_free = pool->current;
    }
    /* Reset the sentinel as the current chunk. */
    pool->current = pool->sentinel;
    pool->sentinel->size = 0;
}

/* Rewinds the cell list's cursor to the beginning. */
inline static void
cell_list_rewind(struct cell_list *cells)
{
    cells->tailpred = &cells->head;
}

/* Rewind the cell list's cursor if x is less than the x of the
 * current cell. */
inline static void
cell_list_maybe_rewind(struct cell_list *cells, int x)
{
    struct cell *tail = *cells->tailpred;
    if (tail && tail->x > x) {
	cell_list_rewind(cells);
    }
}

/* Initialise a new cell list. */
static void
cell_list_init(struct cell_list *cells)
{
    pool_init(cells->cell_pool.base,
	      256*sizeof(struct cell),
	      sizeof(cells->cell_pool.embedded));
    cells->head = NULL;
    cell_list_rewind(cells);
}

/* Deallocate a cell list. */
static void
cell_list_fini(struct cell_list *cells)
{
    pool_fini(cells->cell_pool.base);
    cell_list_init(cells);
}

/* Relinquish all pointers to cells and make the cell list empty. */
inline static void
cell_list_reset(struct cell_list *cells)
{
    cell_list_rewind(cells);
    cells->head = NULL;
    pool_reset(cells->cell_pool.base);
}

/* Find a cell at the given x-coordinate.  Returns NULL if a new cell
 * needed to be allocated but couldn't be.  Cells must be found with
 * non-decreasing x-coordinate until the cell list is rewound using
 * cell_list_rewind(). Ownership of the returned cell is retained by
 * the cell list. */
inline static struct cell *
cell_list_find(struct cell_list *cells, int x)
{
    struct cell **ppred = cells->tailpred;
    struct cell *tail;

    while (1) {
	    tail = *ppred;
	    if (NULL == tail || tail->x >= x) {
		break;
	    }
	    ppred = &tail->next;
	    tail = *ppred;
	    if (NULL == tail || tail->x >= x) {
		break;
	    }
	    ppred = &tail->next;
	    tail = *ppred;
	    if (NULL == tail || tail->x >= x) {
		break;
	    }
	    ppred = &tail->next;
    }
    cells->tailpred = ppred;
    if (tail && tail->x == x) {
	return tail;
    } else {
	struct cell *cell = pool_alloc(
	    cells->cell_pool.base,
	    sizeof(struct cell));
	if (NULL == cell)
	    return NULL;
	*ppred = cell;
	cell->next = tail;
	cell->x = x;
	cell->area = 0;
	cell->cover = 0;
	return cell;
    }
}

/* Find two cells at x1 and x2.	 This is exactly equivalent
 * to
 *
 *   pair.cell1 = cell_list_find(cells, x1);
 *   pair.cell2 = cell_list_find(cells, x2);
 *
 * except with less function call overhead.  In particular,
 * x1 must be less than or equal to x2. */
inline static struct cell_pair
cell_list_find2(struct cell_list *cells, int x1, int x2)
{
    struct cell_pair pair;
    struct cell **ppred = cells->tailpred;
    struct cell *cell1;
    struct cell *cell2;
    struct cell *newcell;

    /* Find first cell at x1. */
    while (1) {
	cell1 = *ppred;
	if (NULL == cell1 || cell1->x > x1)
	    break;
	if (cell1->x == x1)
	    goto found_first;
	ppred = &cell1->next;

	cell1 = *ppred;
	if (NULL == cell1 || cell1->x > x1)
	    break;
	if (cell1->x == x1)
	    goto found_first;
	ppred = &cell1->next;

	cell1 = *ppred;
	if (NULL == cell1 || cell1->x > x1)
	    break;
	if (cell1->x == x1)
	    goto found_first;
	ppred = &cell1->next;

	cell1 = *ppred;
	if (NULL == cell1 || cell1->x > x1)
	    break;
	if (cell1->x == x1)
	    goto found_first;
	ppred = &cell1->next;
    }

    /* New first cell at x1. */
    newcell = pool_alloc(
	cells->cell_pool.base,
	sizeof(struct cell));
    if (NULL != newcell) {
	*ppred = newcell;
	newcell->next = cell1;
	newcell->x = x1;
	newcell->area = 0;
	newcell->cover = 0;
    }
    cell1 = newcell;
 found_first:

    /* Find second cell at x2. */
    while (1) {
	cell2 = *ppred;
	if (NULL == cell2 || cell2->x > x2)
	    break;
	if (cell2->x == x2)
	    goto found_second;
	ppred = &cell2->next;

	cell2 = *ppred;
	if (NULL == cell2 || cell2->x > x2)
	    break;
	if (cell2->x == x2)
	    goto found_second;
	ppred = &cell2->next;

	cell2 = *ppred;
	if (NULL == cell2 || cell2->x > x2)
	    break;
	if (cell2->x == x2)
	    goto found_second;
	ppred = &cell2->next;

	cell2 = *ppred;
	if (NULL == cell2 || cell2->x > x2)
	    break;
	if (cell2->x == x2)
	    goto found_second;
	ppred = &cell2->next;
    }

    /* New second cell at x2. */
    newcell = pool_alloc(
	cells->cell_pool.base,
	sizeof(struct cell));
    if (NULL != newcell) {
	*ppred = newcell;
	newcell->next = cell2;
	newcell->x = x2;
	newcell->area = 0;
	newcell->cover = 0;
    }
    cell2 = newcell;
 found_second:

    cells->tailpred = ppred;
    pair.cell1 = cell1;
    pair.cell2 = cell2;
    return pair;
}

/* Incorporate the contribution of a downwards edge sampled at x on
 * the current subrow to the cell list.  This effectively renders a
 * half-open span starting at x at the subrow. */
static glitter_status_t
cell_list_render_subspan_start_to_cell(
    struct cell_list *cells,
    grid_scaled_x_t x)
{
    struct cell *cell;
    int ix, fx;

    SPLIT_X(x, ix, fx);

    cell = cell_list_find(cells, ix);
    if (cell) {
	cell->area += 2*fx;
	cell->cover += 2*GRID_X;
	return GLITTER_STATUS_SUCCESS;
    }
    return GLITTER_STATUS_NO_MEMORY;
}

/* Render a span on the current subrow at [x1,x2) to the cell list. */
inline static glitter_status_t
cell_list_render_subspan_to_cells(
    struct cell_list *cells,
    grid_scaled_x_t x1,
    grid_scaled_x_t x2)
{
    int ix1, fx1;
    int ix2, fx2;

    SPLIT_X(x1, ix1, fx1);
    SPLIT_X(x2, ix2, fx2);

    if (ix1 != ix2) {
	struct cell_pair p;
	p = cell_list_find2(cells, ix1, ix2);
	if (p.cell1 && p.cell2) {
	    p.cell1->area += 2*fx1;
	    p.cell1->cover += 2*GRID_X;
	    p.cell2->area -= 2*fx2;
	    p.cell2->cover -= 2*GRID_X;
	    return GLITTER_STATUS_SUCCESS;
	}
    }
    else {
	struct cell *cell = cell_list_find(cells, ix1);
	if (cell) {
	    cell->area += 2*(fx1-fx2);
	    return GLITTER_STATUS_SUCCESS;
	}
    }
    return GLITTER_STATUS_NO_MEMORY;
}

/* Computes the analytical coverage of an edge on the current pixel
 * row into the cell list and advances the edge by a full row's worth.
 * This is only called when we know that the edge crosses the current
 * pixel row and doesn't intersect with any other edge while
 * crossing. */
static glitter_status_t
cell_list_render_edge_to_cells(
    struct cell_list *cells,
    struct edge *edge,
    int sign)
{
    struct quorem x1 = edge->x;
    struct quorem x2 = x1;
    grid_scaled_y_t y1, y2, dy;
    grid_scaled_x_t dx;
    int ix1, ix2;
    grid_scaled_x_t fx1, fx2;

    x2.quo += edge->dxdy_full.quo;
    x2.rem += edge->dxdy_full.rem;
    if (x2.rem >= 0) {
	++x2.quo;
	x2.rem -= edge->dy;
    }
    edge->x = x2;

    SPLIT_X(x1.quo, ix1, fx1);
    SPLIT_X(x2.quo, ix2, fx2);

    /* Edge is entirely within a column? */
    if (ix1 == ix2) {
	struct cell *cell = cell_list_find(cells, ix1);
	if (NULL == cell)
	    return GLITTER_STATUS_NO_MEMORY;
	cell->cover += sign*GRID_XY;
	cell->area += sign*(fx1 + fx2)*GRID_Y;
	return GLITTER_STATUS_SUCCESS;
    }

    /* Orient the edge left-to-right. */
    dx = x2.quo - x1.quo;
    if (dx >= 0) {
	y1 = 0;
	y2 = GRID_Y;
    } else {
	int tmp;
	tmp = ix1; ix1 = ix2; ix2 = tmp;
	tmp = fx1; fx1 = fx2; fx2 = tmp;
	dx = -dx;
	sign = -sign;
	y1 = GRID_Y;
	y2 = 0;
    }
    dy = y2 - y1;

    /* Compute area/coverage for a horizontal span of pixels. */
    {
	struct cell_pair pair;
	struct quorem y = floored_divrem((GRID_X - fx1)*dy, dx);

	cell_list_maybe_rewind(cells, ix1);

	pair = cell_list_find2(cells, ix1, ix1+1);
	if (!pair.cell1 || !pair.cell2)
	    return GLITTER_STATUS_NO_MEMORY;

	pair.cell1->area += sign*y.quo*(GRID_X + fx1);
	pair.cell1->cover += sign*y.quo*GRID_X*2;
	y.quo += y1;

	if (ix1+1 < ix2) {
	    struct quorem dydx_full = floored_divrem(GRID_X*dy, dx);
	    struct cell *cell = pair.cell2;

	    ++ix1;
	    do {
		grid_area_t a;
		grid_scaled_y_t next_y = y.quo + dydx_full.quo;
		y.rem += dydx_full.rem;
		if (y.rem >= dx) {
		    ++next_y;
		    y.rem -= dx;
		}

		a = sign*(next_y - y.quo)*GRID_X;

		y.quo = next_y;

		cell->area += a;
		cell->cover += a*2;

		++ix1;
		cell = cell_list_find(cells, ix1);
		if (NULL == cell)
		    return GLITTER_STATUS_NO_MEMORY;
	    } while (ix1 != ix2);

	    pair.cell2 = cell;
	}
	pair.cell2->area += sign*(y2 - y.quo)*fx2;
	pair.cell2->cover += sign*(y2 - y.quo)*GRID_X*2;
    }

    return GLITTER_STATUS_SUCCESS;
}

static void
polygon_init(struct polygon *polygon)
{
    polygon->ymin = polygon->ymax = 0;
    polygon->y_buckets = NULL;
    pool_init(polygon->edge_pool.base,
	      203*sizeof(struct edge), /* ~ 8K */
	      sizeof(polygon->edge_pool.embedded));
}

static void
polygon_fini(struct polygon *polygon)
{
    free(polygon->y_buckets);
    pool_fini(polygon->edge_pool.base);
    polygon_init(polygon);
}

static void *
xrecalloc(void *p, size_t a, size_t b)
{
    size_t total = a*b;
    if (b && total / b != a)
	return NULL;
    p = realloc(p, total);
    if (p)
	memset(p, 0, total);
    return p;
}

/* Relinquishes all allocated edge structs back to the polygon and
 * empties the polygon. The polygon is then prepared to receive new
 * edges and clip them to the vertical range [ymin,ymax).  */
static glitter_status_t
polygon_reset(
    struct polygon *polygon,
    grid_scaled_y_t ymin,
    grid_scaled_y_t ymax)
{
    void *p;
    unsigned h = ymax - ymin;
    unsigned num_buckets = EDGE_Y_BUCKET_INDEX(ymax + EDGE_Y_BUCKET_HEIGHT-1, ymin);

    pool_reset(polygon->edge_pool.base);

    if (h > 0x7FFFFFFFU - EDGE_Y_BUCKET_HEIGHT)
	goto bail_no_mem; /* even if you could, you wouldn't want to. */

    if (num_buckets > 0) {
	p = xrecalloc(
	    polygon->y_buckets,
	    num_buckets,
	    sizeof(struct edge*));
	if (NULL == p)
	    goto bail_no_mem;
    }
    else {
	free(polygon->y_buckets);
	p = NULL;
    }
    polygon->y_buckets = p;

    polygon->ymin = ymin;
    polygon->ymax = ymax;
    return GLITTER_STATUS_SUCCESS;

 bail_no_mem:
    free(polygon->y_buckets);
    polygon->y_buckets = NULL;
    polygon->ymin = 0;
    polygon->ymax = 0;
    return GLITTER_STATUS_NO_MEMORY;
}

static void
_polygon_insert_edge_into_its_y_bucket(
    struct polygon *polygon,
    struct edge *e)
{
    unsigned ix = EDGE_Y_BUCKET_INDEX(e->ytop, polygon->ymin);
    struct edge **ptail = &polygon->y_buckets[ix];
    e->next = *ptail;
    *ptail = e;
}

/* Add a new oriented edge to the polygon.  The direction must be +1
 * or -1. */
inline static glitter_status_t
polygon_add_edge(
    struct polygon *polygon,
    int x0, int y0,
    int x1, int y1,
    int dir)
{
    struct edge *e;
    grid_scaled_x_t dx;
    grid_scaled_y_t dy;
    grid_scaled_y_t ytop, ybot;
    grid_scaled_y_t ymin = polygon->ymin;
    grid_scaled_y_t ymax = polygon->ymax;

    if (y0 == y1)
	return GLITTER_STATUS_SUCCESS;

    if (y0 > y1) {
	int tmp;
	tmp = x0; x0 = x1; x1 = tmp;
	tmp = y0; y0 = y1; y1 = tmp;
	dir = -dir;
    }

    if (y0 >= ymax || y1 <= ymin)
	return GLITTER_STATUS_SUCCESS;

    e = pool_alloc(polygon->edge_pool.base,
		   sizeof(struct edge));
    if (NULL == e)
	return GLITTER_STATUS_NO_MEMORY;

    dx = x1 - x0;
    dy = y1 - y0;
    e->dy = dy;
    e->dxdy = floored_divrem(dx, dy);

    if (ymin <= y0) {
	ytop = y0;
	e->x.quo = x0;
	e->x.rem = 0;
    }
    else {
	ytop = ymin;
	e->x = floored_muldivrem(ymin - y0, dx, dy);
	e->x.quo += x0;
    }

    e->dir = dir;
    e->ytop = ytop;
    ybot = y1 < ymax ? y1 : ymax;
    e->h = ybot - ytop;

    if (e->h >= GRID_Y) {
	e->dxdy_full = floored_muldivrem(GRID_Y, dx, dy);
    }
    else {
	e->dxdy_full.quo = 0;
	e->dxdy_full.rem = 0;
    }

    _polygon_insert_edge_into_its_y_bucket(polygon, e);

    e->x.rem -= dy;		/* Bias the remainder for faster
				 * edge advancement. */
    return GLITTER_STATUS_SUCCESS;
}

/* Empties the active list. */
static void
active_list_reset(
    struct active_list *active)
{
    active->head = NULL;
    active->min_h = 0;
}

static void
active_list_init(struct active_list *active)
{
    active_list_reset(active);
}

static void
active_list_fini(
    struct active_list *active)
{
    active_list_reset(active);
}

/* Merge the edges in an unsorted list of edges into a sorted
 * list. The sort order is edges ascending by edge->x.quo.  Returns
 * the new head of the sorted list. */
static struct edge *
merge_unsorted_edges(struct edge *sorted_head, struct edge *unsorted_head)
{
    struct edge *head = unsorted_head;
    struct edge **pprev = &sorted_head;
    int x;

    while (NULL != head) {
	struct edge *prev = *pprev;
	struct edge *next = head->next;
	x = head->x.quo;

	if (NULL == prev || x < prev->x.quo) {
	    pprev = &sorted_head;
	}

	while (1) {
	    UNROLL3({
		prev = *pprev;
		if (NULL == prev || prev->x.quo >= x)
		    break;
		pprev = &prev->next;
	    });
	}

	head->next = *pprev;
	*pprev = head;

	head = next;
    }
    return sorted_head;
}

/* Test if the edges on the active list can be safely advanced by a
 * full row without intersections. */
inline static int
active_list_can_step_row(
    struct active_list *active)
{
    /* Recomputes the minimum height of all edges on the active
     * list if we don't know the min height well. */
    if (active->min_h <= 0) {
	struct edge *e = active->head;
	int min_h = INT_MAX;

	while (NULL != e) {
	    if (e->h < min_h)
		min_h = e->h;
	    e = e->next;
	}

	active->min_h = min_h;
    }

    /* Don't bother if an edge is going likely to end soon. */
    if (active->min_h >= GRID_Y) {
	/* Check that no intersections would happen in the full step. */
	grid_scaled_x_t prev_x = INT_MIN;
	struct edge *e = active->head;
	while (NULL != e) {
	    struct quorem x = e->x;

	    x.quo += e->dxdy_full.quo;
	    x.rem += e->dxdy_full.rem;
	    if (x.rem >= 0)
		++x.quo;

	    if (x.quo <= prev_x)
		return 0;
	    prev_x = x.quo;
	    e = e->next;
	}
	return 1;
    }
    return 0;
}

/* Merges edges on the given subsample row from the polygon to the
 * active_list. */
inline static void
active_list_merge_edges_from_polygon(
    struct active_list *active,
    grid_scaled_y_t y,
    struct polygon *polygon)
{
    /* Split off the edges on the current subrow and merge them into
     * the active list. */
    unsigned ix = EDGE_Y_BUCKET_INDEX(y, polygon->ymin);
    int min_h = active->min_h;
    struct edge *subrow_edges = NULL;
    struct edge **ptail = &polygon->y_buckets[ix];

    while (1) {
	struct edge *tail = *ptail;
	if (NULL == tail) break;

	if (y == tail->ytop) {
	    *ptail = tail->next;
	    tail->next = subrow_edges;
	    subrow_edges = tail;
	    if (tail->h < min_h)
		min_h = tail->h;
	}
	else {
	    ptail = &tail->next;
	}
    }
    active->head = merge_unsorted_edges(active->head, subrow_edges);
    active->min_h = min_h;
}

/* Advance the edges on the active list by one subsample row by
 * updating their x positions.  Drop edges from the list that end. */
inline static void
active_list_substep_edges(
    struct active_list *active)
{
    struct edge **pprev = &active->head;
    grid_scaled_x_t prev_x = INT_MIN;
    struct edge *unsorted = NULL;

    while (1) {
	struct edge *edge;

	UNROLL3({
	    edge = *pprev;
	    if (NULL == edge)
		break;

	    if (0 != --edge->h) {
		edge->x.quo += edge->dxdy.quo;
		edge->x.rem += edge->dxdy.rem;
		if (edge->x.rem >= 0) {
		    ++edge->x.quo;
		    edge->x.rem -= edge->dy;
		}

		if (edge->x.quo < prev_x) {
		    *pprev = edge->next;
		    edge->next = unsorted;
		    unsorted = edge;
		} else {
		    prev_x = edge->x.quo;
		    pprev = &edge->next;
		}

	    } else {
		*pprev = edge->next;
	    }
	});
    }

    if (unsorted)
	active->head = merge_unsorted_edges(active->head, unsorted);
}

/* Render spans to the cell list corresponding to parts of the polygon
 * that intersect the current subsample row.  Non-zero winding number
 * fill rule. */
inline static glitter_status_t
apply_nonzero_fill_rule_for_subrow(
    struct active_list *active,
    struct cell_list *coverages)
{
    struct edge *edge = active->head;
    int winding = 0;
    int xstart;
    int xend;
    int status;

    cell_list_rewind(coverages);

    while (NULL != edge) {
	xstart = edge->x.quo;
	winding = edge->dir;
	while (1) {
	    edge = edge->next;
	    if (NULL == edge) {
		return cell_list_render_subspan_start_to_cell(
		    coverages, xstart);
	    }
	    winding += edge->dir;
	    if (0 == winding)
		break;
	}

	xend = edge->x.quo;
	status = cell_list_render_subspan_to_cells(coverages, xstart, xend);
	if (status)
	    return status;

	edge = edge->next;
    }

    return GLITTER_STATUS_SUCCESS;
}

/* Render spans to the cell list corresponding to parts of the polygon
 * that intersect the current subsample row.  Even-odd fill rule. */
static glitter_status_t
apply_evenodd_fill_rule_for_subrow(
    struct active_list *active,
    struct cell_list *coverages)
{
    struct edge *edge = active->head;
    int xstart;
    int xend;
    int status;

    cell_list_rewind(coverages);

    while (NULL != edge) {
	xstart = edge->x.quo;

	edge = edge->next;
	if (NULL == edge) {
	    return cell_list_render_subspan_start_to_cell(
		coverages, xstart);
	}

	xend = edge->x.quo;
	status = cell_list_render_subspan_to_cells(coverages, xstart, xend);
	if (status)
	    return status;

	edge = edge->next;
    }

    return GLITTER_STATUS_SUCCESS;
}

/* Compute analytical coverage of the polygon for the current pixel
 * row and step the edges on the active list by one row.  Only called
 * when it's safe to use analytical coverage computations (no new
 * edges start and there are no edge intersections inside the pixel
 * row.) */
static glitter_status_t
apply_nonzero_fill_rule_and_step_edges(
    struct active_list *active,
    struct cell_list *coverages)
{
    struct edge **pprev = &active->head;
    struct edge *left_edge;
    int status;

    left_edge = *pprev;
    while (NULL != left_edge) {
	struct edge *right_edge;
	int winding = left_edge->dir;

	left_edge->h -= GRID_Y;
	if (left_edge->h) {
	    pprev = &left_edge->next;
	}
	else {
	    *pprev = left_edge->next;
	}

	while (1) {
	    right_edge = *pprev;

	    if (NULL == right_edge) {
		return cell_list_render_edge_to_cells(
		    coverages, left_edge, +1);
	    }

	    right_edge->h -= GRID_Y;
	    if (right_edge->h) {
		pprev = &right_edge->next;
	    }
	    else {
		*pprev = right_edge->next;
	    }

	    winding += right_edge->dir;
	    if (0 == winding)
		break;

	    right_edge->x.quo += right_edge->dxdy_full.quo;
	    right_edge->x.rem += right_edge->dxdy_full.rem;
	    if (right_edge->x.rem >= 0) {
		++right_edge->x.quo;
		right_edge->x.rem -= right_edge->dy;
	    }
	}

	status = cell_list_render_edge_to_cells(
	    coverages, left_edge, +1);
	if (status)
	    return status;
	status = cell_list_render_edge_to_cells(
	    coverages, right_edge, -1);
	if (status)
	    return status;

	left_edge = *pprev;
    }

    return GLITTER_STATUS_SUCCESS;
}

/* Compute analytical coverage of the polygon for the current pixel
 * row and step the edges on the active list by one row.  Only called
 * when it's safe to use analytical coverage computations (no new
 * edges start and there are no edge intersections inside the pixel
 * row.) */
static glitter_status_t
apply_evenodd_fill_rule_and_step_edges(
    struct active_list *active,
    struct cell_list *coverages)
{
    struct edge **pprev = &active->head;
    struct edge *left_edge;
    int status;

    left_edge = *pprev;
    while (NULL != left_edge) {
	struct edge *right_edge;

	left_edge->h -= GRID_Y;
	if (left_edge->h) {
	    pprev = &left_edge->next;
	}
	else {
	    *pprev = left_edge->next;
	}

	right_edge = *pprev;

	if (NULL == right_edge) {
	    return cell_list_render_edge_to_cells(
		coverages, left_edge, +1);
	}

	right_edge->h -= GRID_Y;
	if (right_edge->h) {
	    pprev = &right_edge->next;
	}
	else {
	    *pprev = right_edge->next;
	}

	status = cell_list_render_edge_to_cells(
	    coverages, left_edge, +1);
	if (status)
	    return status;
	status = cell_list_render_edge_to_cells(
	    coverages, right_edge, -1);
	if (status)
	    return status;

	left_edge = *pprev;
    }

    return GLITTER_STATUS_SUCCESS;
}

/* If the user hasn't configured a coverage blitter, use a default one
 * that renders to an A8 raster. */
#ifndef GLITTER_BLIT_COVERAGES

/* Blit a span of pixels to an image row.  Tweak this to retarget
 * polygon rendering to something else. */
inline static void
blit_span(
    unsigned char *row_pixels,
    int x, unsigned len,
    grid_area_t coverage)
{
    int alpha = GRID_AREA_TO_ALPHA(coverage);
    if (1 == len) {
	row_pixels[x] = alpha;
    }
    else {
	memset(row_pixels + x, alpha, len);
    }
}

#define GLITTER_BLIT_COVERAGES(coverages, y, xmin, xmax) \
	blit_cells(coverages, raster_pixels + (y)*raster_stride, xmin, xmax)

static void
blit_cells(
    struct cell_list *cells,
    unsigned char *row_pixels,
    int xmin, int xmax)
{
    struct cell *cell = cells->head;
    int prev_x = xmin;
    int cover = 0;
    if (NULL == cell)
	return;

    while (NULL != cell && cell->x < xmin) {
	cover += cell->cover;
	cell = cell->next;
    }

    for (; NULL != cell; cell = cell->next) {
	int x = cell->x;
	int area;
	if (x >= xmax)
	    break;
	if (x > prev_x && 0 != cover) {
	    blit_span(row_pixels, prev_x, x - prev_x, cover);
	}

	cover += cell->cover;
	area = cover - cell->area;
	if (area) {
	    blit_span(row_pixels, x, 1, area);
	}
	prev_x = x+1;
    }

    if (0 != cover && prev_x < xmax) {
	blit_span(row_pixels, prev_x, xmax - prev_x, cover);
    }
}
#endif /* GLITTER_BLIT_COVERAGES */

static void
_glitter_scan_converter_init(glitter_scan_converter_t *converter)
{
    polygon_init(converter->polygon);
    active_list_init(converter->active);
    cell_list_init(converter->coverages);
    converter->xmin=0;
    converter->ymin=0;
    converter->xmax=0;
    converter->ymax=0;
}

static void
_glitter_scan_converter_fini(glitter_scan_converter_t *converter)
{
    polygon_fini(converter->polygon);
    active_list_fini(converter->active);
    cell_list_fini(converter->coverages);
    converter->xmin=0;
    converter->ymin=0;
    converter->xmax=0;
    converter->ymax=0;
}

I glitter_scan_converter_t *
glitter_scan_converter_create(void)
{
    glitter_scan_converter_t *converter =
	malloc(sizeof(struct glitter_scan_converter));
    if (NULL != converter)
	_glitter_scan_converter_init(converter);
    return converter;
}

I void
glitter_scan_converter_destroy(glitter_scan_converter_t *converter)
{
    if (NULL != converter)
	_glitter_scan_converter_fini(converter);
    free(converter);
}

static grid_scaled_t
int_to_grid_scaled(int i, int scale)
{
    /* Clamp to max/min representable scaled number. */
    if (i >= 0) {
	if (i >= INT_MAX/scale)
	    i = INT_MAX/scale;
    }
    else {
	if (i <= INT_MIN/scale)
	    i = INT_MIN/scale;
    }
    return i*scale;
}

#define int_to_grid_scaled_x(x) int_to_grid_scaled((x), GRID_X)
#define int_to_grid_scaled_y(x) int_to_grid_scaled((x), GRID_Y)

I glitter_status_t
glitter_scan_converter_reset(
    glitter_scan_converter_t *converter,
    int xmin, int ymin,
    int xmax, int ymax)
{
    glitter_status_t status;

    converter->xmin = 0; converter->xmax = 0;
    converter->ymin = 0; converter->ymax = 0;

    xmin = int_to_grid_scaled_x(xmin);
    ymin = int_to_grid_scaled_y(ymin);
    xmax = int_to_grid_scaled_x(xmax);
    ymax = int_to_grid_scaled_y(ymax);

    active_list_reset(converter->active);
    cell_list_reset(converter->coverages);
    status = polygon_reset(converter->polygon, ymin, ymax);
    if (status)
	return status;

    converter->xmin = xmin;
    converter->xmax = xmax;
    converter->ymin = ymin;
    converter->ymax = ymax;
    return GLITTER_STATUS_SUCCESS;
}

/* Gah.. this bit of ugly defines INPUT_TO_GRID_X/Y so as to use
 * shifts if possible, and something saneish if not.
 */
#if !defined(INPUT_TO_GRID_Y) && defined(GRID_Y_BITS) && GRID_Y_BITS <= GLITTER_INPUT_BITS
#  define INPUT_TO_GRID_Y(in, out) (out) = (in) >> (GLITTER_INPUT_BITS - GRID_Y_BITS)
#else
#  define INPUT_TO_GRID_Y(in, out) INPUT_TO_GRID_general(in, out, GRID_Y)
#endif

#if !defined(INPUT_TO_GRID_X) && defined(GRID_X_BITS) && GRID_X_BITS <= GLITTER_INPUT_BITS
#  define INPUT_TO_GRID_X(in, out) (out) = (in) >> (GLITTER_INPUT_BITS - GRID_X_BITS)
#else
#  define INPUT_TO_GRID_X(in, out) INPUT_TO_GRID_general(in, out, GRID_X)
#endif

#define INPUT_TO_GRID_general(in, out, grid_scale) do {		\
	long long tmp__ = (long long)(grid_scale) * (in);	\
	tmp__ >>= GLITTER_INPUT_BITS;				\
	(out) = tmp__;						\
} while (0)

I glitter_status_t
glitter_scan_converter_add_edge(
    glitter_scan_converter_t *converter,
    glitter_input_scaled_t x1, glitter_input_scaled_t y1,
    glitter_input_scaled_t x2, glitter_input_scaled_t y2,
    int dir)
{
    /* XXX: possible overflows if GRID_X/Y > 2**GLITTER_INPUT_BITS */
    grid_scaled_y_t sx1, sy1;
    grid_scaled_y_t sx2, sy2;

    INPUT_TO_GRID_Y(y1, sy1);
    INPUT_TO_GRID_Y(y2, sy2);
    if (sy1 == sy2)
	return GLITTER_STATUS_SUCCESS;

    INPUT_TO_GRID_X(x1, sx1);
    INPUT_TO_GRID_X(x2, sx2);

    return polygon_add_edge(
	converter->polygon, sx1, sy1, sx2, sy2, dir);
}

#ifndef GLITTER_BLIT_COVERAGES_BEGIN
# define GLITTER_BLIT_COVERAGES_BEGIN
#endif

#ifndef GLITTER_BLIT_COVERAGES_END
# define GLITTER_BLIT_COVERAGES_END
#endif

#ifndef GLITTER_BLIT_COVERAGES_EMPTY
# define GLITTER_BLIT_COVERAGES_EMPTY(y, xmin, xmax)
#endif

I glitter_status_t
glitter_scan_converter_render(
    glitter_scan_converter_t *converter,
    int nonzero_fill,
    GLITTER_BLIT_COVERAGES_ARGS)
{
    int i;
    int ymax_i = converter->ymax / GRID_Y;
    int ymin_i = converter->ymin / GRID_Y;
    int xmin_i, xmax_i;
    int h = ymax_i - ymin_i;
    struct polygon *polygon = converter->polygon;
    struct cell_list *coverages = converter->coverages;
    struct active_list *active = converter->active;

    xmin_i = converter->xmin / GRID_X;
    xmax_i = converter->xmax / GRID_X;
    if (xmin_i >= xmax_i)
	return GLITTER_STATUS_SUCCESS;

    /* Let the coverage blitter initialise itself. */
    GLITTER_BLIT_COVERAGES_BEGIN;

    /* Render each pixel row. */
    for (i=0; i<h; i++) {
	int do_full_step = 0;
	glitter_status_t status = 0;

	/* Determine if we can ignore this row or use the full pixel
	 * stepper. */
	if (GRID_Y == EDGE_Y_BUCKET_HEIGHT
	    && !polygon->y_buckets[i])
	{
	    if (!active->head) {
		GLITTER_BLIT_COVERAGES_EMPTY(i+ymin_i, xmin_i, xmax_i);
		continue;
	    }
	    do_full_step = active_list_can_step_row(active);
	}

	cell_list_reset(coverages);

	if (do_full_step) {
	    /* Step by a full pixel row's worth. */
	    if (nonzero_fill) {
		status = apply_nonzero_fill_rule_and_step_edges(
		    active, coverages);
	    }
	    else {
		status = apply_evenodd_fill_rule_and_step_edges(
		    active, coverages);
	    }
	}
	else {
	    /* Subsample this row. */
	    grid_scaled_y_t suby;
	    for (suby = 0; suby < GRID_Y; suby++) {
		grid_scaled_y_t y = (i+ymin_i)*GRID_Y + suby;

		active_list_merge_edges_from_polygon(
		    active, y, polygon);

		if (nonzero_fill)
		    status |= apply_nonzero_fill_rule_for_subrow(
			active, coverages);
		else
		    status |= apply_evenodd_fill_rule_for_subrow(
			active, coverages);

		active_list_substep_edges(active);
	    }
	}

	if (status)
	    return status;

	GLITTER_BLIT_COVERAGES(coverages, i+ymin_i, xmin_i, xmax_i);

	if (!active->head) {
	    active->min_h = INT_MAX;
	}
	else {
	    active->min_h -= GRID_Y;
	}
    }

    /* Clean up the coverage blitter. */
    GLITTER_BLIT_COVERAGES_END;

    return GLITTER_STATUS_SUCCESS;
}
