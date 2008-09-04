#include <string.h>
#include <stdlib.h>
#include <SkBitmap.h>
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkPath.h>

struct context {
        SkBitmap bitmap;
        SkCanvas *canvas;
        SkPath path;
        SkPaint paint;
        SkBitmap::Config config;
        void *data;
};

extern "C" struct context *
cx_create()
{
        struct context *cx = new context();
        char const *fmt;
        static struct {
                char const *name;
                SkBitmap::Config config;
        } configs[] = {
                { "ARGB_8888", SkBitmap::kARGB_8888_Config },
                { "ARGB_4444", SkBitmap::kARGB_4444_Config },
                { "RGB_565", SkBitmap::kRGB_565_Config },
                { "A8", SkBitmap::kA8_Config },
                { "A1", SkBitmap::kA1_Config },
                { NULL, SkBitmap::kARGB_8888_Config }
        };
        int i;

        fmt = getenv("SKIA_BITMAP_CONFIG");
        fmt = fmt ? fmt : "ARGB_8888";
        for (i=0; configs[i].name; i++) {
                if (0 == strcmp(configs[i].name, fmt)) {
                        break;
                }
        }
        cx->config = configs[i].config;
        cx->canvas = new SkCanvas(cx->bitmap);

        cx->paint.setAntiAlias(true);
        cx->paint.setColor(SK_ColorWHITE);

        cx->path.setFillType(SkPath::kWinding_FillType);

        cx->data = NULL;
        return cx;
}

extern "C" void
cx_destroy(struct context *cx)
{
        free(cx->data);
        delete cx->canvas;
        delete cx;
}

extern "C" void
cx_reset_clip(struct context *cx,
              int xmin, int ymin, int xmax, int ymax)
{
        SkIRect r;
        r.set(xmin, ymin, xmax, ymax);
        SkRegion clip(r);
        cx->canvas->setClipRegion(clip);
}

extern "C" void
cx_resize(struct context *cx, unsigned width, unsigned height)
{
        delete cx->canvas;
        cx->bitmap.setConfig(cx->config, width, height);
        cx->bitmap.allocPixels();
        cx->canvas = new SkCanvas(cx->bitmap);
        /*cx_reset_clip(cx, 0, 0, width, height);*/
}

extern "C" void
cx_clear(struct context *cx)
{
        cx->canvas->save(); {
                cx_reset_clip(cx,
                              0, 0,
                              cx->bitmap.width(),
                              cx->bitmap.height());
                cx->canvas->drawColor(0, SkPorterDuff::kClear_Mode);
                cx->canvas->restore();
        }
}

extern "C" void
cx_set_fill_rule(struct context *cx, int nonzero_fill)
{
        cx->path.setFillType(nonzero_fill
                             ? SkPath::kWinding_FillType
                             : SkPath::kEvenOdd_FillType);
}

extern "C" void
cx_moveto(struct context *cx, double x, double y)
{
        cx->path.moveTo(SkDoubleToScalar(x),
                        SkDoubleToScalar(y));
}

extern "C" void
cx_lineto(struct context *cx, double x, double y)
{
        cx->path.lineTo(SkDoubleToScalar(x),
                        SkDoubleToScalar(y));
}

extern "C" void
cx_closepath(struct context *cx)
{
        cx->path.close();
}

extern "C" void
cx_fill(struct context *cx)
{
        cx->canvas->drawPath(cx->path, cx->paint);
        cx->path.rewind();
}

extern "C" void
cx_get_pixels(
        struct context *cx,
        unsigned char **OUT_pixels,
        size_t *OUT_stride,
        unsigned *OUT_width,
        unsigned *OUT_height)
{
        unsigned w = cx->bitmap.width();
        unsigned h = cx->bitmap.height();
        size_t stride = w + ((-w)&3U);

        free(cx->data);
        *OUT_pixels = (unsigned char*)calloc(h, stride);
        *OUT_stride = stride;
        *OUT_width = w;
        *OUT_height = h;
        cx->data = *OUT_pixels;

        SkBitmap mask;
        mask.setConfig(SkBitmap::kA8_Config, w, h, stride);
        mask.setPixels(cx->data);

        SkCanvas canvas(mask);

        canvas.drawBitmap(cx->bitmap, 0, 0);
}
