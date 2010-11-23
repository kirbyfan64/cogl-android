/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2007,2008,2009,2010 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *  Ivan Leben    <ivan@openedhand.com>
 *  Øyvind Kolås  <pippin@linux.intel.com>
 *  Neil Roberts  <neil@linux.intel.com>
 *  Robert Bragg  <robert@linux.intel.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cogl.h"
#include "cogl-object.h"
#include "cogl-internal.h"
#include "cogl-context.h"
#include "cogl-journal-private.h"
#include "cogl-pipeline-private.h"
#include "cogl-pipeline-opengl-private.h"
#include "cogl-framebuffer-private.h"
#include "cogl-path-private.h"
#include "cogl-texture-private.h"
#include "cogl-primitives-private.h"
#include "cogl-private.h"
#include "cogl-vertex-attribute-private.h"
#include "tesselator/tesselator.h"

#include <string.h>
#include <math.h>

#define _COGL_MAX_BEZ_RECURSE_DEPTH 16

static void _cogl_path_free (CoglPath *path);

static void _cogl_path_build_vbo (CoglPath *path);

COGL_OBJECT_DEFINE (Path, path);

static void
_cogl_path_data_unref (CoglPathData *data)
{
  if (--data->ref_count <= 0)
    {
      g_array_free (data->path_nodes, TRUE);

      if (data->vbo)
        {
          int i;

          cogl_object_unref (data->vbo);
          cogl_object_unref (data->vbo_indices);

          for (i = 0; i < COGL_PATH_N_ATTRIBUTES; i++)
            cogl_object_unref (data->vbo_attributes[i]);
        }

      g_slice_free (CoglPathData, data);
    }
}

static void
_cogl_path_modify (CoglPath *path)
{
  /* This needs to be called whenever the path is about to be modified
     to implement copy-on-write semantics */

  /* If there is more than one path using the data then we need to
     copy the data instead */
  if (path->data->ref_count != 1)
    {
      CoglPathData *old_data = path->data;

      path->data = g_slice_dup (CoglPathData, old_data);
      path->data->path_nodes = g_array_new (FALSE, FALSE,
                                            sizeof (CoglPathNode));
      g_array_append_vals (path->data->path_nodes,
                           old_data->path_nodes->data,
                           old_data->path_nodes->len);

      path->data->vbo = COGL_INVALID_HANDLE;
      path->data->ref_count = 1;

      _cogl_path_data_unref (old_data);
    }
  /* The path is altered so the vbo will now be invalid */
  else if (path->data->vbo)
    {
      int i;

      cogl_object_unref (path->data->vbo);
      cogl_object_unref (path->data->vbo_indices);
      for (i = 0; i < COGL_PATH_N_ATTRIBUTES; i++)
        cogl_object_unref (path->data->vbo_attributes[i]);

      path->data->vbo = COGL_INVALID_HANDLE;
    }
}

void
cogl2_path_set_fill_rule (CoglPath *path,
                          CoglPathFillRule fill_rule)
{
  g_return_if_fail (cogl_is_path (path));

  if (path->data->fill_rule != fill_rule)
    {
      _cogl_path_modify (path);

      path->data->fill_rule = fill_rule;
    }
}

CoglPathFillRule
cogl2_path_get_fill_rule (CoglPath *path)
{
  g_return_val_if_fail (cogl_is_path (path), COGL_PATH_FILL_RULE_NON_ZERO);

  return path->data->fill_rule;
}

static void
_cogl_path_add_node (CoglPath *path,
                     gboolean new_sub_path,
		     float x,
		     float y)
{
  CoglPathNode new_node;
  CoglPathData *data;

  _cogl_path_modify (path);

  data = path->data;

  new_node.x = x;
  new_node.y = y;
  new_node.path_size = 0;

  if (new_sub_path || data->path_nodes->len == 0)
    data->last_path = data->path_nodes->len;

  g_array_append_val (data->path_nodes, new_node);

  g_array_index (data->path_nodes, CoglPathNode, data->last_path).path_size++;

  if (data->path_nodes->len == 1)
    {
      data->path_nodes_min.x = data->path_nodes_max.x = x;
      data->path_nodes_min.y = data->path_nodes_max.y = y;
    }
  else
    {
      if (x < data->path_nodes_min.x)
        data->path_nodes_min.x = x;
      if (x > data->path_nodes_max.x)
        data->path_nodes_max.x = x;
      if (y < data->path_nodes_min.y)
        data->path_nodes_min.y = y;
      if (y > data->path_nodes_max.y)
        data->path_nodes_max.y = y;
    }
}

