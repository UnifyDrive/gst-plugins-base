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

/* Super-primitive SSA parser - we just want the text and ignore
 * everything else like styles and timing codes etc. for now */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>             /* atoi() */
#include <string.h>

#include "gstzsassparse.h"

GST_DEBUG_CATEGORY_STATIC (zs_ass_parse_debug);
#define GST_CAT_DEFAULT zs_ass_parse_debug

static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-ssa; application/x-ass")
    );

static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("text/x-raw, format=pango-markup")
    );

#define DEFAULT_ENCODING   NULL

#define gst_zs_ass_parse_parent_class parent_class
G_DEFINE_TYPE (GstZSAssParse, gst_zs_ass_parse, GST_TYPE_ELEMENT);

static GstStateChangeReturn gst_zs_ass_parse_change_state (GstElement *
    element, GstStateChange transition);
static gboolean gst_zs_ass_parse_setcaps (GstPad * sinkpad, GstCaps * caps);
static gboolean gst_zs_ass_parse_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_zs_ass_parse_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static GstFlowReturn gst_zs_ass_parse_chain (GstPad * sinkpad, GstObject * parent,
    GstBuffer * buf);

static void
parser_state_dispose (GstZSAssParse * self, ZSAssParserState * state);
static gchar *
parse_Ass (ZSAssParserState * state, const gchar * line);


static int gst_zs_ass_parse_init_ass(GstZSAssParse * parse)
{
    int ret = 0;
    
    parse->library = ass_library_init();
    if (!parse->library) {
        GST_ERROR_OBJECT(parse, "ass_library_init() failed!\n");
        return -1;
    }
    ass_set_fonts_dir(parse->library, NULL);

    parse->renderer = ass_renderer_init(parse->library);
    if (!parse->renderer) {
        GST_ERROR_OBJECT(parse, "ass_renderer_init() failed!\n");
        return -1;
    }

    parse->track = ass_new_track(parse->library);
    if (!parse->track) {
        GST_ERROR_OBJECT(parse, "ass_new_track() failed!\n");
        return -1;
    }

    /* Initialize fonts */
    ass_set_fonts(parse->renderer, NULL, NULL, 1, NULL, 1);

    return ret;
}


static void
gst_zs_ass_parse_dispose (GObject * object)
{
  GstZSAssParse *parse = GST_ZS_ASS_PARSE (object);

  if (parse->track)
    ass_free_track(parse->track);
  if (parse->renderer)
    ass_renderer_done(parse->renderer);
  if (parse->library)
    ass_library_done(parse->library);
  
  g_free (parse->ini);
  parse->ini = NULL;

  GST_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
gst_zs_ass_parse_init (GstZSAssParse * parse)
{
  parse->sinkpad = gst_pad_new_from_static_template (&sink_templ, "sink");
  gst_pad_set_chain_function (parse->sinkpad,
      GST_DEBUG_FUNCPTR (gst_zs_ass_parse_chain));
  gst_pad_set_event_function (parse->sinkpad,
      GST_DEBUG_FUNCPTR (gst_zs_ass_parse_sink_event));
  gst_element_add_pad (GST_ELEMENT (parse), parse->sinkpad);

  parse->srcpad = gst_pad_new_from_static_template (&src_templ, "src");
  gst_pad_set_event_function (parse->srcpad,
      GST_DEBUG_FUNCPTR (gst_zs_ass_parse_src_event));
  gst_element_add_pad (GST_ELEMENT (parse), parse->srcpad);
  gst_pad_use_fixed_caps (parse->srcpad);

  parse->ini = NULL;
  parse->framed = FALSE;
  parse->send_tags = FALSE;

  parse->textbuf = g_string_new (NULL);
  parse->parser_type = GST_ZS_ASS_PARSE_FORMAT_UNKNOWN;
  parse->flushing = FALSE;
  gst_segment_init (&parse->segment, GST_FORMAT_TIME);
  parse->need_segment = TRUE;
  parse->encoding = g_strdup (DEFAULT_ENCODING);
  parse->detected_encoding = NULL;
  parse->adapter = gst_adapter_new ();

  parse->fps_n = 24000;
  parse->fps_d = 1001;
  parse->parse_line = parse_Ass;

  gst_zs_ass_parse_init_ass(parse);
}

static void
gst_zs_ass_parse_class_init (GstZSAssParseClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  object_class->dispose = gst_zs_ass_parse_dispose;

  gst_element_class_add_static_pad_template (element_class, &sink_templ);
  gst_element_class_add_static_pad_template (element_class, &src_templ);
  gst_element_class_set_static_metadata (element_class,
      "ZS ASS Subtitle Parser", "Codec/Parser/Subtitle",
      "Parses ASS subtitle streams",
      "Tim-Philipp Müller <tim centricular net>");

  GST_DEBUG_CATEGORY_INIT (zs_ass_parse_debug, "zsassparse", 0,
      "ZS ASS subtitle parser");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_zs_ass_parse_change_state);
}

