#ifndef NOUVEAU_CONTEXT
#define NOUVEAU_CONTEXT 1

#include "nouveau_private.h"

struct nouveau_ws_device;

struct nouveau_ws_context {
   struct nouveau_object *channel;

   struct nouveau_object *eng2d;
   struct nouveau_object *m2mf;
   struct nouveau_object *compute;
};

int nouveau_ws_context_create(struct nouveau_ws_device *, struct nouveau_ws_context **out);
void nouveau_ws_context_destroy(struct nouveau_ws_context *);

#endif
