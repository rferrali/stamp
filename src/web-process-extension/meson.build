stamp_web_process_extension_sources = [
  'extension.c'
]

stamp_web_process_extension_deps = [
  dependency('webkitgtk-6.0'),
  dependency('webkitgtk-web-process-extension-6.0')
]

stamp_web_process_extension = shared_module('stamp-web-process-extension',
  stamp_web_process_extension_sources,
  dependencies: stamp_web_process_extension_deps,
  install: true,
  install_dir: webprocessextensionsdir,
  install_rpath: pkglibdir
)
