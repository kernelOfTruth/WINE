/*
 *    Text format and layout
 *
 * Copyright 2012, 2014-2015 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "dwrite_private.h"
#include "scripts.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

struct dwrite_textformat_data {
    WCHAR *family_name;
    UINT32 family_len;
    WCHAR *locale;
    UINT32 locale_len;

    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    DWRITE_FONT_STRETCH stretch;

    DWRITE_PARAGRAPH_ALIGNMENT paralign;
    DWRITE_READING_DIRECTION readingdir;
    DWRITE_WORD_WRAPPING wrapping;
    DWRITE_TEXT_ALIGNMENT textalignment;
    DWRITE_FLOW_DIRECTION flow;
    DWRITE_LINE_SPACING_METHOD spacingmethod;
    DWRITE_VERTICAL_GLYPH_ORIENTATION vertical_orientation;

    FLOAT spacing;
    FLOAT baseline;
    FLOAT fontsize;

    DWRITE_TRIMMING trimming;
    IDWriteInlineObject *trimmingsign;

    IDWriteFontCollection *collection;
    IDWriteFontFallback *fallback;
};

enum layout_range_attr_kind {
    LAYOUT_RANGE_ATTR_WEIGHT,
    LAYOUT_RANGE_ATTR_STYLE,
    LAYOUT_RANGE_ATTR_STRETCH,
    LAYOUT_RANGE_ATTR_FONTSIZE,
    LAYOUT_RANGE_ATTR_EFFECT,
    LAYOUT_RANGE_ATTR_INLINE,
    LAYOUT_RANGE_ATTR_UNDERLINE,
    LAYOUT_RANGE_ATTR_STRIKETHROUGH,
    LAYOUT_RANGE_ATTR_PAIR_KERNING,
    LAYOUT_RANGE_ATTR_FONTCOLL,
    LAYOUT_RANGE_ATTR_LOCALE,
    LAYOUT_RANGE_ATTR_FONTFAMILY,
    LAYOUT_RANGE_ATTR_SPACING
};

struct layout_range_attr_value {
    DWRITE_TEXT_RANGE range;
    union {
        DWRITE_FONT_WEIGHT weight;
        DWRITE_FONT_STYLE style;
        DWRITE_FONT_STRETCH stretch;
        FLOAT fontsize;
        IDWriteInlineObject *object;
        IUnknown *effect;
        BOOL underline;
        BOOL strikethrough;
        BOOL pair_kerning;
        IDWriteFontCollection *collection;
        const WCHAR *locale;
        const WCHAR *fontfamily;
        FLOAT spacing[3]; /* in arguments order - leading, trailing, advance */
    } u;
};

enum layout_range_kind {
    LAYOUT_RANGE_REGULAR,
    LAYOUT_RANGE_STRIKETHROUGH,
    LAYOUT_RANGE_EFFECT,
    LAYOUT_RANGE_SPACING
};

struct layout_range_header {
    struct list entry;
    enum layout_range_kind kind;
    DWRITE_TEXT_RANGE range;
};

struct layout_range {
    struct layout_range_header h;
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    FLOAT fontsize;
    DWRITE_FONT_STRETCH stretch;
    IDWriteInlineObject *object;
    BOOL underline;
    BOOL pair_kerning;
    IDWriteFontCollection *collection;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    WCHAR *fontfamily;
};

struct layout_range_bool {
    struct layout_range_header h;
    BOOL value;
};

struct layout_range_effect {
    struct layout_range_header h;
    IUnknown *effect;
};

struct layout_range_spacing {
    struct layout_range_header h;
    FLOAT leading;
    FLOAT trailing;
    FLOAT min_advance;
};

enum layout_run_kind {
    LAYOUT_RUN_REGULAR,
    LAYOUT_RUN_INLINE
};

struct inline_object_run {
    IDWriteInlineObject *object;
    UINT16 length;
};

struct regular_layout_run {
    DWRITE_GLYPH_RUN_DESCRIPTION descr;
    DWRITE_GLYPH_RUN run;
    DWRITE_SCRIPT_ANALYSIS sa;
    UINT16 *glyphs;
    UINT16 *clustermap;
    FLOAT  *advances;
    DWRITE_GLYPH_OFFSET *offsets;
    /* this is actual glyph count after shaping, it's not necessary the same as reported to Draw() */
    UINT32 glyphcount;
};

struct layout_run {
    struct list entry;
    enum layout_run_kind kind;
    union {
        struct inline_object_run object;
        struct regular_layout_run regular;
    } u;
    FLOAT baseline;
    FLOAT height;
};

struct layout_effective_run {
    struct list entry;
    const struct layout_run *run; /* nominal run this one is based on */
    UINT32 start;           /* relative text position, 0 means first text position of a nominal run */
    UINT32 length;          /* length in codepoints that this run covers */
    UINT32 glyphcount;      /* total glyph count in this run */
    FLOAT origin_x;         /* baseline X position */
    FLOAT origin_y;         /* baseline Y position */
    FLOAT align_dx;         /* adjustment from text alignment */
    FLOAT width;            /* run width */
    UINT16 *clustermap;     /* effective clustermap, allocated separately, is not reused from nominal map */
    UINT32 line;
};

struct layout_effective_inline {
    struct list entry;
    IDWriteInlineObject *object;
    IUnknown *effect;
    FLOAT origin_x;
    FLOAT origin_y;
    FLOAT align_dx;
    FLOAT width;
    BOOL  is_sideways;
    BOOL  is_rtl;
    UINT32 line;
};

struct layout_strikethrough {
    struct list entry;
    const struct layout_effective_run *run;
    DWRITE_STRIKETHROUGH s;
};

struct layout_cluster {
    const struct layout_run *run; /* link to nominal run this cluster belongs to */
    UINT32 position;        /* relative to run, first cluster has 0 position */
};

enum layout_recompute_mask {
    RECOMPUTE_NOMINAL_RUNS   = 1 << 0,
    RECOMPUTE_MINIMAL_WIDTH  = 1 << 1,
    RECOMPUTE_EFFECTIVE_RUNS = 1 << 2,
    RECOMPUTE_EVERYTHING     = 0xffff
};

struct dwrite_textlayout {
    IDWriteTextLayout2 IDWriteTextLayout2_iface;
    IDWriteTextFormat1 IDWriteTextFormat1_iface;
    IDWriteTextAnalysisSink IDWriteTextAnalysisSink_iface;
    IDWriteTextAnalysisSource IDWriteTextAnalysisSource_iface;
    LONG ref;

    WCHAR *str;
    UINT32 len;
    struct dwrite_textformat_data format;
    struct list strike_ranges;
    struct list effects;
    struct list spacing;
    struct list ranges;
    struct list runs;
    /* lists ready to use by Draw() */
    struct list eruns;
    struct list inlineobjects;
    struct list strikethrough;
    USHORT recompute;

    DWRITE_LINE_BREAKPOINT *nominal_breakpoints;
    DWRITE_LINE_BREAKPOINT *actual_breakpoints;

    struct layout_cluster *clusters;
    DWRITE_CLUSTER_METRICS *clustermetrics;
    UINT32 cluster_count;
    FLOAT  minwidth;

    DWRITE_LINE_METRICS *lines;
    UINT32 line_alloc;

    DWRITE_TEXT_METRICS1 metrics;

    /* gdi-compatible layout specifics */
    BOOL   gdicompatible;
    FLOAT  pixels_per_dip;
    BOOL   use_gdi_natural;
    DWRITE_MATRIX transform;
};

struct dwrite_textformat {
    IDWriteTextFormat1 IDWriteTextFormat1_iface;
    LONG ref;
    struct dwrite_textformat_data format;
};

struct dwrite_trimmingsign {
    IDWriteInlineObject IDWriteInlineObject_iface;
    LONG ref;

    IDWriteTextLayout *layout;
};

struct dwrite_typography {
    IDWriteTypography IDWriteTypography_iface;
    LONG ref;

    DWRITE_FONT_FEATURE *features;
    UINT32 allocated;
    UINT32 count;
};

static const IDWriteTextFormat1Vtbl dwritetextformatvtbl;

static void release_format_data(struct dwrite_textformat_data *data)
{
    if (data->collection) IDWriteFontCollection_Release(data->collection);
    if (data->fallback) IDWriteFontFallback_Release(data->fallback);
    if (data->trimmingsign) IDWriteInlineObject_Release(data->trimmingsign);
    heap_free(data->family_name);
    heap_free(data->locale);
}

static inline struct dwrite_textlayout *impl_from_IDWriteTextLayout2(IDWriteTextLayout2 *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_textlayout, IDWriteTextLayout2_iface);
}

static inline struct dwrite_textlayout *impl_layout_form_IDWriteTextFormat1(IDWriteTextFormat1 *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_textlayout, IDWriteTextFormat1_iface);
}

static inline struct dwrite_textlayout *impl_from_IDWriteTextAnalysisSink(IDWriteTextAnalysisSink *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_textlayout, IDWriteTextAnalysisSink_iface);
}

static inline struct dwrite_textlayout *impl_from_IDWriteTextAnalysisSource(IDWriteTextAnalysisSource *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_textlayout, IDWriteTextAnalysisSource_iface);
}

static inline struct dwrite_textformat *impl_from_IDWriteTextFormat1(IDWriteTextFormat1 *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_textformat, IDWriteTextFormat1_iface);
}

static inline struct dwrite_trimmingsign *impl_from_IDWriteInlineObject(IDWriteInlineObject *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_trimmingsign, IDWriteInlineObject_iface);
}

static inline struct dwrite_typography *impl_from_IDWriteTypography(IDWriteTypography *iface)
{
    return CONTAINING_RECORD(iface, struct dwrite_typography, IDWriteTypography_iface);
}

static inline const char *debugstr_run(const struct regular_layout_run *run)
{
    return wine_dbg_sprintf("[%u,%u)", run->descr.textPosition, run->descr.textPosition +
        run->descr.stringLength);
}

static inline HRESULT format_set_textalignment(struct dwrite_textformat_data *format, DWRITE_TEXT_ALIGNMENT alignment,
    BOOL *changed)
{
    if ((UINT32)alignment > DWRITE_TEXT_ALIGNMENT_JUSTIFIED)
        return E_INVALIDARG;
    if (changed) *changed = format->textalignment != alignment;
    format->textalignment = alignment;
    return S_OK;
}

static inline HRESULT format_set_paralignment(struct dwrite_textformat_data *format,
    DWRITE_PARAGRAPH_ALIGNMENT alignment, BOOL *changed)
{
    if ((UINT32)alignment > DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
        return E_INVALIDARG;
    if (changed) *changed = format->paralign != alignment;
    format->paralign = alignment;
    return S_OK;
}

static inline HRESULT format_set_readingdirection(struct dwrite_textformat_data *format,
    DWRITE_READING_DIRECTION direction, BOOL *changed)
{
    if ((UINT32)direction > DWRITE_READING_DIRECTION_BOTTOM_TO_TOP)
        return E_INVALIDARG;
    if (changed) *changed = format->readingdir != direction;
    format->readingdir = direction;
    return S_OK;
}

static HRESULT get_fontfallback_from_format(const struct dwrite_textformat_data *format, IDWriteFontFallback **fallback)
{
    *fallback = format->fallback;
    if (*fallback)
        IDWriteFontFallback_AddRef(*fallback);
    return S_OK;
}

static HRESULT set_fontfallback_for_format(struct dwrite_textformat_data *format, IDWriteFontFallback *fallback)
{
    if (format->fallback)
        IDWriteFontFallback_Release(format->fallback);
    format->fallback = fallback;
    if (fallback)
        IDWriteFontFallback_AddRef(fallback);
    return S_OK;
}

static struct layout_run *alloc_layout_run(enum layout_run_kind kind)
{
    struct layout_run *ret;

    ret = heap_alloc(sizeof(*ret));
    if (!ret) return NULL;

    memset(ret, 0, sizeof(*ret));
    ret->kind = kind;
    if (kind == LAYOUT_RUN_REGULAR) {
        ret->u.regular.sa.script = Script_Unknown;
        ret->u.regular.sa.shapes = DWRITE_SCRIPT_SHAPES_DEFAULT;
    }

    return ret;
}

static void free_layout_runs(struct dwrite_textlayout *layout)
{
    struct layout_run *cur, *cur2;
    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->runs, struct layout_run, entry) {
        list_remove(&cur->entry);
        if (cur->kind == LAYOUT_RUN_REGULAR) {
            if (cur->u.regular.run.fontFace)
                IDWriteFontFace_Release(cur->u.regular.run.fontFace);
            heap_free(cur->u.regular.glyphs);
            heap_free(cur->u.regular.clustermap);
            heap_free(cur->u.regular.advances);
            heap_free(cur->u.regular.offsets);
        }
        heap_free(cur);
    }
}

static void free_layout_eruns(struct dwrite_textlayout *layout)
{
    struct layout_effective_inline *in, *in2;
    struct layout_effective_run *cur, *cur2;
    struct layout_strikethrough *s, *s2;

    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->eruns, struct layout_effective_run, entry) {
        list_remove(&cur->entry);
        heap_free(cur->clustermap);
        heap_free(cur);
    }

    LIST_FOR_EACH_ENTRY_SAFE(in, in2, &layout->inlineobjects, struct layout_effective_inline, entry) {
        list_remove(&in->entry);
        heap_free(in);
    }

    LIST_FOR_EACH_ENTRY_SAFE(s, s2, &layout->strikethrough, struct layout_strikethrough, entry) {
        list_remove(&s->entry);
        heap_free(s);
    }
}

/* Used to resolve break condition by forcing stronger condition over weaker. */
static inline DWRITE_BREAK_CONDITION override_break_condition(DWRITE_BREAK_CONDITION existingbreak, DWRITE_BREAK_CONDITION newbreak)
{
    switch (existingbreak) {
    case DWRITE_BREAK_CONDITION_NEUTRAL:
        return newbreak;
    case DWRITE_BREAK_CONDITION_CAN_BREAK:
        return newbreak == DWRITE_BREAK_CONDITION_NEUTRAL ? existingbreak : newbreak;
    /* let's keep stronger conditions as is */
    case DWRITE_BREAK_CONDITION_MAY_NOT_BREAK:
    case DWRITE_BREAK_CONDITION_MUST_BREAK:
        break;
    default:
        ERR("unknown break condition %d\n", existingbreak);
    }

    return existingbreak;
}

/* This helper should be used to get effective range length, in other words it returns number of text
   positions from range starting point to the end of the range, limited by layout text length */
static inline UINT32 get_clipped_range_length(const struct dwrite_textlayout *layout, const struct layout_range *range)
{
    if (range->h.range.startPosition + range->h.range.length <= layout->len)
        return range->h.range.length;
    return layout->len - range->h.range.startPosition;
}

/* Actual breakpoint data gets updated with break condition required by inline object set for range 'cur'. */
static HRESULT layout_update_breakpoints_range(struct dwrite_textlayout *layout, const struct layout_range *cur)
{
    DWRITE_BREAK_CONDITION before, after;
    UINT32 i, length;
    HRESULT hr;

    /* ignore returned conditions if failed */
    hr = IDWriteInlineObject_GetBreakConditions(cur->object, &before, &after);
    if (FAILED(hr))
        after = before = DWRITE_BREAK_CONDITION_NEUTRAL;

    if (!layout->actual_breakpoints) {
        layout->actual_breakpoints = heap_alloc(sizeof(DWRITE_LINE_BREAKPOINT)*layout->len);
        if (!layout->actual_breakpoints)
            return E_OUTOFMEMORY;
        memcpy(layout->actual_breakpoints, layout->nominal_breakpoints, sizeof(DWRITE_LINE_BREAKPOINT)*layout->len);
    }

    length = get_clipped_range_length(layout, cur);
    for (i = cur->h.range.startPosition; i < length + cur->h.range.startPosition; i++) {
        /* for first codepoint check if there's anything before it and update accordingly */
        if (i == cur->h.range.startPosition) {
            if (i > 0)
                layout->actual_breakpoints[i].breakConditionBefore = layout->actual_breakpoints[i-1].breakConditionAfter =
                    override_break_condition(layout->actual_breakpoints[i-1].breakConditionAfter, before);
            else
                layout->actual_breakpoints[i].breakConditionBefore = before;
            layout->actual_breakpoints[i].breakConditionAfter = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        }
        /* similar check for last codepoint */
        else if (i == cur->h.range.startPosition + length - 1) {
            if (i == layout->len - 1)
                layout->actual_breakpoints[i].breakConditionAfter = after;
            else
                layout->actual_breakpoints[i].breakConditionAfter = layout->actual_breakpoints[i+1].breakConditionBefore =
                    override_break_condition(layout->actual_breakpoints[i+1].breakConditionBefore, after);
            layout->actual_breakpoints[i].breakConditionBefore = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        }
        /* for all positions within a range disable breaks */
        else {
            layout->actual_breakpoints[i].breakConditionBefore = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
            layout->actual_breakpoints[i].breakConditionAfter = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        }

        layout->actual_breakpoints[i].isWhitespace = FALSE;
        layout->actual_breakpoints[i].isSoftHyphen = FALSE;
    }

    return S_OK;
}

static struct layout_range *get_layout_range_by_pos(struct dwrite_textlayout *layout, UINT32 pos);

static inline DWRITE_LINE_BREAKPOINT get_effective_breakpoint(const struct dwrite_textlayout *layout, UINT32 pos)
{
    if (layout->actual_breakpoints)
        return layout->actual_breakpoints[pos];
    return layout->nominal_breakpoints[pos];
}

static inline void init_cluster_metrics(const struct dwrite_textlayout *layout, const struct regular_layout_run *run,
    UINT16 start_glyph, UINT16 stop_glyph, UINT32 stop_position, UINT16 length, DWRITE_CLUSTER_METRICS *metrics)
{
    UINT8 breakcondition;
    UINT32 position;
    UINT16 j;

    /* For clusters made of control chars we report zero glyphs, and we need zero cluster
       width as well; advances are already computed at this point and are not necessary zero. */
    metrics->width = 0.0;
    if (run->run.glyphCount) {
        for (j = start_glyph; j < stop_glyph; j++)
            metrics->width += run->run.glyphAdvances[j];
    }
    metrics->length = length;

    position = stop_position;
    if (stop_glyph == run->glyphcount)
        breakcondition = get_effective_breakpoint(layout, stop_position).breakConditionAfter;
    else {
        breakcondition = get_effective_breakpoint(layout, stop_position).breakConditionBefore;
        if (stop_position) position = stop_position - 1;
    }

    metrics->canWrapLineAfter = breakcondition == DWRITE_BREAK_CONDITION_CAN_BREAK ||
                                breakcondition == DWRITE_BREAK_CONDITION_MUST_BREAK;
    if (metrics->length == 1) {
        WORD type = 0;

        GetStringTypeW(CT_CTYPE1, &layout->str[position], 1, &type);
        metrics->isWhitespace = !!(type & C1_SPACE);
        metrics->isNewline = FALSE /* FIXME */;
        metrics->isSoftHyphen = layout->str[position] == 0x00ad /* Unicode Soft Hyphen */;
    }
    else {
        metrics->isWhitespace = FALSE;
        metrics->isNewline = FALSE;
        metrics->isSoftHyphen = FALSE;
    }
    metrics->isRightToLeft = run->run.bidiLevel & 1;
    metrics->padding = 0;
}