static void
_cogl_path_stroke_nodes (CoglPath *path)
{
  unsigned int path_start = 0;
  unsigned long enable_flags = COGL_ENABLE_VERTEX_ARRAY;
  CoglPathData *data = path->data;
  CoglPipeline *copy = NULL;
  CoglPipeline *source;

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  _cogl_journal_flush ();

  /* NB: _cogl_framebuffer_flush_state may disrupt various state (such
   * as the pipeline state) when flushing the clip stack, so should
   * always be done first when preparing to draw. */
  _cogl_framebuffer_flush_state (_cogl_get_framebuffer (), 0);

  _cogl_enable (enable_flags);

  if (G_UNLIKELY (ctx->legacy_state_set))
    {
      CoglPipeline *users_source = cogl_get_source ();
      copy = cogl_pipeline_copy (users_source);
      _cogl_pipeline_apply_legacy_state (copy);
      source = copy;
    }
  else
    source = cogl_get_source ();

  if (cogl_pipeline_get_n_layers (source) != 0)
    {
      /* If we haven't already created a derivative pipeline... */
      if (!copy)
        copy = cogl_pipeline_copy (source);
      _cogl_pipeline_prune_to_n_layers (copy, 0);
      source = copy;
    }

  cogl_push_source (source);

  _cogl_pipeline_flush_gl_state (source, FALSE, 0);

  /* Disable all client texture coordinate arrays */
  _cogl_bitmask_clear_all (&ctx->temp_bitmask);
  _cogl_disable_other_texcoord_arrays (&ctx->temp_bitmask);

  while (path_start < data->path_nodes->len)
    {
      CoglPathNode *node = &g_array_index (data->path_nodes, CoglPathNode,
                                           path_start);

      GE( glVertexPointer (2, GL_FLOAT, sizeof (CoglPathNode), &node->x) );
      GE( glDrawArrays (GL_LINE_STRIP, 0, node->path_size) );

      path_start += node->path_size;
    }

  cogl_pop_source ();
}

void
_cogl_path_get_bounds (CoglPath *path,
                       float *min_x,
                       float *min_y,
                       float *max_x,
                       float *max_y)
{
  CoglPathData *data = path->data;

  if (data->path_nodes->len == 0)
    {
      *min_x = 0.0f;
      *min_y = 0.0f;
      *max_x = 0.0f;
      *max_y = 0.0f;
    }
  else
    {
      *min_x = data->path_nodes_min.x;
      *min_y = data->path_nodes_min.y;
      *max_x = data->path_nodes_max.x;
      *max_y = data->path_nodes_max.y;
    }
}

static void
_cogl_path_fill_nodes_with_stencil_buffer (CoglPath *path)
{
  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  g_assert (ctx->current_clip_stack_valid);

  _cogl_add_path_to_stencil_buffer (path,
                                    ctx->current_clip_stack_uses_stencil,
                                    FALSE);

  _cogl_rectangle_immediate (path->data->path_nodes_min.x,
                             path->data->path_nodes_min.y,
                             path->data->path_nodes_max.x,
                             path->data->path_nodes_max.y);

  /* The stencil buffer now contains garbage so the clip area needs to
   * be rebuilt.
   *
   * NB: We only ever try and update the clip state during
   * _cogl_journal_init (when we flush the framebuffer state) which is
   * only called when the journal first gets something logged in it; so
   * we call cogl_flush() to emtpy the journal.
   */
  _cogl_clip_stack_dirty ();
}

static void
_cogl_path_fill_nodes (CoglPath *path)
{
  const GList *l;

  /* If any of the layers of the current pipeline contain sliced
     textures or textures with waste then it won't work to draw the
     path directly. Instead we can use draw the texture as a quad
     clipped to the stencil buffer. */
  for (l = _cogl_pipeline_get_layers (cogl_get_source ()); l; l = l->next)
    {
      CoglHandle layer = l->data;
      CoglHandle texture = _cogl_pipeline_layer_get_texture (layer);

      if (texture != COGL_INVALID_HANDLE &&
          (cogl_texture_is_sliced (texture) ||
           !_cogl_texture_can_hardware_repeat (texture)))
        {
          if (cogl_features_available (COGL_FEATURE_STENCIL_BUFFER))
            _cogl_path_fill_nodes_with_stencil_buffer (path);
          else
            {
              static gboolean seen_warning = FALSE;

              if (!seen_warning)
                {
                  g_warning ("Paths can not be filled using materials with "
                             "sliced textures unless there is a stencil "
                             "buffer");
                  seen_warning = TRUE;
                }
            }

          return;
        }
    }

  _cogl_path_build_vbo (path);

  _cogl_draw_indexed_vertex_attributes_array (COGL_VERTICES_MODE_TRIANGLES,
                                              0, /* first_vertex */
                                              path->data->vbo_n_indices,
                                              path->data->vbo_indices,
                                              path->data->vbo_attributes);
}

