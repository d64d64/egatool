/*
 * egatool: raylib/raygui front end.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raylib.h"
#if !defined(RAYLIB_VERSION_MAJOR) || RAYLIB_VERSION_MAJOR < 6
#error "EGAtool needs raylib 6 or newer"
#endif

#include "raygui.h"

#include "tinyfiledialogs.h"

#include "clipboard.h"
#include "engine.h"
#include "icon_data.h"
#include "imageio.h"
#include "recipe.h"
#include "thread.h"
#include "topaz_font.h"

/* The control panel runs along the bottom: columns of controls, scrolled
 * sideways when the window is narrower than all of them. */
#define PAD       12
#define ROW_H     28
#define CTRL_W    250 /* one column */
#define COL_GAP   12
#define SLIDER_W  (CTRL_W - 56)
#define PANEL_COLS 6
#define PANEL_CONTENT_H 286
#define SCROLLBAR_H 14
#define PANEL_H   (PANEL_CONTENT_H + SCROLLBAR_H)
#define STATUS_H  30
/* Solid areas' Max change slider: this value (the top) means any change. */
#define CAL_ANY   448

static const char *RES_ITEMS =
    "256 x 192;256 x 240;320 x 200;320 x 240;640 x 350;640 x 480;800 x 600;1024 x 768;Original";
static const int RES_SIZES[][2] = {
    {256, 192}, {256, 240}, {320, 200}, {320, 240}, {640, 350}, {640, 480}, {800, 600},
    {1024, 768}, {0, 0},
};
#define RES_ORIGINAL 8
#define RES_DEFAULT 2 /* 320 x 200 */

/* The GUI font: Amiga Workbench's Topaz, drawn at its own pixel size
 * (other sizes blur it). */
#define FONT_SIZE 16
static Font ui_font;

static void draw_text(const char *text, int x, int y, Color color)
{
    DrawTextEx(ui_font, text, (Vector2){ (float)x, (float)y }, FONT_SIZE, 0, color);
}

static int text_width(const char *text)
{
    return (int)MeasureTextEx(ui_font, text, FONT_SIZE, 0).x;
}

static const char *DITHER_ITEMS =
    "Checkerboard;Ordered;Burkes;Floyd-Steinberg;Sierra;Stucki;None";

/* Palette dropdown: the built-ins (indices = EgtBuiltinPalette), then a
 * loaded palette file if there is one. */
#define PAL_LOADED EGT_PAL_COUNT
enum { VIEW_BEFORE, VIEW_AFTER, VIEW_SPLIT };
/* What the Before side shows. */
enum { BEFORE_PREPARED, BEFORE_SCALED, BEFORE_ORIGINAL };

static const char *IMAGE_PATTERNS[] = {
    "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.tga", "*.psd",
};
static const char *SAVE_PATTERNS[] = { "*.png", "*.bmp", "*.tga" };
static const char *PAL_PATTERNS[] = { "*.pal" };

/* An image on the GPU, always drawn at a whole-number zoom so every source
 * pixel becomes an exact square of screen pixels. */
typedef struct {
    Texture2D tex;
} Layer;

/* A loaded image, shared with the worker thread: freed when the last user
 * (the app, a queued job, the running job) lets go. refs is guarded by the
 * worker's lock. id tells images apart for the worker's caches. */
typedef struct {
    EgtImage img;
    int refs;
    unsigned id;
} SourceRef;

/* Everything a conversion depends on, compared with memcmp to see whether
 * anything changed (so always memset to 0 before filling). */
typedef struct {
    SourceRef *src;
    unsigned src_id;
    int w, h;
    bool nearest;
    int smooth, smooth_threshold;
    float smooth_radius;
    EgtAdjust adj;
    int outline, outline_mode, outline_min;
    float outline_dark;
    EgtPalette pal;
    int dither, prefer_solid, harsh_mix, light_mix, min_region, snap;
    bool calibrate;
    int calibrate_max; /* 0 = any change */
    int limit, consistency;
    /* For the settings saved with the image only. */
    char pal_arg[256], src_name[256];
} Job;

/* Runs the pipeline (resize and smooth -> adjust and outline -> convert) on
 * a background thread, so long conversions don't freeze the window. The
 * app queues the latest settings; a job queued while one runs replaces any
 * older queued one. */
typedef struct {
    EgtSync *sync;
    EgtThread *thread;
    /* Guarded by sync. */
    bool quit, running, has_job, has_result;
    Job job;
    EgtImage res_prepared, res_output;
    bool res_valid;
    int res_dither;
    Job res_job; /* the settings res_output was made with */
    EgtCalibAnchor res_cal[EGT_CALIBRATE_MAX];
    int res_ncal;
    double res_ms;
    /* Worker thread only: caches of the slower steps. */
    EgtImage base;
    Job base_key;
    uint8_t *mask;
    unsigned mask_src;
    int mask_w, mask_h, mask_sens;
    EgtImage detail;
    unsigned detail_src;
    int detail_w, detail_h;
} Worker;

typedef struct {
    SourceRef *src;
    unsigned next_src_id;
    Worker *worker; /* not part of the state compared each frame (it changes on its own) */
    Job submitted;      /* last settings handed to the worker */
    bool have_result;   /* a result arrived since the image was loaded */
    bool busy;          /* the worker has work running or queued */
    double busy_since;  /* when busy last became true */
    int img_w, img_h;   /* size of the shown result */
    EgtImage output;    /* last converted image, for Save and Copy */
    Job output_job;     /* the settings it was made with (src not to be used) */
    EgtCalibAnchor calib[EGT_CALIBRATE_MAX]; /* areas calibrated in the shown result */
    int ncalib;
    bool calibrate;     /* the Solid areas checkbox */
    float calibrate_max; /* Max change slider; CAL_ANY = any */
    int export_scale;   /* Save and Copy enlarge the output this many times (1..4) */
    bool output_valid;
    bool status_is_convert_error; /* clear the status once conversion works again */
    char source_name[256];

    int res_index, pal_index, dither_index, limit_index;
    bool res_edit, dither_edit, pal_edit, limit_edit;
    float consistency; /* hardware limits: neighbouring cells keep the same colours */
    float res_scale;   /* Original resolution: downscale to this percentage */
    float prefer_solid, harsh_mix, brightness, contrast, saturation;
    Vector2 panel_scroll;
    float light_mix;
    float snap; /* ordered/diffusion: solid within this many levels of a palette colour */
    bool area_downscale;
    int smooth;      /* EgtSmooth */
    int smooth_kind; /* method selected while smoothing is off, never NONE */
    float smooth_radius, smooth_threshold, outline, outline_dark, outline_min, min_region;
    int outline_mode;
    EgtPalette loaded_pal;
    bool have_loaded_pal;
    char loaded_pal_name[64];

    Layer in, out;
    /* Before side alternatives: the source only resized (to the shown
     * result's size), and the source itself. Made when first shown. */
    Layer scaled, orig;
    unsigned scaled_src, orig_src; /* SourceRef id they were made from, 0 = none */
    int scaled_w, scaled_h;
    bool scaled_nearest;
    int before_kind;

    int view_mode;
    /* fit: pick the largest zoom that fits the view (kept up to date while
     * the window or image size changes) until the user zooms or pans.
     * recenter: centre the image at the current zoom on the next frame. */
    bool fit, recenter, fullscreen;
    int zoom; /* whole numbers only */
    Vector2 offset; /* image top-left inside its view area */
    bool dragging;

    double convert_ms;
    char status[256];
} App;

static void set_status(App *app, const char *msg)
{
    app->status_is_convert_error = false;
    snprintf(app->status, sizeof app->status, "%s", msg);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > s)
        s = b;
    return s ? s + 1 : path;
}

static bool has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot && strcmp(TextToLower(dot), ext) == 0;
}