/*

  All clusters in a 'run' will be added to 'layout' data, starting at index pointed to by 'cluster'.
  On return 'cluster' is updated to point to next metrics struct to be filled in on next call.
  Note that there's no need to reallocate anything at this point as we allocate one cluster per
  codepoint initially.

*/
static void layout_set_cluster_metrics(struct dwrite_textlayout *layout, const struct layout_run *r, UINT32 *cluster)
{
    DWRITE_CLUSTER_METRICS *metrics = &layout->clustermetrics[*cluster];
    struct layout_cluster *c = &layout->clusters[*cluster];
    const struct regular_layout_run *run = &r->u.regular;
    UINT32 i, start = 0;

    for (i = 0; i < run->descr.stringLength; i++) {
        BOOL end = i == run->descr.stringLength - 1;

        if (run->descr.clusterMap[start] != run->descr.clusterMap[i]) {
            init_cluster_metrics(layout, run, run->descr.clusterMap[start], run->descr.clusterMap[i], i,
                i - start, metrics);
            c->position = start;
            c->run = r;

            *cluster += 1;
            metrics++;
            c++;
            start = i;
        }

        if (end) {
            init_cluster_metrics(layout, run, run->descr.clusterMap[start], run->glyphcount, i,
                i - start + 1, metrics);
            c->position = start;
            c->run = r;

            *cluster += 1;
            return;
        }
    }
}

static inline FLOAT get_scaled_font_metric(UINT32 metric, FLOAT emSize, const DWRITE_FONT_METRICS *metrics)
{
    return (FLOAT)metric * emSize / (FLOAT)metrics->designUnitsPerEm;
}

static HRESULT layout_compute_runs(struct dwrite_textlayout *layout)
{
    IDWriteTextAnalyzer *analyzer;
    struct layout_range *range;
    struct layout_run *r;
    UINT32 cluster = 0;
    HRESULT hr;

    free_layout_eruns(layout);
    free_layout_runs(layout);

    /* Cluster data arrays are allocated once, assuming one text position per cluster. */
    if (!layout->clustermetrics) {
        layout->clustermetrics = heap_alloc(layout->len*sizeof(*layout->clustermetrics));
        layout->clusters = heap_alloc(layout->len*sizeof(*layout->clusters));
        if (!layout->clustermetrics || !layout->clusters) {
            heap_free(layout->clustermetrics);
            heap_free(layout->clusters);
            return E_OUTOFMEMORY;
        }
    }
    layout->cluster_count = 0;

    hr = get_textanalyzer(&analyzer);
    if (FAILED(hr))
        return hr;

    LIST_FOR_EACH_ENTRY(range, &layout->ranges, struct layout_range, h.entry) {
        /* we don't care about ranges that don't contain any text */
        if (range->h.range.startPosition >= layout->len)
            break;

        /* inline objects override actual text in a range */
        if (range->object) {
            hr = layout_update_breakpoints_range(layout, range);
            if (FAILED(hr))
                return hr;

            r = alloc_layout_run(LAYOUT_RUN_INLINE);
            if (!r)
                return E_OUTOFMEMORY;

            r->u.object.object = range->object;
            r->u.object.length = get_clipped_range_length(layout, range);
            list_add_tail(&layout->runs, &r->entry);
            continue;
        }

        /* initial splitting by script */
        hr = IDWriteTextAnalyzer_AnalyzeScript(analyzer, &layout->IDWriteTextAnalysisSource_iface,
            range->h.range.startPosition, get_clipped_range_length(layout, range), &layout->IDWriteTextAnalysisSink_iface);
        if (FAILED(hr))
            break;

        /* this splits it further */
        hr = IDWriteTextAnalyzer_AnalyzeBidi(analyzer, &layout->IDWriteTextAnalysisSource_iface,
            range->h.range.startPosition, get_clipped_range_length(layout, range), &layout->IDWriteTextAnalysisSink_iface);
        if (FAILED(hr))
            break;
    }

    /* fill run info */
    LIST_FOR_EACH_ENTRY(r, &layout->runs, struct layout_run, entry) {
        DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_props = NULL;
        DWRITE_SHAPING_TEXT_PROPERTIES *text_props = NULL;
        struct regular_layout_run *run = &r->u.regular;
        DWRITE_FONT_METRICS fontmetrics = { 0 };
        IDWriteFontFamily *family;
        UINT32 index, max_count;
        IDWriteFont *font;
        BOOL exists = TRUE;

        /* we need to do very little in case of inline objects */
        if (r->kind == LAYOUT_RUN_INLINE) {
            DWRITE_CLUSTER_METRICS *metrics = &layout->clustermetrics[cluster];
            struct layout_cluster *c = &layout->clusters[cluster];
            DWRITE_INLINE_OBJECT_METRICS inlinemetrics;

            metrics->width = 0.0;
            metrics->length = r->u.object.length;
            metrics->canWrapLineAfter = FALSE;
            metrics->isWhitespace = FALSE;
            metrics->isNewline = FALSE;
            metrics->isSoftHyphen = FALSE;
            metrics->isRightToLeft = FALSE;
            metrics->padding = 0;
            c->run = r;
            c->position = 0; /* there's always one cluster per inline object, so 0 is valid value */
            cluster++;

            /* it's not fatal if GetMetrics() fails, all returned metrics are ignored */
            hr = IDWriteInlineObject_GetMetrics(r->u.object.object, &inlinemetrics);
            if (FAILED(hr)) {
                memset(&inlinemetrics, 0, sizeof(inlinemetrics));
                hr = S_OK;
            }
            metrics->width = inlinemetrics.width;
            r->baseline = inlinemetrics.baseline;
            r->height = inlinemetrics.height;

            /* FIXME: use resolved breakpoints in this case too */

            continue;
        }

        range = get_layout_range_by_pos(layout, run->descr.textPosition);

        hr = IDWriteFontCollection_FindFamilyName(range->collection, range->fontfamily, &index, &exists);
        if (FAILED(hr) || !exists) {
            WARN("%s: family %s not found in collection %p\n", debugstr_run(run), debugstr_w(range->fontfamily), range->collection);
            continue;
        }

        hr = IDWriteFontCollection_GetFontFamily(range->collection, index, &family);
        if (FAILED(hr))
            continue;

        hr = IDWriteFontFamily_GetFirstMatchingFont(family, range->weight, range->stretch, range->style, &font);
        IDWriteFontFamily_Release(family);
        if (FAILED(hr)) {
            WARN("%s: failed to get a matching font\n", debugstr_run(run));
            continue;
        }

        hr = IDWriteFont_CreateFontFace(font, &run->run.fontFace);
        IDWriteFont_Release(font);
        if (FAILED(hr))
            continue;

        run->run.fontEmSize = range->fontsize;
        run->descr.localeName = range->locale;
        run->clustermap = heap_alloc(run->descr.stringLength*sizeof(UINT16));

        max_count = 3*run->descr.stringLength/2 + 16;
        run->glyphs = heap_alloc(max_count*sizeof(UINT16));
        if (!run->clustermap || !run->glyphs)
            goto memerr;

        text_props = heap_alloc(run->descr.stringLength*sizeof(DWRITE_SHAPING_TEXT_PROPERTIES));
        glyph_props = heap_alloc(max_count*sizeof(DWRITE_SHAPING_GLYPH_PROPERTIES));
        if (!text_props || !glyph_props)
            goto memerr;

        while (1) {
            hr = IDWriteTextAnalyzer_GetGlyphs(analyzer, run->descr.string, run->descr.stringLength,
                run->run.fontFace, run->run.isSideways, run->run.bidiLevel & 1, &run->sa, run->descr.localeName,
                NULL /* FIXME */, NULL, NULL, 0, max_count, run->clustermap, text_props, run->glyphs, glyph_props,
                &run->glyphcount);
            if (hr == E_NOT_SUFFICIENT_BUFFER) {
                heap_free(run->glyphs);
                heap_free(glyph_props);

                max_count = run->glyphcount;

                run->glyphs = heap_alloc(max_count*sizeof(UINT16));
                glyph_props = heap_alloc(max_count*sizeof(DWRITE_SHAPING_GLYPH_PROPERTIES));
                if (!run->glyphs || !glyph_props)
                    goto memerr;

                continue;
            }

            break;
        }

        if (FAILED(hr)) {
            heap_free(text_props);
            heap_free(glyph_props);
            WARN("%s: shaping failed 0x%08x\n", debugstr_run(run), hr);
            continue;
        }

        run->run.glyphIndices = run->glyphs;
        run->descr.clusterMap = run->clustermap;

        run->advances = heap_alloc(run->glyphcount*sizeof(FLOAT));
        run->offsets = heap_alloc(run->glyphcount*sizeof(DWRITE_GLYPH_OFFSET));
        if (!run->advances || !run->offsets)
            goto memerr;

        /* now set advances and offsets */
        if (layout->gdicompatible)
            hr = IDWriteTextAnalyzer_GetGdiCompatibleGlyphPlacements(analyzer, run->descr.string, run->descr.clusterMap,
                text_props, run->descr.stringLength, run->run.glyphIndices, glyph_props, run->glyphcount,
                run->run.fontFace, run->run.fontEmSize, layout->pixels_per_dip, &layout->transform, layout->use_gdi_natural,
                run->run.isSideways, run->run.bidiLevel & 1, &run->sa, run->descr.localeName, NULL, NULL, 0,
                run->advances, run->offsets);
        else
            hr = IDWriteTextAnalyzer_GetGlyphPlacements(analyzer, run->descr.string, run->descr.clusterMap, text_props,
                run->descr.stringLength, run->run.glyphIndices, glyph_props, run->glyphcount, run->run.fontFace,
                run->run.fontEmSize, run->run.isSideways, run->run.bidiLevel & 1, &run->sa, run->descr.localeName,
                NULL, NULL, 0, run->advances, run->offsets);

        heap_free(text_props);
        heap_free(glyph_props);
        if (FAILED(hr))
            WARN("%s: failed to get glyph placement info, 0x%08x\n", debugstr_run(run), hr);

        run->run.glyphAdvances = run->advances;
        run->run.glyphOffsets = run->offsets;

        /* Special treatment of control script, shaping code adds normal glyphs for it,
           with non-zero advances, and layout code exposes those as zero width clusters,
           so we have to do it manually. */
        if (run->sa.script == Script_Common)
            run->run.glyphCount = 0;
        else
            run->run.glyphCount = run->glyphcount;

        /* baseline derived from font metrics */
        if (layout->gdicompatible) {
            /* FIXME: check return value when it's actually implemented */
            IDWriteFontFace_GetGdiCompatibleMetrics(run->run.fontFace,
                run->run.fontEmSize,
                layout->pixels_per_dip,
                &layout->transform,
                &fontmetrics);
        }
        else
            IDWriteFontFace_GetMetrics(run->run.fontFace, &fontmetrics);

        r->baseline = get_scaled_font_metric(fontmetrics.ascent, run->run.fontEmSize, &fontmetrics);
        r->height = get_scaled_font_metric(fontmetrics.ascent + fontmetrics.descent, run->run.fontEmSize, &fontmetrics);

        layout_set_cluster_metrics(layout, r, &cluster);

        continue;

    memerr:
        heap_free(text_props);
        heap_free(glyph_props);
        heap_free(run->clustermap);
        heap_free(run->glyphs);
        heap_free(run->advances);
        heap_free(run->offsets);
        run->advances = NULL;
        run->offsets = NULL;
        run->clustermap = run->glyphs = NULL;
        hr = E_OUTOFMEMORY;
        break;
    }

    if (hr == S_OK) {
        layout->cluster_count = cluster;
        if (cluster)
            layout->clustermetrics[cluster-1].canWrapLineAfter = TRUE;
    }

    IDWriteTextAnalyzer_Release(analyzer);
    return hr;
}

static HRESULT layout_compute(struct dwrite_textlayout *layout)
{
    HRESULT hr;

    if (!(layout->recompute & RECOMPUTE_NOMINAL_RUNS))
        return S_OK;

    /* nominal breakpoints are evaluated only once, because string never changes */
    if (!layout->nominal_breakpoints) {
        IDWriteTextAnalyzer *analyzer;
        HRESULT hr;

        layout->nominal_breakpoints = heap_alloc(sizeof(DWRITE_LINE_BREAKPOINT)*layout->len);
        if (!layout->nominal_breakpoints)
            return E_OUTOFMEMORY;

        hr = get_textanalyzer(&analyzer);
        if (FAILED(hr))
            return hr;

        hr = IDWriteTextAnalyzer_AnalyzeLineBreakpoints(analyzer, &layout->IDWriteTextAnalysisSource_iface,
            0, layout->len, &layout->IDWriteTextAnalysisSink_iface);
        IDWriteTextAnalyzer_Release(analyzer);
    }
    if (layout->actual_breakpoints) {
        heap_free(layout->actual_breakpoints);
        layout->actual_breakpoints = NULL;
    }

    hr = layout_compute_runs(layout);

    if (TRACE_ON(dwrite)) {
        struct layout_run *cur;

        LIST_FOR_EACH_ENTRY(cur, &layout->runs, struct layout_run, entry) {
            if (cur->kind == LAYOUT_RUN_INLINE)
                TRACE("run inline object %p, len %u\n", cur->u.object.object, cur->u.object.length);
            else
                TRACE("run [%u,%u], len %u, bidilevel %u\n", cur->u.regular.descr.textPosition, cur->u.regular.descr.textPosition +
                    cur->u.regular.descr.stringLength-1, cur->u.regular.descr.stringLength, cur->u.regular.run.bidiLevel);
        }
    }

    layout->recompute &= ~RECOMPUTE_NOMINAL_RUNS;
    return hr;
}

static inline FLOAT get_cluster_range_width(struct dwrite_textlayout *layout, UINT32 start, UINT32 end)
{
    FLOAT width = 0.0;
    for (; start < end; start++)
        width += layout->clustermetrics[start].width;
    return width;
}

static struct layout_range_header *get_layout_range_header_by_pos(struct list *ranges, UINT32 pos)
{
    struct layout_range_header *cur;

    LIST_FOR_EACH_ENTRY(cur, ranges, struct layout_range_header, entry) {
        DWRITE_TEXT_RANGE *r = &cur->range;
        if (r->startPosition <= pos && pos < r->startPosition + r->length)
            return cur;
    }

    return NULL;
}

static inline IUnknown *layout_get_effect_from_pos(struct dwrite_textlayout *layout, UINT32 pos)
{
    struct layout_range_header *h = get_layout_range_header_by_pos(&layout->effects, pos);
    return ((struct layout_range_effect*)h)->effect;
}

static inline BOOL layout_is_erun_rtl(const struct layout_effective_run *erun)
{
    return erun->run->u.regular.run.bidiLevel & 1;
}

/* Effective run is built from consecutive clusters of a single nominal run, 'first_cluster' is 0 based cluster index,
   'cluster_count' indicates how many clusters to add, including first one. */
static HRESULT layout_add_effective_run(struct dwrite_textlayout *layout, const struct layout_run *r, UINT32 first_cluster,
    UINT32 cluster_count, UINT32 line, FLOAT origin_x, BOOL strikethrough)
{
    BOOL is_rtl = layout->format.readingdir == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    UINT32 i, start, length, last_cluster;
    struct layout_effective_run *run;

    if (r->kind == LAYOUT_RUN_INLINE) {
        struct layout_effective_inline *inlineobject;

        inlineobject = heap_alloc(sizeof(*inlineobject));
        if (!inlineobject)
            return E_OUTOFMEMORY;

        inlineobject->object = r->u.object.object;
        inlineobject->width = get_cluster_range_width(layout, first_cluster, first_cluster + cluster_count);
        inlineobject->origin_x = is_rtl ? origin_x - inlineobject->width : origin_x;
        inlineobject->origin_y = 0.0; /* set after line is built */
        inlineobject->align_dx = 0.0;

        /* It's not clear how these two are set, possibly directionality
           is derived from surrounding text (replaced text could have
           different ranges which differ in reading direction). */
        inlineobject->is_sideways = FALSE;
        inlineobject->is_rtl = FALSE;
        inlineobject->line = line;

        /* effect assigned from start position and on is used for inline objects */
        inlineobject->effect = layout_get_effect_from_pos(layout, layout->clusters[first_cluster].position);

        list_add_tail(&layout->inlineobjects, &inlineobject->entry);
        return S_OK;
    }

    run = heap_alloc(sizeof(*run));
    if (!run)
        return E_OUTOFMEMORY;

    /* No need to iterate for that, use simple fact that:
       <last cluster position> = first cluster position> + <sum of cluster lengths not including last one> */
    last_cluster = first_cluster + cluster_count - 1;
    length = layout->clusters[last_cluster].position - layout->clusters[first_cluster].position +
        layout->clustermetrics[last_cluster].length;

    run->clustermap = heap_alloc(sizeof(UINT16)*length);
    if (!run->clustermap) {
        heap_free(run);
        return E_OUTOFMEMORY;
    }

    run->run = r;
    run->start = start = layout->clusters[first_cluster].position;
    run->length = length;
    run->width = get_cluster_range_width(layout, first_cluster, first_cluster + cluster_count);

    /* Check if run direction matches paragraph direction, if it doesn't adjust by
       run width */
    if (layout_is_erun_rtl(run) ^ is_rtl)
        run->origin_x = is_rtl ? origin_x - run->width : origin_x + run->width;
    else
        run->origin_x = origin_x;

    run->origin_y = 0.0; /* set after line is built */
    run->align_dx = 0.0;
    run->line = line;

    if (r->u.regular.run.glyphCount) {
        /* trim from the left */
        run->glyphcount = r->u.regular.run.glyphCount - r->u.regular.clustermap[start];
        /* trim from the right */
        if (start + length < r->u.regular.descr.stringLength - 1)
            run->glyphcount -= r->u.regular.run.glyphCount - r->u.regular.clustermap[start + length];
    }
    else
        run->glyphcount = 0;

    /* cluster map needs to be shifted */
    for (i = 0; i < length; i++)
        run->clustermap[i] = r->u.regular.clustermap[start + i] - r->u.regular.clustermap[start];

    list_add_tail(&layout->eruns, &run->entry);

    /* Strikethrough style is guaranteed to be consistent within effective run,
       it's width equals to run width, thikness and offset are derived from
       font metrics, rest of the values are from layout or run itself */
    if (strikethrough) {
        DWRITE_FONT_METRICS metrics = { 0 };
        struct layout_strikethrough *s;

        s = heap_alloc(sizeof(*s));
        if (!s)
            return E_OUTOFMEMORY;

        if (layout->gdicompatible) {
            HRESULT hr = IDWriteFontFace_GetGdiCompatibleMetrics(
                r->u.regular.run.fontFace,
                r->u.regular.run.fontEmSize,
                layout->pixels_per_dip,
                &layout->transform,
                &metrics);
            if (FAILED(hr))
                WARN("failed to get font metrics, 0x%08x\n", hr);
        }
        else
            IDWriteFontFace_GetMetrics(r->u.regular.run.fontFace, &metrics);

        s->s.width = get_cluster_range_width(layout, first_cluster, first_cluster + cluster_count);
        s->s.thickness = metrics.strikethroughThickness;
        s->s.offset = metrics.strikethroughPosition;
        s->s.readingDirection = layout->format.readingdir;
        s->s.flowDirection = layout->format.flow;
        s->s.localeName = r->u.regular.descr.localeName;
        s->s.measuringMode = DWRITE_MEASURING_MODE_NATURAL; /* FIXME */
        s->run = run;

        list_add_tail(&layout->strikethrough, &s->entry);
    }

    return S_OK;
}

