// SPDX-License-Identifier: GPL-2.0-or-later
/* pps-annotation-layer-pixelize.c
 *  this file is part of papers, a gnome document viewer
 *
 * Copyright (C) 2026
 *
 * Implements a drag-to-draw pixelize annotation tool. Works like the shape
 * (line) tool — the user click-drags to define a rectangular region —
 * but instead of drawing ink the region is rendered as a blocky mosaic
 * ("pixelation") of the underlying page content.
 *
 * Pixelized regions are stored in memory only (not persisted to the PDF in
 * this implementation).  The page texture is sourced from PpsPixbufCache,
 * which already holds the rendered page image used for normal display.
 *
 * Rendering uses a two-pass Cairo approach:
 *   Pass 1 – downsample the clip region to a tiny surface (bilinear)
 *   Pass 2 – scale back up with CAIRO_FILTER_NEAREST → clean pixel blocks
 */

#include "pps-annotation-layer-pixelize.h"

#include <cairo.h>
#include <math.h>

#include "pps-pixbuf-cache.h"

/* Width/height of each "big pixel" block in widget pixels. */
#define PIXEL_SIZE 10.0

/* Minimum drag distance to commit a pixelize region. */
#define MIN_DRAG_PX 4.0

/* A committed pixelize region, stored in document coordinates so it survives
 * zoom/pan changes. */
typedef struct {
	gdouble x1, y1; /* document coords, PDF space (Y up) */
	gdouble x2, y2;
	gint    page;
} PixelizeRegion;

typedef struct {
	gdouble        drag_start_x;
	gdouble        drag_start_y;
	gdouble        drag_offset_x;
	gdouble        drag_offset_y;
	gboolean       dragging;

	GArray        *regions;      /* array of PixelizeRegion, doc coords */
	PpsPixbufCache *pixbuf_cache; /* borrowed reference */
} PpsAnnotationLayerPixelizePrivate;

struct _PpsAnnotationLayerPixelize {
	PpsAnnotationLayer base_instance;
};

G_DEFINE_TYPE_WITH_PRIVATE (PpsAnnotationLayerPixelize, pps_annotation_layer_pixelize, PPS_TYPE_ANNOTATION_LAYER)
#define PIXELIZE_GET_PRIVATE(o) pps_annotation_layer_pixelize_get_instance_private (o)

#define GET_DOC_MODEL(d)   pps_annotation_layer_get_doc_model (PPS_ANNOTATION_LAYER (d))

/* ── Pixelation rendering ───────────────────────────────────────────────── */

/*
 * apply_pixelize_region:
 *
 * Renders a mosaic (pixelation) effect over the page content in the
 * given widget-space rectangle.
 *
 * Two-pass algorithm:
 *   1. Downsample the texture sub-region corresponding to [wx1,wy1]×[wx2,wy2]
 *      into a tiny surface of size (ceil(W/PIXEL_SIZE) × ceil(H/PIXEL_SIZE))
 *      using bilinear interpolation.
 *   2. Scale that tiny surface back up using CAIRO_FILTER_NEAREST so each
 *      small pixel expands into a solid PIXEL_SIZE×PIXEL_SIZE block.
 */