void
_cogl_add_path_to_stencil_buffer (CoglPath *path,
                                  gboolean merge,
                                  gboolean need_clear)
{
  CoglPathData *data = path->data;
  unsigned long enable_flags = COGL_ENABLE_VERTEX_ARRAY;
  CoglFramebuffer *framebuffer = _cogl_get_framebuffer ();
  CoglMatrixStack *modelview_stack =
    _cogl_framebuffer_get_modelview_stack (framebuffer);
  CoglMatrixStack *projection_stack =
    _cogl_framebuffer_get_projection_stack (framebuffer);

  _COGL_GET_CONTEXT (ctx, NO_RETVAL);

  /* This can be called from the clip stack code which doesn't flush
     the matrix stacks between calls so we need to ensure they're
     flushed now */
  _cogl_matrix_stack_flush_to_gl (modelview_stack,
                                  COGL_MATRIX_MODELVIEW);
  _cogl_matrix_stack_flush_to_gl (projection_stack,
                                  COGL_MATRIX_PROJECTION);

  /* Just setup a simple pipeline that doesn't use texturing... */
  cogl_push_source (ctx->stencil_pipeline);

  _cogl_pipeline_flush_gl_state (ctx->stencil_pipeline, FALSE, 0);

  _cogl_enable (enable_flags);

  GE( glEnable (GL_STENCIL_TEST) );

  GE( glColorMask (FALSE, FALSE, FALSE, FALSE) );
  GE( glDepthMask (FALSE) );

  if (merge)
    {
      GE (glStencilMask (2));
      GE (glStencilFunc (GL_LEQUAL, 0x2, 0x6));
    }
  else
    {
      /* If we're not using the stencil buffer for clipping then we
         don't need to clear the whole stencil buffer, just the area
         that will be drawn */
      if (need_clear)
        /* If this is being called from the clip stack code then it
           will have set up a scissor for the minimum bounding box of
           all of the clips. That box will likely mean that this
           _cogl_clear won't need to clear the entire
           buffer. _cogl_clear is used instead of cogl_clear because
           it won't try to flush the journal */
        _cogl_clear (NULL, COGL_BUFFER_BIT_STENCIL);
      else
        {
          /* Just clear the bounding box */
          GE( glStencilMask (~(GLuint) 0) );
          GE( glStencilOp (GL_ZERO, GL_ZERO, GL_ZERO) );
          _cogl_rectangle_immediate (data->path_nodes_min.x,
                                     data->path_nodes_min.y,
                                     data->path_nodes_max.x,
                                     data->path_nodes_max.y);
          /* NB: The rectangle may trash the enable flags */
          _cogl_enable (enable_flags);
        }
      GE (glStencilMask (1));
      GE (glStencilFunc (GL_LEQUAL, 0x1, 0x3));
    }

  GE (glStencilOp (GL_INVERT, GL_INVERT, GL_INVERT));

  if (path->data->path_nodes->len >= 3)
    _cogl_path_fill_nodes (path);

  if (merge)
    {
      /* Now we have the new stencil buffer in bit 1 and the old
         stencil buffer in bit 0 so we need to intersect them */
      GE (glStencilMask (3));
      GE (glStencilFunc (GL_NEVER, 0x2, 0x3));
      GE (glStencilOp (GL_DECR, GL_DECR, GL_DECR));
      /* Decrement all of the bits twice so that only pixels where the
         value is 3 will remain */

      _cogl_matrix_stack_push (projection_stack);
      _cogl_matrix_stack_load_identity (projection_stack);
      _cogl_matrix_stack_flush_to_gl (projection_stack,
                                      COGL_MATRIX_PROJECTION);

      _cogl_matrix_stack_push (modelview_stack);
      _cogl_matrix_stack_load_identity (modelview_stack);
      _cogl_matrix_stack_flush_to_gl (modelview_stack,
                                      COGL_MATRIX_MODELVIEW);

      _cogl_rectangle_immediate (-1.0, -1.0, 1.0, 1.0);
      _cogl_rectangle_immediate (-1.0, -1.0, 1.0, 1.0);

      _cogl_matrix_stack_pop (modelview_stack);
      _cogl_matrix_stack_pop (projection_stack);
    }

  GE (glStencilMask (~(GLuint) 0));
  GE (glDepthMask (TRUE));
  GE (glColorMask (TRUE, TRUE, TRUE, TRUE));

  GE (glStencilFunc (GL_EQUAL, 0x1, 0x1));
  GE (glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP));

  /* restore the original pipeline */
  cogl_pop_source ();
}

void
cogl2_path_fill (CoglPath *path)
{
  g_return_if_fail (cogl_is_path (path));

  if (path->data->path_nodes->len == 0)
    return;

  _cogl_journal_flush ();

  /* NB: _cogl_framebuffer_flush_state may disrupt various state (such
   * as the pipeline state) when flushing the clip stack, so should
   * always be done first when preparing to draw. */
  _cogl_framebuffer_flush_state (_cogl_get_framebuffer (), 0);

  _cogl_path_fill_nodes (path);
}

void
cogl2_path_stroke (CoglPath *path)
{
  g_return_if_fail (cogl_is_path (path));

  if (path->data->path_nodes->len == 0)
    return;

  _cogl_path_stroke_nodes (path);
}

void
cogl2_path_move_to (CoglPath *path,
                    float x,
                    float y)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  _cogl_path_add_node (path, TRUE, x, y);

  data = path->data;

  data->path_start.x = x;
  data->path_start.y = y;

  data->path_pen = data->path_start;
}

void
cogl2_path_rel_move_to (CoglPath *path,
                        float x,
                        float y)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  data = path->data;

  cogl2_path_move_to (path,
                      data->path_pen.x + x,
                      data->path_pen.y + y);
}

void
cogl2_path_line_to (CoglPath *path,
                    float x,
                    float y)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  _cogl_path_add_node (path, FALSE, x, y);

  data = path->data;

  data->path_pen.x = x;
  data->path_pen.y = y;
}

void
cogl2_path_rel_line_to (CoglPath *path,
                        float x,
                        float y)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  data = path->data;

  cogl2_path_line_to (path,
                      data->path_pen.x + x,
                      data->path_pen.y + y);
}

void
cogl2_path_close (CoglPath *path)
{
  g_return_if_fail (cogl_is_path (path));

  _cogl_path_add_node (path, FALSE, path->data->path_start.x,
                       path->data->path_start.y);

  path->data->path_pen = path->data->path_start;
}

