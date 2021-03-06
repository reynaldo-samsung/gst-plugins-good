/* RTP Retransmission sender element for GStreamer
 *
 * gstrtprtxsend.c:
 *
 * Copyright (C) 2013 Collabora Ltd.
 *   @author Julien Isorce <julien.isorce@collabora.co.uk>
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

/**
 * SECTION:element-rtprtxsend
 *
 * See #GstRtpRtxReceive for examples
 * 
 * The purpose of the sender RTX object is to keep a history of RTP packets up
 * to a configurable limit (max-size-time or max-size-packets). It will listen
 * for upstream custom retransmission events (GstRTPRetransmissionRequest) that
 * comes from downstream (#GstRtpSession). When receiving a request it will
 * look up the requested seqnum in its list of stored packets. If the packet
 * is available, it will create a RTX packet according to RFC 4588 and send
 * this as an auxiliary stream. RTX is SSRC-multiplexed
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <string.h>
#include <stdlib.h>

#include "gstrtprtxsend.h"

GST_DEBUG_CATEGORY_STATIC (gst_rtp_rtx_send_debug);
#define GST_CAT_DEFAULT gst_rtp_rtx_send_debug

#define DEFAULT_RTX_PAYLOAD_TYPE 0
#define DEFAULT_MAX_SIZE_TIME    0
#define DEFAULT_MAX_SIZE_PACKETS 100

enum
{
  PROP_0,
  PROP_SSRC_MAP,
  PROP_PAYLOAD_TYPE_MAP,
  PROP_MAX_SIZE_TIME,
  PROP_MAX_SIZE_PACKETS,
  PROP_NUM_RTX_REQUESTS,
  PROP_NUM_RTX_PACKETS,
  PROP_LAST
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );

static gboolean gst_rtp_rtx_send_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_rtp_rtx_send_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static GstFlowReturn gst_rtp_rtx_send_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);

static GstStateChangeReturn gst_rtp_rtx_send_change_state (GstElement *
    element, GstStateChange transition);

static void gst_rtp_rtx_send_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rtp_rtx_send_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_rtp_rtx_send_finalize (GObject * object);

G_DEFINE_TYPE (GstRtpRtxSend, gst_rtp_rtx_send, GST_TYPE_ELEMENT);

typedef struct
{
  guint16 seqnum;
  guint32 timestamp;
  GstBuffer *buffer;
} BufferQueueItem;

static void
buffer_queue_item_free (BufferQueueItem * item)
{
  gst_buffer_unref (item->buffer);
  g_free (item);
}

typedef struct
{
  guint32 rtx_ssrc;
  guint16 next_seqnum;
  gint clock_rate;

  /* history of rtp packets */
  GSequence *queue;
} SSRCRtxData;

static SSRCRtxData *
ssrc_rtx_data_new (guint32 rtx_ssrc)
{
  SSRCRtxData *data = g_new0 (SSRCRtxData, 1);

  data->rtx_ssrc = rtx_ssrc;
  data->next_seqnum = g_random_int_range (0, G_MAXUINT16);
  data->queue = g_sequence_new ((GDestroyNotify) buffer_queue_item_free);

  return data;
}

static void
ssrc_rtx_data_free (SSRCRtxData * data)
{
  g_sequence_free (data->queue);
  g_free (data);
}