static void
apply_pixelize_region (GtkSnapshot *snapshot,
                       GdkTexture  *page_tex,
                       int          widget_w,
                       int          widget_h,
                       gdouble      wx1,
                       gdouble      wy1,
                       gdouble      wx2,
                       gdouble      wy2)
{
	gdouble clip_x = MIN (wx1, wx2);
	gdouble clip_y = MIN (wy1, wy2);
	gdouble clip_w = fabs (wx2 - wx1);
	gdouble clip_h = fabs (wy2 - wy1);

	if (clip_w < 1.0 || clip_h < 1.0)
		return;

	/* ── Texture → pixel data ─────────────────────────────────────────── */
	int tex_w = gdk_texture_get_width (page_tex);
	int tex_h = gdk_texture_get_height (page_tex);

	guchar *data = g_malloc (tex_w * tex_h * 4);
	gdk_texture_download (page_tex, data, tex_w * 4);

	cairo_surface_t *src_surf = cairo_image_surface_create_for_data (
	    data, CAIRO_FORMAT_ARGB32, tex_w, tex_h, tex_w * 4);

	/* ── Pass 1: downsample clip region into a tiny pix_surf ─────────── */
	int pix_w = MAX (1, (int) ceil (clip_w / PIXEL_SIZE));
	int pix_h = MAX (1, (int) ceil (clip_h / PIXEL_SIZE));

	cairo_surface_t *pix_surf = cairo_image_surface_create (
	    CAIRO_FORMAT_ARGB32, pix_w, pix_h);
	cairo_t *pix_cr = cairo_create (pix_surf);

	/* Texture sub-region corresponding to the widget clip region */
	gdouble tcx = clip_x * tex_w / widget_w;
	gdouble tcy = clip_y * tex_h / widget_h;
	gdouble tcw = clip_w * tex_w / widget_w;
	gdouble tch = clip_h * tex_h / widget_h;

	cairo_scale (pix_cr, pix_w / tcw, pix_h / tch);
	cairo_set_source_surface (pix_cr, src_surf, -tcx, -tcy);
	cairo_pattern_set_filter (cairo_get_source (pix_cr), CAIRO_FILTER_BILINEAR);
	cairo_paint (pix_cr);
	cairo_destroy (pix_cr);

	/* ── Pass 2: scale back up with NEAREST filter → pixel blocks ──────
	 *
	 * gtk_snapshot_append_cairo uses WIDGET-SPACE coordinates: (0,0) in
	 * the returned cairo_t is the widget's top-left corner, not the clip
	 * origin.  The pattern matrix maps widget coords → pix_surf coords:
	 *
	 *   pix_surf_x = (widget_x - clip_x) / PIXEL_SIZE
	 *   pix_surf_y = (widget_y - clip_y) / PIXEL_SIZE
	 *
	 * cairo_matrix_translate post-multiplies, so:
	 *   init_scale(1/S) * translate(-clip_x, -clip_y)
	 * gives mat*(x,y) = ((x - clip_x)/S, (y - clip_y)/S)  ✓
	 */
	graphene_rect_t cairo_rect = GRAPHENE_RECT_INIT (
	    (float) clip_x, (float) clip_y, (float) clip_w, (float) clip_h);
	cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &cairo_rect);

	cairo_pattern_t *pat = cairo_pattern_create_for_surface (pix_surf);
	cairo_matrix_t mat;
	cairo_matrix_init_scale (&mat, 1.0 / PIXEL_SIZE, 1.0 / PIXEL_SIZE);
	cairo_matrix_translate (&mat, -clip_x, -clip_y);
	cairo_pattern_set_matrix (pat, &mat);
	cairo_pattern_set_filter (pat, CAIRO_FILTER_NEAREST);
	cairo_set_source (cr, pat);
	cairo_paint (cr);

	cairo_pattern_destroy (pat);
	cairo_destroy (cr);
	cairo_surface_destroy (pix_surf);
	cairo_surface_destroy (src_surf);
	g_free (data);
}

/* ── Snapshot ───────────────────────────────────────────────────────────── */

static void
pps_annotation_layer_pixelize_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
	PpsAnnotationLayerPixelize        *self = PPS_ANNOTATION_LAYER_PIXELIZE (widget);
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);

	if (!priv->dragging && priv->regions->len == 0)
		return;

	gint page_index = pps_annotation_layer_get_page (PPS_ANNOTATION_LAYER (self));

	GdkTexture *page_tex = pps_pixbuf_cache_get_texture (priv->pixbuf_cache, page_index);
	if (!page_tex)
		return;

	int widget_w = gtk_widget_get_width  (widget);
	int widget_h = gtk_widget_get_height (widget);

	/* Live drag preview */
	if (priv->dragging) {
		apply_pixelize_region (snapshot, page_tex, widget_w, widget_h,
		                       priv->drag_start_x,
		                       priv->drag_start_y,
		                       priv->drag_start_x + priv->drag_offset_x,
		                       priv->drag_start_y + priv->drag_offset_y);
	}

	/* Committed regions on this page */
	if (priv->regions->len > 0) {
		gdouble scale = pps_document_model_get_scale (GET_DOC_MODEL (self));

		gdouble page_height = 0.0;
		PpsDocument *doc = pps_document_model_get_document (GET_DOC_MODEL (self));
		pps_document_get_page_size (doc, page_index, NULL, &page_height);

		for (guint i = 0; i < priv->regions->len; i++) {
			PixelizeRegion *r = &g_array_index (priv->regions, PixelizeRegion, i);
			if (r->page != page_index)
				continue;

			/* doc → widget: wx = dx*scale, wy = (page_height - dy)*scale */
			gdouble wx1 = r->x1 * scale;
			gdouble wy1 = (page_height - r->y1) * scale;
			gdouble wx2 = r->x2 * scale;
			gdouble wy2 = (page_height - r->y2) * scale;

			apply_pixelize_region (snapshot, page_tex, widget_w, widget_h,
			                       wx1, wy1, wx2, wy2);
		}
	}
}

/* ── Drag gesture callbacks ─────────────────────────────────────────────── */