void
cogl2_path_line (CoglPath *path,
                 float x_1,
	         float y_1,
	         float x_2,
	         float y_2)
{
  cogl2_path_move_to (path, x_1, y_1);
  cogl2_path_line_to (path, x_2, y_2);
}

void
cogl2_path_polyline (CoglPath *path,
                     const float *coords,
                     int num_points)
{
  int c = 0;

  g_return_if_fail (cogl_is_path (path));

  cogl2_path_move_to (path, coords[0], coords[1]);

  for (c = 1; c < num_points; ++c)
    cogl2_path_line_to (path, coords[2*c], coords[2*c+1]);
}

void
cogl2_path_polygon (CoglPath *path,
                    const float *coords,
                    int num_points)
{
  cogl2_path_polyline (path, coords, num_points);
  cogl2_path_close (path);
}

void
cogl2_path_rectangle (CoglPath *path,
                      float x_1,
                      float y_1,
                      float x_2,
                      float y_2)
{
  cogl2_path_move_to (path, x_1, y_1);
  cogl2_path_line_to (path, x_2, y_1);
  cogl2_path_line_to (path, x_2, y_2);
  cogl2_path_line_to (path, x_1, y_2);
  cogl2_path_close (path);
}

static void
_cogl_path_arc (CoglPath *path,
                float center_x,
	        float center_y,
                float radius_x,
                float radius_y,
                float angle_1,
                float angle_2,
                float angle_step,
                unsigned int move_first)
{
  float a = 0x0;
  float cosa = 0x0;
  float sina = 0x0;
  float px = 0x0;
  float py = 0x0;

  /* Fix invalid angles */

  if (angle_1 == angle_2 || angle_step == 0x0)
    return;

  if (angle_step < 0x0)
    angle_step = -angle_step;

  /* Walk the arc by given step */

  a = angle_1;
  while (a != angle_2)
    {
      cosa = cosf (a * (G_PI/180.0));
      sina = sinf (a * (G_PI/180.0));

      px = center_x + (cosa * radius_x);
      py = center_y + (sina * radius_y);

      if (a == angle_1 && move_first)
	cogl2_path_move_to (path, px, py);
      else
	cogl2_path_line_to (path, px, py);

      if (G_LIKELY (angle_2 > angle_1))
        {
          a += angle_step;
          if (a > angle_2)
            a = angle_2;
        }
      else
        {
          a -= angle_step;
          if (a < angle_2)
            a = angle_2;
        }
    }

  /* Make sure the final point is drawn */

  cosa = cosf (angle_2 * (G_PI/180.0));
  sina = sinf (angle_2 * (G_PI/180.0));

  px = center_x + (cosa * radius_x);
  py = center_y + (sina * radius_y);

  cogl2_path_line_to (path, px, py);
}

void
cogl2_path_arc (CoglPath *path,
                float center_x,
                float center_y,
                float radius_x,
                float radius_y,
                float angle_1,
                float angle_2)
{
  float angle_step = 10;

  g_return_if_fail (cogl_is_path (path));

  /* it is documented that a move to is needed to create a freestanding
   * arc
   */
  _cogl_path_arc (path,
                  center_x, center_y,
	          radius_x, radius_y,
	          angle_1, angle_2,
	          angle_step, 0 /* no move */);
}


static void
_cogl_path_rel_arc (CoglPath *path,
                    float center_x,
                    float center_y,
                    float radius_x,
                    float radius_y,
                    float angle_1,
                    float angle_2,
                    float angle_step)
{
  CoglPathData *data;

  data = path->data;

  _cogl_path_arc (path,
                  data->path_pen.x + center_x,
	          data->path_pen.y + center_y,
	          radius_x, radius_y,
	          angle_1, angle_2,
	          angle_step, 0 /* no move */);
}

void
cogl2_path_ellipse (CoglPath *path,
                    float center_x,
                    float center_y,
                    float radius_x,
                    float radius_y)
{
  float angle_step = 10;

  g_return_if_fail (cogl_is_path (path));

  /* FIXME: if shows to be slow might be optimized
   * by mirroring just a quarter of it */

  _cogl_path_arc (path,
                  center_x, center_y,
	          radius_x, radius_y,
	          0, 360,
	          angle_step, 1 /* move first */);

  cogl2_path_close (path);
}

void
cogl2_path_round_rectangle (CoglPath *path,
                            float x_1,
                            float y_1,
                            float x_2,
                            float y_2,
                            float radius,
                            float arc_step)
{
  float inner_width = x_2 - x_1 - radius * 2;
  float inner_height = y_2 - y_1 - radius * 2;

  g_return_if_fail (cogl_is_path (path));

  cogl2_path_move_to (path, x_1, y_1 + radius);
  _cogl_path_rel_arc (path,
                      radius, 0,
                      radius, radius,
                      180,
                      270,
                      arc_step);

  cogl2_path_line_to (path,
                     path->data->path_pen.x + inner_width,
                     path->data->path_pen.y);
  _cogl_path_rel_arc (path,
                      0, radius,
                      radius, radius,
                      -90,
                      0,
                      arc_step);

  cogl2_path_line_to (path,
                     path->data->path_pen.x,
                     path->data->path_pen.y + inner_height);

  _cogl_path_rel_arc (path,
                      -radius, 0,
                      radius, radius,
                      0,
                      90,
                      arc_step);

  cogl2_path_line_to (path,
                     path->data->path_pen.x - inner_width,
                     path->data->path_pen.y);
  _cogl_path_rel_arc (path,
                      0, -radius,
                      radius, radius,
                      90,
                      180,
                      arc_step);

  cogl2_path_close (path);
}

