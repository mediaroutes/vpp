/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <netinet/in.h>
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/pg/pg.h>
#include <vppinfra/error.h>
#include <l2sess/l2sess.h>
#include <vnet/l2/l2_classify.h>


typedef struct {
  u32 next_index;
  u32 sw_if_index;
  u32 trace_flags;
  u32 session_tables[2];
  u32 session_nexts[2];
  u8 l4_proto;
} l2sess_trace_t;

/* packet trace format function */

#define _(node_name, node_var, is_out, is_ip6, is_track) \
static u8 * format_## node_var ##_trace (u8 * s, va_list * args)      \
{  \
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *); \
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *); \
  l2sess_trace_t * t = va_arg (*args, l2sess_trace_t *); \
 \
  s = format (s, node_name ": sw_if_index %d, next index %d trace_flags %08x L4 proto %d\n" \
                           "                 tables [ %d, %d ] nexts [ %d, %d ]", \
              t->sw_if_index, t->next_index, t->trace_flags, t->l4_proto, \
              t->session_tables[0], t->session_tables[1], \
              t->session_nexts[0], t->session_nexts[1]); \
  return s; \
}
foreach_l2sess_node
#undef _

#define foreach_l2sess_error \
_(SWAPPED, "Mac swap packets processed")

typedef enum {
#define _(sym,str) L2SESS_ERROR_##sym,
  foreach_l2sess_error
#undef _
  L2SESS_N_ERROR,
} l2sess_error_t;

static char * l2sess_error_strings[] = {
#define _(sym,string) string,
  foreach_l2sess_error
#undef _
};

typedef enum {
  L2SESS_NEXT_DROP,
  L2SESS_N_NEXT,
} l2sess_next_t;

u8
l2sess_get_l4_proto(vlib_buffer_t * b0, int node_is_ip6)
{
  u8 proto;
  int proto_offset;
  if (node_is_ip6) {
    proto_offset = 20;
  } else {
    proto_offset = 23;
  }
  proto = *( (u8 *)vlib_buffer_get_current (b0) + proto_offset);
  return proto;
}

static inline int
l4_tcp_or_udp(u8 proto)
{
  return ((proto == 6) || (proto == 17));
}

void
l2sess_get_session_tables(l2sess_main_t * sm, u32 sw_if_index, vlib_buffer_t * b0, int node_is_out, int node_is_ip6, u8 l4_proto, u32 *session_tables, u32 *trace_flags)
{
/*
 * Based on vnet_buffer (b0)->l2_classify.opaque_index, the direction, l3 and l4 protocol, fill a u32[2] array:
 * [0] is the index for the "direct match" path, [1] is for "mirrored match".
 * Store the indices of the tables to add the session to in session_tables[]
 */
  l2_output_classify_main_t *l2om = &l2_output_classify_main;
  l2_input_classify_main_t *l2im = &l2_input_classify_main;

  u32 output_table_index;
  u32 input_table_index;

  if (!l4_tcp_or_udp(l4_proto)) {
    return;
  }

  if(node_is_ip6) {
    vec_validate_init_empty (l2im->classify_table_index_by_sw_if_index[L2_INPUT_CLASSIFY_TABLE_IP6], sw_if_index, ~0);
    input_table_index = l2im->classify_table_index_by_sw_if_index[L2_INPUT_CLASSIFY_TABLE_IP6][sw_if_index];
    vec_validate_init_empty (l2om->classify_table_index_by_sw_if_index[L2_OUTPUT_CLASSIFY_TABLE_IP6], sw_if_index, ~0);
    output_table_index = l2om->classify_table_index_by_sw_if_index[L2_OUTPUT_CLASSIFY_TABLE_IP6][sw_if_index];
  } else {
    vec_validate_init_empty (l2im->classify_table_index_by_sw_if_index[L2_INPUT_CLASSIFY_TABLE_IP4], sw_if_index, ~0);
    input_table_index = l2im->classify_table_index_by_sw_if_index[L2_INPUT_CLASSIFY_TABLE_IP4][sw_if_index];
    vec_validate_init_empty (l2om->classify_table_index_by_sw_if_index[L2_OUTPUT_CLASSIFY_TABLE_IP4], sw_if_index, ~0);
    output_table_index = l2om->classify_table_index_by_sw_if_index[L2_OUTPUT_CLASSIFY_TABLE_IP4][sw_if_index];
  }

  if(node_is_out) {
    session_tables[0] = output_table_index;
    session_tables[1] = input_table_index;
  } else {
    session_tables[0] = input_table_index;
    session_tables[1] = output_table_index;
  }
}

