#ifndef DRAW_MESH_PRIM_H
#define DRAW_MESH_PRIM_H

struct draw_context;
struct draw_prim_info;
struct draw_vertex_info;

void
draw_mesh_prim_run(struct draw_context *draw,
                   unsigned num_per_prim_inputs,
                   void *per_prim_inputs,
                   const struct draw_prim_info *in_prim_info,
                   const struct draw_vertex_info *in_vert_info,
                   struct draw_prim_info *out_prim_info,
                   struct draw_vertex_info *out_vert_info);
#endif
