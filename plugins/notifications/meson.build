notif_moc = qt6.preprocess(
  moc_headers: files('plugin.h', 'pane.h', 'model.h', 'server.h'),
  include_directories: [inc_public],
  dependencies: [qt_gui, qt_dbus],
)
shared_module(
  'notifications',
  files('plugin.cpp', 'pane.cpp', 'model.cpp', 'server.cpp') + notif_moc,
  include_directories: [inc_public],
  dependencies: [sdk_dep, qt_gui, qt_dbus],
  name_prefix: '30-',
  install: true,
  install_dir: plugin_dir,
)
