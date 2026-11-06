app_moc = qt6.preprocess(
  moc_headers: files('shadewindow.h', 'host.h'),
  include_directories: [inc_public, inc_src],
  dependencies: qt_gui,
)

app_args = ['-DSHADE_VERSION="' + meson.project_version() + '"']
app_deps = [core_dep, qt_gui]
app_sources = files('main.cpp', 'shadewindow.cpp', 'host.cpp')
if vterm_dep.found()
  app_deps += [term_dep]
  app_sources += files('terminals.cpp')
else
  app_sources += files('terminals_stub.cpp')
endif
if layer_shell_dep.found()
  app_args += ['-DSHADE_HAVE_LAYER_SHELL']
  app_deps += [layer_shell_dep]
endif

shade_exe = executable(
  'shade',
  app_sources + app_moc,
  include_directories: [inc_public, inc_src],
  cpp_args: app_args,
  dependencies: app_deps,
  install: true,
  install_rpath: get_option('prefix') / get_option('libdir'),
)
