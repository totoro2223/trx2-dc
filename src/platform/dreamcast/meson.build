# Dreamcast (KallistiOS) platform layer.
#
# The desktop build relies on FFmpeg, GLEW/OpenGL 3 and SDL2, none of which
# exist on the Dreamcast. Sources that use them directly are left out of the
# build and replaced by the implementations in this directory. SDL2 headers
# are provided by a thin compatibility layer on top of KallistiOS so that the
# rest of the engine can stay untouched.

dc_deps_dir = meson.get_external_property('dc_deps_dir')
dc_kos_ports_dir = meson.get_external_property('dc_kos_ports_dir')

dc_lib_dirs = [dc_deps_dir / 'lib', dc_kos_ports_dir / 'lib']

dc_dependencies = [
  c_compiler.find_library('GL', dirs: dc_lib_dirs),
  c_compiler.find_library('lua', dirs: dc_lib_dirs),
  c_compiler.find_library('pcre2-8', dirs: dc_lib_dirs),
  c_compiler.find_library('z', dirs: dc_lib_dirs),
  c_compiler.find_library('m', required: false),
]

dc_include_directories = include_directories(
  'include',
  is_system: true,
)

# Path prefixes (relative to src/) of sources not built on the Dreamcast.
dc_excluded_sources = [
  'trx/av/',
  'trx/gl/',
  'trx/game/output/',
]

dc_sources = files(
  'av_stub.c',
  'dc_main.c',
  'dc_watchdog.c',
  'output_stub.c',
  'posix_compat.c',
  'sdl_compat.c',
)

if get_option('buildtype') != 'release'
  add_project_arguments('-DTRX_DC_WATCHDOG', language: 'c')
endif

# Define TRX_DC_FB_CONSOLE to show the KOS debug console on screen instead of
# the serial port (useful on hardware without a serial cable).

# Patched copies of KallistiOS sources, linked whole so that they take the
# place of the library's own objects.
dc_kos_overrides = static_library(
  'dc_kos_overrides',
  'kos/fs_iso9660.c',
  override_options: ['c_std=gnu11', 'warning_level=0'],
  c_args: ['-w'],
)
dc_link_whole = [dc_kos_overrides]

# dc_main.c runs ahead of the engine's main.
dc_link_args = ['-Wl,--wrap=main']

if get_option('buildtype') != 'release'
  dc_sources += files('dc_alloc_trace.c')
  dc_link_args += [
    '-Wl,--wrap=malloc',
    '-Wl,--wrap=calloc',
    '-Wl,--wrap=realloc',
  ]
endif

