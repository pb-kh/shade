stats_moc = qt6.preprocess(
  moc_headers: files('plugin.h', 'pane.h', 'sources.h'),
  include_directories: [inc_public],
  dependencies: [qt_gui, qt_dbus],
)
stats_res = qt6.compile_resources(sources: files('stats.qrc'), name: 'stats')
shared_module(
  'stats',
  files('plugin.cpp', 'pane.cpp', 'sources.cpp') + stats_moc + stats_res,
  include_directories: [inc_public],
  dependencies: [sdk_dep, qt_gui, qt_dbus],
  name_prefix: '10-',
  install: true,
  install_dir: plugin_dir,
)