static void
gst_rtp_rtx_send_class_init (GstRtpRtxSendClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->get_property = gst_rtp_rtx_send_get_property;
  gobject_class->set_property = gst_rtp_rtx_send_set_property;
  gobject_class->finalize = gst_rtp_rtx_send_finalize;

  g_object_class_install_property (gobject_class, PROP_SSRC_MAP,
      g_param_spec_boxed ("ssrc-map", "SSRC Map",
          "Map of SSRCs to their retransmission SSRCs for SSRC-multiplexed mode"
          " (default = random)", GST_TYPE_STRUCTURE,
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PAYLOAD_TYPE_MAP,
      g_param_spec_boxed ("payload-type-map", "Payload Type Map",
          "Map of original payload types to their retransmission payload types",
          GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_SIZE_TIME,
      g_param_spec_uint ("max-size-time", "Max Size Time",
          "Amount of ms to queue (0 = unlimited)", 0, G_MAXUINT,
          DEFAULT_MAX_SIZE_TIME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_SIZE_PACKETS,
      g_param_spec_uint ("max-size-packets", "Max Size Packets",
          "Amount of packets to queue (0 = unlimited)", 0, G_MAXINT16,
          DEFAULT_MAX_SIZE_PACKETS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_RTX_REQUESTS,
      g_param_spec_uint ("num-rtx-requests", "Num RTX Requests",
          "Number of retransmission events received", 0, G_MAXUINT,
          0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_RTX_PACKETS,
      g_param_spec_uint ("num-rtx-packets", "Num RTX Packets",
          " Number of retransmission packets sent", 0, G_MAXUINT,
          0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gst_element_class_set_static_metadata (gstelement_class,
      "RTP Retransmission Sender", "Codec",
      "Retransmit RTP packets when needed, according to RFC4588",
      "Julien Isorce <julien.isorce@collabora.co.uk>");

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_rtp_rtx_send_change_state);
}

static void
gst_rtp_rtx_send_reset (GstRtpRtxSend * rtx, gboolean full)
{
  g_mutex_lock (&rtx->lock);
  g_queue_foreach (rtx->pending, (GFunc) gst_buffer_unref, NULL);
  g_queue_clear (rtx->pending);
  g_hash_table_remove_all (rtx->ssrc_data);
  g_hash_table_remove_all (rtx->rtx_ssrcs);
  rtx->num_rtx_requests = 0;
  rtx->num_rtx_packets = 0;
  g_mutex_unlock (&rtx->lock);
}

static void
gst_rtp_rtx_send_finalize (GObject * object)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (object);

  gst_rtp_rtx_send_reset (rtx, TRUE);

  g_hash_table_unref (rtx->ssrc_data);
  g_hash_table_unref (rtx->rtx_ssrcs);
  if (rtx->external_ssrc_map)
    gst_structure_free (rtx->external_ssrc_map);
  g_hash_table_unref (rtx->rtx_pt_map);
  if (rtx->pending_rtx_pt_map)
    gst_structure_free (rtx->pending_rtx_pt_map);
  g_queue_free (rtx->pending);
  g_mutex_clear (&rtx->lock);

  G_OBJECT_CLASS (gst_rtp_rtx_send_parent_class)->finalize (object);
}

static void
gst_rtp_rtx_send_init (GstRtpRtxSend * rtx)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (rtx);

  rtx->srcpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template (klass,
          "src"), "src");
  GST_PAD_SET_PROXY_CAPS (rtx->srcpad);
  GST_PAD_SET_PROXY_ALLOCATION (rtx->srcpad);
  gst_pad_set_event_function (rtx->srcpad,
      GST_DEBUG_FUNCPTR (gst_rtp_rtx_send_src_event));
  gst_element_add_pad (GST_ELEMENT (rtx), rtx->srcpad);

  rtx->sinkpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template (klass,
          "sink"), "sink");
  GST_PAD_SET_PROXY_CAPS (rtx->sinkpad);
  GST_PAD_SET_PROXY_ALLOCATION (rtx->sinkpad);
  gst_pad_set_event_function (rtx->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_rtx_send_sink_event));
  gst_pad_set_chain_function (rtx->sinkpad,
      GST_DEBUG_FUNCPTR (gst_rtp_rtx_send_chain));
  gst_element_add_pad (GST_ELEMENT (rtx), rtx->sinkpad);

  g_mutex_init (&rtx->lock);
  rtx->pending = g_queue_new ();
  rtx->ssrc_data = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) ssrc_rtx_data_free);
  rtx->rtx_ssrcs = g_hash_table_new (g_direct_hash, g_direct_equal);
  rtx->rtx_pt_map = g_hash_table_new (g_direct_hash, g_direct_equal);
  rtx->rtx_pt_map_changed = FALSE;

  rtx->max_size_time = DEFAULT_MAX_SIZE_TIME;
  rtx->max_size_packets = DEFAULT_MAX_SIZE_PACKETS;
}

static guint32
gst_rtp_rtx_send_choose_ssrc (GstRtpRtxSend * rtx, guint32 choice,
    gboolean consider_choice)
{
  guint32 ssrc = consider_choice ? choice : g_random_int ();

  /* make sure to be different than any other */
  while (g_hash_table_contains (rtx->ssrc_data, GUINT_TO_POINTER (ssrc)) ||
      g_hash_table_contains (rtx->rtx_ssrcs, GUINT_TO_POINTER (ssrc))) {
    ssrc = g_random_int ();
  }

  return ssrc;
}

