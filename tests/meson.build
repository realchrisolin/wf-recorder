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

# Layer 3: newest-wins under capture/encode backlog (no compositor).
buffer_pool_backlog_test = executable(
  'buffer-pool-backlog-test',
  'buffer-pool-backlog-test.cpp',
  include_directories: include_directories('../src'),
  dependencies: [threads_dep],
)

test(
  'buffer-pool-backlog',
  buffer_pool_backlog_test,
  timeout: 30,
)

# Layer 5a: ICC registry requirement / classify rules (no compositor).
icc_proto_check_test = executable(
  'icc-proto-check-test',
  'icc-proto-check-test.cpp',
  include_directories: include_directories('../src'),
)

test(
  'icc-proto-check',
  icc_proto_check_test,
)

# Layer 5b: mock Wayland registry advertising ICC globals.
wayland_server_dep = dependency('wayland-server')
wayland_client_dep = dependency('wayland-client')
mock_wayland_icc_test = executable(
  'mock-wayland-icc-registry-test',
  'mock-wayland-icc-registry-test.cpp',
  include_directories: include_directories('../src'),
  dependencies: [threads_dep, wayland_server_dep, wayland_client_dep, wf_protos],
)

test(
  'mock-wayland-icc-registry',
  mock_wayland_icc_test,
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

# Layer 3: high-fps backlog + SIGINT (Wayland). Skip without display.
smoke_backlog = find_program('smoke-backlog.sh')
test(
  'smoke-backlog',
  smoke_backlog,
  args: [wf_recorder],
  depends: [wf_recorder],
  is_parallel: false,
  timeout: 90,
)

# Layer 3: VAAPI DMA-BUF + SIGINT clean exit. Skip without GPU/Wayland.
smoke_vaapi = find_program('smoke-vaapi-sigint.sh')
test(
  'smoke-vaapi-sigint',
  smoke_vaapi,
  args: [wf_recorder],
  depends: [wf_recorder],
  is_parallel: false,
  timeout: 90,
)
