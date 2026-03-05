d3d9_res = wrc_generator.process('version.rc')

d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp'
])

d3d9_src = [
  'd3d9_main.cpp',
  'd3d9_interface.cpp',
  'd3d9_vr.cpp',
  'd3d9_adapter.cpp',
  'd3d9_monitor.cpp',
  'd3d9_device.cpp',
  'd3d9_state.cpp',
  'd3d9_cursor.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_format.cpp',
  'd3d9_common_texture.cpp',
  'd3d9_constant_buffer.cpp',
  'd3d9_texture.cpp',
  'd3d9_surface.cpp',
  'd3d9_volume.cpp',
  'd3d9_common_buffer.cpp',
  'd3d9_buffer.cpp',
  'd3d9_shader.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_query.cpp',
  'd3d9_shader_validator.cpp',
  'd3d9_multithread.cpp',
  'd3d9_options.cpp',
  'd3d9_stateblock.cpp',
  'd3d9_util.cpp',
  'd3d9_initializer.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_names.cpp',
  'd3d9_swvp_emu.cpp',
  'd3d9_format_helpers.cpp',
  'd3d9_hud.cpp',
  'd3d9_annotation.cpp',
  'd3d9_mem.cpp',
  'd3d9_window.cpp',
  'd3d9_interop.cpp',
  'd3d9_on_12.cpp',
  'd3d9_bridge.cpp'
]

d3d9_ld_args      = []
d3d9_link_depends = []
d3d9_inc_dirs = [ dxvk_include_path ]
d3d9_extra_deps = []

if fs.is_dir('../../../L4D2VR')
  d3d9_inc_dirs += [ include_directories('../../../') ]
endif
if fs.is_dir('../../../L4D2VR/sdk')
  d3d9_inc_dirs += [ include_directories('../../../L4D2VR/sdk') ]
endif

if platform == 'windows'
  if fs.is_dir('../../../thirdparty/openvr/include')
    d3d9_inc_dirs += [ include_directories('../../../thirdparty/openvr/include') ]
  endif

  openvr_lib_dir = ''
  if cpu_family == 'x86' and fs.is_dir('../../../thirdparty/openvr/lib/win32')
    openvr_lib_dir = '../../../thirdparty/openvr/lib/win32'
  elif cpu_family == 'x86_64' and fs.is_dir('../../../thirdparty/openvr/lib/win64')
    openvr_lib_dir = '../../../thirdparty/openvr/lib/win64'
  endif

  if openvr_lib_dir != ''
    openvr_lib = cpp.find_library('openvr_api', dirs: [ openvr_lib_dir ], required: false)
    if openvr_lib.found()
      d3d9_extra_deps += [ openvr_lib ]
    endif
  endif
endif

if platform != 'windows'
  d3d9_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d9.sym') ]
  d3d9_link_depends += files('d3d9.sym')
endif

d3d9_dll = shared_library(dxvk_name_prefix+'d3d9', d3d9_src, glsl_generator.process(d3d9_shaders), d3d9_res,
  dependencies        : [ dxso_dep, dxvk_dep ] + d3d9_extra_deps,
  include_directories : d3d9_inc_dirs,
  install             : true,
  vs_module_defs      : 'd3d9'+def_spec_ext,
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : d3d9_inc_dirs,
)

if platform != 'windows'
  pkg.generate(d3d9_dll,
    filebase: dxvk_pkg_prefix + 'd3d9',
    subdirs:  'dxvk',
  )
endif
