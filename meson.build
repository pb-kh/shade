project(
  'shade',
  'cpp',
  version: '0.2.0',
  license: 'MIT',
  meson_version: '>= 1.1',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_lundef=false',
  ],
)

qt6 = import('qt6')
fs = import('fs')

qt_core = dependency('qt6', modules: ['Core'])
qt_gui = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'])
qt_net = dependency('qt6', modules: ['Core', 'Network'])
qt_dbus = dependency('qt6', modules: ['Core', 'DBus'])
toml_dep = dependency('tomlplusplus', fallback: ['tomlplusplus', 'tomlplusplus_dep'], default_options: ['compile_library=false']).partial_dependency(includes: true)

layer_shell_dep = dependency(
  'LayerShellQt',
  method: 'cmake',
  modules: ['LayerShellQt::Interface'],
  required: get_option('layer_shell'),
)

plugin_dir = get_option('prefix') / get_option('libdir') / 'shade' / 'plugins'
api_args = ['-DSHADE_SYSTEM_PLUGIN_DIR="' + plugin_dir + '"']

inc_public = include_directories('include')
inc_src = include_directories('src')

# --- SDK (public, plugins link against it) ---------------------------------
sdk_moc = qt6.preprocess(
  moc_headers: files('include/shade/pane.h', 'include/shade/icon.h'),
  include_directories: inc_public,
  dependencies: qt_gui,
)
sdk_lib = shared_library(
  'shade-sdk',
  files('sdk/sdk.cpp', 'sdk/icon.cpp') + sdk_moc,
  include_directories: inc_public,
  cpp_args: ['-DSHADE_SDK_BUILD'],
  dependencies: qt_gui,
  version: '1.0.0',
  install: true,
)
sdk_dep = declare_dependency(
  link_with: sdk_lib,
  include_directories: inc_public,
  dependencies: qt_gui,
)
import('pkgconfig').generate(sdk_lib,
  name: 'shade-sdk',
  description: 'SDK for shade plugins (Qt 6 Widgets)',
  requires: ['Qt6Widgets'],
  variables: ['plugindir=${libdir}/shade/plugins'],
)
install_headers(
  files('include/shade/sdk.h', 'include/shade/pane.h', 'include/shade/icon.h', 'include/shade/theme.h'),
  subdir: 'shade',
)

# --- base (QtCore only: shared with shadectl) ------------------------------
base_lib = static_library(
  'shade-base',
  files('src/base/paths.cpp', 'src/base/pidlock.cpp', 'src/base/pluginscan.cpp'),
  include_directories: [inc_src],
  cpp_args: api_args,
  dependencies: qt_core,
  pic: true,
)
base_dep = declare_dependency(link_with: base_lib, include_directories: inc_src, dependencies: qt_core)

# --- core (headless host logic) --------------------------------------------
core_moc = qt6.preprocess(
  moc_headers: files('src/core/model.h', 'src/core/signalbus.h', 'src/core/control.h'),
  include_directories: [inc_public, inc_src],
  dependencies: [qt_gui],
)
core_lib = static_library(
  'shade-core',
  files(
    'src/core/model.cpp',
    'src/core/signalbus.cpp',
    'src/core/config.cpp',
    'src/core/control.cpp',
  ) + core_moc,
  include_directories: [inc_public, inc_src],
  dependencies: [sdk_dep, qt_net, toml_dep, base_dep],
  pic: true,
)
core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: [inc_src],
  dependencies: [sdk_dep, qt_net, base_dep, toml_dep],
)

# --- term (embedded virtual terminals; libvterm) ---------------------------
vterm_dep = dependency('vterm', version: '>=0.3', required: get_option('terminal'),
  fallback: ['libvterm', 'vterm_dep'])
term_dep = disabler()
if vterm_dep.found()
  term_moc = qt6.preprocess(moc_headers: files('src/term/terminal.h'), include_directories: [inc_src], dependencies: qt_gui)
  term_lib = static_library('shade-term', files('src/term/terminal.cpp') + term_moc,
    include_directories: [inc_public, inc_src],
    dependencies: [sdk_dep, vterm_dep],
    pic: true,
  )
  term_dep = declare_dependency(link_with: term_lib, include_directories: inc_src,
    dependencies: [sdk_dep, vterm_dep], compile_args: ['-DSHADE_HAVE_TERMINAL'])
endif

subdir('src/app')
subdir('shadectl')
subdir('plugins')
if get_option('tests')
  subdir('tests')
endif

install_data('data/config.example.toml', install_dir: get_option('datadir') / 'shade')