static void
_cogl_path_bezier3_sub (CoglPath *path,
                        CoglBezCubic *cubic)
{
  CoglBezCubic cubics[_COGL_MAX_BEZ_RECURSE_DEPTH];
  CoglBezCubic *cleft;
  CoglBezCubic *cright;
  CoglBezCubic *c;
  floatVec2 dif1;
  floatVec2 dif2;
  floatVec2 mm;
  floatVec2 c1;
  floatVec2 c2;
  floatVec2 c3;
  floatVec2 c4;
  floatVec2 c5;
  int cindex;

  /* Put first curve on stack */
  cubics[0] = *cubic;
  cindex =  0;

  while (cindex >= 0)
    {
      c = &cubics[cindex];


      /* Calculate distance of control points from their
       * counterparts on the line between end points */
      dif1.x = (c->p2.x * 3) - (c->p1.x * 2) - c->p4.x;
      dif1.y = (c->p2.y * 3) - (c->p1.y * 2) - c->p4.y;
      dif2.x = (c->p3.x * 3) - (c->p4.x * 2) - c->p1.x;
      dif2.y = (c->p3.y * 3) - (c->p4.y * 2) - c->p1.y;

      if (dif1.x < 0)
        dif1.x = -dif1.x;
      if (dif1.y < 0)
        dif1.y = -dif1.y;
      if (dif2.x < 0)
        dif2.x = -dif2.x;
      if (dif2.y < 0)
        dif2.y = -dif2.y;


      /* Pick the greatest of two distances */
      if (dif1.x < dif2.x) dif1.x = dif2.x;
      if (dif1.y < dif2.y) dif1.y = dif2.y;

      /* Cancel if the curve is flat enough */
      if (dif1.x + dif1.y <= 1.0 ||
	  cindex == _COGL_MAX_BEZ_RECURSE_DEPTH-1)
	{
	  /* Add subdivision point (skip last) */
	  if (cindex == 0)
            return;

	  _cogl_path_add_node (path, FALSE, c->p4.x, c->p4.y);

	  --cindex;

          continue;
	}

      /* Left recursion goes on top of stack! */
      cright = c; cleft = &cubics[++cindex];

      /* Subdivide into 2 sub-curves */
      c1.x = ((c->p1.x + c->p2.x) / 2);
      c1.y = ((c->p1.y + c->p2.y) / 2);
      mm.x = ((c->p2.x + c->p3.x) / 2);
      mm.y = ((c->p2.y + c->p3.y) / 2);
      c5.x = ((c->p3.x + c->p4.x) / 2);
      c5.y = ((c->p3.y + c->p4.y) / 2);

      c2.x = ((c1.x + mm.x) / 2);
      c2.y = ((c1.y + mm.y) / 2);
      c4.x = ((mm.x + c5.x) / 2);
      c4.y = ((mm.y + c5.y) / 2);

      c3.x = ((c2.x + c4.x) / 2);
      c3.y = ((c2.y + c4.y) / 2);

      /* Add left recursion to stack */
      cleft->p1 = c->p1;
      cleft->p2 = c1;
      cleft->p3 = c2;
      cleft->p4 = c3;

      /* Add right recursion to stack */
      cright->p1 = c3;
      cright->p2 = c4;
      cright->p3 = c5;
      cright->p4 = c->p4;
    }
}

void
cogl2_path_curve_to (CoglPath *path,
                     float x_1,
                     float y_1,
                     float x_2,
                     float y_2,
                     float x_3,
                     float y_3)
{
  CoglBezCubic cubic;

  g_return_if_fail (cogl_is_path (path));

  /* Prepare cubic curve */
  cubic.p1 = path->data->path_pen;
  cubic.p2.x = x_1;
  cubic.p2.y = y_1;
  cubic.p3.x = x_2;
  cubic.p3.y = y_2;
  cubic.p4.x = x_3;
  cubic.p4.y = y_3;

  /* Run subdivision */
  _cogl_path_bezier3_sub (path, &cubic);

  /* Add last point */
  _cogl_path_add_node (path, FALSE, cubic.p4.x, cubic.p4.y);
  path->data->path_pen = cubic.p4;
}

void
cogl2_path_rel_curve_to (CoglPath *path,
                         float x_1,
                         float y_1,
                         float x_2,
                         float y_2,
                         float x_3,
                         float y_3)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  data = path->data;

  cogl2_path_curve_to (path,
                       data->path_pen.x + x_1,
                       data->path_pen.y + y_1,
                       data->path_pen.x + x_2,
                       data->path_pen.y + y_2,
                       data->path_pen.x + x_3,
                       data->path_pen.y + y_3);
}

CoglPath *
cogl2_path_new (void)
{
  CoglPath *path;
  CoglPathData *data;

  path = g_slice_new (CoglPath);
  data = path->data = g_slice_new (CoglPathData);

  data->ref_count = 1;
  data->fill_rule = COGL_PATH_FILL_RULE_EVEN_ODD;
  data->path_nodes = g_array_new (FALSE, FALSE, sizeof (CoglPathNode));
  data->last_path = 0;
  data->vbo = COGL_INVALID_HANDLE;

  return _cogl_path_object_new (path);
}

