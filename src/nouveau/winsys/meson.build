libnouveau_ws = static_library(
  'nouveau_ws',
  [
    'nouveau_bo.h',
    'nouveau_bo.c',
    'nouveau_context.h',
    'nouveau_context.c',
    'nouveau_device.h',
    'nouveau_device.c',
    'nouveau_private.h',
    'nouveau_push.h',
    'nouveau_push.c',
  ],
  include_directories : [
    inc_include,
    inc_src,
  ],
  dependencies : [
    idep_nvidia_headers,
    dep_libdrm_nouveau,
    idep_nvidia_headers,
  ],
  gnu_symbol_visibility : 'hidden',
)

idep_nouveau_ws = declare_dependency(
  link_with: libnouveau_ws,
  include_directories : include_directories('.')
)
