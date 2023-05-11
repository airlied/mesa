#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "draw/draw_pt.h"
#include "draw/draw_mesh.h"
struct mesh_pipeline_middle_end {
   struct draw_pt_middle_end base;
   struct draw_context *draw;

   struct pt_so_emit *so_emit;
   struct pt_post_vs *post_vs;
};

/** cast wrapper */
static inline struct mesh_pipeline_middle_end *
mesh_pipeline_middle_end(struct draw_pt_middle_end *middle)
{
   return (struct mesh_pipeline_middle_end *) middle;
}

static void
mesh_pipeline_destroy(struct draw_pt_middle_end *middle)
{
   struct mesh_pipeline_middle_end *mpme = mesh_pipeline_middle_end(middle);

   if (mpme->so_emit)
      draw_pt_so_emit_destroy(mpme->so_emit);
   if (mpme->post_vs)
      draw_pt_post_vs_destroy(mpme->post_vs);
   FREE(middle);
}

static void
mesh_middle_end_prepare(struct draw_pt_middle_end *middle,
                        enum pipe_prim_type prim,
                        unsigned opt,
                        unsigned *max_vertices)
{
   struct mesh_pipeline_middle_end *mpme = mesh_pipeline_middle_end(middle);
   struct draw_context *draw = mpme->draw;
   enum pipe_prim_type out_prim = draw->ms.mesh_shader->output_primitive;
   unsigned point_clip = draw->rasterizer->fill_front == PIPE_POLYGON_MODE_POINT ||
                         out_prim == PIPE_PRIM_POINTS;
   draw_pt_post_vs_prepare(mpme->post_vs,
                           draw->clip_xy,
                           draw->clip_z,
                           draw->clip_user,
                           point_clip ? draw->guard_band_points_xy :
                                        draw->guard_band_xy,
                           draw->bypass_viewport,
                           draw->rasterizer->clip_halfz,
                           FALSE);

   draw_pt_so_emit_prepare(mpme->so_emit, false);
}

void
draw_mesh_middle_end_run(struct draw_pt_middle_end *middle,
                         struct draw_vertex_info *vert_info,
                         struct draw_prim_info *prim_info)
{
   struct mesh_pipeline_middle_end *mpme = mesh_pipeline_middle_end(middle);

   boolean clipped = draw_pt_post_vs_run(mpme->post_vs, vert_info, prim_info);

   /* just for primgen query */
   draw_pt_so_emit(mpme->so_emit, 1, vert_info, prim_info);
   draw_pipeline_run_linear(mpme->draw, vert_info, prim_info);
}

struct draw_pt_middle_end *
draw_pt_mesh_pipeline_or_emit(struct draw_context *draw)
{
   struct mesh_pipeline_middle_end *mpme =
      CALLOC_STRUCT(mesh_pipeline_middle_end);
   if (!mpme)
      goto fail;

   mpme->base.prepare = mesh_middle_end_prepare;
//   mpme->base.run = mesh_middle_end_run;
   mpme->base.destroy = mesh_pipeline_destroy;
   mpme->draw = draw;

   mpme->post_vs = draw_pt_post_vs_create(draw);
   if (!mpme->post_vs)
      goto fail;

   mpme->so_emit = draw_pt_so_emit_create(draw);
   if (!mpme->so_emit)
      goto fail;

   return &mpme->base;
 fail:
   if (mpme)
      mesh_pipeline_destroy(&mpme->base);

   return NULL;
}
