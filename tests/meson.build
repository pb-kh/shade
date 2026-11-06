qt_test = dependency('qt6', modules: ['Core', 'Test', 'Widgets', 'Gui', 'Network'])

test_env = environment()
test_env.set('QT_QPA_PLATFORM', 'offscreen')

icon_dirs = ':'.join([
  meson.project_source_root() / 'plugins/stats/icons',
  meson.project_source_root() / 'plugins/media/icons',
  meson.project_source_root() / 'plugins/notifications/icons',
])

unit_tests = {
  'model': files('test_model.cpp'),
  'bus': files('test_bus.cpp'),
  'config': files('test_config.cpp', 'test_data.cpp'),
  'icon': files('test_icon.cpp', 'test_data.cpp'),
  'base': files('test_base.cpp'),
}

foreach name, src : unit_tests
  moc = qt6.preprocess(moc_sources: src[0], include_directories: [inc_src, inc_public], dependencies: qt_test)
  exe = executable('test_' + name, src + moc,
    include_directories: [inc_src, inc_public],
    cpp_args: ['-DSHADE_ICON_DIRS="' + icon_dirs + '"',
               '-DSHADE_EXAMPLE_CONFIG="' + meson.project_source_root() / 'data/config.example.toml' + '"'],
    dependencies: [core_dep, qt_test],
  )
  test(name, exe, env: test_env, suite: 'unit', timeout: 60)
endforeach

# Plugin internals (no D-Bus services needed).
plugin_test_moc = qt6.preprocess(
  moc_sources: files('test_plugins.cpp'),
  moc_headers: files('../plugins/media/art.h', '../plugins/notifications/model.h', '../plugins/stats/sources.h'),
  include_directories: [inc_public],
  dependencies: [qt_test, qt_dbus],
)
test('plugins', executable('test_plugins',
    files('test_plugins.cpp', '../plugins/media/art.cpp', '../plugins/notifications/model.cpp', '../plugins/stats/sources.cpp') + plugin_test_moc,
    include_directories: [inc_public],
    cpp_args: ['-DSHADE_FAKE_PACTL="' + meson.current_source_dir() / 'integration/fake-pactl' + '"'],
    dependencies: [qt_test, qt_dbus, qt_gui, qt_net],
  ),
  env: test_env, suite: 'unit', timeout: 60)

# End-to-end: mock services on a private session bus drive a real shade.
dbus_run_session = find_program('dbus-run-session', required: false)
python3 = find_program('python3', required: false)
if dbus_run_session.found() and python3.found()
  it_env = environment()
  it_env.set('BUILD', meson.project_build_root())
  test('integration', dbus_run_session,
    args: ['--', 'bash', meson.current_source_dir() / 'integration/run.sh'],
    env: it_env, suite: 'integration', timeout: 240, is_parallel: false,
    depends: [shade_exe, shadectl_exe])
endif

if vterm_dep.found()
  term_test_moc = qt6.preprocess(moc_sources: files('test_terminal.cpp'), include_directories: [inc_src, inc_public], dependencies: qt_test)
  test('terminal', executable('test_terminal', files('test_terminal.cpp') + term_test_moc,
      include_directories: [inc_src, inc_public],
      dependencies: [term_dep, qt_test],
    ),
    env: test_env, suite: 'unit', timeout: 60)
endif