static void upload(Layer *layer, const EgtImage *img)
{
    Texture2D *tex = &layer->tex;
    if (tex->id && tex->width == img->width && tex->height == img->height) {
        UpdateTexture(*tex, img->pixels);
        return;
    }
    if (tex->id)
        UnloadTexture(*tex);
    {
        Image rl = { img->pixels, img->width, img->height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        *tex = LoadTextureFromImage(rl);
        SetTextureFilter(*tex, TEXTURE_FILTER_POINT);
    }
}

static void unload_layer(Layer *layer)
{
    if (layer->tex.id)
        UnloadTexture(layer->tex);
}

static void release_source(Worker *wk, SourceRef *src)
{
    egt_sync_lock(wk->sync);
    if (src && --src->refs == 0) {
        egt_image_free(&src->img);
        free(src);
    }
    egt_sync_unlock(wk->sync);
}

static void set_source(App *app, EgtImage img, const char *name)
{
    SourceRef *src = malloc(sizeof *src);
    if (!src) {
        egt_image_free(&img);
        set_status(app, "Out of memory");
        return;
    }
    src->img = img;
    src->refs = 1;
    src->id = ++app->next_src_id;
    release_source(app->worker, app->src);
    app->src = src;
    app->have_result = false;
    snprintf(app->source_name, sizeof app->source_name, "%s", name);
    app->brightness = app->contrast = app->saturation = 0.0f;
    app->recenter = true;
    set_status(app, TextFormat("Loaded %s (%dx%d)", name, img.width, img.height));
}

static void load_image_file(App *app, const char *path)
{
    EgtImage img = egt_load_image(path);
    if (!img.pixels) {
        set_status(app, TextFormat("Cannot load %s: %s", base_name(path), egt_load_error()));
        return;
    }
    set_source(app, img, base_name(path));
}

static void load_palette_file(App *app, const char *path)
{
    if (!egt_load_jasc_palette(path, &app->loaded_pal)) {
        set_status(app, TextFormat("%s is not a JASC palette", base_name(path)));
        return;
    }
    app->have_loaded_pal = true;
    snprintf(app->loaded_pal_name, sizeof app->loaded_pal_name, "%s", base_name(path));
    app->pal_index = PAL_LOADED;
    set_status(app, TextFormat("Palette %s: %d colours", app->loaded_pal_name,
                               app->loaded_pal.count));
}

static void open_image_dialog(App *app)
{
    const char *path = tinyfd_openFileDialog("Open image", "", 7, IMAGE_PATTERNS,
                                             "Images", 0);
    if (path)
        load_image_file(app, path);
}

static void open_palette_dialog(App *app)
{
    const char *path = tinyfd_openFileDialog("Load JASC palette", "", 1, PAL_PATTERNS,
                                             "JASC palette", 0);
    if (path)
        load_palette_file(app, path);
}

static void paste_image(App *app)
{
    EgtImage img = egt_clipboard_get_image();
    if (!img.pixels) {
#if defined(_WIN32)
        set_status(app, "No image in clipboard");
#else
        set_status(app, "No image in clipboard (needs wl-paste or xclip)");
#endif
        return;
    }
    set_source(app, img, "clipboard");
}

/* The output enlarged by the export scale (pixel-exact copies), or the
 * output itself at 1x. Free with export_free(). NULL pixels on failure. */
static EgtImage export_image(const App *app)
{
    int k = app->export_scale > 1 ? app->export_scale : 1;
    if (k == 1)
        return app->output;
    return egt_resize_nearest(&app->output, app->output.width * k, app->output.height * k);
}

static void export_free(const App *app, EgtImage *img)
{
    if (img->pixels != app->output.pixels)
        egt_image_free(img);
}

static char *output_recipe(const App *app, const char *out_path);

static bool save_with_recipe(const App *app, const char *path, const EgtImage *img)
{
    char *comment = output_recipe(app, path);
    bool ok = egt_save_image_comment(path, img, comment);
    free(comment);
    return ok;
}

static void save_output_dialog(App *app)
{
    char def[300];
    const char *path, *dot;
    EgtImage img;
    if (!app->output_valid) {
        set_status(app, "Nothing to save yet");
        return;
    }
    snprintf(def, sizeof def, "%s", app->source_name[0] ? app->source_name : "image");
    dot = strrchr(def, '.');
    if (dot)
        def[dot - def] = '\0';
    strncat(def, app->export_scale > 1 ? TextFormat("_ega_%dx.png", app->export_scale) : "_ega.png",
            sizeof def - strlen(def) - 1);
    path = tinyfd_saveFileDialog("Save image", def, 3, SAVE_PATTERNS, "PNG, BMP or TGA");
    if (!path)
        return;
    img = export_image(app);
    if (!img.pixels)
        set_status(app, "Out of memory");
    else if (save_with_recipe(app, path, &img))
        set_status(app, TextFormat("Saved %s (%dx%d)", base_name(path), img.width, img.height));
    else
        set_status(app, TextFormat("Cannot write %s", base_name(path)));
    export_free(app, &img);
}

static void copy_output(App *app)
{
    EgtImage img;
    if (!app->output_valid) {
        set_status(app, "Nothing to copy yet");
        return;
    }
    img = export_image(app);
    if (!img.pixels)
        set_status(app, "Out of memory");
    else if (egt_clipboard_set_image(&img))
        set_status(app, TextFormat("Copied to clipboard (%dx%d)", img.width, img.height));
    else
        set_status(app, "Copy failed (Linux needs wl-copy or xclip)");
    export_free(app, &img);
}

static const EgtPalette *current_palette(App *app, EgtPalette *tmp)
{
    if (app->pal_index == PAL_LOADED && app->have_loaded_pal)
        return &app->loaded_pal;
    egt_builtin_palette(app->pal_index < PAL_LOADED ? (EgtBuiltinPalette)app->pal_index
                                                    : EGT_PAL_EGA, tmp);
    return tmp;
}

/* ---- background conversion ----------------------------------------------- */

/* Runs one job; fills prepared and output (output valid unless the palette
 * doesn't suit the dither). Uses and updates the worker's caches. */
static bool run_job(Worker *wk, const Job *job, EgtImage *prepared, EgtImage *output,
                    EgtCalibAnchor *cal, int *ncal)
{
    const EgtImage *source = &job->src->img;
    EgtFilters filt = {0};
    EgtConvertOptions opt = {0};

    /* Resize and smooth: only when those inputs changed. */
    if (!wk->base.pixels || wk->base_key.src_id != job->src_id || wk->base_key.w != job->w ||
        wk->base_key.h != job->h || wk->base_key.nearest != job->nearest ||
        wk->base_key.smooth != job->smooth || wk->base_key.smooth_radius != job->smooth_radius ||
        wk->base_key.smooth_threshold != job->smooth_threshold) {
        EgtFilters sf = {0};
        egt_image_free(&wk->base);
        sf.smooth = (EgtSmooth)job->smooth;
        sf.smooth_radius = job->smooth_radius;
        sf.smooth_threshold = job->smooth_threshold;
        wk->base = egt_resize_smoothed(source, job->w, job->h, job->nearest, &sf);
        wk->base_key = *job;
        if (!wk->base.pixels)
            return false;
    }

    *prepared = egt_image_copy(&wk->base);
    *output = egt_image_alloc(job->w, job->h);
    if (!prepared->pixels || !output->pixels)
        return false;
    egt_adjust(prepared, &job->adj);
    *ncal = 0;
    if (job->calibrate) {
        *ncal = egt_calibrate(prepared, &job->pal, job->calibrate_max, cal);
        if (*ncal < 0) {
            *ncal = 0;
            return false;
        }
    }

    filt.smooth = EGT_SMOOTH_NONE; /* already applied when resizing */
    filt.smooth_radius = 1;
    filt.outline = job->outline;
    filt.outline_darkness = job->outline_dark;
    filt.outline_mode = (EgtOutlineMode)job->outline_mode;
    filt.outline_min_length = job->outline_min;
    if (filt.outline_mode == EGT_OUTLINE_HIRES && filt.outline > 0) {
        if (!wk->mask || wk->mask_src != job->src_id || wk->mask_w != job->w ||
            wk->mask_h != job->h || wk->mask_sens != filt.outline) {
            free(wk->mask);
            wk->mask = egt_outline_mask_hires(source, job->w, job->h, filt.outline);
            wk->mask_src = job->src_id;
            wk->mask_w = job->w;
            wk->mask_h = job->h;
            wk->mask_sens = filt.outline;
        }
        filt.outline_mask = wk->mask;
    }
    egt_filter(prepared, &filt);

    opt.dither = (EgtDither)job->dither;
    opt.prefer_solid = job->prefer_solid;
    opt.harsh_mix = job->harsh_mix;
    opt.light_mix = job->light_mix;
    opt.min_region = job->min_region;
    opt.snap = job->snap;
    opt.limit = (EgtLimit)job->limit;
    opt.consistency = job->consistency;
    if (opt.dither == EGT_DITHER_EGA_CHECKER && opt.harsh_mix > 0) {
        if (!wk->detail.pixels || wk->detail_src != job->src_id ||
            wk->detail_w != job->w || wk->detail_h != job->h) {
            egt_image_free(&wk->detail);
            wk->detail = egt_detail_image(source, job->w, job->h);
            wk->detail_src = job->src_id;
            wk->detail_w = job->w;
            wk->detail_h = job->h;
        }
        if (wk->detail.pixels)
            opt.detail = &wk->detail;
    }
    return egt_convert(prepared, output, &job->pal, &opt);
}

static void worker_main(void *arg)
{
    Worker *wk = arg;
    egt_sync_lock(wk->sync);
    for (;;) {
        Job job;
        EgtImage prepared = {0}, output = {0};
        EgtCalibAnchor cal[EGT_CALIBRATE_MAX];
        int ncal = 0;
        bool valid;
        double t0;
        while (!wk->has_job && !wk->quit)
            egt_sync_wait(wk->sync);
        if (wk->quit)
            break;
        job = wk->job; /* takes over the job's source reference */
        wk->has_job = false;
        wk->running = true;
        egt_sync_unlock(wk->sync);

        t0 = GetTime();
        valid = run_job(wk, &job, &prepared, &output, cal, &ncal);

        egt_sync_lock(wk->sync);
        /* Replace a result the app hasn't picked up yet. */
        egt_image_free(&wk->res_prepared);
        egt_image_free(&wk->res_output);
        wk->res_prepared = prepared;
        wk->res_output = output;
        wk->res_valid = valid && prepared.pixels;
        wk->res_dither = job.dither;
        wk->res_job = job;
        memcpy(wk->res_cal, cal, sizeof cal);
        wk->res_ncal = ncal;
        wk->res_ms = (GetTime() - t0) * 1000.0;
        wk->has_result = true;
        wk->running = false;
        if (--job.src->refs == 0) {
            egt_image_free(&job.src->img);
            free(job.src);
        }
    }
    egt_sync_unlock(wk->sync);
}

static bool worker_start(Worker *wk)
{
    wk->sync = egt_sync_create();
    if (!wk->sync)
        return false;
    wk->thread = egt_thread_start(worker_main, wk);
    return wk->thread != NULL;
}

/* Waits for a running job to finish (on exit). */
static void worker_stop(Worker *wk)
{
    if (wk->thread) {
        egt_sync_lock(wk->sync);
        wk->quit = true;
        egt_sync_signal(wk->sync);
        egt_sync_unlock(wk->sync);
        egt_thread_join(wk->thread);
    }
    if (wk->has_job && --wk->job.src->refs == 0) {
        egt_image_free(&wk->job.src->img);
        free(wk->job.src);
    }
    egt_image_free(&wk->res_prepared);
    egt_image_free(&wk->res_output);
    egt_image_free(&wk->base);
    egt_image_free(&wk->detail);
    free(wk->mask);
    if (wk->sync)
        egt_sync_destroy(wk->sync);
}

/* The Original resolution: the image's own size times the Scale slider. */
static void original_size(const App *app, int *w, int *h)
{
    float k = roundf(app->res_scale) / 100.0f;
    *w = (int)lroundf(app->src->img.width * k);
    *h = (int)lroundf(app->src->img.height * k);
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
}

/* Current settings as a job (src NULL if there is no image). */
static void make_job(App *app, Job *job)
{
    EgtPalette tmp;
    memset(job, 0, sizeof *job);
    if (!app->src)
        return;
    job->src = app->src;
    job->src_id = app->src->id;
    job->w = RES_SIZES[app->res_index][0];
    job->h = RES_SIZES[app->res_index][1];
    if (app->res_index == RES_ORIGINAL)
        original_size(app, &job->w, &job->h);
    job->nearest = !app->area_downscale;
    job->smooth = app->smooth;
    if (job->smooth != EGT_SMOOTH_NONE) {
        job->smooth_radius = egt_snap_radius(app->smooth_radius);
        job->smooth_threshold = (int)roundf(app->smooth_threshold);
    }
    /* As shown on the sliders (and written in saved settings). */
    job->adj.brightness = (int)roundf(app->brightness);
    job->adj.contrast = (int)roundf(app->contrast);
    job->adj.saturation = roundf(app->saturation * 100) / 100;
    job->outline = (int)roundf(app->outline);
    if (job->outline > 0) {
        job->outline_mode = app->outline_mode;
        job->outline_min = (int)roundf(app->outline_min);
        job->outline_dark = app->outline_dark;
    }
    {
        /* Only the used entries: the rest of tmp is uninitialised, and
         * copying it made every frame look like a settings change. */
        const EgtPalette *pal = current_palette(app, &tmp);
        job->pal.count = pal->count;
        memcpy(job->pal.colors, pal->colors, (size_t)pal->count * sizeof pal->colors[0]);
    }
    job->dither = app->dither_index;
    if (job->dither == EGT_DITHER_EGA_CHECKER) {
        job->prefer_solid = (int)roundf(app->prefer_solid);
        job->harsh_mix = (int)roundf(app->harsh_mix);
        job->light_mix = (int)roundf(app->light_mix);
    } else {
        job->calibrate = app->calibrate;
        if (job->calibrate && roundf(app->calibrate_max) < CAL_ANY)
            job->calibrate_max = (int)roundf(app->calibrate_max);
        if (job->dither != EGT_DITHER_NONE)
            job->snap = (int)roundf(app->snap);
    }
    if (egt_dither_supports_cleanup((EgtDither)job->dither))
        job->min_region = (int)roundf(app->min_region);
    job->limit = app->limit_index;
    if (job->limit != EGT_LIMIT_NONE) {
        /* Not used with limits. */
        job->harsh_mix = 0;
        job->min_region = 0;
        job->consistency = (int)roundf(app->consistency);
    }
    snprintf(job->pal_arg, sizeof job->pal_arg, "%s",
             app->pal_index == PAL_LOADED && app->have_loaded_pal ? app->loaded_pal_name
             : egt_builtin_palette_arg(app->pal_index < PAL_LOADED ? (EgtBuiltinPalette)app->pal_index
                                                                   : EGT_PAL_EGA));
    snprintf(job->src_name, sizeof job->src_name, "%s", app->source_name);
}

/* The settings that made the current output, as a egatool-cli command
 * line for the saved PNG. malloc'd, or NULL. */
static char *output_recipe(const App *app, const char *out_path)
{
    const Job *j = &app->output_job;
    EgtRecipe r;
    memset(&r, 0, sizeof r);
    r.source = j->src_name[0] && strcmp(j->src_name, "clipboard") ? j->src_name : NULL;
    r.output = out_path ? base_name(out_path) : NULL;
    r.palette = j->pal_arg;
    r.width = j->w;
    r.height = j->h;
    r.nearest = j->nearest;
    r.adj = j->adj;
    r.filt.smooth = (EgtSmooth)j->smooth;
    r.filt.smooth_radius = j->smooth_radius;
    r.filt.smooth_threshold = j->smooth_threshold;
    r.filt.outline = j->outline;
    r.filt.outline_mode = (EgtOutlineMode)j->outline_mode;
    r.filt.outline_min_length = j->outline_min;
    r.filt.outline_darkness = j->outline_dark;
    r.opt.dither = (EgtDither)j->dither;
    r.opt.prefer_solid = j->prefer_solid;
    r.opt.harsh_mix = j->harsh_mix;
    r.opt.light_mix = j->light_mix;
    r.opt.min_region = j->min_region;
    r.opt.snap = j->snap;
    r.opt.limit = (EgtLimit)j->limit;
    r.opt.consistency = j->consistency;
    r.scale = app->export_scale;
    r.calibrate = j->calibrate;
    r.calibrate_max = j->calibrate_max;
    return egt_recipe_string(&r);
}

/* Once per frame: queue the settings if they changed, pick up a result. */
static void pipeline_tick(App *app)
{
    Worker *wk = app->worker;
    Job job;
    EgtImage prepared = {0}, output = {0};
    bool got = false, valid = false, prepared_ok;
    int dither = 0;
    double ms = 0;
    Job res_job;
    EgtCalibAnchor res_cal[EGT_CALIBRATE_MAX];
    int res_ncal = 0;

    make_job(app, &job);
    egt_sync_lock(wk->sync);
    if (job.src && memcmp(&job, &app->submitted, sizeof job) != 0) {
        if (wk->has_job && --wk->job.src->refs == 0) { /* drop an older queued job */
            egt_image_free(&wk->job.src->img);
            free(wk->job.src);
        }
        job.src->refs++;
        wk->job = job;
        wk->has_job = true;
        app->submitted = job;
        egt_sync_signal(wk->sync);
    }
    if (wk->has_result) {
        prepared = wk->res_prepared;
        output = wk->res_output;
        valid = wk->res_valid;
        dither = wk->res_dither;
        ms = wk->res_ms;
        res_job = wk->res_job;
        memcpy(res_cal, wk->res_cal, sizeof res_cal);
        res_ncal = wk->res_ncal;
        wk->res_prepared = (EgtImage){0};
        wk->res_output = (EgtImage){0};
        wk->has_result = false;
        got = true;
    }
    {
        bool busy = wk->has_job || wk->running;
        if (busy && !app->busy)
            app->busy_since = GetTime();
        app->busy = busy;
    }
    egt_sync_unlock(wk->sync);

    if (!got)
        return;
    prepared_ok = prepared.pixels != NULL;
    /* A result of an image that's since been replaced still shows until
     * the new one is ready: better than an empty view. */
    if (prepared.pixels) {
        if (prepared.width != app->img_w || prepared.height != app->img_h)
            app->recenter = true; /* a new size keeps the zoom, re-centred */
        app->img_w = prepared.width;
        app->img_h = prepared.height;
        upload(&app->in, &prepared);
        memcpy(app->calib, res_cal, sizeof res_cal);
        app->ncalib = res_ncal;
    }
    egt_image_free(&prepared);
    egt_image_free(&app->output);
    app->output_valid = valid;
    app->have_result = true;
    app->convert_ms = ms;
    if (valid) {
        upload(&app->out, &output);
        app->output = output;
        app->output_job = res_job;
        if (app->status_is_convert_error)
            set_status(app, "");
    } else {
        egt_image_free(&output);
        if (!prepared_ok || res_job.limit != EGT_LIMIT_NONE) {
            set_status(app, "Out of memory");
        } else {
            set_status(app, TextFormat("%s dithering needs a palette of at most %d colours",
                                       egt_dither_name((EgtDither)dither), EGT_CHECKER_MAX_COLORS));
            app->status_is_convert_error = true;
        }
    }
}

/* Show the working indicator only once a conversion has run for a while:
 * quick ones (most slider moves) would just make it blink. */
#define WORKING_DELAY 0.3

static bool show_working(const App *app)
{
    return app->busy && GetTime() - app->busy_since >= WORKING_DELAY;
}

/* ---- view ---------------------------------------------------------------- */

static Rectangle view_rect(const App *app)
{
    float h = (float)GetScreenHeight() - (app->fullscreen ? 0 : PANEL_H + STATUS_H);
    return (Rectangle){ 0, 0, (float)GetScreenWidth(), h };
}

/* The one or two areas images are drawn into; returns how many. */
static int view_areas(const App *app, Rectangle out[2])
{
    Rectangle r = view_rect(app);
    if (app->view_mode == VIEW_SPLIT && !app->fullscreen) {
        float half = floorf(r.width / 2);
        out[0] = (Rectangle){ r.x, r.y, half - 1, r.height };
        out[1] = (Rectangle){ r.x + half + 1, r.y, r.width - half - 1, r.height };
        return 2;
    }
    out[0] = r;
    return 1;
}

#define MAX_ZOOM 16

static void center_view(App *app)
{
    Rectangle a[2];
    view_areas(app, a);
    if (!app->img_w)
        return;
    app->offset.x = floorf((a[0].width - (float)app->img_w * app->zoom) / 2);
    app->offset.y = floorf((a[0].height - (float)app->img_h * app->zoom) / 2);
}

/* Largest whole zoom at which the image fits (at least 1x). */
static void fit_view(App *app)
{
    Rectangle a[2];
    int zx, zy;
    view_areas(app, a);
    if (!app->img_w)
        return;
    zx = (int)((a[0].width - 16) / app->img_w);
    zy = (int)((a[0].height - 16) / app->img_h);
    app->zoom = zx < zy ? zx : zy;
    if (app->zoom < 1)
        app->zoom = 1;
    if (app->zoom > MAX_ZOOM)
        app->zoom = MAX_ZOOM;
    center_view(app);
}

static void set_zoom(App *app, int zoom, Vector2 anchor)
{
    float k;
    zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
    k = (float)zoom / app->zoom;
    app->offset.x = roundf(anchor.x - (anchor.x - app->offset.x) * k);
    app->offset.y = roundf(anchor.y - (anchor.y - app->offset.y) * k);
    app->zoom = zoom;
    app->fit = false;
}

/* Zoom buttons and keys: set the zoom and put the image back in the
 * middle of the view (the mouse wheel zooms around the pointer instead). */
static void zoom_centered(App *app, int zoom)
{
    app->zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
    app->fit = false;
    center_view(app);
}

/* After the view area changed, keep the zoom and keep the image point that
 * was at the centre of the old area centred. (Fit mode simply refits on the
 * next frame.) */
static void keep_center(App *app, Rectangle old_area)
{
    Rectangle a[2];
    if (app->fit)
        return;
    view_areas(app, a);
    app->offset.x = roundf(app->offset.x + (a[0].width - old_area.width) / 2);
    app->offset.y = roundf(app->offset.y + (a[0].height - old_area.height) / 2);
}

static void set_view_mode(App *app, int mode)
{
    Rectangle a[2];
    view_areas(app, a);
    app->view_mode = mode;
    keep_center(app, a[0]);
}

static void handle_view_input(App *app)
{
    Rectangle a[2];
    int n = view_areas(app, a), i;
    Vector2 m = GetMousePosition();
    float wheel = GetMouseWheelMove();

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, view_rect(app)))
        app->dragging = true;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        app->dragging = false;
    if (app->dragging) {
        Vector2 d = GetMouseDelta();
        app->offset.x += d.x;
        app->offset.y += d.y;
        if (d.x != 0 || d.y != 0)
            app->fit = false;
    }
    if (wheel != 0) {
        for (i = 0; i < n; i++) {
            if (CheckCollisionPointRec(m, a[i])) {
                set_zoom(app, app->zoom + (wheel > 0 ? 1 : -1),
                         (Vector2){ m.x - a[i].x, m.y - a[i].y });
                break;
            }
        }
    }
}