static HRESULT layout_set_line_metrics(struct dwrite_textlayout *layout, DWRITE_LINE_METRICS *metrics, UINT32 *line)
{
    if (!layout->line_alloc) {
        layout->line_alloc = 5;
        layout->lines = heap_alloc(layout->line_alloc*sizeof(*layout->lines));
        if (!layout->lines)
            return E_OUTOFMEMORY;
    }

    if (layout->metrics.lineCount == layout->line_alloc) {
        DWRITE_LINE_METRICS *l = heap_realloc(layout->lines, layout->line_alloc*2*sizeof(*layout->lines));
        if (!l)
            return E_OUTOFMEMORY;
        layout->lines = l;
        layout->line_alloc *= 2;
    }

    layout->lines[*line] = *metrics;
    layout->metrics.lineCount += 1;
    *line += 1;
    return S_OK;
}

static inline BOOL layout_get_strikethrough_from_pos(struct dwrite_textlayout *layout, UINT32 pos)
{
    struct layout_range_header *h = get_layout_range_header_by_pos(&layout->strike_ranges, pos);
    return ((struct layout_range_bool*)h)->value;
}

static inline struct layout_effective_run *layout_get_next_erun(struct dwrite_textlayout *layout,
    const struct layout_effective_run *cur)
{
    struct list *e;

    if (!cur)
        e = list_head(&layout->eruns);
    else
        e = list_next(&layout->eruns, &cur->entry);
    if (!e)
        return NULL;
    return LIST_ENTRY(e, struct layout_effective_run, entry);
}

static inline struct layout_effective_inline *layout_get_next_inline_run(struct dwrite_textlayout *layout,
    const struct layout_effective_inline *cur)
{
    struct list *e;

    if (!cur)
        e = list_head(&layout->inlineobjects);
    else
        e = list_next(&layout->inlineobjects, &cur->entry);
    if (!e)
        return NULL;
    return LIST_ENTRY(e, struct layout_effective_inline, entry);
}

static FLOAT layout_get_line_width(struct dwrite_textlayout *layout,
    struct layout_effective_run *erun, struct layout_effective_inline *inrun, UINT32 line)
{
    FLOAT width = 0.0;

    while (erun && erun->line == line) {
        width += erun->width;
        erun = layout_get_next_erun(layout, erun);
        if (!erun)
            break;
    }

    while (inrun && inrun->line == line) {
        width += inrun->width;
        inrun = layout_get_next_inline_run(layout, inrun);
        if (!inrun)
            break;
    }

    return width;
}

static void layout_apply_leading_alignment(struct dwrite_textlayout *layout)
{
    BOOL is_rtl = layout->format.readingdir == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    struct layout_effective_inline *inrun;
    struct layout_effective_run *erun;

    erun = layout_get_next_erun(layout, NULL);
    inrun = layout_get_next_inline_run(layout, NULL);

    while (erun) {
        erun->align_dx = 0.0;
        erun = layout_get_next_erun(layout, erun);
    }

    while (inrun) {
        inrun->align_dx = 0.0;
        inrun = layout_get_next_inline_run(layout, inrun);
    }

    layout->metrics.left = is_rtl ? layout->metrics.layoutWidth - layout->metrics.width : 0.0;
}

static void layout_apply_trailing_alignment(struct dwrite_textlayout *layout)
{
    BOOL is_rtl = layout->format.readingdir == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    struct layout_effective_inline *inrun;
    struct layout_effective_run *erun;
    UINT32 line;

    erun = layout_get_next_erun(layout, NULL);
    inrun = layout_get_next_inline_run(layout, NULL);

    for (line = 0; line < layout->metrics.lineCount; line++) {
        FLOAT width = layout_get_line_width(layout, erun, inrun, line);
        FLOAT shift = layout->metrics.layoutWidth - width;

        if (is_rtl)
            shift *= -1.0;

        while (erun && erun->line == line) {
            erun->align_dx = shift;
            erun = layout_get_next_erun(layout, erun);
        }

        while (inrun && inrun->line == line) {
            inrun->align_dx = shift;
            inrun = layout_get_next_inline_run(layout, inrun);
        }
    }

    layout->metrics.left = is_rtl ? 0.0 : layout->metrics.layoutWidth - layout->metrics.width;
}

static void layout_apply_centered_alignment(struct dwrite_textlayout *layout)
{
    BOOL is_rtl = layout->format.readingdir == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    struct layout_effective_inline *inrun;
    struct layout_effective_run *erun;
    UINT32 line;

    erun = layout_get_next_erun(layout, NULL);
    inrun = layout_get_next_inline_run(layout, NULL);

    for (line = 0; line < layout->metrics.lineCount; line++) {
        FLOAT width = layout_get_line_width(layout, erun, inrun, line);
        FLOAT shift = (layout->metrics.layoutWidth - width) / 2.0;

        if (is_rtl)
            shift *= -1.0;

        while (erun && erun->line == line) {
            erun->align_dx = shift;
            erun = layout_get_next_erun(layout, erun);
        }

        while (inrun && inrun->line == line) {
            inrun->align_dx = shift;
            inrun = layout_get_next_inline_run(layout, inrun);
        }
    }

    layout->metrics.left = (layout->metrics.layoutWidth - layout->metrics.width) / 2.0;
}

static void layout_apply_text_alignment(struct dwrite_textlayout *layout)
{
    switch (layout->format.textalignment)
    {
    case DWRITE_TEXT_ALIGNMENT_LEADING:
        layout_apply_leading_alignment(layout);
        break;
    case DWRITE_TEXT_ALIGNMENT_TRAILING:
        layout_apply_trailing_alignment(layout);
        break;
    case DWRITE_TEXT_ALIGNMENT_CENTER:
        layout_apply_centered_alignment(layout);
        break;
    case DWRITE_TEXT_ALIGNMENT_JUSTIFIED:
        FIXME("alignment %d not implemented\n", layout->format.textalignment);
        break;
    default:
        ;
    }
}

static void layout_apply_par_alignment(struct dwrite_textlayout *layout)
{
    struct layout_effective_inline *inrun;
    struct layout_effective_run *erun;
    FLOAT origin_y = 0.0;
    UINT32 line;

    /* alignment mode defines origin, after that all run origins are updated
       the same way */

    switch (layout->format.paralign)
    {
    case DWRITE_PARAGRAPH_ALIGNMENT_NEAR:
        origin_y = 0.0;
        break;
    case DWRITE_PARAGRAPH_ALIGNMENT_FAR:
        origin_y = layout->metrics.layoutHeight - layout->metrics.height;
        break;
    case DWRITE_PARAGRAPH_ALIGNMENT_CENTER:
        origin_y = (layout->metrics.layoutHeight - layout->metrics.height) / 2.0;
        break;
    default:
        ;
    }

    layout->metrics.top = origin_y;

    erun = layout_get_next_erun(layout, NULL);
    inrun = layout_get_next_inline_run(layout, NULL);
    for (line = 0; line < layout->metrics.lineCount; line++) {
        origin_y += layout->lines[line].baseline;

        while (erun && erun->line == line) {
            erun->origin_y = origin_y;
            erun = layout_get_next_erun(layout, erun);
        }

        while (inrun && inrun->line == line) {
            inrun->origin_y = origin_y;
            inrun = layout_get_next_inline_run(layout, inrun);
        }
    }
}

static HRESULT layout_compute_effective_runs(struct dwrite_textlayout *layout)
{
    BOOL is_rtl = layout->format.readingdir == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    struct layout_effective_inline *inrun;
    struct layout_effective_run *erun;
    const struct layout_run *run;
    DWRITE_LINE_METRICS metrics;
    FLOAT width, origin_x, origin_y;
    UINT32 i, start, line, textpos;
    HRESULT hr;
    BOOL s[2];

    if (!(layout->recompute & RECOMPUTE_EFFECTIVE_RUNS))
        return S_OK;

    hr = layout_compute(layout);
    if (FAILED(hr))
        return hr;

    layout->metrics.lineCount = 0;
    origin_x = is_rtl ? layout->metrics.layoutWidth : 0.0;
    line = 0;
    run = layout->clusters[0].run;
    memset(&metrics, 0, sizeof(metrics));
    s[0] = s[1] = layout_get_strikethrough_from_pos(layout, 0);

    for (i = 0, start = 0, textpos = 0, width = 0.0; i < layout->cluster_count; i++) {
        BOOL overflow;

        s[1] = layout_get_strikethrough_from_pos(layout, textpos);

        /* switched to next nominal run, at this point all previous pending clusters are already
           checked for layout line overflow, so new effective run will fit in current line */
        if (run != layout->clusters[i].run || s[0] != s[1]) {
            hr = layout_add_effective_run(layout, run, start, i - start, line, origin_x, s[0]);
            if (FAILED(hr))
                return hr;
            origin_x += is_rtl ? -get_cluster_range_width(layout, start, i) :
                                  get_cluster_range_width(layout, start, i);
            run = layout->clusters[i].run;
            start = i;
        }

        overflow = layout->clustermetrics[i].canWrapLineAfter &&
            (width + layout->clustermetrics[i].width > layout->metrics.layoutWidth);
        /* check if we got new */
        if (overflow ||
            layout->clustermetrics[i].isNewline || /* always wrap on new line */
            i == layout->cluster_count - 1) /* end of the text */ {

            UINT32 strlength, last_cluster = i, index;
            FLOAT descent, trailingspacewidth;

            if (!overflow) {
                width += layout->clustermetrics[i].width;
                metrics.length += layout->clustermetrics[i].length;
                last_cluster = i;
            }
            else
                last_cluster = i ? i - 1 : i;

            if (i >= start) {
                hr = layout_add_effective_run(layout, run, start, last_cluster - start + 1, line, origin_x, s[0]);
                if (FAILED(hr))
                    return hr;
                /* we don't need to update origin for next run as we're going to wrap */
            }

            /* take a look at clusters we got for this line in reverse order to set
               trailing properties for current line */
            strlength = metrics.length;
            index = last_cluster;
            trailingspacewidth = 0.0;
            while (strlength) {
                DWRITE_CLUSTER_METRICS *cluster = &layout->clustermetrics[index];

                if (!cluster->isNewline && !cluster->isWhitespace)
                    break;

                if (cluster->isNewline) {
                    metrics.trailingWhitespaceLength += cluster->length;
                    metrics.newlineLength += cluster->length;
                }

                if (cluster->isWhitespace) {
                    metrics.trailingWhitespaceLength += cluster->length;
                    trailingspacewidth += cluster->width;
                }

                strlength -= cluster->length;
                index--;
            }

            /* look for max baseline and descent for this line */
            strlength = metrics.length;
            index = last_cluster;
            metrics.baseline = 0.0;
            descent = 0.0;
            while (strlength) {
                DWRITE_CLUSTER_METRICS *cluster = &layout->clustermetrics[index];
                const struct layout_run *cur = layout->clusters[index].run;
                FLOAT cur_descent = cur->height - cur->baseline;

                if (cur->baseline > metrics.baseline)
                    metrics.baseline = cur->baseline;

                if (cur_descent > descent)
                    descent = cur_descent;

                strlength -= cluster->length;
                index--;
            }
            metrics.height = descent + metrics.baseline;

            if (width > layout->metrics.widthIncludingTrailingWhitespace)
                layout->metrics.widthIncludingTrailingWhitespace = width;
            if (width - trailingspacewidth > layout->metrics.width)
                layout->metrics.width = width - trailingspacewidth;

            metrics.isTrimmed = width > layout->metrics.layoutWidth;
            hr = layout_set_line_metrics(layout, &metrics, &line);
            if (FAILED(hr))
                return hr;

            width = layout->clustermetrics[i].width;
            memset(&metrics, 0, sizeof(metrics));
            origin_x = is_rtl ? layout->metrics.layoutWidth : 0.0;
            start = i;
        }
        else {
            metrics.length += layout->clustermetrics[i].length;
            width += layout->clustermetrics[i].width;
        }

        s[0] = s[1];
        textpos += layout->clustermetrics[i].length;
    }

    layout->metrics.left = is_rtl ? layout->metrics.layoutWidth - layout->metrics.width : 0;
    layout->metrics.top = 0.0;
    layout->metrics.maxBidiReorderingDepth = 1; /* FIXME */
    layout->metrics.height = 0.0;

    /* Now all line info is here, update effective runs positions in flow direction */
    erun = layout_get_next_erun(layout, NULL);
    inrun = layout_get_next_inline_run(layout, NULL);

    origin_y = 0.0;
    for (line = 0; line < layout->metrics.lineCount; line++) {

        origin_y += layout->lines[line].baseline;

        /* For all runs on this line */
        while (erun && erun->line == line) {
            erun->origin_y = origin_y;
            erun = layout_get_next_erun(layout, erun);
        }

        /* Same for inline runs */
        while (inrun && inrun->line == line) {
            inrun->origin_y = origin_y;
            inrun = layout_get_next_inline_run(layout, inrun);
        }

        layout->metrics.height += layout->lines[line].height;
    }

    /* initial alignment is always leading */
    if (layout->format.textalignment != DWRITE_TEXT_ALIGNMENT_LEADING)
        layout_apply_text_alignment(layout);

    /* initial paragraph alignment is always near */
    if (layout->format.paralign != DWRITE_PARAGRAPH_ALIGNMENT_NEAR)
        layout_apply_par_alignment(layout);

    layout->metrics.heightIncludingTrailingWhitespace = layout->metrics.height; /* FIXME: not true for vertical text */

    layout->recompute &= ~RECOMPUTE_EFFECTIVE_RUNS;
    return hr;
}

static BOOL is_same_layout_attrvalue(struct layout_range_header const *h, enum layout_range_attr_kind attr, struct layout_range_attr_value *value)
{
    struct layout_range_spacing const *range_spacing = (struct layout_range_spacing*)h;
    struct layout_range_effect const *range_effect = (struct layout_range_effect*)h;
    struct layout_range_bool const *range_bool = (struct layout_range_bool*)h;
    struct layout_range const *range = (struct layout_range*)h;

    switch (attr) {
    case LAYOUT_RANGE_ATTR_WEIGHT:
        return range->weight == value->u.weight;
    case LAYOUT_RANGE_ATTR_STYLE:
        return range->style == value->u.style;
    case LAYOUT_RANGE_ATTR_STRETCH:
        return range->stretch == value->u.stretch;
    case LAYOUT_RANGE_ATTR_FONTSIZE:
        return range->fontsize == value->u.fontsize;
    case LAYOUT_RANGE_ATTR_INLINE:
        return range->object == value->u.object;
    case LAYOUT_RANGE_ATTR_EFFECT:
        return range_effect->effect == value->u.effect;
    case LAYOUT_RANGE_ATTR_UNDERLINE:
        return range->underline == value->u.underline;
    case LAYOUT_RANGE_ATTR_STRIKETHROUGH:
        return range_bool->value == value->u.strikethrough;
    case LAYOUT_RANGE_ATTR_PAIR_KERNING:
        return range->pair_kerning == value->u.pair_kerning;
    case LAYOUT_RANGE_ATTR_FONTCOLL:
        return range->collection == value->u.collection;
    case LAYOUT_RANGE_ATTR_LOCALE:
        return strcmpW(range->locale, value->u.locale) == 0;
    case LAYOUT_RANGE_ATTR_FONTFAMILY:
        return strcmpW(range->fontfamily, value->u.fontfamily) == 0;
    case LAYOUT_RANGE_ATTR_SPACING:
        return range_spacing->leading == value->u.spacing[0] &&
               range_spacing->trailing == value->u.spacing[1] &&
               range_spacing->min_advance == value->u.spacing[2];
    default:
        ;
    }

    return FALSE;
}

static inline BOOL is_same_layout_attributes(struct layout_range_header const *hleft, struct layout_range_header const *hright)
{
    switch (hleft->kind)
    {
    case LAYOUT_RANGE_REGULAR:
    {
        struct layout_range const *left = (struct layout_range const*)hleft;
        struct layout_range const *right = (struct layout_range const*)hright;
        return left->weight == right->weight &&
               left->style  == right->style &&
               left->stretch == right->stretch &&
               left->fontsize == right->fontsize &&
               left->object == right->object &&
               left->underline == right->underline &&
               left->pair_kerning == right->pair_kerning &&
               left->collection == right->collection &&
              !strcmpW(left->locale, right->locale) &&
              !strcmpW(left->fontfamily, right->fontfamily);
    }
    case LAYOUT_RANGE_STRIKETHROUGH:
    {
        struct layout_range_bool const *left = (struct layout_range_bool const*)hleft;
        struct layout_range_bool const *right = (struct layout_range_bool const*)hright;
        return left->value == right->value;
    }
    case LAYOUT_RANGE_EFFECT:
    {
        struct layout_range_effect const *left = (struct layout_range_effect const*)hleft;
        struct layout_range_effect const *right = (struct layout_range_effect const*)hright;
        return left->effect == right->effect;
    }
    case LAYOUT_RANGE_SPACING:
    {
        struct layout_range_spacing const *left = (struct layout_range_spacing const*)hleft;
        struct layout_range_spacing const *right = (struct layout_range_spacing const*)hright;
        return left->leading == right->leading &&
               left->trailing == right->trailing &&
               left->min_advance == right->min_advance;
    }
    default:
        FIXME("unknown range kind %d\n", hleft->kind);
        return FALSE;
    }
}