static SSRCRtxData *
gst_rtp_rtx_send_get_ssrc_data (GstRtpRtxSend * rtx, guint32 ssrc)
{
  SSRCRtxData *data;
  guint32 rtx_ssrc;
  gboolean consider = FALSE;

  if (G_UNLIKELY (!g_hash_table_contains (rtx->ssrc_data,
              GUINT_TO_POINTER (ssrc)))) {
    if (rtx->external_ssrc_map) {
      gchar *ssrc_str;
      ssrc_str = g_strdup_printf ("%" G_GUINT32_FORMAT, ssrc);
      consider = gst_structure_get_uint (rtx->external_ssrc_map, ssrc_str,
          &rtx_ssrc);
      g_free (ssrc_str);
    }
    rtx_ssrc = gst_rtp_rtx_send_choose_ssrc (rtx, rtx_ssrc, consider);
    data = ssrc_rtx_data_new (rtx_ssrc);
    g_hash_table_insert (rtx->ssrc_data, GUINT_TO_POINTER (ssrc), data);
    g_hash_table_insert (rtx->rtx_ssrcs, GUINT_TO_POINTER (rtx_ssrc),
        GUINT_TO_POINTER (ssrc));
  } else {
    data = g_hash_table_lookup (rtx->ssrc_data, GUINT_TO_POINTER (ssrc));
  }
  return data;
}

static gint
buffer_queue_items_cmp (BufferQueueItem * a, BufferQueueItem * b,
    gpointer user_data)
{
  /* gst_rtp_buffer_compare_seqnum returns the opposite of what we want,
   * it returns negative when seqnum1 > seqnum2 and we want negative
   * when b > a, i.e. a is smaller, so it comes first in the sequence */
  return gst_rtp_buffer_compare_seqnum (b->seqnum, a->seqnum);
}

static gboolean
gst_rtp_rtx_send_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (parent);
  gboolean res;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
    {
      const GstStructure *s = gst_event_get_structure (event);

      /* This event usually comes from the downstream gstrtpsession */
      if (gst_structure_has_name (s, "GstRTPRetransmissionRequest")) {
        guint seqnum = 0;
        guint ssrc = 0;

        /* retrieve seqnum of the packet that need to be restransmisted */
        if (!gst_structure_get_uint (s, "seqnum", &seqnum))
          seqnum = -1;

        /* retrieve ssrc of the packet that need to be restransmisted */
        if (!gst_structure_get_uint (s, "ssrc", &ssrc))
          ssrc = -1;

        GST_DEBUG_OBJECT (rtx,
            "request seqnum: %" G_GUINT32_FORMAT ", ssrc: %" G_GUINT32_FORMAT,
            seqnum, ssrc);

        g_mutex_lock (&rtx->lock);
        /* check if request is for us */
        if (g_hash_table_contains (rtx->ssrc_data, GUINT_TO_POINTER (ssrc))) {
          SSRCRtxData *data;
          GSequenceIter *iter;
          BufferQueueItem search_item;

          /* update statistics */
          ++rtx->num_rtx_requests;

          data = gst_rtp_rtx_send_get_ssrc_data (rtx, ssrc);

          search_item.seqnum = seqnum;
          iter = g_sequence_lookup (data->queue, &search_item,
              (GCompareDataFunc) buffer_queue_items_cmp, NULL);
          if (iter) {
            BufferQueueItem *item = g_sequence_get (iter);
            GST_DEBUG_OBJECT (rtx, "found %" G_GUINT16_FORMAT, item->seqnum);
            g_queue_push_tail (rtx->pending, gst_buffer_ref (item->buffer));
          }
        }
        g_mutex_unlock (&rtx->lock);

        gst_event_unref (event);
        res = TRUE;

        /* This event usually comes from the downstream gstrtpsession */
      } else if (gst_structure_has_name (s, "GstRTPCollision")) {
        guint ssrc = 0;

        if (!gst_structure_get_uint (s, "ssrc", &ssrc))
          ssrc = -1;

        GST_DEBUG_OBJECT (rtx, "collision ssrc: %" G_GUINT32_FORMAT, ssrc);

        g_mutex_lock (&rtx->lock);

        /* choose another ssrc for our retransmited stream */
        if (g_hash_table_contains (rtx->rtx_ssrcs, GUINT_TO_POINTER (ssrc))) {
          guint master_ssrc;
          SSRCRtxData *data;

          master_ssrc = GPOINTER_TO_UINT (g_hash_table_lookup (rtx->rtx_ssrcs,
                  GUINT_TO_POINTER (ssrc)));
          data = gst_rtp_rtx_send_get_ssrc_data (rtx, master_ssrc);

          /* change rtx_ssrc and update the reverse map */
          data->rtx_ssrc = gst_rtp_rtx_send_choose_ssrc (rtx, 0, FALSE);
          g_hash_table_remove (rtx->rtx_ssrcs, GUINT_TO_POINTER (ssrc));
          g_hash_table_insert (rtx->rtx_ssrcs,
              GUINT_TO_POINTER (data->rtx_ssrc),
              GUINT_TO_POINTER (master_ssrc));

          g_mutex_unlock (&rtx->lock);

          /* no need to forward to payloader because we make sure to have
           * a different ssrc
           */
          gst_event_unref (event);
          res = TRUE;
        } else {
          /* if master ssrc has collided, remove it from our data, as it
           * is not going to be used any longer */
          if (g_hash_table_contains (rtx->ssrc_data, GUINT_TO_POINTER (ssrc))) {
            SSRCRtxData *data;
            data = gst_rtp_rtx_send_get_ssrc_data (rtx, ssrc);
            g_hash_table_remove (rtx->rtx_ssrcs,
                GUINT_TO_POINTER (data->rtx_ssrc));
            g_hash_table_remove (rtx->ssrc_data, GUINT_TO_POINTER (ssrc));
          }

          g_mutex_unlock (&rtx->lock);

          /* forward event to payloader in case collided ssrc is
           * master stream */
          res = gst_pad_event_default (pad, parent, event);
        }
      } else {
        res = gst_pad_event_default (pad, parent, event);
      }
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static gboolean
gst_rtp_rtx_send_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      GstStructure *s;
      guint ssrc;
      SSRCRtxData *data;

      gst_event_parse_caps (event, &caps);
      g_assert (gst_caps_is_fixed (caps));

      s = gst_caps_get_structure (caps, 0);
      gst_structure_get_uint (s, "ssrc", &ssrc);
      data = gst_rtp_rtx_send_get_ssrc_data (rtx, ssrc);
      gst_structure_get_int (s, "clock-rate", &data->clock_rate);

      GST_DEBUG_OBJECT (rtx, "got clock-rate from caps: %d for ssrc: %u",
          data->clock_rate, ssrc);

      break;
    }
    default:
      break;
  }
  return gst_pad_event_default (pad, parent, event);
}

