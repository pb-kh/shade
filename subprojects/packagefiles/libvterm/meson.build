# Meson build for libvterm (MIT, https://www.leonerd.org.uk/code/libvterm/).
# The release tarball ships the generated encoding tables, so no Perl step.
project('libvterm', 'c', version: '0.3.3', license: 'MIT', default_options: ['c_std=c99', 'warning_level=0'])

vterm_inc = include_directories('include')
vterm_lib = static_library('vterm',
  files('src/encoding.c', 'src/keyboard.c', 'src/mouse.c', 'src/parser.c', 'src/pen.c',
        'src/screen.c', 'src/state.c', 'src/unicode.c', 'src/vterm.c'),
  include_directories: [vterm_inc, include_directories('src')],
  c_args: ['-D_XOPEN_SOURCE=500', '-D_DEFAULT_SOURCE'],
  pic: true,
)
vterm_dep = declare_dependency(link_with: vterm_lib, include_directories: vterm_inc)
meson.override_dependency('vterm', vterm_dep)
