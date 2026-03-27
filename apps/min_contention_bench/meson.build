link_with = [
  lock_lib,
  utils_lib,
]

include_directories = [
  lock_includes,
  utils_includes,
]

numa_dep = dependency('numa', required : false)


min_contention_bench = executable(
  'min_contention_bench',
  'min_contention_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)