static gboolean
structure_to_hash_table (GQuark field_id, const GValue * value, gpointer hash)
{
  const gchar *field_str;
  guint field_uint;
  guint value_uint;

  field_str = g_quark_to_string (field_id);
  field_uint = atoi (field_str);
  value_uint = g_value_get_uint (value);
  g_hash_table_insert ((GHashTable *) hash, GUINT_TO_POINTER (field_uint),
      GUINT_TO_POINTER (value_uint));

  return TRUE;
}

/* like rtp_jitter_buffer_get_ts_diff() */
static guint32
gst_rtp_rtx_send_get_ts_diff (SSRCRtxData * data)
{
  guint64 high_ts, low_ts;
  BufferQueueItem *high_buf, *low_buf;
  guint32 result;

  high_buf =
      g_sequence_get (g_sequence_iter_prev (g_sequence_get_end_iter
          (data->queue)));
  low_buf = g_sequence_get (g_sequence_get_begin_iter (data->queue));

  if (!high_buf || !low_buf || high_buf == low_buf)
    return 0;

  high_ts = high_buf->timestamp;
  low_ts = low_buf->timestamp;

  /* it needs to work if ts wraps */
  if (high_ts >= low_ts) {
    result = (guint32) (high_ts - low_ts);
  } else {
    result = (guint32) (high_ts + G_MAXUINT32 + 1 - low_ts);
  }

  /* return value in ms instead of clock ticks */
  return (guint32) gst_util_uint64_scale_int (result, 1000, data->clock_rate);
}

/* Copy fixed header and extension. Add OSN before to copy payload
 * Copy memory to avoid to manually copy each rtp buffer field.
 */
