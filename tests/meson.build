threads_dep = dependency('threads')
libavcodec_dep = dependency('libavcodec')
libavutil_dep = dependency('libavutil')

buffer_pool_test = executable(
  'buffer-pool-test',
  'buffer-pool-test.cpp',
  include_directories: include_directories('../src'),
  dependencies: [threads_dep],
)

test(
  'buffer-pool',
  buffer_pool_test,
  timeout: 30,
)

signal_exit_test = executable(
  'signal-exit-test',
  'signal-exit-test.cpp',
)

test(
  'signal-exit',
  signal_exit_test,
)

ffmpeg_pixfmt_test = executable(
  'ffmpeg-pixfmt-test',
  'ffmpeg-pixfmt-test.cpp',
  dependencies: [libavcodec_dep, libavutil_dep],
)

test(
  'ffmpeg-pixfmt',
  ffmpeg_pixfmt_test,
  timeout: 30,
)

# Layer 2: ICC client feature detection (no compositor required).
icc_features = find_program('icc-client-features.sh')
test(
  'icc-client-features',
  icc_features,
  args: [wf_recorder],
  depends: [wf_recorder],
  timeout: 30,
)

# Layer 2: Wayland smoke (list outputs + short capture + SIGINT).
# Exit 77 = skip when WAYLAND_DISPLAY is unset (headless CI).
smoke_wayland = find_program('smoke-wayland.sh')
test(
  'smoke-wayland',
  smoke_wayland,
  args: [wf_recorder],
  depends: [wf_recorder],
  is_parallel: false,
  timeout: 60,
)