void
l2sess_get_session_nexts(l2sess_main_t * sm, u32 sw_if_index, vlib_buffer_t * b0, int node_is_out, int node_is_ip6, u8 l4_proto, u32 *session_nexts, u32 *trace_flags)
{
/*
 * Based on vnet_buffer (b0)->l2_classify.opaque_index, the direction, l3 and l4 protocol, fill a u32[2] array:
 * [0] is the index for the "direct match" path, [1] is for "mirrored match".
 * Store the match_next_index in session_nexts[] for a new session entry which is being added to session tables.
 */
  u32 input_node_index;
  u32 output_node_index;

  if (!l4_tcp_or_udp(l4_proto)) {
    return;
  }

  input_node_index = sm->next_slot_track_node_by_is_ip6_is_out[node_is_ip6][0];
  output_node_index = sm->next_slot_track_node_by_is_ip6_is_out[node_is_ip6][1];

  if(node_is_out) {
    session_nexts[0] = output_node_index;
    session_nexts[1] = input_node_index;
  } else {
    session_nexts[0] = input_node_index;
    session_nexts[1] = output_node_index;
  }
}


static inline void
swap_bytes(vlib_buffer_t * b0, int off_a, int off_b, int nbytes)
{
  u8 tmp;
  u8 *pa = vlib_buffer_get_current (b0) + off_a;
  u8 *pb = vlib_buffer_get_current (b0) + off_b;
  while(nbytes--) {
    tmp = *pa;
    *pa++ = *pb;
    *pb++ = tmp;
  }
}

/*
 * This quite pro[bv]ably is a terrible idea performance wise. Moreso doing it twice.
 * Would having a long (ish) chunk of memory work better for this ?
 * We will see when we get to the performance of this.
 */
void
l2sess_flip_l3l4_fields(vlib_buffer_t * b0, int node_is_ip6, u8 l4_proto)
{
  if (!l4_tcp_or_udp(l4_proto)) {
    return;
  }
  if (node_is_ip6) {
    swap_bytes(b0, 22, 38, 16); /* L3 */
    swap_bytes(b0, 54, 56, 2); /* L4 (when no EH!) */
  } else {
    swap_bytes(b0, 26, 30, 4); /* L3 */
    swap_bytes(b0, 34, 36, 2); /* L4 */
  }
}

void
l2sess_add_session(vlib_buffer_t * b0,  int node_is_out, int node_is_ip6, u32 session_table, u32 session_match_next, u32 opaque_index)
{
  vnet_classify_main_t * cm = &vnet_classify_main;
  printf("Adding session to table %d with next %d\n", session_table, session_match_next);
  vnet_classify_add_del_session(cm, session_table, vlib_buffer_get_current (b0), session_match_next, opaque_index, 0, 1);
}


static void
session_side_account_buffer(vlib_buffer_t * b0, l2s_session_side_t *ss, u64 now)
{
  ss->active_time = now;
  ss->n_packets++;
  ss->n_bytes += b0->current_data + b0->current_length;
}


static void *
get_ptr_to_offset(vlib_buffer_t * b0, int offset)
{
  u8 *p = vlib_buffer_get_current (b0) + offset;
  return p;
}


/*
 * FIXME: Hardcoded offsets are ugly, although if casting to structs one
 * would need to take care about alignment.. So let's for now be naive and simple.
 */

void
session_store_ip4_l3l4_info(vlib_buffer_t * b0, l2s_session_t *sess, int node_is_out)
{
    clib_memcpy(&sess->side[1-node_is_out].addr.ip4, get_ptr_to_offset(b0, 26), 4);
    clib_memcpy(&sess->side[node_is_out].addr.ip4, get_ptr_to_offset(b0, 30), 4);
    sess->side[1-node_is_out].port = ntohs(*(u16 *)get_ptr_to_offset(b0, 34));
    sess->side[node_is_out].port = ntohs(*(u16 *)get_ptr_to_offset(b0, 36));
}

void
session_store_ip6_l3l4_info(vlib_buffer_t * b0, l2s_session_t *sess, int node_is_out)
{
    clib_memcpy(&sess->side[1-node_is_out].addr.ip6, get_ptr_to_offset(b0, 22), 16);
    clib_memcpy(&sess->side[node_is_out].addr.ip4, get_ptr_to_offset(b0, 38), 16);
    sess->side[1-node_is_out].port = ntohs(*(u16 *)get_ptr_to_offset(b0, 54));
    sess->side[node_is_out].port = ntohs(*(u16 *)get_ptr_to_offset(b0, 56));
}