static GstBuffer *
_gst_rtp_rtx_buffer_new (GstRtpRtxSend * rtx, GstBuffer * buffer)
{
  GstMemory *mem = NULL;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  GstRTPBuffer new_rtp = GST_RTP_BUFFER_INIT;
  GstBuffer *new_buffer = gst_buffer_new ();
  GstMapInfo map;
  guint payload_len = 0;
  SSRCRtxData *data;
  guint32 ssrc;
  guint16 seqnum;
  guint8 fmtp;

  gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp);

  /* get needed data from GstRtpRtxSend */
  ssrc = gst_rtp_buffer_get_ssrc (&rtp);
  data = gst_rtp_rtx_send_get_ssrc_data (rtx, ssrc);
  ssrc = data->rtx_ssrc;
  seqnum = data->next_seqnum++;
  fmtp = GPOINTER_TO_UINT (g_hash_table_lookup (rtx->rtx_pt_map,
          GUINT_TO_POINTER (gst_rtp_buffer_get_payload_type (&rtp))));

  GST_DEBUG_OBJECT (rtx,
      "retransmit seqnum: %" G_GUINT16_FORMAT ", ssrc: %" G_GUINT32_FORMAT,
      seqnum, ssrc);

  /* gst_rtp_buffer_map does not map the payload so do it now */
  gst_rtp_buffer_get_payload (&rtp);

  /* If payload type is not set through SDP/property then
   * just bump the value */
  if (fmtp < 96)
    fmtp = gst_rtp_buffer_get_payload_type (&rtp) + 1;

  /* copy fixed header */
  mem = gst_memory_copy (rtp.map[0].memory, 0, rtp.size[0]);
  gst_buffer_append_memory (new_buffer, mem);

  /* copy extension if any */
  if (rtp.size[1]) {
    mem = gst_memory_copy (rtp.map[1].memory, 0, rtp.size[1]);
    gst_buffer_append_memory (new_buffer, mem);
  }

  /* copy payload and add OSN just before */
  payload_len = 2 + rtp.size[2];
  mem = gst_allocator_alloc (NULL, payload_len, NULL);

  gst_memory_map (mem, &map, GST_MAP_WRITE);
  GST_WRITE_UINT16_BE (map.data, gst_rtp_buffer_get_seq (&rtp));
  if (rtp.size[2])
    memcpy (map.data + 2, rtp.data[2], rtp.size[2]);
  gst_memory_unmap (mem, &map);
  gst_buffer_append_memory (new_buffer, mem);

  /* everything needed is copied */
  gst_rtp_buffer_unmap (&rtp);

  /* set ssrc, seqnum and fmtp */
  gst_rtp_buffer_map (new_buffer, GST_MAP_WRITE, &new_rtp);
  gst_rtp_buffer_set_ssrc (&new_rtp, ssrc);
  gst_rtp_buffer_set_seq (&new_rtp, seqnum);
  gst_rtp_buffer_set_payload_type (&new_rtp, fmtp);
  /* RFC 4588: let other elements do the padding, as normal */
  gst_rtp_buffer_set_padding (&new_rtp, FALSE);
  gst_rtp_buffer_unmap (&new_rtp);

  return new_buffer;
}

/* push pending retransmission packet.
 * it constructs rtx packet from original packets */
static void
do_push (GstBuffer * buffer, GstRtpRtxSend * rtx)
{
  gst_pad_push (rtx->srcpad, _gst_rtp_rtx_buffer_new (rtx, buffer));
}