CoglPath *
cogl_path_copy (CoglPath *old_path)
{
  CoglPath *new_path;

  g_return_val_if_fail (cogl_is_path (old_path), NULL);

  new_path = g_slice_new (CoglPath);
  new_path->data = old_path->data;
  new_path->data->ref_count++;

  return _cogl_path_object_new (new_path);
}

static void
_cogl_path_free (CoglPath *path)
{
  _cogl_path_data_unref (path->data);
  g_slice_free (CoglPath, path);
}

/* If second order beziers were needed the following code could
 * be re-enabled:
 */
#if 0

static void
_cogl_path_bezier2_sub (CoglPath *path,
                        CoglBezQuad *quad)
{
  CoglBezQuad quads[_COGL_MAX_BEZ_RECURSE_DEPTH];
  CoglBezQuad *qleft;
  CoglBezQuad *qright;
  CoglBezQuad *q;
  floatVec2 mid;
  floatVec2 dif;
  floatVec2 c1;
  floatVec2 c2;
  floatVec2 c3;
  int qindex;

  /* Put first curve on stack */
  quads[0] = *quad;
  qindex =  0;

  /* While stack is not empty */
  while (qindex >= 0)
    {

      q = &quads[qindex];

      /* Calculate distance of control point from its
       * counterpart on the line between end points */
      mid.x = ((q->p1.x + q->p3.x) / 2);
      mid.y = ((q->p1.y + q->p3.y) / 2);
      dif.x = (q->p2.x - mid.x);
      dif.y = (q->p2.y - mid.y);
      if (dif.x < 0) dif.x = -dif.x;
      if (dif.y < 0) dif.y = -dif.y;

      /* Cancel if the curve is flat enough */
      if (dif.x + dif.y <= 1.0 ||
          qindex == _COGL_MAX_BEZ_RECURSE_DEPTH - 1)
	{
	  /* Add subdivision point (skip last) */
	  if (qindex == 0) return;
	  _cogl_path_add_node (path, FALSE, q->p3.x, q->p3.y);
	  --qindex; continue;
	}

      /* Left recursion goes on top of stack! */
      qright = q; qleft = &quads[++qindex];

      /* Subdivide into 2 sub-curves */
      c1.x = ((q->p1.x + q->p2.x) / 2);
      c1.y = ((q->p1.y + q->p2.y) / 2);
      c3.x = ((q->p2.x + q->p3.x) / 2);
      c3.y = ((q->p2.y + q->p3.y) / 2);
      c2.x = ((c1.x + c3.x) / 2);
      c2.y = ((c1.y + c3.y) / 2);

      /* Add left recursion onto stack */
      qleft->p1 = q->p1;
      qleft->p2 = c1;
      qleft->p3 = c2;

      /* Add right recursion onto stack */
      qright->p1 = c2;
      qright->p2 = c3;
      qright->p3 = q->p3;
    }
}

void
cogl_path_curve2_to (CoglPath *path,
                     float x_1,
                     float y_1,
                     float x_2,
                     float y_2)
{
  CoglBezQuad quad;

  /* Prepare quadratic curve */
  quad.p1 = path->data->path_pen;
  quad.p2.x = x_1;
  quad.p2.y = y_1;
  quad.p3.x = x_2;
  quad.p3.y = y_2;

  /* Run subdivision */
  _cogl_path_bezier2_sub (&quad);

  /* Add last point */
  _cogl_path_add_node (FALSE, quad.p3.x, quad.p3.y);
  path->data->path_pen = quad.p3;
}

void
cogl_rel_curve2_to (CoglPath *path,
                    float x_1,
                    float y_1,
                    float x_2,
                    float y_2)
{
  CoglPathData *data;

  g_return_if_fail (cogl_is_path (path));

  data = path->data;

  cogl_path_curve2_to (data->path_pen.x + x_1,
                       data->path_pen.y + y_1,
                       data->path_pen.x + x_2,
                       data->path_pen.y + y_2);
}

#endif

typedef struct _CoglPathTesselator CoglPathTesselator;
typedef struct _CoglPathTesselatorVertex CoglPathTesselatorVertex;

struct _CoglPathTesselator
{
  GLUtesselator *glu_tess;
  GLenum primitive_type;
  int vertex_number;
  /* Array of CoglPathTesselatorVertex. This needs to grow when the
     combine callback is called */
  GArray *vertices;
  /* Array of integers for the indices into the vertices array. Each
     element will either be guint8, guint16 or guint32 depending on
     the number of vertices */
  GArray *indices;
  CoglIndicesType indices_type;
  /* Indices used to split fans and strips */
  int index_a, index_b;
};

struct _CoglPathTesselatorVertex
{
  float x, y, s, t;
};

static void
_cogl_path_tesselator_begin (GLenum type,
                             CoglPathTesselator *tess)
{
  g_assert (type == GL_TRIANGLES ||
            type == GL_TRIANGLE_FAN ||
            type == GL_TRIANGLE_STRIP);

  tess->primitive_type = type;
  tess->vertex_number = 0;
}

static CoglIndicesType
_cogl_path_tesselator_get_indices_type_for_size (int n_vertices)
{
  if (n_vertices <= 256)
    return COGL_INDICES_TYPE_UNSIGNED_BYTE;
  else if (n_vertices <= 65536)
    return COGL_INDICES_TYPE_UNSIGNED_SHORT;
  else
    return COGL_INDICES_TYPE_UNSIGNED_INT;
}