static inline BOOL is_same_text_range(const DWRITE_TEXT_RANGE *left, const DWRITE_TEXT_RANGE *right)
{
    return left->startPosition == right->startPosition && left->length == right->length;
}

/* Allocates range and inits it with default values from text format. */
static struct layout_range_header *alloc_layout_range(struct dwrite_textlayout *layout, const DWRITE_TEXT_RANGE *r,
    enum layout_range_kind kind)
{
    struct layout_range_header *h;

    switch (kind)
    {
    case LAYOUT_RANGE_REGULAR:
    {
        struct layout_range *range;

        range = heap_alloc(sizeof(*range));
        if (!range) return NULL;

        range->weight = layout->format.weight;
        range->style  = layout->format.style;
        range->stretch = layout->format.stretch;
        range->fontsize = layout->format.fontsize;
        range->object = NULL;
        range->underline = FALSE;
        range->pair_kerning = FALSE;

        range->fontfamily = heap_strdupW(layout->format.family_name);
        if (!range->fontfamily) {
            heap_free(range);
            return NULL;
        }

        range->collection = layout->format.collection;
        if (range->collection)
            IDWriteFontCollection_AddRef(range->collection);
        strcpyW(range->locale, layout->format.locale);

        h = &range->h;
        break;
    }
    case LAYOUT_RANGE_STRIKETHROUGH:
    {
        struct layout_range_bool *range;

        range = heap_alloc(sizeof(*range));
        if (!range) return NULL;

        range->value = FALSE;
        h = &range->h;
        break;
    }
    case LAYOUT_RANGE_EFFECT:
    {
        struct layout_range_effect *range;

        range = heap_alloc(sizeof(*range));
        if (!range) return NULL;

        range->effect = NULL;
        h = &range->h;
        break;
    }
    case LAYOUT_RANGE_SPACING:
    {
        struct layout_range_spacing *range;

        range = heap_alloc(sizeof(*range));
        if (!range) return NULL;

        range->leading = 0.0;
        range->trailing = 0.0;
        range->min_advance = 0.0;
        h = &range->h;
        break;
    }
    default:
        FIXME("unknown range kind %d\n", kind);
        return NULL;
    }

    h->kind = kind;
    h->range = *r;
    return h;
}

static struct layout_range_header *alloc_layout_range_from(struct layout_range_header *h, const DWRITE_TEXT_RANGE *r)
{
    struct layout_range_header *ret;

    switch (h->kind)
    {
    case LAYOUT_RANGE_REGULAR:
    {
        struct layout_range *from = (struct layout_range*)h;

        struct layout_range *range = heap_alloc(sizeof(*range));
        if (!range) return NULL;

        *range = *from;
        range->fontfamily = heap_strdupW(from->fontfamily);
        if (!range->fontfamily) {
            heap_free(range);
            return NULL;
        }

        /* update refcounts */
        if (range->object)
            IDWriteInlineObject_AddRef(range->object);
        if (range->collection)
            IDWriteFontCollection_AddRef(range->collection);
        ret = &range->h;
        break;
    }
    case LAYOUT_RANGE_STRIKETHROUGH:
    {
        struct layout_range_bool *strike = heap_alloc(sizeof(*strike));
        if (!strike) return NULL;

        *strike = *(struct layout_range_bool*)h;
        ret = &strike->h;
        break;
    }
    case LAYOUT_RANGE_EFFECT:
    {
        struct layout_range_effect *effect = heap_alloc(sizeof(*effect));
        if (!effect) return NULL;

        *effect = *(struct layout_range_effect*)h;
        if (effect->effect)
            IUnknown_AddRef(effect->effect);
        ret = &effect->h;
        break;
    }
    case LAYOUT_RANGE_SPACING:
    {
        struct layout_range_spacing *spacing = heap_alloc(sizeof(*spacing));
        if (!spacing) return NULL;

        *spacing = *(struct layout_range_spacing*)h;
        ret = &spacing->h;
        break;
    }
    default:
        FIXME("unknown range kind %d\n", h->kind);
        return NULL;
    }

    ret->range = *r;
    return ret;
}

static void free_layout_range(struct layout_range_header *h)
{
    if (!h)
        return;

    switch (h->kind)
    {
    case LAYOUT_RANGE_REGULAR:
    {
        struct layout_range *range = (struct layout_range*)h;

        if (range->object)
            IDWriteInlineObject_Release(range->object);
        if (range->collection)
            IDWriteFontCollection_Release(range->collection);
        heap_free(range->fontfamily);
        break;
    }
    case LAYOUT_RANGE_EFFECT:
    {
        struct layout_range_effect *effect = (struct layout_range_effect*)h;
        if (effect->effect)
            IUnknown_Release(effect->effect);
        break;
    }
    default:
        ;
    }

    heap_free(h);
}

static void free_layout_ranges_list(struct dwrite_textlayout *layout)
{
    struct layout_range_header *cur, *cur2;

    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->ranges, struct layout_range_header, entry) {
        list_remove(&cur->entry);
        free_layout_range(cur);
    }

    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->strike_ranges, struct layout_range_header, entry) {
        list_remove(&cur->entry);
        free_layout_range(cur);
    }

    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->effects, struct layout_range_header, entry) {
        list_remove(&cur->entry);
        free_layout_range(cur);
    }

    LIST_FOR_EACH_ENTRY_SAFE(cur, cur2, &layout->spacing, struct layout_range_header, entry) {
        list_remove(&cur->entry);
        free_layout_range(cur);
    }
}

static struct layout_range_header *find_outer_range(struct list *ranges, const DWRITE_TEXT_RANGE *range)
{
    struct layout_range_header *cur;

    LIST_FOR_EACH_ENTRY(cur, ranges, struct layout_range_header, entry) {

        if (cur->range.startPosition > range->startPosition)
            return NULL;

        if ((cur->range.startPosition + cur->range.length < range->startPosition + range->length) &&
            (range->startPosition < cur->range.startPosition + cur->range.length))
            return NULL;
        if (cur->range.startPosition + cur->range.length >= range->startPosition + range->length)
            return cur;
    }

    return NULL;
}

static struct layout_range *get_layout_range_by_pos(struct dwrite_textlayout *layout, UINT32 pos)
{
    struct layout_range *cur;

    LIST_FOR_EACH_ENTRY(cur, &layout->ranges, struct layout_range, h.entry) {
        DWRITE_TEXT_RANGE *r = &cur->h.range;
        if (r->startPosition <= pos && pos < r->startPosition + r->length)
            return cur;
    }

    return NULL;
}

static inline BOOL set_layout_range_iface_attr(IUnknown **dest, IUnknown *value)
{
    if (*dest == value) return FALSE;

    if (*dest)
        IUnknown_Release(*dest);
    *dest = value;
    if (*dest)
        IUnknown_AddRef(*dest);

    return TRUE;
}

static BOOL set_layout_range_attrval(struct layout_range_header *h, enum layout_range_attr_kind attr, struct layout_range_attr_value *value)
{
    struct layout_range_spacing *dest_spacing = (struct layout_range_spacing*)h;
    struct layout_range_effect *dest_effect = (struct layout_range_effect*)h;
    struct layout_range_bool *dest_bool = (struct layout_range_bool*)h;
    struct layout_range *dest = (struct layout_range*)h;

    BOOL changed = FALSE;

    switch (attr) {
    case LAYOUT_RANGE_ATTR_WEIGHT:
        changed = dest->weight != value->u.weight;
        dest->weight = value->u.weight;
        break;
    case LAYOUT_RANGE_ATTR_STYLE:
        changed = dest->style != value->u.style;
        dest->style = value->u.style;
        break;
    case LAYOUT_RANGE_ATTR_STRETCH:
        changed = dest->stretch != value->u.stretch;
        dest->stretch = value->u.stretch;
        break;
    case LAYOUT_RANGE_ATTR_FONTSIZE:
        changed = dest->fontsize != value->u.fontsize;
        dest->fontsize = value->u.fontsize;
        break;
    case LAYOUT_RANGE_ATTR_INLINE:
        changed = set_layout_range_iface_attr((IUnknown**)&dest->object, (IUnknown*)value->u.object);
        break;
    case LAYOUT_RANGE_ATTR_EFFECT:
        changed = set_layout_range_iface_attr((IUnknown**)&dest_effect->effect, (IUnknown*)value->u.effect);
        break;
    case LAYOUT_RANGE_ATTR_UNDERLINE:
        changed = dest->underline != value->u.underline;
        dest->underline = value->u.underline;
        break;
    case LAYOUT_RANGE_ATTR_STRIKETHROUGH:
        changed = dest_bool->value != value->u.strikethrough;
        dest_bool->value = value->u.strikethrough;
        break;
    case LAYOUT_RANGE_ATTR_PAIR_KERNING:
        changed = dest->pair_kerning != value->u.pair_kerning;
        dest->pair_kerning = value->u.pair_kerning;
        break;
    case LAYOUT_RANGE_ATTR_FONTCOLL:
        changed = set_layout_range_iface_attr((IUnknown**)&dest->collection, (IUnknown*)value->u.collection);
        break;
    case LAYOUT_RANGE_ATTR_LOCALE:
        changed = strcmpW(dest->locale, value->u.locale) != 0;
        if (changed)
            strcpyW(dest->locale, value->u.locale);
        break;
    case LAYOUT_RANGE_ATTR_FONTFAMILY:
        changed = strcmpW(dest->fontfamily, value->u.fontfamily) != 0;
        if (changed) {
            heap_free(dest->fontfamily);
            dest->fontfamily = heap_strdupW(value->u.fontfamily);
        }
        break;
    case LAYOUT_RANGE_ATTR_SPACING:
        changed = dest_spacing->leading != value->u.spacing[0] ||
            dest_spacing->trailing != value->u.spacing[1] ||
            dest_spacing->min_advance != value->u.spacing[2];
        dest_spacing->leading = value->u.spacing[0];
        dest_spacing->trailing = value->u.spacing[1];
        dest_spacing->min_advance = value->u.spacing[2];
        break;
    default:
        ;
    }

    return changed;
}

static inline BOOL is_in_layout_range(const DWRITE_TEXT_RANGE *outer, const DWRITE_TEXT_RANGE *inner)
{
    return (inner->startPosition >= outer->startPosition) &&
           (inner->startPosition + inner->length <= outer->startPosition + outer->length);
}

static inline HRESULT return_range(const struct layout_range_header *h, DWRITE_TEXT_RANGE *r)
{
    if (r) *r = h->range;
    return S_OK;
}

/* Set attribute value for given range, does all needed splitting/merging of existing ranges. */
static HRESULT set_layout_range_attr(struct dwrite_textlayout *layout, enum layout_range_attr_kind attr, struct layout_range_attr_value *value)
{
    struct layout_range_header *cur, *right, *left, *outer;
    BOOL changed = FALSE;
    struct list *ranges;
    DWRITE_TEXT_RANGE r;

    /* ignore zero length ranges */
    if (value->range.length == 0)
        return S_OK;

    /* select from ranges lists */
    switch (attr)
    {
    case LAYOUT_RANGE_ATTR_WEIGHT:
    case LAYOUT_RANGE_ATTR_STYLE:
    case LAYOUT_RANGE_ATTR_STRETCH:
    case LAYOUT_RANGE_ATTR_FONTSIZE:
    case LAYOUT_RANGE_ATTR_INLINE:
    case LAYOUT_RANGE_ATTR_UNDERLINE:
    case LAYOUT_RANGE_ATTR_PAIR_KERNING:
    case LAYOUT_RANGE_ATTR_FONTCOLL:
    case LAYOUT_RANGE_ATTR_LOCALE:
    case LAYOUT_RANGE_ATTR_FONTFAMILY:
        ranges = &layout->ranges;
        break;
    case LAYOUT_RANGE_ATTR_STRIKETHROUGH:
        ranges = &layout->strike_ranges;
        break;
    case LAYOUT_RANGE_ATTR_EFFECT:
        ranges = &layout->effects;
        break;
    case LAYOUT_RANGE_ATTR_SPACING:
        ranges = &layout->spacing;
        break;
    default:
        FIXME("unknown attr kind %d\n", attr);
        return E_FAIL;
    }

    /* If new range is completely within existing range, split existing range in two */
    if ((outer = find_outer_range(ranges, &value->range))) {

        /* no need to add same range */
        if (is_same_layout_attrvalue(outer, attr, value))
            return S_OK;

        /* for matching range bounds just replace data */
        if (is_same_text_range(&outer->range, &value->range)) {
            changed = set_layout_range_attrval(outer, attr, value);
            goto done;
        }

        /* add new range to the left */
        if (value->range.startPosition == outer->range.startPosition) {
            left = alloc_layout_range_from(outer, &value->range);
            if (!left) return E_OUTOFMEMORY;

            changed = set_layout_range_attrval(left, attr, value);
            list_add_before(&outer->entry, &left->entry);
            outer->range.startPosition += value->range.length;
            outer->range.length -= value->range.length;
            goto done;
        }

        /* add new range to the right */
        if (value->range.startPosition + value->range.length == outer->range.startPosition + outer->range.length) {
            right = alloc_layout_range_from(outer, &value->range);
            if (!right) return E_OUTOFMEMORY;

            changed = set_layout_range_attrval(right, attr, value);
            list_add_after(&outer->entry, &right->entry);
            outer->range.length -= value->range.length;
            goto done;
        }

        r.startPosition = value->range.startPosition + value->range.length;
        r.length = outer->range.length + outer->range.startPosition - r.startPosition;

        /* right part */
        right = alloc_layout_range_from(outer, &r);
        /* new range in the middle */
        cur = alloc_layout_range_from(outer, &value->range);
        if (!right || !cur) {
            free_layout_range(right);
            free_layout_range(cur);
            return E_OUTOFMEMORY;
        }

        /* reuse container range as a left part */
        outer->range.length = value->range.startPosition - outer->range.startPosition;

        /* new part */
        set_layout_range_attrval(cur, attr, value);

        list_add_after(&outer->entry, &cur->entry);
        list_add_after(&cur->entry, &right->entry);

        return S_OK;
    }

    /* Now it's only possible that given range contains some existing ranges, fully or partially.
       Update all of them. */
    left = get_layout_range_header_by_pos(ranges, value->range.startPosition);
    if (left->range.startPosition == value->range.startPosition)
        changed = set_layout_range_attrval(left, attr, value);
    else /* need to split */ {
        r.startPosition = value->range.startPosition;
        r.length = left->range.length - value->range.startPosition + left->range.startPosition;
        left->range.length -= r.length;
        cur = alloc_layout_range_from(left, &r);
        changed = set_layout_range_attrval(cur, attr, value);
        list_add_after(&left->entry, &cur->entry);
    }
    cur = LIST_ENTRY(list_next(ranges, &left->entry), struct layout_range_header, entry);

    /* for all existing ranges covered by new one update value */
    while (cur && is_in_layout_range(&value->range, &cur->range)) {
        changed = set_layout_range_attrval(cur, attr, value);
        cur = LIST_ENTRY(list_next(ranges, &cur->entry), struct layout_range_header, entry);
    }

    /* it's possible rightmost range intersects */
    if (cur && (cur->range.startPosition < value->range.startPosition + value->range.length)) {
        r.startPosition = cur->range.startPosition;
        r.length = value->range.startPosition + value->range.length - cur->range.startPosition;
        left = alloc_layout_range_from(cur, &r);
        changed = set_layout_range_attrval(left, attr, value);
        cur->range.startPosition += left->range.length;
        cur->range.length -= left->range.length;
        list_add_before(&cur->entry, &left->entry);
    }

done:
    if (changed) {
        struct list *next, *i;

        layout->recompute = RECOMPUTE_EVERYTHING;
        i = list_head(ranges);
        while ((next = list_next(ranges, i))) {
            struct layout_range_header *next_range = LIST_ENTRY(next, struct layout_range_header, entry);

            cur = LIST_ENTRY(i, struct layout_range_header, entry);
            if (is_same_layout_attributes(cur, next_range)) {
                /* remove similar range */
                cur->range.length += next_range->range.length;
                list_remove(next);
                free_layout_range(next_range);
            }
            else
                i = list_next(ranges, i);
        }
    }

    return S_OK;
}

static inline const WCHAR *get_string_attribute_ptr(struct layout_range *range, enum layout_range_attr_kind kind)
{
    const WCHAR *str;

    switch (kind) {
        case LAYOUT_RANGE_ATTR_LOCALE:
            str = range->locale;
            break;
        case LAYOUT_RANGE_ATTR_FONTFAMILY:
            str = range->fontfamily;
            break;
        default:
            str = NULL;
    }

    return str;
}

static HRESULT get_string_attribute_length(struct dwrite_textlayout *layout, enum layout_range_attr_kind kind, UINT32 position,
    UINT32 *length, DWRITE_TEXT_RANGE *r)
{
    struct layout_range *range;
    const WCHAR *str;

    range = get_layout_range_by_pos(layout, position);
    if (!range) {
        *length = 0;
        return S_OK;
    }

    str = get_string_attribute_ptr(range, kind);
    *length = strlenW(str);
    return return_range(&range->h, r);
}

static HRESULT get_string_attribute_value(struct dwrite_textlayout *layout, enum layout_range_attr_kind kind, UINT32 position,
    WCHAR *ret, UINT32 length, DWRITE_TEXT_RANGE *r)
{
    struct layout_range *range;
    const WCHAR *str;

    if (length == 0)
        return E_INVALIDARG;

    ret[0] = 0;
    range = get_layout_range_by_pos(layout, position);
    if (!range)
        return E_INVALIDARG;

    str = get_string_attribute_ptr(range, kind);
    if (length < strlenW(str) + 1)
        return E_NOT_SUFFICIENT_BUFFER;

    strcpyW(ret, str);
    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_QueryInterface(IDWriteTextLayout2 *iface, REFIID riid, void **obj)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualIID(riid, &IID_IDWriteTextLayout2) ||
        IsEqualIID(riid, &IID_IDWriteTextLayout1) ||
        IsEqualIID(riid, &IID_IDWriteTextLayout) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
    }
    else if (IsEqualIID(riid, &IID_IDWriteTextFormat1) ||
             IsEqualIID(riid, &IID_IDWriteTextFormat))
        *obj = &This->IDWriteTextFormat1_iface;

    if (*obj) {
        IDWriteTextLayout2_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetextlayout_AddRef(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI dwritetextlayout_Release(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref) {
        free_layout_ranges_list(This);
        free_layout_eruns(This);
        free_layout_runs(This);
        release_format_data(&This->format);
        heap_free(This->nominal_breakpoints);
        heap_free(This->actual_breakpoints);
        heap_free(This->clustermetrics);
        heap_free(This->clusters);
        heap_free(This->lines);
        heap_free(This->str);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI dwritetextlayout_SetTextAlignment(IDWriteTextLayout2 *iface, DWRITE_TEXT_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    return IDWriteTextFormat1_SetTextAlignment(&This->IDWriteTextFormat1_iface, alignment);
}

static HRESULT WINAPI dwritetextlayout_SetParagraphAlignment(IDWriteTextLayout2 *iface, DWRITE_PARAGRAPH_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    return IDWriteTextFormat1_SetParagraphAlignment(&This->IDWriteTextFormat1_iface, alignment);
}

static HRESULT WINAPI dwritetextlayout_SetWordWrapping(IDWriteTextLayout2 *iface, DWRITE_WORD_WRAPPING wrapping)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%d)\n", This, wrapping);
    return IDWriteTextFormat1_SetWordWrapping(&This->IDWriteTextFormat1_iface, wrapping);
}

static HRESULT WINAPI dwritetextlayout_SetReadingDirection(IDWriteTextLayout2 *iface, DWRITE_READING_DIRECTION direction)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    return IDWriteTextFormat1_SetReadingDirection(&This->IDWriteTextFormat1_iface, direction);
}