static void
on_drag_begin (GtkGestureDrag             *gesture,
               gdouble                     x,
               gdouble                     y,
               PpsAnnotationLayerPixelize *self)
{
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);
	gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

	priv->drag_start_x  = x;
	priv->drag_start_y  = y;
	priv->drag_offset_x = 0.0;
	priv->drag_offset_y = 0.0;
	priv->dragging      = TRUE;

	gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_update (GtkGestureDrag             *gesture,
                gdouble                     offset_x,
                gdouble                     offset_y,
                PpsAnnotationLayerPixelize *self)
{
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);
	priv->drag_offset_x = offset_x;
	priv->drag_offset_y = offset_y;
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag             *gesture,
             gdouble                     offset_x,
             gdouble                     offset_y,
             PpsAnnotationLayerPixelize *self)
{
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);

	priv->dragging = FALSE;
	gtk_widget_queue_draw (GTK_WIDGET (self));

	gdouble wx1 = priv->drag_start_x;
	gdouble wy1 = priv->drag_start_y;
	gdouble wx2 = wx1 + offset_x;
	gdouble wy2 = wy1 + offset_y;

	if (fabs (wx2 - wx1) < MIN_DRAG_PX && fabs (wy2 - wy1) < MIN_DRAG_PX)
		return;

	/* Convert widget → document coordinates (PDF Y is flipped) */
	gdouble scale = pps_document_model_get_scale (GET_DOC_MODEL (self));
	gint    page  = pps_annotation_layer_get_page (PPS_ANNOTATION_LAYER (self));

	gdouble page_height = 0.0;
	PpsDocument *doc = pps_document_model_get_document (GET_DOC_MODEL (self));
	pps_document_get_page_size (doc, page, NULL, &page_height);

	PixelizeRegion r = {
		.x1   = wx1 / scale,
		.y1   = page_height - wy1 / scale,
		.x2   = wx2 / scale,
		.y2   = page_height - wy2 / scale,
		.page = page,
	};

	g_array_append_val (priv->regions, r);
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ── GObject boilerplate ────────────────────────────────────────────────── */

static void
pps_annotation_layer_pixelize_finalize (GObject *object)
{
	PpsAnnotationLayerPixelize        *self = PPS_ANNOTATION_LAYER_PIXELIZE (object);
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);

	g_array_unref (priv->regions);

	G_OBJECT_CLASS (pps_annotation_layer_pixelize_parent_class)->finalize (object);
}

static void
pps_annotation_layer_pixelize_init (PpsAnnotationLayerPixelize *self)
{
	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (self);

	priv->dragging = FALSE;
	priv->regions  = g_array_new (FALSE, FALSE, sizeof (PixelizeRegion));

	gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "crosshair");

	GtkGestureDrag *drag = GTK_GESTURE_DRAG (gtk_gesture_drag_new ());
	gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 1);

	g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
	g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
	g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);

	gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
}

static void
pps_annotation_layer_pixelize_class_init (PpsAnnotationLayerPixelizeClass *klass)
{
	GObjectClass   *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->finalize = pps_annotation_layer_pixelize_finalize;
	widget_class->snapshot = pps_annotation_layer_pixelize_snapshot;
}

/**
 * pps_annotation_layer_pixelize_new:
 * @document: the #PpsDocument being viewed
 * @model: the #PpsDocumentModel
 * @annotations_context: the #PpsAnnotationsContext
 * @pixbuf_cache: the #PpsPixbufCache that holds rendered page textures
 *
 * Creates a new pixelize annotation layer.  The layer renders a blocky mosaic
 * of the page content over regions the user drags with the pixelize tool.
 *
 * Returns: (transfer full): a new #GtkWidget
 */
GtkWidget *
pps_annotation_layer_pixelize_new (PpsDocument           *document,
                                    PpsDocumentModel      *model,
                                    PpsAnnotationsContext *annotations_context,
                                    PpsPixbufCache        *pixbuf_cache)
{
	g_return_val_if_fail (PPS_IS_DOCUMENT (document), NULL);
	g_return_val_if_fail (PPS_IS_PIXBUF_CACHE (pixbuf_cache), NULL);

	PpsAnnotationLayerPixelize *layer = g_object_new (
	    PPS_TYPE_ANNOTATION_LAYER_PIXELIZE,
	    "model",               model,
	    "annotations-context", annotations_context,
	    NULL);

	PpsAnnotationLayerPixelizePrivate *priv = PIXELIZE_GET_PRIVATE (layer);
	priv->pixbuf_cache = pixbuf_cache;

	return GTK_WIDGET (layer);
}