static void
delete_session(l2sess_main_t * sm, u32 sw_if_index, u32 session_idx)
{
  /* FIXME: delete the session and its associated classifier entries */
}

static int
session_is_alive(l2sess_main_t * sm, l2s_session_t *sess, u64 now)
{
  u64 last_active = sess->side[0].active_time > sess->side[1].active_time ? sess->side[0].active_time : sess->side[1].active_time;

  u64 idle_timeout = 0;
  switch (sess->l4_proto) {
    case  6: idle_timeout = sm->tcp_session_idle_timeout; break;
    case 17: idle_timeout = sm->udp_session_idle_timeout; break;
    default: idle_timeout = 0;
  }
  return ((now - last_active) < idle_timeout);
}

static uword
l2sess_node_fn (vlib_main_t * vm,
		  vlib_node_runtime_t * node,
		  vlib_frame_t * frame)
{
  u32 n_left_from, * from, * to_next;
  l2sess_next_t next_index;
  u32 pkts_swapped = 0;
  u32 cached_sw_if_index = (u32) ~ 0;
  u32 cached_next_index = (u32) ~ 0;
  u32 feature_bitmap0;
  u32 trace_flags0;

  l2sess_main_t * sm = &l2sess_main;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index,
			   to_next, n_left_to_next);

      /* Only a single loop for now for simplicity */

      while (n_left_from > 0 && n_left_to_next > 0)
	{
          u32 bi0;
	  vlib_buffer_t * b0;
          u32 next0 = L2SESS_NEXT_DROP;
          u32 sw_if_index0;
          //ethernet_header_t *en0;

          /* speculatively enqueue b0 to the current next frame */
	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);
          //en0 = vlib_buffer_get_current (b0);

