project(
	'libdisplay-info',
	'c',
	version: '0.0.0',
	license: 'MIT',
	meson_version: '>= 0.55.0',
	default_options: [
		'c_std=c11',
		'warning_level=3',
	],
)

pnp_ids = files('pnp.ids')

gen_search_table = find_program('tool/gen-search-table.py')
pnp_id_table = custom_target(
	'pnp-id-table.c',
	command: [ gen_search_table, pnp_ids, '@OUTPUT@', 'pnp_id_table' ],
	output: 'pnp-id-table.c',
)

cc = meson.get_compiler('c')

math = cc.find_library('m', required: false)

if cc.get_id() != 'msvc'
	add_project_arguments(['-D_POSIX_C_SOURCE=200809L'], language: 'c')
	add_project_arguments(['-Dstatic_array=static'], language: 'c')
else
	add_project_arguments(['-Dstatic_array='], language: 'c')
	add_project_arguments(['-Dssize_t=intptr_t'], language: 'c')
endif

add_project_arguments(cc.get_supported_arguments([
	'-Wundef',
	'-Wmissing-prototypes',
	'-Walloca',
	'-Wdeclaration-after-statement',
	'-Wconversion',

	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',

	'-Werror=implicit',
]), language: 'c')

symbols_file = 'libdisplay-info.map'
symbols_flag = '-Wl,--version-script,@0@'.format(meson.current_source_dir() / symbols_file)

di_lib = static_library(
	'display-info',
	[
		'cta.c',
		'displayid.c',
		'dmt-table.c',
		'edid.c',
		'gtf.c',
		'info.c',
		'log.c',
		'memory-stream.c',
		pnp_id_table,
	],
	include_directories: include_directories('include'),
	dependencies: [math],
	link_args: symbols_flag,
	link_depends: symbols_file,
	install: false,
)

di_dep = declare_dependency(
	link_with: di_lib,
	include_directories: include_directories('include'),
)

#di_edid_decode = executable(
#	'di-edid-decode',
#	[
#		'di-edid-decode/cta.c',
#		'di-edid-decode/displayid.c',
#		'di-edid-decode/edid.c',
#		'di-edid-decode/main.c',
#	],
#	dependencies: [di_dep, math],
#	install: true,
#)
#
#subdir('test')