static void draw_image(const App *app, Layer *layer, Rectangle area, const char *label)
{
    /* Whole screen pixels and a whole zoom: every image pixel is an exact
     * square, so dither patterns stay intact. */
    Rectangle dst = {
        area.x + roundf(app->offset.x), area.y + roundf(app->offset.y),
        (float)(app->img_w * app->zoom), (float)(app->img_h * app->zoom),
    };
    Texture2D tex = layer && layer->tex.id ? layer->tex : (Texture2D){0};
    Rectangle src = { 0, 0, (float)tex.width, (float)tex.height };
    if (tex.id && app->img_w && (tex.width != app->img_w || tex.height != app->img_h)) {
        /* A different size (the original): the same zoom in its own
         * pixels, with the same part of the picture at the area's centre,
         * so panning moves both sides alike. */
        float cx = area.width / 2, cy = area.height / 2;
        float u = (cx - roundf(app->offset.x)) / dst.width;
        float v = (cy - roundf(app->offset.y)) / dst.height;
        dst.width = (float)(tex.width * app->zoom);
        dst.height = (float)(tex.height * app->zoom);
        dst.x = area.x + roundf(cx - u * dst.width);
        dst.y = area.y + roundf(cy - v * dst.height);
    }
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    if (tex.id) {
        DrawTexturePro(tex, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
    } else if (app->have_result) { /* the conversion failed */
        const char *msg = TextFormat("Needs a palette of at most %d colours",
                                     EGT_CHECKER_MAX_COLORS);
        draw_text(msg, (int)(area.x + (area.width - text_width(msg)) / 2),
                  (int)(area.y + area.height / 2 - FONT_SIZE / 2), GRAY);
    }
    if (label && !app->fullscreen) {
        DrawRectangle((int)area.x, (int)area.y, text_width(label) + 16, 26, Fade(BLACK, 0.6f));
        draw_text(label, (int)area.x + 8, (int)area.y + 5, RAYWHITE);
    }
    EndScissorMode();
}

/* The Before side's image, made or refreshed when needed: Scaled follows
 * the shown result's size and the Smooth downscale setting. Falls back to
 * Prepared if it can't be made. */
static Layer *before_layer(App *app)
{
    if (!app->src || app->before_kind == BEFORE_PREPARED)
        return &app->in;
    if (app->before_kind == BEFORE_ORIGINAL) {
        if (app->orig_src != app->src->id) {
            upload(&app->orig, &app->src->img);
            app->orig_src = app->src->id;
        }
        return &app->orig;
    }
    if (!app->img_w)
        return &app->in;
    if (app->scaled_src != app->src->id || app->scaled_w != app->img_w ||
        app->scaled_h != app->img_h || app->scaled_nearest != !app->area_downscale) {
        const EgtImage *s = &app->src->img;
        EgtImage img = !app->area_downscale ? egt_resize_nearest(s, app->img_w, app->img_h)
                                            : egt_resize_area(s, app->img_w, app->img_h);
        if (!img.pixels)
            return &app->in;
        upload(&app->scaled, &img);
        egt_image_free(&img);
        app->scaled_src = app->src->id;
        app->scaled_w = app->img_w;
        app->scaled_h = app->img_h;
        app->scaled_nearest = !app->area_downscale;
    }
    return &app->scaled;
}

static void draw_view(App *app)
{
    Rectangle a[2];
    Rectangle r = view_rect(app);
    int n = view_areas(app, a);
    Layer *out = app->output_valid ? &app->out : NULL;
    const char *before_label, *after_label;
    Layer *before;

    DrawRectangleRec(r, app->fullscreen ? BLACK : (Color){ 40, 40, 44, 255 });
    if (!app->src) {
        const char *msg = "Open, paste or drop an image here";
        draw_text(msg, (int)(r.x + (r.width - text_width(msg)) / 2),
                  (int)(r.height / 2 - FONT_SIZE / 2), GRAY);
        return;
    }
    /* Before: the image as prepared for conversion (resized, adjusted,
     * filtered). After: converted to the palette. */
    before = before_layer(app);
    before_label = TextFormat("%s | %dx%d",
                              app->before_kind == BEFORE_SCALED ? "Scaled"
                              : app->before_kind == BEFORE_ORIGINAL ? "Original" : "Prepared",
                              before->tex.id ? before->tex.width : app->img_w,
                              before->tex.id ? before->tex.height : app->img_h);
    after_label = TextFormat("Converted | %s | %s%s%s%s",
                             app->pal_index == PAL_LOADED ? app->loaded_pal_name
                                 : egt_builtin_palette_name((EgtBuiltinPalette)app->pal_index),
                             egt_dither_name((EgtDither)app->dither_index),
                             app->limit_index ? " | " : "",
                             app->limit_index ? egt_limit_name((EgtLimit)app->limit_index) : "",
                             show_working(app) ? " | working..." : "");
    if (n == 2) {
        DrawRectangle((int)a[0].x + (int)a[0].width, 0, 2, (int)r.height, BLACK);
        draw_image(app, before, a[0], before_label);
        draw_image(app, out, a[1], after_label);
    } else if (app->view_mode == VIEW_BEFORE && !app->fullscreen) {
        draw_image(app, before, a[0], before_label);
    } else {
        draw_image(app, out, a[0], after_label);
    }
}

static void toggle_fullscreen(App *app)
{
    app->fullscreen = !app->fullscreen;
    ToggleBorderlessWindowed();
    app->recenter = true;
}

/* ---- panel ---------------------------------------------------------------- */

/* Left edge of the column being laid out, for slider() and friends. */
static float col_x;

static bool slider(const char *label, float y, float *value, float lo, float hi,
                   const char *fmt, float shown)
{
    float old = *value;
    GuiLabel((Rectangle){ col_x, y, CTRL_W, 20 }, label);
    GuiSliderBar((Rectangle){ col_x, y + 22, SLIDER_W, 20 }, NULL,
                 TextFormat(fmt, shown), value, lo, hi);
    /* A mouse can't land exactly on 0 (one pixel is a whole step): within
     * a pixel of it counts as 0. */
    if (*value != old && lo < 0 && hi > 0 && fabsf(*value) < (hi - lo) / SLIDER_W)
        *value = 0;
    return old != *value;
}

/* Starts the next column: a titled frame, returns the first row's y. */
static float column(Rectangle view, float scroll_x, int index, const char *title)
{
    float x = view.x + scroll_x + PAD + index * (CTRL_W + 2 * PAD + COL_GAP);
    GuiGroupBox((Rectangle){ x - PAD / 2, view.y + 12, CTRL_W + PAD, PANEL_CONTENT_H - 18 }, title);
    col_x = x + 0;
    return view.y + 26;
}

static void draw_panel(App *app)
{
    bool dropdown_open = app->res_edit || app->dither_edit || app->pal_edit || app->limit_edit;
    float y, res_y, dither_y, pal_y, res_x, dither_x, pal_x, limit_x, limit_y;
    float half = (CTRL_W - 8) / 2.0f;
    Rectangle bounds = { 0, (float)(GetScreenHeight() - STATUS_H - PANEL_H),
                         (float)GetScreenWidth(), PANEL_H };
    Rectangle content = { 0, 0,
                          PANEL_COLS * (CTRL_W + 2 * PAD + COL_GAP) - COL_GAP + PAD, PANEL_H };
    Rectangle view;

    /* The wheel scrolls the panel sideways (raygui only does that with
     * Shift held). */
    if (!dropdown_open && CheckCollisionPointRec(GetMousePosition(), bounds))
        app->panel_scroll.x += GetMouseWheelMove() * 60;
    if (dropdown_open)
        GuiLock();
    content.height = bounds.height - SCROLLBAR_H; /* horizontal bar only */
    GuiScrollPanel(bounds, NULL, content, &app->panel_scroll, &view);
    app->panel_scroll.y = 0;
    view.y = bounds.y;
    view.height = PANEL_CONTENT_H;
    DrawLine(0, (int)bounds.y, GetScreenWidth(), (int)bounds.y, GRAY);
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);

    /* Image and view */
    y = column(view, app->panel_scroll.x, 0, "Image");
    if (GuiButton((Rectangle){ col_x, y, half, ROW_H }, "Open..."))
        open_image_dialog(app);
    if (GuiButton((Rectangle){ col_x + half + 8, y, half, ROW_H }, "Paste"))
        paste_image(app);
    y += ROW_H + 8;
    GuiLabel((Rectangle){ col_x, y, CTRL_W, 20 }, "Resolution");
    if (app->res_index == RES_ORIGINAL) {
        /* Downscale an image of any shape, keeping its proportions. */
        GuiLabel((Rectangle){ col_x + 96, y, 48, 20 }, "Scale");
        GuiSliderBar((Rectangle){ col_x + 146, y + 2, 58, 16 }, NULL,
                     TextFormat("%.0f%%", roundf(app->res_scale)), &app->res_scale, 5, 100);
    }
    res_x = col_x;
    res_y = y + 22;
    y += 22 + ROW_H + 10;
    {
        int mode = app->view_mode;
        GuiToggleGroup((Rectangle){ col_x, y, (CTRL_W - 2 * 2) / 3.0f, ROW_H },
                       "Before;After;Split", &mode);
        if (mode != app->view_mode)
            set_view_mode(app, mode);
    }
    y += ROW_H + 4;
    /* What Before shows: as converted from, only resized, or the source. */
    GuiToggleGroup((Rectangle){ col_x, y, (CTRL_W - 2 * 2) / 3.0f, ROW_H },
                   "Prepared;Scaled;Original", &app->before_kind);
    y += ROW_H + 8;
    {
        /* Toggles showing the state: Fit while fitting, and the zoom in
         * use (also the one Fit picked). Clicking an active one is a no-op. */
        static const int ZOOMS[4] = { 1, 2, 3, 4 };
        float qw = (CTRL_W - 5 * 4) / 6.0f;
        bool on = app->fit;
        int i;
        GuiToggle((Rectangle){ col_x, y, qw, ROW_H }, "Fit", &on);
        if (on && !app->fit)
            app->fit = true;
        for (i = 0; i < 4; i++) {
            on = app->zoom == ZOOMS[i];
            GuiToggle((Rectangle){ col_x + (i + 1) * (qw + 4), y, qw, ROW_H },
                      TextFormat("%dx", ZOOMS[i]), &on);
            /* Clicking the active one re-centres (and stops fitting). */
            if (on != (app->zoom == ZOOMS[i]))
                zoom_centered(app, ZOOMS[i]);
        }
        if (GuiButton((Rectangle){ col_x + 5 * (qw + 4), y, qw, ROW_H }, "Full"))
            toggle_fullscreen(app); /* also F11 */
    }
    y += ROW_H + 10;
    {
        /* Save and Copy, then the scale they export at. */
        float bw = 64, sw = (CTRL_W - 2 * bw - 4 - 8 - 3 * 2) / 4.0f;
        int scale = app->export_scale - 1;
        if (GuiButton((Rectangle){ col_x, y, bw, ROW_H }, "Save"))
            save_output_dialog(app);
        if (GuiButton((Rectangle){ col_x + bw + 4, y, bw, ROW_H }, "Copy"))
            copy_output(app);
        GuiToggleGroup((Rectangle){ col_x + 2 * bw + 12, y, sw, ROW_H }, "1x;2x;3x;4x", &scale);
        app->export_scale = scale + 1;
    }

    /* Colour */
    y = column(view, app->panel_scroll.x, 1, "Colour");
    GuiLabel((Rectangle){ col_x, y, CTRL_W - 96, 20 }, "Palette");
    if (GuiButton((Rectangle){ col_x + CTRL_W - 90, y - 3, 90, 24 }, "Load..."))
        open_palette_dialog(app);
    pal_x = col_x;
    pal_y = y + 24;
    y += 24 + ROW_H + 8;
    GuiLabel((Rectangle){ col_x, y, CTRL_W, 20 }, "Dither");
    if (app->dither_index == EGT_DITHER_EGA_CHECKER) {
        /* How checkerboards are judged: 0 = pixel values (close-up
         * look), 100 = emitted light (distant look). */
        GuiLabel((Rectangle){ col_x + 88, y, 60, 20 }, "Light");
        GuiSliderBar((Rectangle){ col_x + 150, y + 2, 80, 16 }, NULL,
                     TextFormat("%.0f", roundf(app->light_mix)), &app->light_mix, 0, 100);
    } else if (app->dither_index != EGT_DITHER_NONE) {
        /* Colours this close to a palette colour stay solid instead of
         * collecting stray dots (near-black skies, near-white walls). */
        GuiLabel((Rectangle){ col_x + 88, y, 60, 20 }, "Snap");
        GuiSliderBar((Rectangle){ col_x + 150, y + 2, 80, 16 }, NULL,
                     roundf(app->snap) > 0 ? TextFormat("%.0f", roundf(app->snap)) : "Off",
                     &app->snap, 0, 64);
    }
    dither_x = col_x;
    dither_y = y + 22;
    y += 22 + ROW_H + 8;
    if (app->dither_index != EGT_DITHER_EGA_CHECKER)
        GuiDisable();
    slider("Prefer solid colours", y, &app->prefer_solid, 0, 100, "%.0f", roundf(app->prefer_solid));
    y += 46;
    if (app->limit_index != EGT_LIMIT_NONE)
        GuiDisable(); /* the cells choose the colours */
    {
        int area = egt_harsh_mix_area((int)roundf(app->harsh_mix),
                                      app->img_w ? app->img_w : 640,
                                      app->img_w ? app->img_h : 480);
        const char *label = area == 0 ? "Calm harsh mixes"
                          : area < 0 ? "Calm harsh mixes: everywhere"
                                     : TextFormat("Calm harsh mixes: < %d px", area);
        slider(label, y, &app->harsh_mix, 0, 100,
                          area == 0 ? "Off" : "%.0f", roundf(app->harsh_mix));
    }
    GuiEnable();

    /* Adjustments */
    y = column(view, app->panel_scroll.x, 2, "Adjust");
    slider("Brightness", y, &app->brightness, -128, 127, "%.0f", app->brightness);
    y += 46;
    slider("Contrast", y, &app->contrast, -127, 127, "%.0f", roundf(app->contrast));
    y += 46;
    slider("Saturation", y, &app->saturation, -1.0f, 1.5f, "%+.2f", app->saturation);
    y += 46 + 8;
    if (GuiButton((Rectangle){ col_x, y, half, ROW_H }, "Auto") && app->src) {
        /* Sets the sliders, so the result can still be fine-tuned. */
        EgtAdjust a;
        egt_auto_levels(&app->src->img, &a);
        app->brightness = (float)a.brightness;
        app->contrast = (float)a.contrast;
    }
    if (GuiButton((Rectangle){ col_x + half + 8, y, half, ROW_H }, "Reset")) {
        app->brightness = app->contrast = app->saturation = 0.0f;
    }
    y += ROW_H + 10;
    {
        /* Make the largest flat areas exact palette colours (not for
         * checkerboard). The swatches show each area's colour and what it
         * became; "none" when no area qualifies. */
        bool checker = app->dither_index == EGT_DITHER_EGA_CHECKER;
        if (checker)
            GuiDisable();
        GuiCheckBox((Rectangle){ col_x, y + 4, 20, 20 }, "Solid areas", &app->calibrate);
        GuiEnable();
        if (app->calibrate && !checker && app->output_job.calibrate) {
            float sx = col_x + 152;
            int k;
            if (app->ncalib == 0)
                GuiLabel((Rectangle){ sx, y + 4, CTRL_W - 152, 20 }, "none found");
            for (k = 0; k < app->ncalib && sx + 28 <= col_x + CTRL_W; k++) {
                EgtCalibAnchor *a = &app->calib[k];
                DrawRectangle((int)sx, (int)y + 6, 14, 16, (Color){ a->from.r, a->from.g, a->from.b, 255 });
                DrawRectangle((int)sx + 14, (int)y + 6, 14, 16, (Color){ a->to.r, a->to.g, a->to.b, 255 });
                DrawRectangleLines((int)sx, (int)y + 6, 28, 16, GRAY);
                sx += 32;
            }
        }
        /* How big a colour change an area may get: at the top, any (a
         * blue sky may turn CGA red); lower keeps changes modest. */
        y += ROW_H + 2;
        if (checker || !app->calibrate)
            GuiDisable();
        GuiLabel((Rectangle){ col_x, y, 100, 20 }, "Max change");
        GuiSliderBar((Rectangle){ col_x + 104, y + 2, SLIDER_W - 104, 16 }, NULL,
                     roundf(app->calibrate_max) >= CAL_ANY ? "Any"
                         : TextFormat("%.0f", roundf(app->calibrate_max)),
                     &app->calibrate_max, 16, CAL_ANY);
        GuiEnable();
    }

    /* Smoothing */
    y = column(view, app->panel_scroll.x, 3, "Smoothing");
    {
        GuiCheckBox((Rectangle){ col_x, y + 4, 20, 20 }, "Smooth downscale", &app->area_downscale);
    }
    y += ROW_H + 6;
    {
        /* A checkbox plus the three methods: "Off" as a fourth button
         * leaves too little room for the names. */
        bool on = app->smooth != EGT_SMOOTH_NONE;
        int kind = app->smooth_kind - 1;
        GuiCheckBox((Rectangle){ col_x, y + 2, 20, 20 }, "Smoothing", &on);
        GuiToggleGroup((Rectangle){ col_x, y + 26, (CTRL_W - 2 * 2) / 3.0f, ROW_H },
                       "Median;Paint;Surface", &kind);
        if (kind + 1 != app->smooth_kind)
            on = true; /* picking a method turns smoothing on */
        app->smooth_kind = kind + 1;
        app->smooth = on ? app->smooth_kind : EGT_SMOOTH_NONE;
    }
    y += 26 + ROW_H + 8;
    if (app->smooth == EGT_SMOOTH_NONE)
        GuiDisable();
    slider("Smoothing radius", y, &app->smooth_radius, 0.25f, 4, "%.2f",
           egt_snap_radius(app->smooth_radius));
    GuiEnable();
    y += 46;
    if (app->smooth != EGT_SMOOTH_SURFACE)
        GuiDisable();
    slider("Surface threshold", y, &app->smooth_threshold, 1, 100, "%.0f",
           roundf(app->smooth_threshold));
    GuiEnable();

    /* Outlines and cleanup */
    y = column(view, app->panel_scroll.x, 4, "Outlines and cleanup");
    slider("Outlines", y, &app->outline, 0, 100,
                      roundf(app->outline) > 0 ? "%.0f" : "Off", roundf(app->outline));
    GuiToggleGroup((Rectangle){ col_x + CTRL_W - 2 * 76 - 2, y - 3, 76, 24 }, "Basic;Hi-res",
                   &app->outline_mode);
    y += 46;
    if (roundf(app->outline) == 0)
        GuiDisable();
    slider("Outline darkness", y, &app->outline_dark, 0, 1, "%.2f",
                      app->outline_dark);
    y += 46;
    slider("Min line length", y, &app->outline_min, 0, 50,
                      roundf(app->outline_min) > 1 ? "%.0f" : "Off", roundf(app->outline_min));
    GuiEnable();
    y += 46;
    {
        bool cleanup = egt_dither_supports_cleanup((EgtDither)app->dither_index) &&
                       app->limit_index == EGT_LIMIT_NONE;
        if (!cleanup)
            GuiDisable();
        slider(cleanup ? "Clean up regions under"
                       : app->limit_index ? "Clean up (not with limits)"
                                          : "Clean up (not with this dither)",
                      y, &app->min_region, 0, 64,
                      roundf(app->min_region) > 1 ? "%.0f" : "Off", roundf(app->min_region));
    }
    GuiEnable();
    y += 46 + 8;
    if (GuiButton((Rectangle){ col_x, y, CTRL_W, ROW_H }, "Reset filters")) {
        app->area_downscale = true;
        app->smooth = EGT_SMOOTH_NONE;
        app->smooth_radius = 1;
        app->smooth_threshold = 25;
        app->outline = 0;
        app->outline_dark = 1;
        app->outline_min = 0;
        app->outline_mode = EGT_OUTLINE_BASIC;
        app->min_region = 0;
    }

    /* Hardware colour limits */
    y = column(view, app->panel_scroll.x, 5, "Hardware");
    GuiLabel((Rectangle){ col_x, y, CTRL_W, 20 }, "Colour limits");
    limit_x = col_x;
    limit_y = y + 22;
    y += 22 + ROW_H + 8;
    {
        /* The rule, on up to two lines. */
        char desc[128], *nl;
        snprintf(desc, sizeof desc, "%s", egt_limit_desc((EgtLimit)app->limit_index));
        nl = strchr(desc, '\n');
        if (nl)
            *nl = '\0';
        GuiLabel((Rectangle){ col_x, y, CTRL_W, 18 }, desc);
        if (nl)
            GuiLabel((Rectangle){ col_x, y + 18, CTRL_W, 18 }, nl + 1);
    }
    y += 2 * 18 + 10;
    if (app->limit_index == EGT_LIMIT_NONE)
        GuiDisable();
    slider("Cell consistency", y, &app->consistency, 0, 300, "%.0f", roundf(app->consistency));
    GuiEnable();
    EndScissorMode();

    /* Status bar: messages on the left, image info on the right. */
    {
        Rectangle bar = { 0, (float)(GetScreenHeight() - STATUS_H),
                          (float)GetScreenWidth(), STATUS_H };
        GuiStatusBar(bar, app->status);
        if (app->img_w || app->busy) {
            int align = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_RIGHT);
            if (show_working(app)) {
                /* Working indicator: a dot running along a short track. */
                static const char *FRAMES[] = { "o...", ".o..", "..o.", "...o", "..o.", ".o.." };
                GuiLabel((Rectangle){ bar.x, bar.y, bar.width - PAD, bar.height },
                         TextFormat("Working %s", FRAMES[(int)(GetTime() * 8) % 6]));
            } else {
                GuiLabel((Rectangle){ bar.x, bar.y, bar.width - PAD, bar.height },
                         TextFormat("%dx%d  %.0f ms", app->img_w, app->img_h, app->convert_ms));
            }
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, align);
        }
    }

    /* Dropdowns last and unclipped so their open lists (rolling up over the
     * image) draw over everything: the closed ones first, then the open one
     * on top, with the others locked so clicks on its list don't leak
     * through. */
    {
        char pal_items[512];
        int k, pass;
        pal_items[0] = '\0';
        for (k = 0; k < EGT_PAL_COUNT; k++)
            snprintf(pal_items + strlen(pal_items), sizeof pal_items - strlen(pal_items),
                     "%s%s", k ? ";" : "", egt_builtin_palette_name((EgtBuiltinPalette)k));
        if (app->have_loaded_pal)
            snprintf(pal_items + strlen(pal_items), sizeof pal_items - strlen(pal_items),
                     ";File: %s", app->loaded_pal_name);
        char limit_items[256];
        int old_limit = app->limit_index;
        bool was_open[4] = { app->res_edit, app->dither_edit, app->pal_edit, app->limit_edit };
        bool *edits[4] = { &app->res_edit, &app->dither_edit, &app->pal_edit, &app->limit_edit };
        int *indices[4] = { &app->res_index, &app->dither_index, &app->pal_index, &app->limit_index };
        float xs[4] = { res_x, dither_x, pal_x, limit_x }, ys[4] = { res_y, dither_y, pal_y, limit_y };
        const char *lists[4];
        limit_items[0] = '\0';
        for (k = 0; k < EGT_LIMIT_COUNT; k++)
            snprintf(limit_items + strlen(limit_items), sizeof limit_items - strlen(limit_items),
                     "%s%s", k ? ";" : "", egt_limit_name((EgtLimit)k));
        lists[0] = RES_ITEMS;
        lists[1] = DITHER_ITEMS;
        lists[2] = pal_items;
        lists[3] = limit_items;
        for (pass = 0; pass < 2; pass++) {
            for (k = 0; k < 4; k++) {
                bool *edit = edits[k];
                int *index = indices[k];
                Rectangle r = { xs[k], ys[k], CTRL_W, ROW_H };
                const char *items = lists[k];
                if (was_open[k] != (pass == 1))
                    continue;
                if (*edit) {
                    /* The list rolls up from the box: in a short window,
                     * shrink the rows (box included) so every entry fits. */
                    int n = 1, spacing = GuiGetStyle(DROPDOWNBOX, DROPDOWN_ITEMS_SPACING);
                    const char *c;
                    float room = r.y + r.height - 4, h;
                    for (c = items; *c; c++)
                        n += *c == ';';
                    h = room / (n + 1) - spacing;
                    if (h < r.height) {
                        if (h < 14)
                            h = 14;
                        r.y += r.height - h;
                        r.height = h;
                    }
                }
                if (dropdown_open && !*edit)
                    GuiLock();
                if (GuiDropdownBox(r, items, index, *edit))
                    *edit = !*edit;
                GuiUnlock();
            }
        }
        /* Holding Shift changes only the limit (a hidden shortcut). */
        if (app->limit_index != old_limit && app->limit_index != EGT_LIMIT_NONE &&
            !IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT)) {
            /* Picking a machine also picks its palette and screen size;
             * either can be changed afterwards (any palette works). */
            EgtBuiltinPalette pal;
            int w, h, i;
            egt_limit_machine((EgtLimit)app->limit_index, &pal, &w, &h);
            app->pal_index = (int)pal;
            /* Original (with its scale) is a deliberate choice: keep it. */
            for (i = 0; i < RES_ORIGINAL && app->res_index != RES_ORIGINAL; i++)
                if (RES_SIZES[i][0] == w && RES_SIZES[i][1] == h)
                    app->res_index = i;
        }
    }
}

