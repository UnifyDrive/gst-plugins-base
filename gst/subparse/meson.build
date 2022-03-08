subparse_sources = [
  'gstzsassparse.c',
  'gstssaparse.c',
  'gstsubparse.c',
  'samiparse.c',
  'tmplayerparse.c',
  'mpl2parse.c',
  'qttextparse.c',
]

libass_dep = dependency('libass', version: '>= 0.14.0')
gstsubparse = library('gstsubparse',
  subparse_sources,
  c_args : gst_plugins_base_args,
  include_directories: [configinc, libsinc],
  dependencies : [gst_base_dep, libass_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstsubparse, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstsubparse]
