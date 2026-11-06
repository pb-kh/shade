media_moc = qt6.preprocess(
  moc_headers: files('plugin.h', 'pane.h', 'mpris.h', 'art.h'),
  include_directories: [inc_public],
  dependencies: [qt_gui, qt_dbus, qt_net],
)
media_res = qt6.compile_resources(sources: files('media.qrc'), name: 'media')
shared_module(
  'media',
  files('plugin.cpp', 'pane.cpp', 'mpris.cpp', 'art.cpp') + media_moc + media_res,
  include_directories: [inc_public],
  dependencies: [sdk_dep, qt_gui, qt_dbus, qt_net],
  name_prefix: '20-',
  install: true,
  install_dir: plugin_dir,
)
