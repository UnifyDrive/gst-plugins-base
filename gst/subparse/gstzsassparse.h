/* GStreamer SSA subtitle parser
 * Copyright (c) 2006 Tim-Philipp Müller <tim centricular net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_ZS_ASS_PARSE_H__
#define __GST_ZS_ASS_PARSE_H__

#include <gst/gst.h>
#include <ass/ass.h>
#include <gst/base/gstadapter.h>


G_BEGIN_DECLS

#define GST_TYPE_ZS_ASS_PARSE            (gst_zs_ass_parse_get_type ())
#define GST_ZS_ASS_PARSE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj),  GST_TYPE_ZS_ASS_PARSE, GstZSAssParse))
#define GST_ZS_ASS_PARSE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GST_TYPE_ZS_ASS_PARSE, GstZSAssParseClass))
#define GST_IS_ZS_ASS_PARSE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj),  GST_TYPE_ZS_ASS_PARSE))
#define GST_IS_ZS_ASS_PARSE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GST_TYPE_ZS_ASS_PARSE))

typedef struct _GstZSAssParse GstZSAssParse;
typedef struct _GstZSAssParseClass GstZSAssParseClass;

/* format enum */
typedef enum
{
  GST_ZS_ASS_PARSE_FORMAT_UNKNOWN = 0,
  GST_ZS_ASS_PARSE_FORMAT_ASS = 1,
  GST_ZS_ASS_PARSE_FORMAT_SSA = 2
} GstZSAssParseFormat;

typedef struct {
  int      state;
  GString *buf;
  guint64  start_time;
  guint64  duration;
  guint64  max_duration; /* to clamp duration, 0 = no limit (used by tmplayer parser) */
  GstSegment *segment;
  gpointer user_data;
  gboolean have_internal_fps; /* If TRUE don't overwrite fps by property */
  gint fps_n, fps_d;     /* used by frame based parsers */
  guint8 line_position;          /* percent value */
  gint line_number;              /* line number, can be positive or negative */
  guint8 text_position;          /* percent value */
  guint8 text_size;          /* percent value */
  gchar *vertical;        /* "", "vertical", "vertical-lr" */
  gchar *alignment;       /* "", "start", "middle", "end" */
  gconstpointer allowed_tags; /* list of markup tags allowed in the cue text. */
  gboolean allows_tag_attributes;
  guint8 format_num;
} ZSAssParserState;

typedef gchar* (*ZSAssParser) (ZSAssParserState *state, const gchar *line);


struct _GstZSAssParse {
  GstElement element;

  GstPad         *sinkpad;
  GstPad         *srcpad;

  /* contains the input in the input encoding */
  GstAdapter *adapter;
  /* contains the UTF-8 decoded input */
  GString *textbuf;

  GstZSAssParseFormat parser_type;
  gboolean parser_detected;
  const gchar *subtitle_codec;

  ZSAssParser parse_line;
  ZSAssParserState state;

  /* seek */
  guint64 offset;
  
  /* Segment */
  GstSegment    segment;
  gboolean      need_segment;
  
  gboolean flushing;
  gboolean valid_utf8;
  gchar   *detected_encoding;
  gchar   *encoding;

  gboolean first_buffer;

  /* used by frame based parsers */
  gint fps_n, fps_d;          

  gboolean        framed;
  gboolean        send_tags;

  gchar          *ini;
  GstTask        *task;
  GRecMutex       task_lock;

  ASS_Library  *library;
  ASS_Renderer *renderer;
  ASS_Track    *track;

};

struct _GstZSAssParseClass {
  GstElementClass   parent_class;
};

GType gst_zs_ass_parse_get_type (void);

G_END_DECLS

#endif /* __GST_ZS_ASS_PARSE_H__ */

