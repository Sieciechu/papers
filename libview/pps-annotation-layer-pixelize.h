// SPDX-License-Identifier: GPL-2.0-or-later
/* pps-annotation-layer-pixelize.h
 *  this file is part of papers, a gnome document viewer
 *
 * Copyright (C) 2026
 */
#pragma once

#if !defined(PAPERS_COMPILATION)
#error "This is a private header."
#endif

#include "pps-annotation-layer.h"
#include "pps-pixbuf-cache.h"

#define PPS_TYPE_ANNOTATION_LAYER_PIXELIZE (pps_annotation_layer_pixelize_get_type ())
G_DECLARE_FINAL_TYPE (PpsAnnotationLayerPixelize, pps_annotation_layer_pixelize, PPS, ANNOTATION_LAYER_PIXELIZE, PpsAnnotationLayer)

GtkWidget *pps_annotation_layer_pixelize_new (PpsDocument           *document,
                                               PpsDocumentModel      *model,
                                               PpsAnnotationsContext *annotations_context,
                                               PpsPixbufCache        *pixbuf_cache);