static HRESULT WINAPI dwritetextlayout_SetFlowDirection(IDWriteTextLayout2 *iface, DWRITE_FLOW_DIRECTION direction)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%d)\n", This, direction);
    return IDWriteTextFormat1_SetFlowDirection(&This->IDWriteTextFormat1_iface, direction);
}

static HRESULT WINAPI dwritetextlayout_SetIncrementalTabStop(IDWriteTextLayout2 *iface, FLOAT tabstop)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%.2f)\n", This, tabstop);
    return IDWriteTextFormat1_SetIncrementalTabStop(&This->IDWriteTextFormat1_iface, tabstop);
}

static HRESULT WINAPI dwritetextlayout_SetTrimming(IDWriteTextLayout2 *iface, DWRITE_TRIMMING const *trimming,
    IDWriteInlineObject *trimming_sign)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p %p)\n", This, trimming, trimming_sign);
    return IDWriteTextFormat1_SetTrimming(&This->IDWriteTextFormat1_iface, trimming, trimming_sign);
}

static HRESULT WINAPI dwritetextlayout_SetLineSpacing(IDWriteTextLayout2 *iface, DWRITE_LINE_SPACING_METHOD spacing,
    FLOAT line_spacing, FLOAT baseline)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%d %.2f %.2f)\n", This, spacing, line_spacing, baseline);
    return IDWriteTextFormat1_SetLineSpacing(&This->IDWriteTextFormat1_iface, spacing, line_spacing, baseline);
}

static DWRITE_TEXT_ALIGNMENT WINAPI dwritetextlayout_GetTextAlignment(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    return IDWriteTextFormat1_GetTextAlignment(&This->IDWriteTextFormat1_iface);
}

static DWRITE_PARAGRAPH_ALIGNMENT WINAPI dwritetextlayout_GetParagraphAlignment(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetParagraphAlignment(&This->IDWriteTextFormat1_iface);
}

static DWRITE_WORD_WRAPPING WINAPI dwritetextlayout_GetWordWrapping(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetWordWrapping(&This->IDWriteTextFormat1_iface);
}

static DWRITE_READING_DIRECTION WINAPI dwritetextlayout_GetReadingDirection(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetReadingDirection(&This->IDWriteTextFormat1_iface);
}

static DWRITE_FLOW_DIRECTION WINAPI dwritetextlayout_GetFlowDirection(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFlowDirection(&This->IDWriteTextFormat1_iface);
}

static FLOAT WINAPI dwritetextlayout_GetIncrementalTabStop(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetIncrementalTabStop(&This->IDWriteTextFormat1_iface);
}

static HRESULT WINAPI dwritetextlayout_GetTrimming(IDWriteTextLayout2 *iface, DWRITE_TRIMMING *options,
    IDWriteInlineObject **trimming_sign)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p %p)\n", This, options, trimming_sign);
    return IDWriteTextFormat1_GetTrimming(&This->IDWriteTextFormat1_iface, options, trimming_sign);
}

static HRESULT WINAPI dwritetextlayout_GetLineSpacing(IDWriteTextLayout2 *iface, DWRITE_LINE_SPACING_METHOD *method,
    FLOAT *spacing, FLOAT *baseline)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p %p %p)\n", This, method, spacing, baseline);
    return IDWriteTextFormat1_GetLineSpacing(&This->IDWriteTextFormat1_iface, method, spacing, baseline);
}

static HRESULT WINAPI dwritetextlayout_GetFontCollection(IDWriteTextLayout2 *iface, IDWriteFontCollection **collection)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p)\n", This, collection);
    return IDWriteTextFormat1_GetFontCollection(&This->IDWriteTextFormat1_iface, collection);
}

static UINT32 WINAPI dwritetextlayout_GetFontFamilyNameLength(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFontFamilyNameLength(&This->IDWriteTextFormat1_iface);
}

static HRESULT WINAPI dwritetextlayout_GetFontFamilyName(IDWriteTextLayout2 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p %u)\n", This, name, size);
    return IDWriteTextFormat1_GetFontFamilyName(&This->IDWriteTextFormat1_iface, name, size);
}

static DWRITE_FONT_WEIGHT WINAPI dwritetextlayout_GetFontWeight(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFontWeight(&This->IDWriteTextFormat1_iface);
}

static DWRITE_FONT_STYLE WINAPI dwritetextlayout_GetFontStyle(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFontStyle(&This->IDWriteTextFormat1_iface);
}

static DWRITE_FONT_STRETCH WINAPI dwritetextlayout_GetFontStretch(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFontStretch(&This->IDWriteTextFormat1_iface);
}

static FLOAT WINAPI dwritetextlayout_GetFontSize(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetFontSize(&This->IDWriteTextFormat1_iface);
}

static UINT32 WINAPI dwritetextlayout_GetLocaleNameLength(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return IDWriteTextFormat1_GetLocaleNameLength(&This->IDWriteTextFormat1_iface);
}

static HRESULT WINAPI dwritetextlayout_GetLocaleName(IDWriteTextLayout2 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p %u)\n", This, name, size);
    return IDWriteTextFormat1_GetLocaleName(&This->IDWriteTextFormat1_iface, name, size);
}

static HRESULT WINAPI dwritetextlayout_SetMaxWidth(IDWriteTextLayout2 *iface, FLOAT maxWidth)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);

    TRACE("(%p)->(%.2f)\n", This, maxWidth);

    if (maxWidth < 0.0)
        return E_INVALIDARG;

    This->metrics.layoutWidth = maxWidth;
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_SetMaxHeight(IDWriteTextLayout2 *iface, FLOAT maxHeight)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);

    TRACE("(%p)->(%.2f)\n", This, maxHeight);

    if (maxHeight < 0.0)
        return E_INVALIDARG;

    This->metrics.layoutHeight = maxHeight;
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_SetFontCollection(IDWriteTextLayout2 *iface, IDWriteFontCollection* collection, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%p %s)\n", This, collection, debugstr_range(&range));

    value.range = range;
    value.u.collection = collection;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_FONTCOLL, &value);
}

static HRESULT WINAPI dwritetextlayout_SetFontFamilyName(IDWriteTextLayout2 *iface, WCHAR const *name, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%s %s)\n", This, debugstr_w(name), debugstr_range(&range));

    if (!name)
        return E_INVALIDARG;

    value.range = range;
    value.u.fontfamily = name;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_FONTFAMILY, &value);
}

static HRESULT WINAPI dwritetextlayout_SetFontWeight(IDWriteTextLayout2 *iface, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, weight, debugstr_range(&range));

    value.range = range;
    value.u.weight = weight;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_WEIGHT, &value);
}

static HRESULT WINAPI dwritetextlayout_SetFontStyle(IDWriteTextLayout2 *iface, DWRITE_FONT_STYLE style, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, style, debugstr_range(&range));

    if ((UINT32)style > DWRITE_FONT_STYLE_ITALIC)
        return E_INVALIDARG;

    value.range = range;
    value.u.style = style;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_STYLE, &value);
}

static HRESULT WINAPI dwritetextlayout_SetFontStretch(IDWriteTextLayout2 *iface, DWRITE_FONT_STRETCH stretch, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, stretch, debugstr_range(&range));

    if (stretch == DWRITE_FONT_STRETCH_UNDEFINED || (UINT32)stretch > DWRITE_FONT_STRETCH_ULTRA_EXPANDED)
        return E_INVALIDARG;

    value.range = range;
    value.u.stretch = stretch;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_STRETCH, &value);
}

static HRESULT WINAPI dwritetextlayout_SetFontSize(IDWriteTextLayout2 *iface, FLOAT size, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%.2f %s)\n", This, size, debugstr_range(&range));

    if (size <= 0.0)
        return E_INVALIDARG;

    value.range = range;
    value.u.fontsize = size;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_FONTSIZE, &value);
}

static HRESULT WINAPI dwritetextlayout_SetUnderline(IDWriteTextLayout2 *iface, BOOL underline, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, underline, debugstr_range(&range));

    value.range = range;
    value.u.underline = underline;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_UNDERLINE, &value);
}

static HRESULT WINAPI dwritetextlayout_SetStrikethrough(IDWriteTextLayout2 *iface, BOOL strikethrough, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, strikethrough, debugstr_range(&range));

    value.range = range;
    value.u.strikethrough = strikethrough;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_STRIKETHROUGH, &value);
}

static HRESULT WINAPI dwritetextlayout_SetDrawingEffect(IDWriteTextLayout2 *iface, IUnknown* effect, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%p %s)\n", This, effect, debugstr_range(&range));

    value.range = range;
    value.u.effect = effect;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_EFFECT, &value);
}

static HRESULT WINAPI dwritetextlayout_SetInlineObject(IDWriteTextLayout2 *iface, IDWriteInlineObject *object, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%p %s)\n", This, object, debugstr_range(&range));

    value.range = range;
    value.u.object = object;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_INLINE, &value);
}

static HRESULT WINAPI dwritetextlayout_SetTypography(IDWriteTextLayout2 *iface, IDWriteTypography* typography, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%p %s): stub\n", This, typography, debugstr_range(&range));
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_SetLocaleName(IDWriteTextLayout2 *iface, WCHAR const* locale, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%s %s)\n", This, debugstr_w(locale), debugstr_range(&range));

    if (!locale || strlenW(locale) > LOCALE_NAME_MAX_LENGTH-1)
        return E_INVALIDARG;

    value.range = range;
    value.u.locale = locale;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_LOCALE, &value);
}

static FLOAT WINAPI dwritetextlayout_GetMaxWidth(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return This->metrics.layoutWidth;
}

static FLOAT WINAPI dwritetextlayout_GetMaxHeight(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return This->metrics.layoutHeight;
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontCollection(IDWriteTextLayout2 *iface, UINT32 position,
    IDWriteFontCollection** collection, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, collection, r);

    if (position >= This->len)
        return S_OK;

    range = get_layout_range_by_pos(This, position);
    *collection = range->collection;
    if (*collection)
        IDWriteFontCollection_AddRef(*collection);

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontFamilyNameLength(IDWriteTextLayout2 *iface,
    UINT32 position, UINT32 *length, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%d %p %p)\n", This, position, length, r);
    return get_string_attribute_length(This, LAYOUT_RANGE_ATTR_FONTFAMILY, position, length, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontFamilyName(IDWriteTextLayout2 *iface,
    UINT32 position, WCHAR *name, UINT32 length, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%u %p %u %p)\n", This, position, name, length, r);
    return get_string_attribute_value(This, LAYOUT_RANGE_ATTR_FONTFAMILY, position, name, length, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontWeight(IDWriteTextLayout2 *iface,
    UINT32 position, DWRITE_FONT_WEIGHT *weight, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, weight, r);

    if (position >= This->len)
        return S_OK;

    range = get_layout_range_by_pos(This, position);
    *weight = range->weight;

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontStyle(IDWriteTextLayout2 *iface,
    UINT32 position, DWRITE_FONT_STYLE *style, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, style, r);

    range = get_layout_range_by_pos(This, position);
    *style = range->style;
    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontStretch(IDWriteTextLayout2 *iface,
    UINT32 position, DWRITE_FONT_STRETCH *stretch, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, stretch, r);

    range = get_layout_range_by_pos(This, position);
    *stretch = range->stretch;
    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetFontSize(IDWriteTextLayout2 *iface,
    UINT32 position, FLOAT *size, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, size, r);

    range = get_layout_range_by_pos(This, position);
    *size = range->fontsize;
    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_GetUnderline(IDWriteTextLayout2 *iface,
    UINT32 position, BOOL *underline, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, underline, r);

    if (position >= This->len)
        return S_OK;

    range = get_layout_range_by_pos(This, position);
    *underline = range->underline;

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_GetStrikethrough(IDWriteTextLayout2 *iface,
    UINT32 position, BOOL *strikethrough, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_bool *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, strikethrough, r);

    range = (struct layout_range_bool*)get_layout_range_header_by_pos(&This->strike_ranges, position);
    *strikethrough = range->value;

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_GetDrawingEffect(IDWriteTextLayout2 *iface,
    UINT32 position, IUnknown **effect, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_effect *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, effect, r);

    range = (struct layout_range_effect*)get_layout_range_header_by_pos(&This->effects, position);
    *effect = range->effect;
    if (*effect)
        IUnknown_AddRef(*effect);

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_GetInlineObject(IDWriteTextLayout2 *iface,
    UINT32 position, IDWriteInlineObject **object, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, object, r);

    if (position >= This->len)
        return S_OK;

    range = get_layout_range_by_pos(This, position);
    *object = range->object;
    if (*object)
        IDWriteInlineObject_AddRef(*object);

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout_GetTypography(IDWriteTextLayout2 *iface,
    UINT32 position, IDWriteTypography** typography, DWRITE_TEXT_RANGE *range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%u %p %p): stub\n", This, position, typography, range);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_layout_GetLocaleNameLength(IDWriteTextLayout2 *iface,
    UINT32 position, UINT32* length, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%u %p %p)\n", This, position, length, r);
    return get_string_attribute_length(This, LAYOUT_RANGE_ATTR_LOCALE, position, length, r);
}

static HRESULT WINAPI dwritetextlayout_layout_GetLocaleName(IDWriteTextLayout2 *iface,
    UINT32 position, WCHAR* locale, UINT32 length, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%u %p %u %p)\n", This, position, locale, length, r);
    return get_string_attribute_value(This, LAYOUT_RANGE_ATTR_LOCALE, position, locale, length, r);
}

static HRESULT WINAPI dwritetextlayout_Draw(IDWriteTextLayout2 *iface,
    void *context, IDWriteTextRenderer* renderer, FLOAT origin_x, FLOAT origin_y)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_effective_inline *inlineobject;
    struct layout_effective_run *run;
    struct layout_strikethrough *s;
    HRESULT hr;

    TRACE("(%p)->(%p %p %.2f %.2f)\n", This, context, renderer, origin_x, origin_y);

    hr = layout_compute_effective_runs(This);
    if (FAILED(hr))
        return hr;

    /* 1. Regular runs */
    LIST_FOR_EACH_ENTRY(run, &This->eruns, struct layout_effective_run, entry) {
        const struct regular_layout_run *regular = &run->run->u.regular;
        UINT32 start_glyph = regular->clustermap[run->start];
        DWRITE_GLYPH_RUN_DESCRIPTION descr;
        DWRITE_GLYPH_RUN glyph_run;

        /* Everything but cluster map will be reused from nominal run, as we only need
           to adjust some pointers. Cluster map however is rebuilt when effective run is added,
           it can't be reused because it has to start with 0 index for each reported run. */
        glyph_run = regular->run;
        glyph_run.glyphCount = run->glyphcount;

        /* fixup glyph data arrays */
        glyph_run.glyphIndices += start_glyph;
        glyph_run.glyphAdvances += start_glyph;
        glyph_run.glyphOffsets += start_glyph;

        /* description */
        descr = regular->descr;
        descr.stringLength = run->length;
        descr.string += run->start;
        descr.clusterMap = run->clustermap;
        descr.textPosition += run->start;

        /* return value is ignored */
        IDWriteTextRenderer_DrawGlyphRun(renderer,
            context,
            run->origin_x + run->align_dx + origin_x,
            run->origin_y + origin_y,
            DWRITE_MEASURING_MODE_NATURAL,
            &glyph_run,
            &descr,
            NULL);
    }

    /* 2. Inline objects */
    LIST_FOR_EACH_ENTRY(inlineobject, &This->inlineobjects, struct layout_effective_inline, entry) {
        IDWriteTextRenderer_DrawInlineObject(renderer,
            context,
            inlineobject->origin_x + inlineobject->align_dx + origin_x,
            inlineobject->origin_y + origin_y,
            inlineobject->object,
            inlineobject->is_sideways,
            inlineobject->is_rtl,
            inlineobject->effect);
    }

    /* TODO: 3. Underlines */

    /* 4. Strikethrough */
    LIST_FOR_EACH_ENTRY(s, &This->strikethrough, struct layout_strikethrough, entry) {
        IDWriteTextRenderer_DrawStrikethrough(renderer,
            context,
            s->run->origin_x,
            s->run->origin_y,
            &s->s,
            NULL);
    }

    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_GetLineMetrics(IDWriteTextLayout2 *iface,
    DWRITE_LINE_METRICS *metrics, UINT32 max_count, UINT32 *count)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    HRESULT hr;

    TRACE("(%p)->(%p %u %p)\n", This, metrics, max_count, count);

    hr = layout_compute_effective_runs(This);
    if (FAILED(hr))
        return hr;

    if (metrics)
        memcpy(metrics, This->lines, sizeof(*metrics)*min(max_count, This->metrics.lineCount));

    *count = This->metrics.lineCount;
    return max_count >= This->metrics.lineCount ? S_OK : E_NOT_SUFFICIENT_BUFFER;
}

static HRESULT WINAPI dwritetextlayout_GetMetrics(IDWriteTextLayout2 *iface, DWRITE_TEXT_METRICS *metrics)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    DWRITE_TEXT_METRICS1 metrics1;
    HRESULT hr;

    TRACE("(%p)->(%p)\n", This, metrics);

    hr = IDWriteTextLayout2_GetMetrics(iface, &metrics1);
    if (hr == S_OK)
        memcpy(metrics, &metrics1, sizeof(*metrics));

    return hr;
}