static void
_cogl_path_tesselator_allocate_indices_array (CoglPathTesselator *tess)
{
  switch (tess->indices_type)
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE:
      tess->indices = g_array_new (FALSE, FALSE, sizeof (guint8));
      break;

    case COGL_INDICES_TYPE_UNSIGNED_SHORT:
      tess->indices = g_array_new (FALSE, FALSE, sizeof (guint16));
      break;

    case COGL_INDICES_TYPE_UNSIGNED_INT:
      tess->indices = g_array_new (FALSE, FALSE, sizeof (guint32));
      break;
    }
}

static void
_cogl_path_tesselator_add_index (CoglPathTesselator *tess, int vertex_index)
{
  switch (tess->indices_type)
    {
    case COGL_INDICES_TYPE_UNSIGNED_BYTE:
      {
        guint8 val = vertex_index;
        g_array_append_val (tess->indices, val);
      }
      break;

    case COGL_INDICES_TYPE_UNSIGNED_SHORT:
      {
        guint16 val = vertex_index;
        g_array_append_val (tess->indices, val);
      }
      break;

    case COGL_INDICES_TYPE_UNSIGNED_INT:
      {
        guint32 val = vertex_index;
        g_array_append_val (tess->indices, val);
      }
      break;
    }
}

static void
_cogl_path_tesselator_vertex (void *vertex_data,
                              CoglPathTesselator *tess)
{
  int vertex_index;

  vertex_index = GPOINTER_TO_INT (vertex_data);

  /* This tries to convert all of the primitives into GL_TRIANGLES
     with indices to share vertices */
  switch (tess->primitive_type)
    {
    case GL_TRIANGLES:
      /* Directly use the vertex */
      _cogl_path_tesselator_add_index (tess, vertex_index);
      break;

    case GL_TRIANGLE_FAN:
      if (tess->vertex_number == 0)
        tess->index_a = vertex_index;
      else if (tess->vertex_number == 1)
        tess->index_b = vertex_index;
      else
        {
          /* Create a triangle with the first vertex, the previous
             vertex and this vertex */
          _cogl_path_tesselator_add_index (tess, tess->index_a);
          _cogl_path_tesselator_add_index (tess, tess->index_b);
          _cogl_path_tesselator_add_index (tess, vertex_index);
          /* Next time we will use this vertex as the previous
             vertex */
          tess->index_b = vertex_index;
        }
      break;

    case GL_TRIANGLE_STRIP:
      if (tess->vertex_number == 0)
        tess->index_a = vertex_index;
      else if (tess->vertex_number == 1)
        tess->index_b = vertex_index;
      else
        {
          _cogl_path_tesselator_add_index (tess, tess->index_a);
          _cogl_path_tesselator_add_index (tess, tess->index_b);
          _cogl_path_tesselator_add_index (tess, vertex_index);
          if (tess->vertex_number & 1)
            tess->index_b = vertex_index;
          else
            tess->index_a = vertex_index;
        }
      break;

    default:
      g_assert_not_reached ();
    }

  tess->vertex_number++;
}

static void
_cogl_path_tesselator_end (CoglPathTesselator *tess)
{
  tess->primitive_type = GL_FALSE;
}

static void
_cogl_path_tesselator_combine (double coords[3],
                               void *vertex_data[4],
                               float weight[4],
                               void **out_data,
                               CoglPathTesselator *tess)
{
  CoglPathTesselatorVertex *vertex;
  CoglIndicesType new_indices_type;
  int i;

  /* Add a new vertex to the array */
  g_array_set_size (tess->vertices, tess->vertices->len + 1);
  vertex = &g_array_index (tess->vertices,
                           CoglPathTesselatorVertex,
                           tess->vertices->len - 1);
  /* The data is just the index to the vertex */
  *out_data = GINT_TO_POINTER (tess->vertices->len - 1);
  /* Set the coordinates of the new vertex */
  vertex->x = coords[0];
  vertex->y = coords[1];
  /* Generate the texture coordinates as the weighted average of the
     four incoming coordinates */
  vertex->s = 0.0f;
  vertex->t = 0.0f;
  for (i = 0; i < 4; i++)
    {
      CoglPathTesselatorVertex *old_vertex =
        &g_array_index (tess->vertices, CoglPathTesselatorVertex,
                        GPOINTER_TO_INT (vertex_data[i]));
      vertex->s += old_vertex->s * weight[i];
      vertex->t += old_vertex->t * weight[i];
    }

  /* Check if we've reached the limit for the data type of our indices */
  new_indices_type =
    _cogl_path_tesselator_get_indices_type_for_size (tess->vertices->len);
  if (new_indices_type != tess->indices_type)
    {
      CoglIndicesType old_indices_type = new_indices_type;
      GArray *old_vertices = tess->indices;

      /* Copy the indices to an array of the new type */
      tess->indices_type = new_indices_type;
      _cogl_path_tesselator_allocate_indices_array (tess);

      switch (old_indices_type)
        {
        case COGL_INDICES_TYPE_UNSIGNED_BYTE:
          for (i = 0; i < old_vertices->len; i++)
            _cogl_path_tesselator_add_index (tess,
                                             g_array_index (old_vertices,
                                                            guint8, i));
          break;

        case COGL_INDICES_TYPE_UNSIGNED_SHORT:
          for (i = 0; i < old_vertices->len; i++)
            _cogl_path_tesselator_add_index (tess,
                                             g_array_index (old_vertices,
                                                            guint16, i));
          break;

        case COGL_INDICES_TYPE_UNSIGNED_INT:
          for (i = 0; i < old_vertices->len; i++)
            _cogl_path_tesselator_add_index (tess,
                                             g_array_index (old_vertices,
                                                            guint32, i));
          break;
        }

      g_array_free (old_vertices, TRUE);
    }
}

