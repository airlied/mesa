nvk_files = files(
  'nvk_bo_sync.c',
  'nvk_bo_sync.h',
  'nvk_buffer.c',
  'nvk_buffer.h',
  'nvk_cmd_blit.c',
  'nvk_cmd_buffer.c',
  'nvk_cmd_buffer.h',
  'nvk_cmd_copy.c',
  'nvk_compute_pipeline.c',
  'nvk_descriptor_set.h',
  'nvk_descriptor_set.c',
  'nvk_descriptor_set_layout.c',
  'nvk_descriptor_set_layout.h',
  'nvk_device.c',
  'nvk_device.h',
  'nvk_device_memory.c',
  'nvk_device_memory.h',
  'nvk_format.c',
  'nvk_format.h',
  'nvk_image.c',
  'nvk_image.h',
  'nvk_image_view.c',
  'nvk_image_view.h',
  'nvk_instance.c',
  'nvk_instance.h',
  'nvk_nir.h',
  'nvk_nir_lower_descriptors.c',
  'nvk_physical_device.c',
  'nvk_physical_device.h',
  'nvk_pipeline.c',
  'nvk_pipeline.h',
  'nvk_pipeline_layout.c',
  'nvk_pipeline_layout.h',
  'nvk_private.h',
  'nvk_sampler.c',
  'nvk_sampler.h',
  'nvk_shader.c',
  'nvk_shader.h',
  'nvk_wsi.c',
  'nvk_wsi.h'
)

nouveau_icd = custom_target(
  'nouveau_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'nouveau_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.3', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_nouveau.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

nvk_entrypoints = custom_target(
  'nvk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['nvk_entrypoints.h', 'nvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'nvk',
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

nvk_deps = [
  dep_libdrm,
  idep_nir,
  idep_nouveau_ws,
  idep_nvidia_headers,
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
  idep_vulkan_wsi_headers,
]

libvulkan_nouveau = shared_library(
  'vulkan_nouveau',
  [
    nvk_entrypoints,
    cl_generated,
    cl_nv_generated,
    nvk_files,
  ],
  include_directories : [
    inc_gallium,
    inc_include,
    inc_src,
  ],
  link_with : [libnouveau_codegen, libnouveau_codegen_nogal],
  dependencies : [nvk_deps, idep_nouveau_codegen],
  gnu_symbol_visibility : 'hidden',
  install : true,
)