static HRESULT WINAPI dwritetextlayout_GetOverhangMetrics(IDWriteTextLayout2 *iface, DWRITE_OVERHANG_METRICS *overhangs)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%p): stub\n", This, overhangs);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_GetClusterMetrics(IDWriteTextLayout2 *iface,
    DWRITE_CLUSTER_METRICS *metrics, UINT32 max_count, UINT32 *count)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    HRESULT hr;

    TRACE("(%p)->(%p %u %p)\n", This, metrics, max_count, count);

    hr = layout_compute(This);
    if (FAILED(hr))
        return hr;

    if (metrics)
        memcpy(metrics, This->clustermetrics, sizeof(DWRITE_CLUSTER_METRICS)*min(max_count, This->cluster_count));

    *count = This->cluster_count;
    return max_count >= This->cluster_count ? S_OK : E_NOT_SUFFICIENT_BUFFER;
}

/* Only to be used with DetermineMinWidth() to find the longest cluster sequence that we don't want to try
   too hard to break. */
static inline BOOL is_terminal_cluster(struct dwrite_textlayout *layout, UINT32 index)
{
    if (layout->clustermetrics[index].isWhitespace || layout->clustermetrics[index].isNewline ||
       (index == layout->cluster_count - 1))
        return TRUE;
    /* check next one */
    return (index < layout->cluster_count - 1) && layout->clustermetrics[index+1].isWhitespace;
}

static HRESULT WINAPI dwritetextlayout_DetermineMinWidth(IDWriteTextLayout2 *iface, FLOAT* min_width)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FLOAT width;
    HRESULT hr;
    UINT32 i;

    TRACE("(%p)->(%p)\n", This, min_width);

    if (!min_width)
        return E_INVALIDARG;

    if (!(This->recompute & RECOMPUTE_MINIMAL_WIDTH))
        goto width_done;

    *min_width = 0.0;
    hr = layout_compute(This);
    if (FAILED(hr))
        return hr;

    for (i = 0; i < This->cluster_count;) {
        if (is_terminal_cluster(This, i)) {
            width = This->clustermetrics[i].width;
            i++;
        }
        else {
            width = 0.0;
            while (!is_terminal_cluster(This, i)) {
                width += This->clustermetrics[i].width;
                i++;
            }
            /* count last one too */
            width += This->clustermetrics[i].width;
        }

        if (width > This->minwidth)
            This->minwidth = width;
    }
    This->recompute &= ~RECOMPUTE_MINIMAL_WIDTH;

width_done:
    *min_width = This->minwidth;
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_HitTestPoint(IDWriteTextLayout2 *iface,
    FLOAT pointX, FLOAT pointY, BOOL* is_trailinghit, BOOL* is_inside, DWRITE_HIT_TEST_METRICS *metrics)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%f %f %p %p %p): stub\n", This, pointX, pointY, is_trailinghit, is_inside, metrics);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_HitTestTextPosition(IDWriteTextLayout2 *iface,
    UINT32 textPosition, BOOL is_trailinghit, FLOAT* pointX, FLOAT* pointY, DWRITE_HIT_TEST_METRICS *metrics)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%u %d %p %p %p): stub\n", This, textPosition, is_trailinghit, pointX, pointY, metrics);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_HitTestTextRange(IDWriteTextLayout2 *iface,
    UINT32 textPosition, UINT32 textLength, FLOAT originX, FLOAT originY,
    DWRITE_HIT_TEST_METRICS *metrics, UINT32 max_metricscount, UINT32* actual_metricscount)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%u %u %f %f %p %u %p): stub\n", This, textPosition, textLength, originX, originY, metrics,
        max_metricscount, actual_metricscount);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout1_SetPairKerning(IDWriteTextLayout2 *iface, BOOL is_pairkerning_enabled,
        DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%d %s)\n", This, is_pairkerning_enabled, debugstr_range(&range));

    value.range = range;
    value.u.pair_kerning = !!is_pairkerning_enabled;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_PAIR_KERNING, &value);
}

static HRESULT WINAPI dwritetextlayout1_GetPairKerning(IDWriteTextLayout2 *iface, UINT32 position, BOOL *is_pairkerning_enabled,
        DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range *range;

    TRACE("(%p)->(%u %p %p)\n", This, position, is_pairkerning_enabled, r);

    if (position >= This->len)
        return S_OK;

    range = get_layout_range_by_pos(This, position);
    *is_pairkerning_enabled = range->pair_kerning;

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout1_SetCharacterSpacing(IDWriteTextLayout2 *iface, FLOAT leading, FLOAT trailing,
    FLOAT min_advance, DWRITE_TEXT_RANGE range)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_attr_value value;

    TRACE("(%p)->(%.2f %.2f %.2f %s)\n", This, leading, trailing, min_advance, debugstr_range(&range));

    if (min_advance < 0.0)
        return E_INVALIDARG;

    value.range = range;
    value.u.spacing[0] = leading;
    value.u.spacing[1] = trailing;
    value.u.spacing[2] = min_advance;
    return set_layout_range_attr(This, LAYOUT_RANGE_ATTR_SPACING, &value);
}

static HRESULT WINAPI dwritetextlayout1_GetCharacterSpacing(IDWriteTextLayout2 *iface, UINT32 position, FLOAT *leading,
    FLOAT *trailing, FLOAT *min_advance, DWRITE_TEXT_RANGE *r)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    struct layout_range_spacing *range;

    TRACE("(%p)->(%u %p %p %p %p)\n", This, position, leading, trailing, min_advance, r);

    range = (struct layout_range_spacing*)get_layout_range_header_by_pos(&This->spacing, position);
    *leading = range->leading;
    *trailing = range->trailing;
    *min_advance = range->min_advance;

    return return_range(&range->h, r);
}

static HRESULT WINAPI dwritetextlayout2_GetMetrics(IDWriteTextLayout2 *iface, DWRITE_TEXT_METRICS1 *metrics)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    HRESULT hr;

    TRACE("(%p)->(%p)\n", This, metrics);

    hr = layout_compute_effective_runs(This);
    if (FAILED(hr))
        return hr;

    *metrics = This->metrics;
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout2_SetVerticalGlyphOrientation(IDWriteTextLayout2 *iface, DWRITE_VERTICAL_GLYPH_ORIENTATION orientation)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);

    TRACE("(%p)->(%d)\n", This, orientation);

    if ((UINT32)orientation > DWRITE_VERTICAL_GLYPH_ORIENTATION_STACKED)
        return E_INVALIDARG;

    This->format.vertical_orientation = orientation;
    return S_OK;
}

static DWRITE_VERTICAL_GLYPH_ORIENTATION WINAPI dwritetextlayout2_GetVerticalGlyphOrientation(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)\n", This);
    return This->format.vertical_orientation;
}

static HRESULT WINAPI dwritetextlayout2_SetLastLineWrapping(IDWriteTextLayout2 *iface, BOOL lastline_wrapping_enabled)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%d): stub\n", This, lastline_wrapping_enabled);
    return E_NOTIMPL;
}

static BOOL WINAPI dwritetextlayout2_GetLastLineWrapping(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p): stub\n", This);
    return FALSE;
}

static HRESULT WINAPI dwritetextlayout2_SetOpticalAlignment(IDWriteTextLayout2 *iface, DWRITE_OPTICAL_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p)->(%d): stub\n", This, alignment);
    return E_NOTIMPL;
}

static DWRITE_OPTICAL_ALIGNMENT WINAPI dwritetextlayout2_GetOpticalAlignment(IDWriteTextLayout2 *iface)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    FIXME("(%p): stub\n", This);
    return DWRITE_OPTICAL_ALIGNMENT_NONE;
}

static HRESULT WINAPI dwritetextlayout2_SetFontFallback(IDWriteTextLayout2 *iface, IDWriteFontFallback *fallback)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return set_fontfallback_for_format(&This->format, fallback);
}

static HRESULT WINAPI dwritetextlayout2_GetFontFallback(IDWriteTextLayout2 *iface, IDWriteFontFallback **fallback)
{
    struct dwrite_textlayout *This = impl_from_IDWriteTextLayout2(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return get_fontfallback_from_format(&This->format, fallback);
}

static const IDWriteTextLayout2Vtbl dwritetextlayoutvtbl = {
    dwritetextlayout_QueryInterface,
    dwritetextlayout_AddRef,
    dwritetextlayout_Release,
    dwritetextlayout_SetTextAlignment,
    dwritetextlayout_SetParagraphAlignment,
    dwritetextlayout_SetWordWrapping,
    dwritetextlayout_SetReadingDirection,
    dwritetextlayout_SetFlowDirection,
    dwritetextlayout_SetIncrementalTabStop,
    dwritetextlayout_SetTrimming,
    dwritetextlayout_SetLineSpacing,
    dwritetextlayout_GetTextAlignment,
    dwritetextlayout_GetParagraphAlignment,
    dwritetextlayout_GetWordWrapping,
    dwritetextlayout_GetReadingDirection,
    dwritetextlayout_GetFlowDirection,
    dwritetextlayout_GetIncrementalTabStop,
    dwritetextlayout_GetTrimming,
    dwritetextlayout_GetLineSpacing,
    dwritetextlayout_GetFontCollection,
    dwritetextlayout_GetFontFamilyNameLength,
    dwritetextlayout_GetFontFamilyName,
    dwritetextlayout_GetFontWeight,
    dwritetextlayout_GetFontStyle,
    dwritetextlayout_GetFontStretch,
    dwritetextlayout_GetFontSize,
    dwritetextlayout_GetLocaleNameLength,
    dwritetextlayout_GetLocaleName,
    dwritetextlayout_SetMaxWidth,
    dwritetextlayout_SetMaxHeight,
    dwritetextlayout_SetFontCollection,
    dwritetextlayout_SetFontFamilyName,
    dwritetextlayout_SetFontWeight,
    dwritetextlayout_SetFontStyle,
    dwritetextlayout_SetFontStretch,
    dwritetextlayout_SetFontSize,
    dwritetextlayout_SetUnderline,
    dwritetextlayout_SetStrikethrough,
    dwritetextlayout_SetDrawingEffect,
    dwritetextlayout_SetInlineObject,
    dwritetextlayout_SetTypography,
    dwritetextlayout_SetLocaleName,
    dwritetextlayout_GetMaxWidth,
    dwritetextlayout_GetMaxHeight,
    dwritetextlayout_layout_GetFontCollection,
    dwritetextlayout_layout_GetFontFamilyNameLength,
    dwritetextlayout_layout_GetFontFamilyName,
    dwritetextlayout_layout_GetFontWeight,
    dwritetextlayout_layout_GetFontStyle,
    dwritetextlayout_layout_GetFontStretch,
    dwritetextlayout_layout_GetFontSize,
    dwritetextlayout_GetUnderline,
    dwritetextlayout_GetStrikethrough,
    dwritetextlayout_GetDrawingEffect,
    dwritetextlayout_GetInlineObject,
    dwritetextlayout_GetTypography,
    dwritetextlayout_layout_GetLocaleNameLength,
    dwritetextlayout_layout_GetLocaleName,
    dwritetextlayout_Draw,
    dwritetextlayout_GetLineMetrics,
    dwritetextlayout_GetMetrics,
    dwritetextlayout_GetOverhangMetrics,
    dwritetextlayout_GetClusterMetrics,
    dwritetextlayout_DetermineMinWidth,
    dwritetextlayout_HitTestPoint,
    dwritetextlayout_HitTestTextPosition,
    dwritetextlayout_HitTestTextRange,
    dwritetextlayout1_SetPairKerning,
    dwritetextlayout1_GetPairKerning,
    dwritetextlayout1_SetCharacterSpacing,
    dwritetextlayout1_GetCharacterSpacing,
    dwritetextlayout2_GetMetrics,
    dwritetextlayout2_SetVerticalGlyphOrientation,
    dwritetextlayout2_GetVerticalGlyphOrientation,
    dwritetextlayout2_SetLastLineWrapping,
    dwritetextlayout2_GetLastLineWrapping,
    dwritetextlayout2_SetOpticalAlignment,
    dwritetextlayout2_GetOpticalAlignment,
    dwritetextlayout2_SetFontFallback,
    dwritetextlayout2_GetFontFallback
};

static HRESULT WINAPI dwritetextformat1_layout_QueryInterface(IDWriteTextFormat1 *iface, REFIID riid, void **obj)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);
    return IDWriteTextLayout2_QueryInterface(&This->IDWriteTextLayout2_iface, riid, obj);
}

static ULONG WINAPI dwritetextformat1_layout_AddRef(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    return IDWriteTextLayout2_AddRef(&This->IDWriteTextLayout2_iface);
}

static ULONG WINAPI dwritetextformat1_layout_Release(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    return IDWriteTextLayout2_Release(&This->IDWriteTextLayout2_iface);
}

static HRESULT WINAPI dwritetextformat1_layout_SetTextAlignment(IDWriteTextFormat1 *iface, DWRITE_TEXT_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    BOOL changed;
    HRESULT hr;

    TRACE("(%p)->(%d)\n", This, alignment);

    hr = format_set_textalignment(&This->format, alignment, &changed);
    if (FAILED(hr))
        return hr;

    /* if layout is not ready there's nothing to align */
    if (changed && !(This->recompute & RECOMPUTE_EFFECTIVE_RUNS))
        layout_apply_text_alignment(This);

    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_SetParagraphAlignment(IDWriteTextFormat1 *iface, DWRITE_PARAGRAPH_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    BOOL changed;
    HRESULT hr;

    TRACE("(%p)->(%d)\n", This, alignment);

    hr = format_set_paralignment(&This->format, alignment, &changed);
    if (FAILED(hr))
        return hr;

    /* if layout is not ready there's nothing to align */
    if (changed && !(This->recompute & RECOMPUTE_EFFECTIVE_RUNS))
        layout_apply_par_alignment(This);

    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_SetWordWrapping(IDWriteTextFormat1 *iface, DWRITE_WORD_WRAPPING wrapping)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, wrapping);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextformat1_layout_SetReadingDirection(IDWriteTextFormat1 *iface, DWRITE_READING_DIRECTION direction)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    BOOL changed;
    HRESULT hr;

    TRACE("(%p)->(%d)\n", This, direction);

    hr = format_set_readingdirection(&This->format, direction, &changed);
    if (FAILED(hr))
        return hr;

    if (changed)
        This->recompute = RECOMPUTE_EVERYTHING;

    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_SetFlowDirection(IDWriteTextFormat1 *iface, DWRITE_FLOW_DIRECTION direction)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, direction);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextformat1_layout_SetIncrementalTabStop(IDWriteTextFormat1 *iface, FLOAT tabstop)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%f): stub\n", This, tabstop);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextformat1_layout_SetTrimming(IDWriteTextFormat1 *iface, DWRITE_TRIMMING const *trimming,
    IDWriteInlineObject *trimming_sign)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%p %p): stub\n", This, trimming, trimming_sign);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextformat1_layout_SetLineSpacing(IDWriteTextFormat1 *iface, DWRITE_LINE_SPACING_METHOD spacing,
    FLOAT line_spacing, FLOAT baseline)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d %f %f): stub\n", This, spacing, line_spacing, baseline);
    return E_NOTIMPL;
}

static DWRITE_TEXT_ALIGNMENT WINAPI dwritetextformat1_layout_GetTextAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.textalignment;
}

static DWRITE_PARAGRAPH_ALIGNMENT WINAPI dwritetextformat1_layout_GetParagraphAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.paralign;
}

static DWRITE_WORD_WRAPPING WINAPI dwritetextformat1_layout_GetWordWrapping(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return This->format.wrapping;
}

static DWRITE_READING_DIRECTION WINAPI dwritetextformat1_layout_GetReadingDirection(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.readingdir;
}

static DWRITE_FLOW_DIRECTION WINAPI dwritetextformat1_layout_GetFlowDirection(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.flow;
}

static FLOAT WINAPI dwritetextformat1_layout_GetIncrementalTabStop(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return 0.0;
}

static HRESULT WINAPI dwritetextformat1_layout_GetTrimming(IDWriteTextFormat1 *iface, DWRITE_TRIMMING *options,
    IDWriteInlineObject **trimming_sign)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %p)\n", This, options, trimming_sign);

    *options = This->format.trimming;
    *trimming_sign = This->format.trimmingsign;
    if (*trimming_sign)
        IDWriteInlineObject_AddRef(*trimming_sign);
    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_GetLineSpacing(IDWriteTextFormat1 *iface, DWRITE_LINE_SPACING_METHOD *method,
    FLOAT *spacing, FLOAT *baseline)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %p %p)\n", This, method, spacing, baseline);

    *method = This->format.spacingmethod;
    *spacing = This->format.spacing;
    *baseline = This->format.baseline;
    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_GetFontCollection(IDWriteTextFormat1 *iface, IDWriteFontCollection **collection)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p)\n", This, collection);

    *collection = This->format.collection;
    if (*collection)
        IDWriteFontCollection_AddRef(*collection);
    return S_OK;
}

static UINT32 WINAPI dwritetextformat1_layout_GetFontFamilyNameLength(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.family_len;
}

static HRESULT WINAPI dwritetextformat1_layout_GetFontFamilyName(IDWriteTextFormat1 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %u)\n", This, name, size);

    if (size <= This->format.family_len) return E_NOT_SUFFICIENT_BUFFER;
    strcpyW(name, This->format.family_name);
    return S_OK;
}

static DWRITE_FONT_WEIGHT WINAPI dwritetextformat1_layout_GetFontWeight(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.weight;
}

static DWRITE_FONT_STYLE WINAPI dwritetextformat1_layout_GetFontStyle(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.style;
}

static DWRITE_FONT_STRETCH WINAPI dwritetextformat1_layout_GetFontStretch(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.stretch;
}

static FLOAT WINAPI dwritetextformat1_layout_GetFontSize(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.fontsize;
}

static UINT32 WINAPI dwritetextformat1_layout_GetLocaleNameLength(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.locale_len;
}

static HRESULT WINAPI dwritetextformat1_layout_GetLocaleName(IDWriteTextFormat1 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %u)\n", This, name, size);

    if (size <= This->format.locale_len) return E_NOT_SUFFICIENT_BUFFER;
    strcpyW(name, This->format.locale);
    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_layout_SetVerticalGlyphOrientation(IDWriteTextFormat1 *iface, DWRITE_VERTICAL_GLYPH_ORIENTATION orientation)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, orientation);
    return E_NOTIMPL;
}

static DWRITE_VERTICAL_GLYPH_ORIENTATION WINAPI dwritetextformat1_layout_GetVerticalGlyphOrientation(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return DWRITE_VERTICAL_GLYPH_ORIENTATION_DEFAULT;
}

static HRESULT WINAPI dwritetextformat1_layout_SetLastLineWrapping(IDWriteTextFormat1 *iface, BOOL lastline_wrapping_enabled)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, lastline_wrapping_enabled);
    return E_NOTIMPL;
}

static BOOL WINAPI dwritetextformat1_layout_GetLastLineWrapping(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return FALSE;
}