static void
_cogl_path_build_vbo (CoglPath *path)
{
  CoglPathTesselator tess;
  unsigned int path_start = 0;
  CoglPathData *data = path->data;
  int i;

  /* If we've already got a vbo then we don't need to do anything */
  if (data->vbo)
    return;

  tess.primitive_type = FALSE;

  /* Generate a vertex for each point on the path */
  tess.vertices = g_array_new (FALSE, FALSE, sizeof (CoglPathTesselatorVertex));
  g_array_set_size (tess.vertices, data->path_nodes->len);
  for (i = 0; i < data->path_nodes->len; i++)
    {
      CoglPathNode *node =
        &g_array_index (data->path_nodes, CoglPathNode, i);
      CoglPathTesselatorVertex *vertex =
        &g_array_index (tess.vertices, CoglPathTesselatorVertex, i);

      vertex->x = node->x;
      vertex->y = node->y;

      /* Add texture coordinates so that a texture would be drawn to
         fit the bounding box of the path and then cropped by the
         path */
      if (data->path_nodes_min.x == data->path_nodes_max.x)
        vertex->s = 0.0f;
      else
        vertex->s = ((node->x - data->path_nodes_min.x)
                     / (data->path_nodes_max.x - data->path_nodes_min.x));
      if (data->path_nodes_min.y == data->path_nodes_max.y)
        vertex->t = 0.0f;
      else
        vertex->t = ((node->y - data->path_nodes_min.y)
                     / (data->path_nodes_max.y - data->path_nodes_min.y));
    }

  tess.indices_type =
    _cogl_path_tesselator_get_indices_type_for_size (data->path_nodes->len);
  _cogl_path_tesselator_allocate_indices_array (&tess);

  tess.glu_tess = gluNewTess ();

  if (data->fill_rule == COGL_PATH_FILL_RULE_EVEN_ODD)
    gluTessProperty (tess.glu_tess, GLU_TESS_WINDING_RULE,
                     GLU_TESS_WINDING_ODD);
  else
    gluTessProperty (tess.glu_tess, GLU_TESS_WINDING_RULE,
                     GLU_TESS_WINDING_NONZERO);

  /* All vertices are on the xy-plane */
  gluTessNormal (tess.glu_tess, 0.0, 0.0, 1.0);

  gluTessCallback (tess.glu_tess, GLU_TESS_BEGIN_DATA,
                   _cogl_path_tesselator_begin);
  gluTessCallback (tess.glu_tess, GLU_TESS_VERTEX_DATA,
                   _cogl_path_tesselator_vertex);
  gluTessCallback (tess.glu_tess, GLU_TESS_END_DATA,
                   _cogl_path_tesselator_end);
  gluTessCallback (tess.glu_tess, GLU_TESS_COMBINE_DATA,
                   _cogl_path_tesselator_combine);

  gluTessBeginPolygon (tess.glu_tess, &tess);

  while (path_start < data->path_nodes->len)
    {
      CoglPathNode *node =
        &g_array_index (data->path_nodes, CoglPathNode, path_start);

      gluTessBeginContour (tess.glu_tess);

      for (i = 0; i < node->path_size; i++)
        {
          double vertex[3] = { node[i].x, node[i].y, 0.0 };
          gluTessVertex (tess.glu_tess, vertex,
                         GINT_TO_POINTER (i + path_start));
        }

      gluTessEndContour (tess.glu_tess);

      path_start += node->path_size;
    }

  gluTessEndPolygon (tess.glu_tess);

  gluDeleteTess (tess.glu_tess);

  data->vbo = cogl_vertex_array_new (sizeof (CoglPathTesselatorVertex) *
                                     tess.vertices->len,
                                     tess.vertices->data);
  g_array_free (tess.vertices, TRUE);

  data->vbo_attributes[0] =
    cogl_vertex_attribute_new (data->vbo,
                               "cogl_position_in",
                               sizeof (CoglPathTesselatorVertex),
                               G_STRUCT_OFFSET (CoglPathTesselatorVertex, x),
                               2, /* n_components */
                               COGL_VERTEX_ATTRIBUTE_TYPE_FLOAT);
  data->vbo_attributes[1] =
    cogl_vertex_attribute_new (data->vbo,
                               "cogl_tex_coord0_in",
                               sizeof (CoglPathTesselatorVertex),
                               G_STRUCT_OFFSET (CoglPathTesselatorVertex, s),
                               2, /* n_components */
                               COGL_VERTEX_ATTRIBUTE_TYPE_FLOAT);
  /* NULL terminator */
  data->vbo_attributes[2] = NULL;

  data->vbo_indices = cogl_indices_new (tess.indices_type,
                                        tess.indices->data,
                                        tess.indices->len);
  data->vbo_n_indices = tess.indices->len;
  g_array_free (tess.indices, TRUE);
}