static GstFlowReturn
gst_rtp_rtx_send_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (parent);
  GstFlowReturn ret = GST_FLOW_ERROR;
  GQueue *pending = NULL;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  BufferQueueItem *item;
  SSRCRtxData *data;
  guint16 seqnum;
  guint8 payload_type;
  guint32 ssrc, rtptime;

  /* read the information we want from the buffer */
  gst_rtp_buffer_map (buffer, GST_MAP_READ, &rtp);
  seqnum = gst_rtp_buffer_get_seq (&rtp);
  payload_type = gst_rtp_buffer_get_payload_type (&rtp);
  ssrc = gst_rtp_buffer_get_ssrc (&rtp);
  rtptime = gst_rtp_buffer_get_timestamp (&rtp);
  gst_rtp_buffer_unmap (&rtp);

  g_mutex_lock (&rtx->lock);

  /* transfer payload type while holding the lock */
  if (rtx->rtx_pt_map_changed) {
    g_hash_table_remove_all (rtx->rtx_pt_map);
    gst_structure_foreach (rtx->pending_rtx_pt_map, structure_to_hash_table,
        rtx->rtx_pt_map);
    rtx->rtx_pt_map_changed = FALSE;
  }

  /* do not store the buffer if it's payload type is unknown */
  if (g_hash_table_contains (rtx->rtx_pt_map, GUINT_TO_POINTER (payload_type))) {
    data = gst_rtp_rtx_send_get_ssrc_data (rtx, ssrc);

    /* add current rtp buffer to queue history */
    item = g_new0 (BufferQueueItem, 1);
    item->seqnum = seqnum;
    item->timestamp = rtptime;
    item->buffer = gst_buffer_ref (buffer);
    g_sequence_append (data->queue, item);

    /* remove oldest packets from history if they are too many */
    if (rtx->max_size_packets) {
      while (g_sequence_get_length (data->queue) > rtx->max_size_packets)
        g_sequence_remove (g_sequence_get_begin_iter (data->queue));
    }
    if (rtx->max_size_time) {
      while (gst_rtp_rtx_send_get_ts_diff (data) > rtx->max_size_time)
        g_sequence_remove (g_sequence_get_begin_iter (data->queue));
    }
  }

  /* within lock, get packets that have to be retransmited */
  if (g_queue_get_length (rtx->pending) > 0) {
    pending = rtx->pending;
    rtx->pending = g_queue_new ();

    /* update statistics - assume we will succeed to retransmit those packets */
    rtx->num_rtx_packets += g_queue_get_length (pending);
  }

  /* no need to hold the lock to push rtx packets */
  g_mutex_unlock (&rtx->lock);

  /* retransmit requested packets */
  if (pending) {
    g_queue_foreach (pending, (GFunc) do_push, rtx);
    g_queue_free_full (pending, (GDestroyNotify) gst_buffer_unref);
  }

  GST_LOG_OBJECT (rtx,
      "push seqnum: %" G_GUINT16_FORMAT ", ssrc: %" G_GUINT32_FORMAT, seqnum,
      ssrc);

  /* push current rtp packet */
  ret = gst_pad_push (rtx->srcpad, buffer);

  return ret;
}

static void
gst_rtp_rtx_send_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (object);

  switch (prop_id) {
    case PROP_PAYLOAD_TYPE_MAP:
      g_mutex_lock (&rtx->lock);
      g_value_set_boxed (value, rtx->pending_rtx_pt_map);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_MAX_SIZE_TIME:
      g_mutex_lock (&rtx->lock);
      g_value_set_uint (value, rtx->max_size_time);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_MAX_SIZE_PACKETS:
      g_mutex_lock (&rtx->lock);
      g_value_set_uint (value, rtx->max_size_packets);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_NUM_RTX_REQUESTS:
      g_mutex_lock (&rtx->lock);
      g_value_set_uint (value, rtx->num_rtx_requests);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_NUM_RTX_PACKETS:
      g_mutex_lock (&rtx->lock);
      g_value_set_uint (value, rtx->num_rtx_packets);
      g_mutex_unlock (&rtx->lock);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtp_rtx_send_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRtpRtxSend *rtx = GST_RTP_RTX_SEND (object);

  switch (prop_id) {
    case PROP_SSRC_MAP:
      g_mutex_lock (&rtx->lock);
      if (rtx->external_ssrc_map)
        gst_structure_free (rtx->external_ssrc_map);
      rtx->external_ssrc_map = g_value_dup_boxed (value);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_PAYLOAD_TYPE_MAP:
      g_mutex_lock (&rtx->lock);
      if (rtx->pending_rtx_pt_map)
        gst_structure_free (rtx->pending_rtx_pt_map);
      rtx->pending_rtx_pt_map = g_value_dup_boxed (value);
      rtx->rtx_pt_map_changed = TRUE;
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_MAX_SIZE_TIME:
      g_mutex_lock (&rtx->lock);
      rtx->max_size_time = g_value_get_uint (value);
      g_mutex_unlock (&rtx->lock);
      break;
    case PROP_MAX_SIZE_PACKETS:
      g_mutex_lock (&rtx->lock);
      rtx->max_size_packets = g_value_get_uint (value);
      g_mutex_unlock (&rtx->lock);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_rtp_rtx_send_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstRtpRtxSend *rtx;

  rtx = GST_RTP_RTX_SEND (element);

  switch (transition) {
    default:
      break;
  }

  ret =
      GST_ELEMENT_CLASS (gst_rtp_rtx_send_parent_class)->change_state (element,
      transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_rtp_rtx_send_reset (rtx, TRUE);
      break;
    default:
      break;
  }

  return ret;
}

gboolean
gst_rtp_rtx_send_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_rtp_rtx_send_debug, "rtprtxsend", 0,
      "rtp retransmission sender");

  return gst_element_register (plugin, "rtprtxsend", GST_RANK_NONE,
      GST_TYPE_RTP_RTX_SEND);
}
