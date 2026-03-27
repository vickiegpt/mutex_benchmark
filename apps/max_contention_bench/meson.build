link_with = [
  lock_lib,
  utils_lib,
]

include_directories = [
  lock_includes,
  utils_includes,
]

numa_dep = dependency('numa', required : false)

max_contention_bench = executable(
  'max_contention_bench',
  'max_contention_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)