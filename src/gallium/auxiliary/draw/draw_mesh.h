#ifndef DRAW_MESH_H
#define DRAW_MESH_H

#include "draw_context.h"
#include "tgsi/tgsi_exec.h"
#include "draw_private.h"

struct draw_context;

struct draw_mesh_shader {
   struct draw_context *draw;

   struct tgsi_shader_info info;

   unsigned position_output;
   unsigned viewport_index_output;
   unsigned clipvertex_output;
   unsigned ccdistance_output[PIPE_MAX_CLIP_OR_CULL_DISTANCE_ELEMENT_COUNT];
   unsigned output_primitive;
};

#endif