/* ---- main ----------------------------------------------------------------- */

static bool key_step(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

/* Arrow keys move the picture 12% of its shown size that way. Both sides
 * share the pan, so a bigger Original moves by 12% of its own size. */
static void pan_keys(App *app)
{
    float sx = roundf(0.12f * (float)(app->img_w * app->zoom));
    float sy = roundf(0.12f * (float)(app->img_h * app->zoom));
    float dx = 0, dy = 0;
    if (!app->img_w)
        return;
    if (key_step(KEY_LEFT))
        dx -= sx;
    if (key_step(KEY_RIGHT))
        dx += sx;
    if (key_step(KEY_UP))
        dy -= sy;
    if (key_step(KEY_DOWN))
        dy += sy;
    if (dx != 0 || dy != 0) {
        app->offset.x += dx;
        app->offset.y += dy;
        app->fit = false;
    }
}

static void handle_shortcuts(App *app)
{
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (IsKeyPressed(KEY_F11) || (app->fullscreen && IsKeyPressed(KEY_ESCAPE)))
        toggle_fullscreen(app);
    if (ctrl && IsKeyPressed(KEY_O))
        open_image_dialog(app);
    if (ctrl && IsKeyPressed(KEY_V))
        paste_image(app);
    if (ctrl && IsKeyPressed(KEY_S))
        save_output_dialog(app);
    if (ctrl && IsKeyPressed(KEY_C))
        copy_output(app);
    if (!ctrl) {
        if (IsKeyPressed(KEY_F))
            app->fit = true;
        if (IsKeyPressed(KEY_ONE))
            zoom_centered(app, 1);
        if (IsKeyPressed(KEY_TWO))
            zoom_centered(app, 2);
        if (IsKeyPressed(KEY_THREE))
            zoom_centered(app, 3);
        if (IsKeyPressed(KEY_FOUR))
            zoom_centered(app, 4);
        if (IsKeyPressed(KEY_TAB))
            set_view_mode(app, (app->view_mode + 1) % 3);
        pan_keys(app);
    }
}

static void handle_dropped_files(App *app)
{
    FilePathList files;
    unsigned int i;
    if (!IsFileDropped())
        return;
    files = LoadDroppedFiles();
    for (i = 0; i < files.count; i++) {
        if (has_ext(files.paths[i], ".pal"))
            load_palette_file(app, files.paths[i]);
        else
            load_image_file(app, files.paths[i]);
    }
    UnloadDroppedFiles(files);
}

int main(int argc, char **argv)
{
    static App app, before;

    app.res_index = RES_DEFAULT;
    app.res_scale = 100;
    app.consistency = 30;
    app.calibrate_max = CAL_ANY;
    app.pal_index = EGT_PAL_EGA;
    app.dither_index = EGT_DITHER_EGA_CHECKER;
    app.prefer_solid = EGT_PREFER_SOLID_DEFAULT;
    app.harsh_mix = 10;
    app.snap = 16;
    app.view_mode = VIEW_SPLIT;
    app.fit = true;
    app.area_downscale = true;
    app.smooth_radius = 1;
    app.smooth_threshold = 25;
    app.smooth_kind = EGT_SMOOTH_SURFACE;
    app.outline_dark = 1;
    app.zoom = 1;
    app.export_scale = 1;
    set_status(&app, "Ready");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    /* Wide enough for every panel column, but within the screen. */
    InitWindow(1720, 960, "EGAtool " EGT_VERSION);
    {
        /* Title bar and taskbar icon (the .exe resource only covers Explorer). */
        Image icon = { ICON_RGBA, ICON_W, ICON_H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        SetWindowIcon(icon);
    }
    {
        int m = GetCurrentMonitor(), mw = GetMonitorWidth(m), mh = GetMonitorHeight(m);
        int w = 1720, h = 960;
        if (mw > 0 && mh > 0 && (w > mw * 95 / 100 || h > mh * 90 / 100)) {
            w = w < mw * 95 / 100 ? w : mw * 95 / 100;
            h = h < mh * 90 / 100 ? h : mh * 90 / 100;
            SetWindowSize(w, h);
            SetWindowPosition((mw - w) / 2, (mh - h) / 2);
        }
    }
    SetWindowMinSize(1024, 700);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    ui_font = LoadFontFromMemory(".ttf", amiga_topaz_ttf, (int)amiga_topaz_ttf_len, FONT_SIZE, NULL, 0);
    SetTextureFilter(ui_font.texture, TEXTURE_FILTER_POINT);
    GuiSetFont(ui_font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, FONT_SIZE);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(DROPDOWNBOX, DROPDOWN_ROLL_UP, 1); /* the panel is at the bottom */

    app.worker = calloc(1, sizeof *app.worker);
    if (!app.worker || !worker_start(app.worker)) {
        fprintf(stderr, "cannot start the worker thread\n");
        return 1;
    }
    if (argc > 1)
        load_image_file(&app, argv[1]);

    while (!WindowShouldClose()) {
        handle_dropped_files(&app);
        handle_shortcuts(&app);
        if (!app.res_edit && !app.dither_edit && !app.pal_edit && !app.limit_edit)
            handle_view_input(&app);

        pipeline_tick(&app);
        /* Fit mode persists (tracking window resizes) until the user zooms or pans. */
        if (app.fit)
            fit_view(&app);
        else if (app.recenter)
            center_view(&app);
        app.recenter = false;

        BeginDrawing();
        ClearBackground(BLACK);
        draw_view(&app);
        before = app;
        if (!app.fullscreen)
            draw_panel(&app);
        /* Redraw only on input (this is a tool, not a game). The panel acts
         * after the view is drawn, though, so if it changed anything
         * run the next frame right away. */
        if (memcmp(&before, &app, sizeof app) != 0 || app.busy)
            DisableEventWaiting(); /* busy: animate and watch for the result */
        else
            EnableEventWaiting();
        EndDrawing();
    }

    unload_layer(&app.in);
    unload_layer(&app.out);
    unload_layer(&app.scaled);
    unload_layer(&app.orig);
    UnloadFont(ui_font);
    CloseWindow();
    worker_stop(app.worker);
    if (app.src && --app.src->refs == 0) {
        egt_image_free(&app.src->img);
        free(app.src);
    }
    free(app.worker);
    egt_image_free(&app.output);
    return 0;
}