static HRESULT WINAPI dwritetextformat1_layout_SetOpticalAlignment(IDWriteTextFormat1 *iface, DWRITE_OPTICAL_ALIGNMENT alignment)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, alignment);
    return E_NOTIMPL;
}

static DWRITE_OPTICAL_ALIGNMENT WINAPI dwritetextformat1_layout_GetOpticalAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return DWRITE_OPTICAL_ALIGNMENT_NONE;
}

static HRESULT WINAPI dwritetextformat1_layout_SetFontFallback(IDWriteTextFormat1 *iface, IDWriteFontFallback *fallback)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return IDWriteTextLayout2_SetFontFallback(&This->IDWriteTextLayout2_iface, fallback);
}

static HRESULT WINAPI dwritetextformat1_layout_GetFontFallback(IDWriteTextFormat1 *iface, IDWriteFontFallback **fallback)
{
    struct dwrite_textlayout *This = impl_layout_form_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return IDWriteTextLayout2_GetFontFallback(&This->IDWriteTextLayout2_iface, fallback);
}

static const IDWriteTextFormat1Vtbl dwritetextformat1_layout_vtbl = {
    dwritetextformat1_layout_QueryInterface,
    dwritetextformat1_layout_AddRef,
    dwritetextformat1_layout_Release,
    dwritetextformat1_layout_SetTextAlignment,
    dwritetextformat1_layout_SetParagraphAlignment,
    dwritetextformat1_layout_SetWordWrapping,
    dwritetextformat1_layout_SetReadingDirection,
    dwritetextformat1_layout_SetFlowDirection,
    dwritetextformat1_layout_SetIncrementalTabStop,
    dwritetextformat1_layout_SetTrimming,
    dwritetextformat1_layout_SetLineSpacing,
    dwritetextformat1_layout_GetTextAlignment,
    dwritetextformat1_layout_GetParagraphAlignment,
    dwritetextformat1_layout_GetWordWrapping,
    dwritetextformat1_layout_GetReadingDirection,
    dwritetextformat1_layout_GetFlowDirection,
    dwritetextformat1_layout_GetIncrementalTabStop,
    dwritetextformat1_layout_GetTrimming,
    dwritetextformat1_layout_GetLineSpacing,
    dwritetextformat1_layout_GetFontCollection,
    dwritetextformat1_layout_GetFontFamilyNameLength,
    dwritetextformat1_layout_GetFontFamilyName,
    dwritetextformat1_layout_GetFontWeight,
    dwritetextformat1_layout_GetFontStyle,
    dwritetextformat1_layout_GetFontStretch,
    dwritetextformat1_layout_GetFontSize,
    dwritetextformat1_layout_GetLocaleNameLength,
    dwritetextformat1_layout_GetLocaleName,
    dwritetextformat1_layout_SetVerticalGlyphOrientation,
    dwritetextformat1_layout_GetVerticalGlyphOrientation,
    dwritetextformat1_layout_SetLastLineWrapping,
    dwritetextformat1_layout_GetLastLineWrapping,
    dwritetextformat1_layout_SetOpticalAlignment,
    dwritetextformat1_layout_GetOpticalAlignment,
    dwritetextformat1_layout_SetFontFallback,
    dwritetextformat1_layout_GetFontFallback
};

static HRESULT WINAPI dwritetextlayout_sink_QueryInterface(IDWriteTextAnalysisSink *iface,
    REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IDWriteTextAnalysisSink) || IsEqualIID(riid, &IID_IUnknown)) {
        *obj = iface;
        IDWriteTextAnalysisSink_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetextlayout_sink_AddRef(IDWriteTextAnalysisSink *iface)
{
    return 2;
}

static ULONG WINAPI dwritetextlayout_sink_Release(IDWriteTextAnalysisSink *iface)
{
    return 1;
}

static HRESULT WINAPI dwritetextlayout_sink_SetScriptAnalysis(IDWriteTextAnalysisSink *iface,
    UINT32 position, UINT32 length, DWRITE_SCRIPT_ANALYSIS const* sa)
{
    struct dwrite_textlayout *layout = impl_from_IDWriteTextAnalysisSink(iface);
    struct layout_run *run;

    TRACE("%u %u script=%d\n", position, length, sa->script);

    run = alloc_layout_run(LAYOUT_RUN_REGULAR);
    if (!run)
        return E_OUTOFMEMORY;

    run->u.regular.descr.string = &layout->str[position];
    run->u.regular.descr.stringLength = length;
    run->u.regular.descr.textPosition = position;
    run->u.regular.sa = *sa;
    list_add_tail(&layout->runs, &run->entry);
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_sink_SetLineBreakpoints(IDWriteTextAnalysisSink *iface,
    UINT32 position, UINT32 length, DWRITE_LINE_BREAKPOINT const* breakpoints)
{
    struct dwrite_textlayout *layout = impl_from_IDWriteTextAnalysisSink(iface);

    if (position + length > layout->len)
        return E_FAIL;

    memcpy(&layout->nominal_breakpoints[position], breakpoints, length*sizeof(DWRITE_LINE_BREAKPOINT));
    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_sink_SetBidiLevel(IDWriteTextAnalysisSink *iface, UINT32 position,
    UINT32 length, UINT8 explicitLevel, UINT8 resolvedLevel)
{
    struct dwrite_textlayout *layout = impl_from_IDWriteTextAnalysisSink(iface);
    struct layout_run *cur_run;

    TRACE("%u %u %u %u\n", position, length, explicitLevel, resolvedLevel);

    LIST_FOR_EACH_ENTRY(cur_run, &layout->runs, struct layout_run, entry) {
        struct regular_layout_run *cur = &cur_run->u.regular;
        struct layout_run *run;

        if (cur_run->kind == LAYOUT_RUN_INLINE)
            continue;

        /* FIXME: levels are reported in a natural forward direction, so start loop from a run we ended on */
        if (position < cur->descr.textPosition || position >= cur->descr.textPosition + cur->descr.stringLength)
            continue;

        /* full hit - just set run level */
        if (cur->descr.textPosition == position && cur->descr.stringLength == length) {
            cur->run.bidiLevel = resolvedLevel;
            break;
        }

        /* current run is fully covered, move to next one */
        if (cur->descr.textPosition == position && cur->descr.stringLength < length) {
            cur->run.bidiLevel = resolvedLevel;
            position += cur->descr.stringLength;
            length -= cur->descr.stringLength;
            continue;
        }

        /* all fully covered runs are processed at this point, reuse existing run for remaining
           reported bidi range and add another run for the rest of original one */

        run = alloc_layout_run(LAYOUT_RUN_REGULAR);
        if (!run)
            return E_OUTOFMEMORY;

        *run = *cur_run;
        run->u.regular.descr.textPosition = position + length;
        run->u.regular.descr.stringLength = cur->descr.stringLength - length;
        run->u.regular.descr.string = &layout->str[position + length];

        /* reduce existing run */
        cur->run.bidiLevel = resolvedLevel;
        cur->descr.stringLength = length;

        list_add_after(&cur_run->entry, &run->entry);
        break;
    }

    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_sink_SetNumberSubstitution(IDWriteTextAnalysisSink *iface,
    UINT32 position, UINT32 length, IDWriteNumberSubstitution* substitution)
{
    return E_NOTIMPL;
}

static const IDWriteTextAnalysisSinkVtbl dwritetextlayoutsinkvtbl = {
    dwritetextlayout_sink_QueryInterface,
    dwritetextlayout_sink_AddRef,
    dwritetextlayout_sink_Release,
    dwritetextlayout_sink_SetScriptAnalysis,
    dwritetextlayout_sink_SetLineBreakpoints,
    dwritetextlayout_sink_SetBidiLevel,
    dwritetextlayout_sink_SetNumberSubstitution
};

static HRESULT WINAPI dwritetextlayout_source_QueryInterface(IDWriteTextAnalysisSource *iface,
    REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IDWriteTextAnalysisSource) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IDWriteTextAnalysisSource_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetextlayout_source_AddRef(IDWriteTextAnalysisSource *iface)
{
    return 2;
}

static ULONG WINAPI dwritetextlayout_source_Release(IDWriteTextAnalysisSource *iface)
{
    return 1;
}

static HRESULT WINAPI dwritetextlayout_source_GetTextAtPosition(IDWriteTextAnalysisSource *iface,
    UINT32 position, WCHAR const** text, UINT32* text_len)
{
    struct dwrite_textlayout *layout = impl_from_IDWriteTextAnalysisSource(iface);

    TRACE("(%p)->(%u %p %p)\n", layout, position, text, text_len);

    if (position < layout->len) {
        *text = &layout->str[position];
        *text_len = layout->len - position;
    }
    else {
        *text = NULL;
        *text_len = 0;
    }

    return S_OK;
}

static HRESULT WINAPI dwritetextlayout_source_GetTextBeforePosition(IDWriteTextAnalysisSource *iface,
    UINT32 position, WCHAR const** text, UINT32* text_len)
{
    FIXME("%u %p %p: stub\n", position, text, text_len);
    return E_NOTIMPL;
}

static DWRITE_READING_DIRECTION WINAPI dwritetextlayout_source_GetParagraphReadingDirection(IDWriteTextAnalysisSource *iface)
{
    struct dwrite_textlayout *layout = impl_from_IDWriteTextAnalysisSource(iface);
    return IDWriteTextLayout2_GetReadingDirection(&layout->IDWriteTextLayout2_iface);
}

static HRESULT WINAPI dwritetextlayout_source_GetLocaleName(IDWriteTextAnalysisSource *iface,
    UINT32 position, UINT32* text_len, WCHAR const** locale)
{
    FIXME("%u %p %p: stub\n", position, text_len, locale);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextlayout_source_GetNumberSubstitution(IDWriteTextAnalysisSource *iface,
    UINT32 position, UINT32* text_len, IDWriteNumberSubstitution **substitution)
{
    FIXME("%u %p %p: stub\n", position, text_len, substitution);
    return E_NOTIMPL;
}

static const IDWriteTextAnalysisSourceVtbl dwritetextlayoutsourcevtbl = {
    dwritetextlayout_source_QueryInterface,
    dwritetextlayout_source_AddRef,
    dwritetextlayout_source_Release,
    dwritetextlayout_source_GetTextAtPosition,
    dwritetextlayout_source_GetTextBeforePosition,
    dwritetextlayout_source_GetParagraphReadingDirection,
    dwritetextlayout_source_GetLocaleName,
    dwritetextlayout_source_GetNumberSubstitution
};

static HRESULT layout_format_from_textformat(struct dwrite_textlayout *layout, IDWriteTextFormat *format)
{
    IDWriteTextFormat1 *format1;
    UINT32 len;
    HRESULT hr;

    layout->format.weight  = IDWriteTextFormat_GetFontWeight(format);
    layout->format.style   = IDWriteTextFormat_GetFontStyle(format);
    layout->format.stretch = IDWriteTextFormat_GetFontStretch(format);
    layout->format.fontsize= IDWriteTextFormat_GetFontSize(format);
    layout->format.textalignment = IDWriteTextFormat_GetTextAlignment(format);
    layout->format.paralign = IDWriteTextFormat_GetParagraphAlignment(format);
    layout->format.wrapping = IDWriteTextFormat_GetWordWrapping(format);
    layout->format.readingdir = IDWriteTextFormat_GetReadingDirection(format);
    layout->format.flow = IDWriteTextFormat_GetFlowDirection(format);
    layout->format.fallback = NULL;
    hr = IDWriteTextFormat_GetLineSpacing(format, &layout->format.spacingmethod,
        &layout->format.spacing, &layout->format.baseline);
    if (FAILED(hr))
        return hr;

    hr = IDWriteTextFormat_GetTrimming(format, &layout->format.trimming, &layout->format.trimmingsign);
    if (FAILED(hr))
        return hr;

    /* locale name and length */
    len = IDWriteTextFormat_GetLocaleNameLength(format);
    layout->format.locale = heap_alloc((len+1)*sizeof(WCHAR));
    if (!layout->format.locale)
        return E_OUTOFMEMORY;

    hr = IDWriteTextFormat_GetLocaleName(format, layout->format.locale, len+1);
    if (FAILED(hr))
        return hr;
    layout->format.locale_len = len;

    /* font family name and length */
    len = IDWriteTextFormat_GetFontFamilyNameLength(format);
    layout->format.family_name = heap_alloc((len+1)*sizeof(WCHAR));
    if (!layout->format.family_name)
        return E_OUTOFMEMORY;

    hr = IDWriteTextFormat_GetFontFamilyName(format, layout->format.family_name, len+1);
    if (FAILED(hr))
        return hr;
    layout->format.family_len = len;

    hr = IDWriteTextFormat_QueryInterface(format, &IID_IDWriteTextFormat1, (void**)&format1);
    if (hr == S_OK) {
        layout->format.vertical_orientation = IDWriteTextFormat1_GetVerticalGlyphOrientation(format1);
        IDWriteTextFormat1_GetFontFallback(format1, &layout->format.fallback);
        IDWriteTextFormat1_Release(format1);
    }
    else
        layout->format.vertical_orientation = DWRITE_VERTICAL_GLYPH_ORIENTATION_DEFAULT;

    return IDWriteTextFormat_GetFontCollection(format, &layout->format.collection);
}

static HRESULT init_textlayout(const WCHAR *str, UINT32 len, IDWriteTextFormat *format, FLOAT maxwidth, FLOAT maxheight, struct dwrite_textlayout *layout)
{
    struct layout_range_header *range, *strike, *effect, *spacing;
    DWRITE_TEXT_RANGE r = { 0, ~0u };
    HRESULT hr;

    layout->IDWriteTextLayout2_iface.lpVtbl = &dwritetextlayoutvtbl;
    layout->IDWriteTextFormat1_iface.lpVtbl = &dwritetextformat1_layout_vtbl;
    layout->IDWriteTextAnalysisSink_iface.lpVtbl = &dwritetextlayoutsinkvtbl;
    layout->IDWriteTextAnalysisSource_iface.lpVtbl = &dwritetextlayoutsourcevtbl;
    layout->ref = 1;
    layout->len = len;
    layout->recompute = RECOMPUTE_EVERYTHING;
    layout->nominal_breakpoints = NULL;
    layout->actual_breakpoints = NULL;
    layout->cluster_count = 0;
    layout->clustermetrics = NULL;
    layout->clusters = NULL;
    layout->lines = NULL;
    layout->line_alloc = 0;
    layout->minwidth = 0.0;
    list_init(&layout->eruns);
    list_init(&layout->inlineobjects);
    list_init(&layout->strikethrough);
    list_init(&layout->runs);
    list_init(&layout->ranges);
    list_init(&layout->strike_ranges);
    list_init(&layout->effects);
    list_init(&layout->spacing);
    memset(&layout->format, 0, sizeof(layout->format));
    memset(&layout->metrics, 0, sizeof(layout->metrics));
    layout->metrics.layoutWidth = maxwidth;
    layout->metrics.layoutHeight = maxheight;

    layout->gdicompatible = FALSE;
    layout->pixels_per_dip = 0.0;
    layout->use_gdi_natural = FALSE;
    memset(&layout->transform, 0, sizeof(layout->transform));

    layout->str = heap_strdupnW(str, len);
    if (len && !layout->str) {
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    hr = layout_format_from_textformat(layout, format);
    if (FAILED(hr))
        goto fail;

    range = alloc_layout_range(layout, &r, LAYOUT_RANGE_REGULAR);
    strike = alloc_layout_range(layout, &r, LAYOUT_RANGE_STRIKETHROUGH);
    effect = alloc_layout_range(layout, &r, LAYOUT_RANGE_EFFECT);
    spacing = alloc_layout_range(layout, &r, LAYOUT_RANGE_SPACING);
    if (!range || !strike || !effect || !spacing) {
        free_layout_range(range);
        free_layout_range(strike);
        free_layout_range(effect);
        free_layout_range(spacing);
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    list_add_head(&layout->ranges, &range->entry);
    list_add_head(&layout->strike_ranges, &strike->entry);
    list_add_head(&layout->effects, &effect->entry);
    list_add_head(&layout->spacing, &spacing->entry);
    return S_OK;

fail:
    IDWriteTextLayout2_Release(&layout->IDWriteTextLayout2_iface);
    return hr;
}

HRESULT create_textlayout(const WCHAR *str, UINT32 len, IDWriteTextFormat *format, FLOAT maxwidth, FLOAT maxheight, IDWriteTextLayout **ret)
{
    struct dwrite_textlayout *layout;
    HRESULT hr;

    *ret = NULL;

    layout = heap_alloc(sizeof(struct dwrite_textlayout));
    if (!layout) return E_OUTOFMEMORY;

    hr = init_textlayout(str, len, format, maxwidth, maxheight, layout);
    if (hr == S_OK)
        *ret = (IDWriteTextLayout*)&layout->IDWriteTextLayout2_iface;

    return hr;
}

HRESULT create_gdicompat_textlayout(const WCHAR *str, UINT32 len, IDWriteTextFormat *format, FLOAT maxwidth, FLOAT maxheight,
    FLOAT pixels_per_dip, const DWRITE_MATRIX *transform, BOOL use_gdi_natural, IDWriteTextLayout **ret)
{
    struct dwrite_textlayout *layout;
    HRESULT hr;

    *ret = NULL;

    layout = heap_alloc(sizeof(struct dwrite_textlayout));
    if (!layout) return E_OUTOFMEMORY;

    hr = init_textlayout(str, len, format, maxwidth, maxheight, layout);
    if (hr == S_OK) {
        /* set gdi-specific properties */
        layout->gdicompatible = TRUE;
        layout->pixels_per_dip = pixels_per_dip;
        layout->use_gdi_natural = use_gdi_natural;
        layout->transform = transform ? *transform : identity;

        *ret = (IDWriteTextLayout*)&layout->IDWriteTextLayout2_iface;
    }

    return hr;
}

static HRESULT WINAPI dwritetrimmingsign_QueryInterface(IDWriteInlineObject *iface, REFIID riid, void **obj)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDWriteInlineObject)) {
        *obj = iface;
        IDWriteInlineObject_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetrimmingsign_AddRef(IDWriteInlineObject *iface)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI dwritetrimmingsign_Release(IDWriteInlineObject *iface)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref) {
        IDWriteTextLayout_Release(This->layout);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI dwritetrimmingsign_Draw(IDWriteInlineObject *iface, void *context, IDWriteTextRenderer *renderer,
    FLOAT originX, FLOAT originY, BOOL is_sideways, BOOL is_rtl, IUnknown *effect)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);
    DWRITE_TEXT_RANGE range = { 0, ~0u };

    TRACE("(%p)->(%p %p %.2f %.2f %d %d %p)\n", This, context, renderer, originX, originY, is_sideways, is_rtl, effect);

    IDWriteTextLayout_SetDrawingEffect(This->layout, effect, range);
    return IDWriteTextLayout_Draw(This->layout, context, renderer, originX, originY);
}

static HRESULT WINAPI dwritetrimmingsign_GetMetrics(IDWriteInlineObject *iface, DWRITE_INLINE_OBJECT_METRICS *metrics)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);
    FIXME("(%p)->(%p): stub\n", This, metrics);
    memset(metrics, 0, sizeof(*metrics));
    return S_OK;
}

static HRESULT WINAPI dwritetrimmingsign_GetOverhangMetrics(IDWriteInlineObject *iface, DWRITE_OVERHANG_METRICS *overhangs)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);
    FIXME("(%p)->(%p): stub\n", This, overhangs);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetrimmingsign_GetBreakConditions(IDWriteInlineObject *iface, DWRITE_BREAK_CONDITION *before,
        DWRITE_BREAK_CONDITION *after)
{
    struct dwrite_trimmingsign *This = impl_from_IDWriteInlineObject(iface);

    TRACE("(%p)->(%p %p)\n", This, before, after);

    *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
}

static const IDWriteInlineObjectVtbl dwritetrimmingsignvtbl = {
    dwritetrimmingsign_QueryInterface,
    dwritetrimmingsign_AddRef,
    dwritetrimmingsign_Release,
    dwritetrimmingsign_Draw,
    dwritetrimmingsign_GetMetrics,
    dwritetrimmingsign_GetOverhangMetrics,
    dwritetrimmingsign_GetBreakConditions
};

static inline BOOL is_reading_direction_horz(DWRITE_READING_DIRECTION direction)
{
    return (direction == DWRITE_READING_DIRECTION_LEFT_TO_RIGHT) ||
           (direction == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
}

static inline BOOL is_reading_direction_vert(DWRITE_READING_DIRECTION direction)
{
    return (direction == DWRITE_READING_DIRECTION_TOP_TO_BOTTOM) ||
           (direction == DWRITE_READING_DIRECTION_BOTTOM_TO_TOP);
}

static inline BOOL is_flow_direction_horz(DWRITE_FLOW_DIRECTION direction)
{
    return (direction == DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT) ||
           (direction == DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT);
}

static inline BOOL is_flow_direction_vert(DWRITE_FLOW_DIRECTION direction)
{
    return (direction == DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM) ||
           (direction == DWRITE_FLOW_DIRECTION_BOTTOM_TO_TOP);
}

HRESULT create_trimmingsign(IDWriteFactory2 *factory, IDWriteTextFormat *format, IDWriteInlineObject **sign)
{
    static const WCHAR ellipsisW = 0x2026;
    struct dwrite_trimmingsign *This;
    DWRITE_READING_DIRECTION reading;
    DWRITE_FLOW_DIRECTION flow;
    HRESULT hr;

    *sign = NULL;

    /* Validate reading/flow direction here, layout creation won't complain about
       invalid combinations. */
    reading = IDWriteTextFormat_GetReadingDirection(format);
    flow = IDWriteTextFormat_GetFlowDirection(format);

    if ((is_reading_direction_horz(reading) && is_flow_direction_horz(flow)) ||
        (is_reading_direction_vert(reading) && is_flow_direction_vert(flow)))
        return DWRITE_E_FLOWDIRECTIONCONFLICTS;

    This = heap_alloc(sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IDWriteInlineObject_iface.lpVtbl = &dwritetrimmingsignvtbl;
    This->ref = 1;

    hr = IDWriteFactory2_CreateTextLayout(factory, &ellipsisW, 1, format, 0.0, 0.0, &This->layout);
    if (FAILED(hr)) {
        heap_free(This);
        return hr;
    }

    IDWriteTextLayout_SetWordWrapping(This->layout, DWRITE_WORD_WRAPPING_NO_WRAP);
    *sign = &This->IDWriteInlineObject_iface;

    return S_OK;
}

static HRESULT WINAPI dwritetextformat_QueryInterface(IDWriteTextFormat1 *iface, REFIID riid, void **obj)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteTextFormat1) ||
        IsEqualIID(riid, &IID_IDWriteTextFormat)  ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IDWriteTextFormat1_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;

    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetextformat_AddRef(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI dwritetextformat_Release(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref)
    {
        release_format_data(&This->format);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI dwritetextformat_SetTextAlignment(IDWriteTextFormat1 *iface, DWRITE_TEXT_ALIGNMENT alignment)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%d)\n", This, alignment);
    return format_set_textalignment(&This->format, alignment, NULL);
}

static HRESULT WINAPI dwritetextformat_SetParagraphAlignment(IDWriteTextFormat1 *iface, DWRITE_PARAGRAPH_ALIGNMENT alignment)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%d)\n", This, alignment);
    return format_set_paralignment(&This->format, alignment, NULL);
}