static GstStateChangeReturn gst_zs_ass_parse_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstZSAssParse *self = GST_ZS_ASS_PARSE (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* format detection will init the parser state */
      self->offset = 0;
      self->parser_type = GST_ZS_ASS_PARSE_FORMAT_UNKNOWN;
      self->valid_utf8 = TRUE;
      self->first_buffer = TRUE;
      g_free (self->detected_encoding);
      self->detected_encoding = NULL;
      g_string_truncate (self->textbuf, 0);
      gst_adapter_clear (self->adapter);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      parser_state_dispose (self, &self->state);
      self->parser_type = GST_ZS_ASS_PARSE_FORMAT_UNKNOWN;
      g_free (self->ini);
      self->ini = NULL;
      self->framed = FALSE;
      break;
    default:
      break;
  }

  return ret;
}



static gboolean
gst_zs_ass_parse_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = TRUE;
  GstZSAssParse *self = GST_ZS_ASS_PARSE (parent);

  GST_DEBUG ("Handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
    {
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      gdouble rate;
      gboolean update;

      gst_event_parse_seek (event, &rate, &format, &flags,
          &start_type, &start, &stop_type, &stop);

      if (format != GST_FORMAT_TIME) {
        GST_WARNING_OBJECT (self, "we only support seeking in TIME format");
        gst_event_unref (event);
        goto beach;
      }

      /* Convert that seek to a seeking in bytes at position 0,
         FIXME: could use an index */
      ret = gst_pad_push_event (self->sinkpad,
          gst_event_new_seek (rate, GST_FORMAT_BYTES, flags,
              GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, 0));

      if (ret) {
        /* Apply the seek to our segment */
        gst_segment_do_seek (&self->segment, rate, format, flags,
            start_type, start, stop_type, stop, &update);

        GST_DEBUG_OBJECT (self, "segment after seek: %" GST_SEGMENT_FORMAT,
            &self->segment);

        /* will mark need_segment when receiving segment from upstream,
         * after FLUSH and all that has happened,
         * rather than racing with chain */
      } else {
        GST_WARNING_OBJECT (self, "seek to 0 bytes failed");
      }

      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

beach:
  return ret;
}

static gboolean
gst_zs_ass_parse_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res;
  GstZSAssParse *self = GST_ZS_ASS_PARSE (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      res = gst_zs_ass_parse_setcaps (pad, caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      const GstSegment *s;
      gst_event_parse_segment (event, &s);
      if (s->format == GST_FORMAT_TIME)
        gst_event_copy_segment (event, &self->segment);
      GST_DEBUG_OBJECT (self, "newsegment (%s)",
          gst_format_get_name (self->segment.format));

      /* if not time format, we'll either start with a 0 timestamp anyway or
       * it's following a seek in which case we'll have saved the requested
       * seek segment and don't want to overwrite it (remember that on a seek
       * we always just seek back to the start in BYTES format and just throw
       * away all text that's before the requested position; if the subtitles
       * come from an upstream demuxer, it won't be able to handle our BYTES
       * seek request and instead send us a newsegment from the seek request
       * it received via its video pads instead, so all is fine then too) */
      res = TRUE;
      gst_event_unref (event);
      /* in either case, let's not simply discard this event;
       * trigger sending of the saved requested seek segment
       * or the one taken here from upstream */
      self->need_segment = TRUE;
      break;
    }
    case GST_EVENT_FLUSH_START:
    {
      self->flushing = TRUE;

      res = gst_pad_event_default (pad, parent, event);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      self->flushing = FALSE;

      res = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static gboolean
gst_zs_ass_parse_setcaps (GstPad * sinkpad, GstCaps * caps)
{
  GstZSAssParse *parse = GST_ZS_ASS_PARSE (GST_PAD_PARENT (sinkpad));
  GstCaps *outcaps;
  const GValue *val;
  GstStructure *s;
  const guchar bom_utf8[] = { 0xEF, 0xBB, 0xBF };
  const gchar *end;
  GstBuffer *priv;
  GstMapInfo map;
  gchar *ptr;
  gsize left, bad_offset;
  gboolean ret;
  const guchar test_utf8[] = { \
 0x5b, 0x53, 0x63, 0x72, 0x69, 0x70, 0x74, 0x20, 0x49, 0x6e, \
 0x66, 0x6f, 0x5d, 0x0d, 0x0a, 0x3b, 0x20, 0x53, 0x63, 0x72, \
 0x69, 0x70, 0x74, 0x20, 0x67, 0x65, 0x6e, 0x65, 0x72, 0x61, \
 0x74, 0x65, 0x64, 0x20, 0x62, 0x79, 0x20, 0x41, 0x65, 0x67, \
 0x69, 0x73, 0x75, 0x62, 0x20, 0x33, 0x2e, 0x32, 0x2e, 0x32, \
 0x0d, 0x0a, 0x3b, 0x20, 0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, \
 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x61, 0x65, 0x67, 0x69, 0x73, \
 0x75, 0x62, 0x2e, 0x6f, 0x72, 0x67, 0x2f, 0x0d, 0x0a, 0x54, \
 0x69, 0x74, 0x6c, 0x65, 0x3a, 0x20, 0x0d, 0x0a, 0x4f, 0x72, \
 0x69, 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x20, 0x53, 0x63, 0x72, \
 0x69, 0x70, 0x74, 0x3a, 0x20, 0x0d, 0x0a, 0x4f, 0x72, 0x69, \
 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x20, 0x54, 0x72, 0x61, 0x6e, \
 0x73, 0x6c, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x3a, 0x20, 0x0d, \
 0x0a, 0x4f, 0x72, 0x69, 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x20, \
 0x54, 0x69, 0x6d, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x0d, 0x0a, \
 0x4f, 0x72, 0x69, 0x67, 0x69, 0x6e, 0x61, 0x6c, 0x20, 0x45, \
 0x64, 0x69, 0x74, 0x69, 0x6e, 0x67, 0x3a, 0x20, 0x0d, 0x0a, \
 0x53, 0x63, 0x72, 0x69, 0x70, 0x74, 0x20, 0x55, 0x70, 0x64, \
 0x61, 0x74, 0x65, 0x64, 0x20, 0x42, 0x79, 0x3a, 0x20, 0x0d, \
 0x0a, 0x55, 0x70, 0x64, 0x61, 0x74, 0x65, 0x20, 0x44, 0x65, \
 0x74, 0x61, 0x69, 0x6c, 0x73, 0x3a, 0x20, 0x0d, 0x0a, 0x53, \
 0x63, 0x72, 0x69, 0x70, 0x74, 0x54, 0x79, 0x70, 0x65, 0x3a, \
 0x20, 0x76, 0x34, 0x2e, 0x30, 0x30, 0x2b, 0x0d, 0x0a, 0x50, \
 0x6c, 0x61, 0x79, 0x52, 0x65, 0x73, 0x58, 0x3a, 0x20, 0x33, \
 0x38, 0x34, 0x0d, 0x0a, 0x50, 0x6c, 0x61, 0x79, 0x52, 0x65, \
 0x73, 0x59, 0x3a, 0x20, 0x32, 0x38, 0x38, 0x0d, 0x0a, 0x54, \
 0x69, 0x6d, 0x65, 0x72, 0x3a, 0x20, 0x31, 0x30, 0x30, 0x2e, \
 0x30, 0x30, 0x30, 0x30, 0x0d, 0x0a, 0x53, 0x79, 0x6e, 0x63, \
 0x68, 0x20, 0x50, 0x6f, 0x69, 0x6e, 0x74, 0x3a, 0x20, 0x30, \
 0x0d, 0x0a, 0x57, 0x72, 0x61, 0x70, 0x53, 0x74, 0x79, 0x6c, \
 0x65, 0x3a, 0x20, 0x30, 0x0d, 0x0a, 0x53, 0x63, 0x61, 0x6c, \
 0x65, 0x64, 0x42, 0x6f, 0x72, 0x64, 0x65, 0x72, 0x41, 0x6e, \
 0x64, 0x53, 0x68, 0x61, 0x64, 0x6f, 0x77, 0x3a, 0x20, 0x6e, \
 0x6f, 0x0d, 0x0a, 0x0d, 0x0a, 0x5b, 0x41, 0x65, 0x67, 0x69, \
 0x73, 0x75, 0x62, 0x20, 0x50, 0x72, 0x6f, 0x6a, 0x65, 0x63, \
 0x74, 0x20, 0x47, 0x61, 0x72, 0x62, 0x61, 0x67, 0x65, 0x5d, \
 0x0d, 0x0a, 0x4c, 0x61, 0x73, 0x74, 0x20, 0x53, 0x74, 0x79, \
 0x6c, 0x65, 0x20, 0x53, 0x74, 0x6f, 0x72, 0x61, 0x67, 0x65, \
 0x3a, 0x20, 0x44, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0x0d, \
 0x0a, 0x41, 0x63, 0x74, 0x69, 0x76, 0x65, 0x20, 0x4c, 0x69, \
 0x6e, 0x65, 0x3a, 0x20, 0x32, 0x38, 0x32, 0x0d, 0x0a, 0x0d, \
 0x0a, 0x5b, 0x56, 0x34, 0x2b, 0x20, 0x53, 0x74, 0x79, 0x6c, \
 0x65, 0x73, 0x5d, 0x0d, 0x0a, 0x46, 0x6f, 0x72, 0x6d, 0x61, \
 0x74, 0x3a, 0x20, 0x4e, 0x61, 0x6d, 0x65, 0x2c, 0x20, 0x46, \
 0x6f, 0x6e, 0x74, 0x6e, 0x61, 0x6d, 0x65, 0x2c, 0x20, 0x46, \
 0x6f, 0x6e, 0x74, 0x73, 0x69, 0x7a, 0x65, 0x2c, 0x20, 0x50, \
 0x72, 0x69, 0x6d, 0x61, 0x72, 0x79, 0x43, 0x6f, 0x6c, 0x6f, \
 0x75, 0x72, 0x2c, 0x20, 0x53, 0x65, 0x63, 0x6f, 0x6e, 0x64, \
 0x61, 0x72, 0x79, 0x43, 0x6f, 0x6c, 0x6f, 0x75, 0x72, 0x2c, \
 0x20, 0x4f, 0x75, 0x74, 0x6c, 0x69, 0x6e, 0x65, 0x43, 0x6f, \
 0x6c, 0x6f, 0x75, 0x72, 0x2c, 0x20, 0x42, 0x61, 0x63, 0x6b, \
 0x43, 0x6f, 0x6c, 0x6f, 0x75, 0x72, 0x2c, 0x20, 0x42, 0x6f, \
 0x6c, 0x64, 0x2c, 0x20, 0x49, 0x74, 0x61, 0x6c, 0x69, 0x63, \
 0x2c, 0x20, 0x55, 0x6e, 0x64, 0x65, 0x72, 0x6c, 0x69, 0x6e, \
 0x65, 0x2c, 0x20, 0x53, 0x74, 0x72, 0x69, 0x6b, 0x65, 0x4f, \
 0x75, 0x74, 0x2c, 0x20, 0x53, 0x63, 0x61, 0x6c, 0x65, 0x58, \
 0x2c, 0x20, 0x53, 0x63, 0x61, 0x6c, 0x65, 0x59, 0x2c, 0x20, \
 0x53, 0x70, 0x61, 0x63, 0x69, 0x6e, 0x67, 0x2c, 0x20, 0x41, \
 0x6e, 0x67, 0x6c, 0x65, 0x2c, 0x20, 0x42, 0x6f, 0x72, 0x64, \
 0x65, 0x72, 0x53, 0x74, 0x79, 0x6c, 0x65, 0x2c, 0x20, 0x4f, \
 0x75, 0x74, 0x6c, 0x69, 0x6e, 0x65, 0x2c, 0x20, 0x53, 0x68, \
 0x61, 0x64, 0x6f, 0x77, 0x2c, 0x20, 0x41, 0x6c, 0x69, 0x67, \
 0x6e, 0x6d, 0x65, 0x6e, 0x74, 0x2c, 0x20, 0x4d, 0x61, 0x72, \
 0x67, 0x69, 0x6e, 0x4c, 0x2c, 0x20, 0x4d, 0x61, 0x72, 0x67, \
 0x69, 0x6e, 0x52, 0x2c, 0x20, 0x4d, 0x61, 0x72, 0x67, 0x69, \
 0x6e, 0x56, 0x2c, 0x20, 0x45, 0x6e, 0x63, 0x6f, 0x64, 0x69, \
 0x6e, 0x67, 0x0d, 0x0a, 0x53, 0x74, 0x79, 0x6c, 0x65, 0x3a, \
 0x20, 0x44, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0x2c, 0xe6, \
 0x96, 0xb9, 0xe6, 0xad, 0xa3, 0xe9, 0xbb, 0x91, 0xe4, 0xbd, \
 0x93, 0x5f, 0x47, 0x42, 0x4b, 0x2c, 0x32, 0x30, 0x2c, 0x26, \
 0x48, 0x30, 0x30, 0x43, 0x33, 0x43, 0x33, 0x43, 0x36, 0x2c, \
 0x26, 0x48, 0x46, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
 0x2c, 0x26, 0x48, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, \
 0x30, 0x2c, 0x26, 0x48, 0x33, 0x32, 0x30, 0x30, 0x30, 0x30, \
 0x30, 0x30, 0x2c, 0x2d, 0x31, 0x2c, 0x30, 0x2c, 0x30, 0x2c, \
 0x30, 0x2c, 0x31, 0x30, 0x30, 0x2c, 0x31, 0x30, 0x30, 0x2c, \
 0x30, 0x2c, 0x30, 0x2c, 0x31, 0x2c, 0x31, 0x2c, 0x30, 0x2c, \
 0x32, 0x2c, 0x35, 0x2c, 0x35, 0x2c, 0x32, 0x2c, 0x31, 0x33, \
 0x34, 0x0d, 0x0a, 0x53, 0x74, 0x79, 0x6c, 0x65, 0x3a, 0x20, \
 0xe8, 0x8b, 0xb1, 0xe6, 0x96, 0x87, 0xe5, 0xb0, 0x8f, 0xe5, \
 0xad, 0x97, 0xe5, 0xb9, 0x95, 0x2c, 0x4d, 0x69, 0x63, 0x72, \
 0x6f, 0x73, 0x6f, 0x66, 0x74, 0x20, 0x59, 0x61, 0x48, 0x65, \
 0x69, 0x2c, 0x31, 0x34, 0x2c, 0x26, 0x48, 0x30, 0x39, 0x30, \
 0x39, 0x42, 0x32, 0x46, 0x38, 0x2c, 0x26, 0x48, 0x46, 0x30, \
 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x2c, 0x26, 0x48, 0x30, \
 0x30, 0x30, 0x30, 0x31, 0x33, 0x31, 0x33, 0x2c, 0x26, 0x48, \
 0x31, 0x35, 0x30, 0x32, 0x31, 0x42, 0x31, 0x44, 0x2c, 0x30, \
 0x2c, 0x30, 0x2c, 0x30, 0x2c, 0x30, 0x2c, 0x31, 0x30, 0x30, \
 0x2c, 0x31, 0x30, 0x30, 0x2c, 0x30, 0x2c, 0x30, 0x2c, 0x31, \
 0x2c, 0x31, 0x2c, 0x30, 0x2c, 0x32, 0x2c, 0x35, 0x2c, 0x35, \
 0x2c, 0x33, 0x2c, 0x31, 0x0d, 0x0a, 0x0d, 0x0a, 0x5b, 0x45, \
 0x76, 0x65, 0x6e, 0x74, 0x73, 0x5d, 0x0d, 0x0a, 0x46, 0x6f, \
 0x72, 0x6d, 0x61, 0x74, 0x3a, 0x20, 0x4c, 0x61, 0x79, 0x65, \
 0x72, 0x2c, 0x20, 0x53, 0x74, 0x61, 0x72, 0x74, 0x2c, 0x20, \
 0x45, 0x6e, 0x64, 0x2c, 0x20, 0x53, 0x74, 0x79, 0x6c, 0x65, \
 0x2c, 0x20, 0x4e, 0x61, 0x6d, 0x65, 0x2c, 0x20, 0x4d, 0x61, \
 0x72, 0x67, 0x69, 0x6e, 0x4c, 0x2c, 0x20, 0x4d, 0x61, 0x72, \
 0x67, 0x69, 0x6e, 0x52, 0x2c, 0x20, 0x4d, 0x61, 0x72, 0x67, \
 0x69, 0x6e, 0x56, 0x2c, 0x20, 0x45, 0x66, 0x66, 0x65, 0x63, \
 0x74, 0x2c, 0x20, 0x54, 0x65, 0x78, 0x74, 0x0d, 0x0a };

  s = gst_caps_get_structure (caps, 0);
  val = gst_structure_get_value (s, "codec_data");
  if (val == NULL) {
    parse->framed = FALSE;
    GST_WARNING ("Only ASS subtitles embedded in containers are supported, use const codec_data");
    //return FALSE;
    priv = gst_buffer_new_allocate (NULL, sizeof(test_utf8), NULL);
    gst_buffer_fill(priv, 0, test_utf8, sizeof(test_utf8));
  }else {
    priv = (GstBuffer *) g_value_get_boxed (val);
    gst_buffer_ref (priv);
    GST_WARNING ("ASS subtitles embedded in containers are supported,find codec_data!!");
  }
  parse->framed = TRUE;
  parse->send_tags = TRUE;
  g_return_val_if_fail (priv != NULL, FALSE);


  if (!gst_buffer_map (priv, &map, GST_MAP_READ)) {
    gst_buffer_unref (priv);
    return FALSE;
  }

  GST_MEMDUMP_OBJECT (parse, "init section", map.data, map.size);

  ptr = (gchar *) map.data;
  left = map.size;

  /* skip UTF-8 BOM */
  if (left >= 3 && memcmp (ptr, bom_utf8, 3) == 0) {
    ptr += 3;
    left -= 3;
  }

  if (!strstr (ptr, "[Script Info]"))
    goto invalid_init;

  if (!g_utf8_validate (ptr, left, &end)) {
    bad_offset = (gsize) (end - ptr);
    GST_WARNING_OBJECT (parse, "Init section is not valid UTF-8. Problem at "
        "byte offset %" G_GSIZE_FORMAT, bad_offset);
    /* continue with valid UTF-8 data */
    left = bad_offset;
  }

  /* FIXME: parse initial section */
  if (parse->ini)
    g_free (parse->ini);
  parse->ini = g_strndup (ptr, left);
  GST_LOG_OBJECT (parse, "Init section:\n%s", parse->ini);

  gst_buffer_unmap (priv, &map);
  gst_buffer_unref (priv);

  outcaps = gst_caps_new_simple ("text/x-raw",
      "format", G_TYPE_STRING, "pango-markup", NULL);

  ret = gst_pad_set_caps (parse->srcpad, outcaps);
  gst_caps_unref (outcaps);

  return ret;

  /* ERRORS */
invalid_init:
  {
    GST_WARNING_OBJECT (parse, "Invalid Init section - no Script Info header");
    gst_buffer_unmap (priv, &map);
    gst_buffer_unref (priv);
    return FALSE;
  }
}


static gchar *
gst_convert_to_utf8 (const gchar * str, gsize len, const gchar * encoding,
    gsize * consumed, GError ** err)
{
  gchar *ret = NULL;

  *consumed = 0;
  /* The char cast is necessary in glib < 2.24 */
  ret =
      g_convert_with_fallback (str, len, "UTF-8", encoding, (char *) "*",
      consumed, NULL, err);
  if (ret == NULL)
    return ret;

  /* + 3 to skip UTF-8 BOM if it was added */
  len = strlen (ret);
  if (len >= 3 && (guint8) ret[0] == 0xEF && (guint8) ret[1] == 0xBB
      && (guint8) ret[2] == 0xBF)
    memmove (ret, ret + 3, len + 1 - 3);

  return ret;
}


static gchar *
detect_encoding (const gchar * str, gsize len)
{
  if (len >= 3 && (guint8) str[0] == 0xEF && (guint8) str[1] == 0xBB
      && (guint8) str[2] == 0xBF)
    return g_strdup ("UTF-8");

  if (len >= 2 && (guint8) str[0] == 0xFE && (guint8) str[1] == 0xFF)
    return g_strdup ("UTF-16BE");

  if (len >= 2 && (guint8) str[0] == 0xFF && (guint8) str[1] == 0xFE)
    return g_strdup ("UTF-16LE");

  if (len >= 4 && (guint8) str[0] == 0x00 && (guint8) str[1] == 0x00
      && (guint8) str[2] == 0xFE && (guint8) str[3] == 0xFF)
    return g_strdup ("UTF-32BE");

  if (len >= 4 && (guint8) str[0] == 0xFF && (guint8) str[1] == 0xFE
      && (guint8) str[2] == 0x00 && (guint8) str[3] == 0x00)
    return g_strdup ("UTF-32LE");

  return NULL;
}

static gchar *
convert_encoding (GstZSAssParse * self, const gchar * str, gsize len,
    gsize * consumed)
{
  const gchar *encoding;
  GError *err = NULL;
  gchar *ret = NULL;

  *consumed = 0;

  /* First try any detected encoding */
  if (self->detected_encoding) {
    ret =
        gst_convert_to_utf8 (str, len, self->detected_encoding, consumed, &err);

    if (!err)
      return ret;

    GST_WARNING_OBJECT (self, "could not convert string from '%s' to UTF-8: %s",
        self->detected_encoding, err->message);
    g_free (self->detected_encoding);
    self->detected_encoding = NULL;
    g_clear_error (&err);
  }

  /* Otherwise check if it's UTF8 */
  if (self->valid_utf8) {
    if (g_utf8_validate (str, len, NULL)) {
      GST_LOG_OBJECT (self, "valid UTF-8, no conversion needed");
      *consumed = len;
      return g_strndup (str, len);
    }
    GST_INFO_OBJECT (self, "invalid UTF-8!");
    self->valid_utf8 = FALSE;
  }

  /* Else try fallback */
  encoding = self->encoding;
  if (encoding == NULL || *encoding == '\0') {
    encoding = g_getenv ("GST_SUBTITLE_ENCODING");
  }
  if (encoding == NULL || *encoding == '\0') {
    /* if local encoding is UTF-8 and no encoding specified
     * via the environment variable, assume ISO-8859-15 */
    if (g_get_charset (&encoding)) {
      encoding = "ISO-8859-15";
    }
  }

  ret = gst_convert_to_utf8 (str, len, encoding, consumed, &err);

  if (err) {
    GST_WARNING_OBJECT (self, "could not convert string from '%s' to UTF-8: %s",
        encoding, err->message);
    g_clear_error (&err);

    /* invalid input encoding, fall back to ISO-8859-15 (always succeeds) */
    ret = gst_convert_to_utf8 (str, len, "ISO-8859-15", consumed, NULL);
  }

  GST_LOG_OBJECT (self,
      "successfully converted %" G_GSIZE_FORMAT " characters from %s to UTF-8"
      "%s", len, encoding, (err) ? " , using ISO-8859-15 as fallback" : "");

  return ret;
}


static gchar *
get_next_line (GstZSAssParse * self)
{
  char *line = NULL;
  const char *line_end;
  int line_len;
  gboolean have_r = FALSE;

  line_end = strchr (self->textbuf->str, '\n');

  if (!line_end) {
    /* end-of-line not found; return for more data */
    return NULL;
  }

  /* get rid of '\r' */
  if (line_end != self->textbuf->str && *(line_end - 1) == '\r') {
    line_end--;
    have_r = TRUE;
  }

  line_len = line_end - self->textbuf->str;
  line = g_strndup (self->textbuf->str, line_len);
  self->textbuf = g_string_erase (self->textbuf, 0,
      line_len + (have_r ? 2 : 1));
  return line;
}

static void
parser_state_init (ZSAssParserState * state)
{
  GST_DEBUG ("initialising parser");

  if (state->buf) {
    g_string_truncate (state->buf, 0);
  } else {
    state->buf = g_string_new (NULL);
  }

  state->start_time = 0;
  state->duration = 0;
  state->max_duration = 0;      /* no limit */
  state->state = 0;
  state->segment = NULL;
  state->format_num = 0;
}

static void
parser_state_dispose (GstZSAssParse * self, ZSAssParserState * state)
{
  if (state->buf) {
    g_string_free (state->buf, TRUE);
    state->buf = NULL;
  }

  g_free (state->vertical);
  state->vertical = NULL;
  g_free (state->alignment);
  state->alignment = NULL;

  if (state->user_data) {
    switch (self->parser_type) {
      
      default:
        break;
    }
  }
  state->allowed_tags = NULL;
}


static void
feed_textbuf (GstZSAssParse * self, GstBuffer * buf)
{
  gboolean discont;
  gsize consumed;
  gchar *input = NULL;
  const guint8 *data;
  gsize avail;

  discont = GST_BUFFER_IS_DISCONT (buf);

  if (GST_BUFFER_OFFSET_IS_VALID (buf) &&
      GST_BUFFER_OFFSET (buf) != self->offset) {
    self->offset = GST_BUFFER_OFFSET (buf);
    discont = TRUE;
  }

  if (discont) {
    GST_WARNING ("discontinuity");
    /* flush the parser state */
    parser_state_init (&self->state);
    g_string_truncate (self->textbuf, 0);
    gst_adapter_clear (self->adapter);
    /* we could set a flag to make sure that the next buffer we push out also
     * has the DISCONT flag set, but there's no point really given that it's
     * subtitles which are discontinuous by nature. */
  }

  self->offset += gst_buffer_get_size (buf);

  gst_adapter_push (self->adapter, buf);

  avail = gst_adapter_available (self->adapter);
  data = gst_adapter_map (self->adapter, avail);
  input = convert_encoding (self, (const gchar *) data, avail, &consumed);

  if (input && consumed > 0) {
    self->textbuf = g_string_append (self->textbuf, input);
    gst_adapter_unmap (self->adapter);
    gst_adapter_flush (self->adapter, consumed);
  } else {
    gst_adapter_unmap (self->adapter);
  }

  g_free (input);
}

static gboolean
parse_ass_time (const gchar * ts_string, GstClockTime * t)
{
  gchar s[128] = { '\0', };
  gchar *end, *p;
  guint hour, min, sec, msec, len;

  while (*ts_string == ' ')
    ++ts_string;

  g_strlcpy (s, ts_string, sizeof (s));
  if ((end = strstr (s, ",")))
    *end = '\0';
  g_strchomp (s);

  /* ms may be in these formats:
   * hh:mm:ss,500 = 500ms
   * hh:mm:ss,  5 =   5ms
   * hh:mm:ss, 5  =  50ms
   * hh:mm:ss, 50 =  50ms
   * hh:mm:ss,5   = 500ms
   * and the same with . instead of ,.
   * sscanf() doesn't differentiate between '  5' and '5' so munge
   * the white spaces within the timestamp to '0' (I'm sure there's a
   * way to make sscanf() do this for us, but how?)
   */
  g_strdelimit (s, " ", '0');
  g_strdelimit (s, ".", ',');

  /* make sure we have exactly three digits after he comma */
  p = strchr (s, ',');
  if (p == NULL) {
    /* If there isn't a ',' the timestamp is broken */
    /* https://gitlab.freedesktop.org/gstreamer/gst-plugins-base/issues/532#note_100179 */
    GST_WARNING ("failed to parse subrip timestamp string '%s'", s);
    return FALSE;
  }

  ++p;
  len = strlen (p);
  if (len > 3) {
    p[3] = '\0';
  } else
    while (len < 3) {
      g_strlcat (&p[len], "0", 2);
      ++len;
    }

  GST_WARNING ("parsing timestamp '%s'", s);
  if (sscanf (s, "%u:%u:%u,%u", &hour, &min, &sec, &msec) != 4) {
    GST_WARNING ("failed to parse subrip timestamp string '%s'", s);
    return FALSE;
  }

  *t = ((hour * 3600) + (min * 60) + sec) * GST_SECOND + msec * GST_MSECOND;
  return TRUE;
}


static gchar *
parse_Ass (ZSAssParserState * state, const gchar * line)
{
  gchar *ret = NULL;
  GstClockTime ts_start, ts_end;
  gchar *ts_start_pos, *ts_end_pos;
  int i = 0;

  if (!strstr (line, "Dialogue") && !strstr (line, "Events") && !strstr (line, "Format")) {
    GST_DEBUG ("Drop it!!!\n");
    return NULL;
  }

  if (strstr (line, "Events")) {
    state->state = 1;
    return NULL;
  }else if (state->state ==1 && strstr (line, "Format")) {
    state->state = 2;
    ret = line;
    do {
        ret = strstr(ret, ",");
        if (ret) {
            state->format_num++;
            ret++;
        }
    }while(ret);
    GST_DEBUG ("state->format_num=%d!\n", state->format_num);
    return NULL;
  }

  if (state->state == 2 || 1) {
      ts_start_pos = strstr (line, ",") + 1;
      ts_end_pos = strstr (ts_start_pos, ",") + 1;
      parse_ass_time (ts_start_pos, &ts_start);
      parse_ass_time (ts_end_pos, &ts_end);
      state->start_time = ts_start;
      state->duration = ts_end - ts_start;

      i = state->format_num - 2;
      ret = ts_end_pos;
      do {
        ret = strstr (ret, ",") + 1;
        i--;
      }while(i > 1);
      ret++;
  }

  return ret;
}


static GstFlowReturn
handle_buffer (GstZSAssParse * self, GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  //GstCaps *caps = NULL;
  gchar *line, *subtitle;
  gboolean need_tags = FALSE;

  if (self->first_buffer) {
    GstMapInfo map;

    gst_buffer_map (buf, &map, GST_MAP_READ);
    self->detected_encoding = detect_encoding ((gchar *) map.data, map.size);
    gst_buffer_unmap (buf, &map);
    self->first_buffer = FALSE;
    self->state.fps_n = self->fps_n;
    self->state.fps_d = self->fps_d;
  }

  feed_textbuf (self, buf);

  /* make sure we know the format */
  if (G_UNLIKELY (self->parser_type == GST_ZS_ASS_PARSE_FORMAT_UNKNOWN)) {
    
  }

  /* Push newsegment if needed */
  if (self->need_segment) {
    GST_DEBUG_OBJECT (self, "pushing newsegment event with %" GST_SEGMENT_FORMAT,
        &self->segment);

    gst_pad_push_event (self->srcpad, gst_event_new_segment (&self->segment));
    self->need_segment = FALSE;
  }

  if (need_tags) {
    /* push tags */
    if (self->subtitle_codec != NULL) {
      GstTagList *tags;

      tags = gst_tag_list_new (GST_TAG_SUBTITLE_CODEC, self->subtitle_codec,
          NULL);
      gst_pad_push_event (self->srcpad, gst_event_new_tag (tags));
    }
  }

  while (!self->flushing && (line = get_next_line (self))) {
    guint offset = 0;

    /* Set segment on our parser state machine */
    self->state.segment = &self->segment;
    /* Now parse the line, out of segment lines will just return NULL */
    GST_DEBUG_OBJECT (self, "State %d. Parsing line '%s'", self->state.state,
        line + offset);
    subtitle = self->parse_line (&self->state, line + offset);

    if (subtitle) {
      guint subtitle_len = strlen (subtitle);

      /* +1 for terminating NUL character */
      buf = gst_buffer_new_and_alloc (subtitle_len + 1);

      /* copy terminating NUL character as well */
      gst_buffer_fill (buf, 0, subtitle, subtitle_len + 1);
      gst_buffer_set_size (buf, subtitle_len);

      GST_BUFFER_TIMESTAMP (buf) = self->state.start_time;
      GST_BUFFER_DURATION (buf) = self->state.duration;

      /* in some cases (e.g. tmplayer) we can only determine the duration
       * of a text chunk from the timestamp of the next text chunk; in those
       * cases, we probably want to limit the duration to something
       * reasonable, so we don't end up showing some text for e.g. 40 seconds
       * just because nothing else is being said during that time */
      if (self->state.max_duration > 0 && GST_BUFFER_DURATION_IS_VALID (buf)) {
        if (GST_BUFFER_DURATION (buf) > self->state.max_duration)
          GST_BUFFER_DURATION (buf) = self->state.max_duration;
      }

      self->segment.position = self->state.start_time;

      GST_DEBUG_OBJECT (self, "Sending text '%s', %" GST_TIME_FORMAT " + %"
          GST_TIME_FORMAT, subtitle, GST_TIME_ARGS (self->state.start_time),
          GST_TIME_ARGS (self->state.duration));

      g_free (self->state.vertical);
      self->state.vertical = NULL;
      g_free (self->state.alignment);
      self->state.alignment = NULL;

      ret = gst_pad_push (self->srcpad, buf);

      /* move this forward (the tmplayer parser needs this) */
      if (self->state.duration != GST_CLOCK_TIME_NONE)
        self->state.start_time += self->state.duration;

      subtitle = NULL;

      if (ret != GST_FLOW_OK) {
        GST_DEBUG_OBJECT (self, "flow: %s", gst_flow_get_name (ret));
        break;
      }
    }
    g_free (line);
  }

  return ret;
}


#if 0
static GstFlowReturn
gst_zs_ass_parse_chain (GstPad * sinkpad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret;
  GstZSAssParse *parse = GST_ZS_ASS_PARSE (parent);
  GstClockTime ts;
  gchar *txt;
  GstMapInfo map;

  if (G_UNLIKELY (!parse->framed))
    goto not_framed;

  if (G_UNLIKELY (parse->send_tags)) {
    GstTagList *tags;

    tags = gst_tag_list_new_empty ();
    gst_tag_list_add (tags, GST_TAG_MERGE_APPEND, GST_TAG_SUBTITLE_CODEC,
        "SubStation Alpha", NULL);
    gst_pad_push_event (parse->srcpad, gst_event_new_tag (tags));
    parse->send_tags = FALSE;
  }

  /* make double-sure it's 0-terminated and all */
  gst_buffer_map (buf, &map, GST_MAP_READ);
  txt = g_strndup ((gchar *) map.data, map.size);
  gst_buffer_unmap (buf, &map);

  if (txt == NULL)
    goto empty_text;

  ts = GST_BUFFER_TIMESTAMP (buf);
  ret = gst_zs_ass_parse_push_line (parse, txt, ts, GST_BUFFER_DURATION (buf));

  if (ret != GST_FLOW_OK && GST_CLOCK_TIME_IS_VALID (ts)) {
    GstSegment segment;

    /* just advance time without sending anything */
    gst_segment_init (&segment, GST_FORMAT_TIME);
    segment.start = ts;
    segment.time = ts;
    gst_pad_push_event (parse->srcpad, gst_event_new_segment (&segment));
    ret = GST_FLOW_OK;
  }

  gst_buffer_unref (buf);
  g_free (txt);

  return ret;

/* ERRORS */
not_framed:
  {
    GST_ELEMENT_ERROR (parse, STREAM, FORMAT, (NULL),
        ("Only SSA subtitles embedded in containers are supported"));
    gst_buffer_unref (buf);
    return GST_FLOW_NOT_NEGOTIATED;
  }
empty_text:
  {
    GST_ELEMENT_WARNING (parse, STREAM, FORMAT, (NULL),
        ("Received empty subtitle"));
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }
}
#else
static GstFlowReturn
gst_zs_ass_parse_chain (GstPad * sinkpad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret;
  GstZSAssParse *parse = GST_ZS_ASS_PARSE (parent);

  if (G_UNLIKELY (!parse->framed))
    goto not_framed;

  if (G_UNLIKELY (parse->send_tags)) {
    GstTagList *tags;

    tags = gst_tag_list_new_empty ();
    gst_tag_list_add (tags, GST_TAG_MERGE_APPEND, GST_TAG_SUBTITLE_CODEC,
        "SubStation Alpha", NULL);
    gst_pad_push_event (parse->srcpad, gst_event_new_tag (tags));
    parse->send_tags = FALSE;
  }

  ret = handle_buffer (parse, buf);

  gst_buffer_unref (buf);

  return ret;

/* ERRORS */
not_framed:
  {
    GST_ELEMENT_ERROR (parse, STREAM, FORMAT, (NULL),
        ("Only SSA subtitles embedded in containers are supported"));
    gst_buffer_unref (buf);
    return GST_FLOW_NOT_NEGOTIATED;
  }
}
#endif

