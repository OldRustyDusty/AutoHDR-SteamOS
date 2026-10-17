project('hdrket', 'cpp',
  version : '0.1.1',
  license : 'MIT',
  meson_version : '>= 0.60',
  default_options : ['cpp_std=c++20', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

# Headers only: a layer must not link the loader; it gets every function
# pointer through the chain.
vulkan_dep = dependency('vulkan', required : false).partial_dependency(compile_args : true, includes : true)
thread_dep = dependency('threads')

glslang = find_program('glslangValidator')

# Each output encoding is its own SPIR-V binary, compiled here at build time.
# Nothing the user can change needs a shader rebuild (requirement R2).
shader_variants = [
  ['itm.comp', 'itm_scrgb_spv', []],
  ['itm.comp', 'itm_hdr10_spv', ['-DOUT_HDR10']],
]
shader_headers = []
foreach v : shader_variants
  shader_headers += custom_target(v[1],
    input : 'src/shaders/' + v[0],
    output : v[1] + '.h',
    depend_files : files('src/shaders/common.glsl'),
    command : [glslang, '-V', '--target-env', 'vulkan1.0', '-I' + meson.current_source_dir() / 'src/shaders',
               '--vn', v[1]] + v[2] + ['-o', '@OUTPUT@', '@INPUT@'])
endforeach

python = find_program('python3')
ui_header = custom_target('ui_html',
  input : 'ui/index.html',
  output : 'ui_html.h',
  command : [python, files('scripts/embed.py'), '@INPUT@', '@OUTPUT@', 'ui_html'])

sources = files(
  'src/layer.cpp',
  'src/swapchain.cpp',
  'src/present.cpp',
  'src/formats.cpp',
  'src/params.cpp',
  'src/httpd.cpp',
  'src/log.cpp',
)

hdrket_lib = shared_library('hdrket',
  sources, shader_headers, ui_header,
  dependencies : [vulkan_dep, thread_dep],
  cpp_args : ['-fvisibility=hidden', '-Wno-missing-field-initializers', '-DHDRKET_VERSION="' + meson.project_version() + '"'],
  # Steam's runtime container may carry an older libstdc++ than the host.
  # Export only the two loader entry points so the bundled C++ runtime can't
  # interpose on the game's.
  link_args : ['-static-libstdc++', '-static-libgcc', '-Wl,-z,defs', '-Wl,--exclude-libs,ALL',
               '-Wl,--version-script=' + meson.current_source_dir() / 'layer/hdrket.map'],
  link_depends : 'layer/hdrket.map',
  install : true,
  install_dir : get_option('libdir'))

manifest = configuration_data()
manifest.set('LIB_PATH', get_option('prefix') / get_option('libdir') / 'libhdrket.so')
configure_file(
  input : 'layer/hdrket.json.in',
  output : 'hdrket.json',
  configuration : manifest,
  install_dir : get_option('datadir') / 'vulkan' / 'implicit_layer.d')