/*
 * The non-boilerplate is in the block below.
 * Note first a magic macro block that sets up the behavior qualifiers:
 *     node_is_out : 1 = is output, 0 = is input
 *     node_is_ip6 : 1 = is ip6, 0 = is ip4
 *     node_is_track : 1 = is a state tracking node, 0 - is a session addition node
 *
 * Subsequently the code adjusts its behavior depending on these variables.
 * It's most probably not great performance wise but much easier to work with.
 *
 */
{
  int node_is_out = -1;
  CLIB_UNUSED(int node_is_ip6) = -1;
  CLIB_UNUSED(int node_is_track) = -1;
  u32 node_index = 0;
  u32 session_tables[2] = { ~0, ~0 };
  u32 session_nexts[2] = { ~0, ~0 };
  l2_output_next_nodes_st *next_nodes;
  u32 *input_feat_next_node_index;
  u8 l4_proto;
  u64 now = clib_cpu_time_now();

#define _(node_name, node_var, is_out, is_ip6, is_track) \
if(node_var.index == node->node_index) { node_is_out = is_out; node_is_ip6 = is_ip6; node_is_track = is_track; node_index = node_var.index; \
                                         next_nodes = &sm->node_var ## _next_nodes; \
                                         input_feat_next_node_index = sm->node_var ## _input_next_node_index; }
foreach_l2sess_node
#undef _
  trace_flags0 = 0;
  if (node_is_out) {
    sw_if_index0 = vnet_buffer(b0)->sw_if_index[VLIB_TX];
  } else {
    sw_if_index0 = vnet_buffer(b0)->sw_if_index[VLIB_RX];
  }
  /* potentially also remove the nodes here */
  feature_bitmap0 = vnet_buffer (b0)->l2.feature_bitmap;

  if (node_is_track) {
    u32 sess_index =  vnet_buffer (b0)->l2_classify.opaque_index;
    l2s_session_t *sess = sm->sessions_by_sw_if_index[sw_if_index0] + sess_index;
    if (session_is_alive(sm, sess, now)) {
      /* lightweight TCP state tracking goes here */
      session_side_account_buffer(b0, &sess->side[node_is_out], now);
    } else {
      delete_session(sm, sw_if_index0, sess_index);
      /* FIXME: drop the packet that hit the obsolete node, for now. We really ought to recycle it. */
      next0 = 0;
    }
  } else {
   /*
    * "-add" node: take l2opaque which arrived to us, and deduce
    * the tables out of that. ~0 means the topmost classifier table
    * applied for this AF on the RX(for input)/TX(for output)) sw_if_index.
    * Also add the mirrored session to the paired table.
    */
    l2s_session_t *sess;
    u32 sess_index;

    l4_proto = l2sess_get_l4_proto(b0, node_is_ip6);

    vec_validate(sm->sessions_by_sw_if_index, sw_if_index0);
    pool_get(sm->sessions_by_sw_if_index[sw_if_index0], sess);
    sess_index = sess - sm->sessions_by_sw_if_index[sw_if_index0];
    sess->create_time = now;
    sess->side[node_is_out].active_time = now;
    sess->side[1-node_is_out].active_time = now;
    sess->l4_proto = l4_proto;
    sess->is_ip6 = node_is_ip6;
    if (node_is_ip6) {
      session_store_ip6_l3l4_info(b0, sess, node_is_out);
    } else {
      session_store_ip4_l3l4_info(b0, sess, node_is_out);
    }

    l2sess_get_session_tables(sm, sw_if_index0, b0, node_is_out, node_is_ip6, l4_proto, session_tables, &trace_flags0);
    l2sess_get_session_nexts(sm, sw_if_index0, b0, node_is_out, node_is_ip6, l4_proto, session_nexts, &trace_flags0);
    l2sess_flip_l3l4_fields(b0, node_is_ip6, l4_proto);
    if (session_tables[1] != ~0) {
      l2sess_add_session(b0, node_is_out, node_is_ip6, session_tables[1], session_nexts[1], sess_index);
    }
    l2sess_flip_l3l4_fields(b0, node_is_ip6, l4_proto);
    if (session_tables[0] != ~0) {
      l2sess_add_session(b0, node_is_out, node_is_ip6, session_tables[0], session_nexts[0], sess_index);
    }
  }
  if (node_is_out) {
              if(feature_bitmap0) {
                trace_flags0 |= 0x10;
              }
              if(sw_if_index0 == cached_sw_if_index) {
                trace_flags0 |= 0x20;
              }
              l2_output_dispatch (sm->vlib_main,
                                  sm->vnet_main,
                                  node,
                                  node_index,
                                  &cached_sw_if_index,
                                  &cached_next_index,
                                  next_nodes,
                                  b0, sw_if_index0, feature_bitmap0, &next0);
              trace_flags0 |= 2;

  } else {
              next0 =
                feat_bitmap_get_next_node_index (input_feat_next_node_index,
                                                 feature_bitmap0);
                trace_flags0 |= 4;

  }



	  if (next0 >= node->n_next_nodes) {
            trace_flags0 |= 1;
          }

          if (PREDICT_FALSE((node->flags & VLIB_NODE_FLAG_TRACE)
                            && (b0->flags & VLIB_BUFFER_IS_TRACED))) {
            l2sess_trace_t *t =
               vlib_add_trace (vm, node, b0, sizeof (*t));
            t->sw_if_index = sw_if_index0;
            t->next_index = next0;
            t->trace_flags = trace_flags0;
            t->l4_proto = l4_proto;
            t->session_tables[0] = session_tables[0];
            t->session_tables[1] = session_tables[1];
            t->session_nexts[0] = session_nexts[0];
            t->session_nexts[1] = session_nexts[1];
            }

}
          pkts_swapped += 1;
	  if (next0 >= node->n_next_nodes) {
            next0 = 0;
          }

          /* verify speculative enqueue, maybe switch current next frame */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  vlib_node_increment_counter (vm, node->node_index,
                               L2SESS_ERROR_SWAPPED, pkts_swapped);
  return frame->n_vectors;
}


#define _(node_name, node_var, is_out, is_ip6, is_track) \
static uword \
node_var ## node_fn (vlib_main_t * vm, \
                  vlib_node_runtime_t * node, \
                  vlib_frame_t * frame) \
{ \
  return l2sess_node_fn(vm, node, frame); \
} \
VLIB_REGISTER_NODE (node_var) = {                        \
  .function = node_var ## node_fn,                            \
  .name = node_name,                                     \
  .vector_size = sizeof (u32),          \
  .format_trace = format_ ## node_var ## _trace,  \
  .type = VLIB_NODE_TYPE_INTERNAL,   \
                                                  \
  .n_errors = ARRAY_LEN(l2sess_error_strings), \
  .error_strings = l2sess_error_strings,       \
                                                  \
  .n_next_nodes = L2SESS_N_NEXT,                \
  .next_nodes = {                              \
        [L2SESS_NEXT_DROP] = "error-drop",    \
  },                                          \
};
foreach_l2sess_node
#undef _