static HRESULT WINAPI dwritetextformat_SetWordWrapping(IDWriteTextFormat1 *iface, DWRITE_WORD_WRAPPING wrapping)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%d)\n", This, wrapping);

    if ((UINT32)wrapping > DWRITE_WORD_WRAPPING_CHARACTER)
        return E_INVALIDARG;

    This->format.wrapping = wrapping;
    return S_OK;
}

static HRESULT WINAPI dwritetextformat_SetReadingDirection(IDWriteTextFormat1 *iface, DWRITE_READING_DIRECTION direction)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%d)\n", This, direction);
    return format_set_readingdirection(&This->format, direction, NULL);
}

static HRESULT WINAPI dwritetextformat_SetFlowDirection(IDWriteTextFormat1 *iface, DWRITE_FLOW_DIRECTION direction)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%d)\n", This, direction);

    if ((UINT32)direction > DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT)
        return E_INVALIDARG;

    This->format.flow = direction;
    return S_OK;
}

static HRESULT WINAPI dwritetextformat_SetIncrementalTabStop(IDWriteTextFormat1 *iface, FLOAT tabstop)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%f): stub\n", This, tabstop);
    return E_NOTIMPL;
}

static HRESULT WINAPI dwritetextformat_SetTrimming(IDWriteTextFormat1 *iface, DWRITE_TRIMMING const *trimming,
    IDWriteInlineObject *trimming_sign)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p %p)\n", This, trimming, trimming_sign);

    This->format.trimming = *trimming;
    if (This->format.trimmingsign)
        IDWriteInlineObject_Release(This->format.trimmingsign);
    This->format.trimmingsign = trimming_sign;
    if (This->format.trimmingsign)
        IDWriteInlineObject_AddRef(This->format.trimmingsign);
    return S_OK;
}

static HRESULT WINAPI dwritetextformat_SetLineSpacing(IDWriteTextFormat1 *iface, DWRITE_LINE_SPACING_METHOD method,
    FLOAT spacing, FLOAT baseline)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%d %f %f)\n", This, method, spacing, baseline);

    if (spacing < 0.0 || (UINT32)method > DWRITE_LINE_SPACING_METHOD_UNIFORM)
        return E_INVALIDARG;

    This->format.spacingmethod = method;
    This->format.spacing = spacing;
    This->format.baseline = baseline;
    return S_OK;
}

static DWRITE_TEXT_ALIGNMENT WINAPI dwritetextformat_GetTextAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.textalignment;
}

static DWRITE_PARAGRAPH_ALIGNMENT WINAPI dwritetextformat_GetParagraphAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.paralign;
}

static DWRITE_WORD_WRAPPING WINAPI dwritetextformat_GetWordWrapping(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.wrapping;
}

static DWRITE_READING_DIRECTION WINAPI dwritetextformat_GetReadingDirection(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.readingdir;
}

static DWRITE_FLOW_DIRECTION WINAPI dwritetextformat_GetFlowDirection(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.flow;
}

static FLOAT WINAPI dwritetextformat_GetIncrementalTabStop(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return 0.0;
}

static HRESULT WINAPI dwritetextformat_GetTrimming(IDWriteTextFormat1 *iface, DWRITE_TRIMMING *options,
    IDWriteInlineObject **trimming_sign)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p %p)\n", This, options, trimming_sign);

    *options = This->format.trimming;
    if ((*trimming_sign = This->format.trimmingsign))
        IDWriteInlineObject_AddRef(*trimming_sign);

    return S_OK;
}

static HRESULT WINAPI dwritetextformat_GetLineSpacing(IDWriteTextFormat1 *iface, DWRITE_LINE_SPACING_METHOD *method,
    FLOAT *spacing, FLOAT *baseline)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p %p %p)\n", This, method, spacing, baseline);

    *method = This->format.spacingmethod;
    *spacing = This->format.spacing;
    *baseline = This->format.baseline;
    return S_OK;
}

static HRESULT WINAPI dwritetextformat_GetFontCollection(IDWriteTextFormat1 *iface, IDWriteFontCollection **collection)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p)\n", This, collection);

    *collection = This->format.collection;
    IDWriteFontCollection_AddRef(*collection);

    return S_OK;
}

static UINT32 WINAPI dwritetextformat_GetFontFamilyNameLength(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.family_len;
}

static HRESULT WINAPI dwritetextformat_GetFontFamilyName(IDWriteTextFormat1 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %u)\n", This, name, size);

    if (size <= This->format.family_len) return E_NOT_SUFFICIENT_BUFFER;
    strcpyW(name, This->format.family_name);
    return S_OK;
}

static DWRITE_FONT_WEIGHT WINAPI dwritetextformat_GetFontWeight(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.weight;
}

static DWRITE_FONT_STYLE WINAPI dwritetextformat_GetFontStyle(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.style;
}

static DWRITE_FONT_STRETCH WINAPI dwritetextformat_GetFontStretch(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.stretch;
}

static FLOAT WINAPI dwritetextformat_GetFontSize(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.fontsize;
}

static UINT32 WINAPI dwritetextformat_GetLocaleNameLength(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.locale_len;
}

static HRESULT WINAPI dwritetextformat_GetLocaleName(IDWriteTextFormat1 *iface, WCHAR *name, UINT32 size)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%p %u)\n", This, name, size);

    if (size <= This->format.locale_len) return E_NOT_SUFFICIENT_BUFFER;
    strcpyW(name, This->format.locale);
    return S_OK;
}

static HRESULT WINAPI dwritetextformat1_SetVerticalGlyphOrientation(IDWriteTextFormat1 *iface, DWRITE_VERTICAL_GLYPH_ORIENTATION orientation)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);

    TRACE("(%p)->(%d)\n", This, orientation);

    if ((UINT32)orientation > DWRITE_VERTICAL_GLYPH_ORIENTATION_STACKED)
        return E_INVALIDARG;

    This->format.vertical_orientation = orientation;
    return S_OK;
}

static DWRITE_VERTICAL_GLYPH_ORIENTATION WINAPI dwritetextformat1_GetVerticalGlyphOrientation(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)\n", This);
    return This->format.vertical_orientation;
}

static HRESULT WINAPI dwritetextformat1_SetLastLineWrapping(IDWriteTextFormat1 *iface, BOOL lastline_wrapping_enabled)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, lastline_wrapping_enabled);
    return E_NOTIMPL;
}

static BOOL WINAPI dwritetextformat1_GetLastLineWrapping(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return FALSE;
}

static HRESULT WINAPI dwritetextformat1_SetOpticalAlignment(IDWriteTextFormat1 *iface, DWRITE_OPTICAL_ALIGNMENT alignment)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p)->(%d): stub\n", This, alignment);
    return E_NOTIMPL;
}

static DWRITE_OPTICAL_ALIGNMENT WINAPI dwritetextformat1_GetOpticalAlignment(IDWriteTextFormat1 *iface)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    FIXME("(%p): stub\n", This);
    return DWRITE_OPTICAL_ALIGNMENT_NONE;
}

static HRESULT WINAPI dwritetextformat1_SetFontFallback(IDWriteTextFormat1 *iface, IDWriteFontFallback *fallback)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return set_fontfallback_for_format(&This->format, fallback);
}

static HRESULT WINAPI dwritetextformat1_GetFontFallback(IDWriteTextFormat1 *iface, IDWriteFontFallback **fallback)
{
    struct dwrite_textformat *This = impl_from_IDWriteTextFormat1(iface);
    TRACE("(%p)->(%p)\n", This, fallback);
    return get_fontfallback_from_format(&This->format, fallback);
}

static const IDWriteTextFormat1Vtbl dwritetextformatvtbl = {
    dwritetextformat_QueryInterface,
    dwritetextformat_AddRef,
    dwritetextformat_Release,
    dwritetextformat_SetTextAlignment,
    dwritetextformat_SetParagraphAlignment,
    dwritetextformat_SetWordWrapping,
    dwritetextformat_SetReadingDirection,
    dwritetextformat_SetFlowDirection,
    dwritetextformat_SetIncrementalTabStop,
    dwritetextformat_SetTrimming,
    dwritetextformat_SetLineSpacing,
    dwritetextformat_GetTextAlignment,
    dwritetextformat_GetParagraphAlignment,
    dwritetextformat_GetWordWrapping,
    dwritetextformat_GetReadingDirection,
    dwritetextformat_GetFlowDirection,
    dwritetextformat_GetIncrementalTabStop,
    dwritetextformat_GetTrimming,
    dwritetextformat_GetLineSpacing,
    dwritetextformat_GetFontCollection,
    dwritetextformat_GetFontFamilyNameLength,
    dwritetextformat_GetFontFamilyName,
    dwritetextformat_GetFontWeight,
    dwritetextformat_GetFontStyle,
    dwritetextformat_GetFontStretch,
    dwritetextformat_GetFontSize,
    dwritetextformat_GetLocaleNameLength,
    dwritetextformat_GetLocaleName,
    dwritetextformat1_SetVerticalGlyphOrientation,
    dwritetextformat1_GetVerticalGlyphOrientation,
    dwritetextformat1_SetLastLineWrapping,
    dwritetextformat1_GetLastLineWrapping,
    dwritetextformat1_SetOpticalAlignment,
    dwritetextformat1_GetOpticalAlignment,
    dwritetextformat1_SetFontFallback,
    dwritetextformat1_GetFontFallback
};

HRESULT create_textformat(const WCHAR *family_name, IDWriteFontCollection *collection, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
    DWRITE_FONT_STRETCH stretch, FLOAT size, const WCHAR *locale, IDWriteTextFormat **format)
{
    struct dwrite_textformat *This;

    *format = NULL;

    This = heap_alloc(sizeof(struct dwrite_textformat));
    if (!This) return E_OUTOFMEMORY;

    This->IDWriteTextFormat1_iface.lpVtbl = &dwritetextformatvtbl;
    This->ref = 1;
    This->format.family_name = heap_strdupW(family_name);
    This->format.family_len = strlenW(family_name);
    This->format.locale = heap_strdupW(locale);
    This->format.locale_len = strlenW(locale);
    This->format.weight = weight;
    This->format.style = style;
    This->format.fontsize = size;
    This->format.stretch = stretch;
    This->format.textalignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    This->format.paralign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    This->format.wrapping = DWRITE_WORD_WRAPPING_WRAP;
    This->format.readingdir = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    This->format.flow = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;
    This->format.spacingmethod = DWRITE_LINE_SPACING_METHOD_DEFAULT;
    This->format.vertical_orientation = DWRITE_VERTICAL_GLYPH_ORIENTATION_DEFAULT;
    This->format.spacing = 0.0;
    This->format.baseline = 0.0;
    This->format.trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
    This->format.trimming.delimiter = 0;
    This->format.trimming.delimiterCount = 0;
    This->format.trimmingsign = NULL;
    This->format.collection = collection;
    This->format.fallback = NULL;
    IDWriteFontCollection_AddRef(collection);

    *format = (IDWriteTextFormat*)&This->IDWriteTextFormat1_iface;

    return S_OK;
}

static HRESULT WINAPI dwritetypography_QueryInterface(IDWriteTypography *iface, REFIID riid, void **obj)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);

    TRACE("(%p)->(%s %p)\n", typography, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IDWriteTypography) || IsEqualIID(riid, &IID_IUnknown)) {
        *obj = iface;
        IDWriteTypography_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;

    return E_NOINTERFACE;
}

static ULONG WINAPI dwritetypography_AddRef(IDWriteTypography *iface)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);
    ULONG ref = InterlockedIncrement(&typography->ref);
    TRACE("(%p)->(%d)\n", typography, ref);
    return ref;
}

static ULONG WINAPI dwritetypography_Release(IDWriteTypography *iface)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);
    ULONG ref = InterlockedDecrement(&typography->ref);

    TRACE("(%p)->(%d)\n", typography, ref);

    if (!ref) {
        heap_free(typography->features);
        heap_free(typography);
    }

    return ref;
}

static HRESULT WINAPI dwritetypography_AddFontFeature(IDWriteTypography *iface, DWRITE_FONT_FEATURE feature)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);

    TRACE("(%p)->(%x %u)\n", typography, feature.nameTag, feature.parameter);

    if (typography->count == typography->allocated) {
        DWRITE_FONT_FEATURE *ptr = heap_realloc(typography->features, 2*typography->allocated*sizeof(DWRITE_FONT_FEATURE));
        if (!ptr)
            return E_OUTOFMEMORY;

        typography->features = ptr;
        typography->allocated *= 2;
    }

    typography->features[typography->count++] = feature;
    return S_OK;
}

static UINT32 WINAPI dwritetypography_GetFontFeatureCount(IDWriteTypography *iface)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);
    TRACE("(%p)\n", typography);
    return typography->count;
}

static HRESULT WINAPI dwritetypography_GetFontFeature(IDWriteTypography *iface, UINT32 index, DWRITE_FONT_FEATURE *feature)
{
    struct dwrite_typography *typography = impl_from_IDWriteTypography(iface);

    TRACE("(%p)->(%u %p)\n", typography, index, feature);

    if (index >= typography->count)
        return E_INVALIDARG;

    *feature = typography->features[index];
    return S_OK;
}

static const IDWriteTypographyVtbl dwritetypographyvtbl = {
    dwritetypography_QueryInterface,
    dwritetypography_AddRef,
    dwritetypography_Release,
    dwritetypography_AddFontFeature,
    dwritetypography_GetFontFeatureCount,
    dwritetypography_GetFontFeature
};

HRESULT create_typography(IDWriteTypography **ret)
{
    struct dwrite_typography *typography;

    *ret = NULL;

    typography = heap_alloc(sizeof(*typography));
    if (!typography)
        return E_OUTOFMEMORY;

    typography->IDWriteTypography_iface.lpVtbl = &dwritetypographyvtbl;
    typography->ref = 1;
    typography->allocated = 2;
    typography->count = 0;

    typography->features = heap_alloc(typography->allocated*sizeof(DWRITE_FONT_FEATURE));
    if (!typography->features) {
        heap_free(typography);
        return E_OUTOFMEMORY;
    }

    *ret = &typography->IDWriteTypography_iface;
    return S_OK;
}
