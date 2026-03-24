# CXL GPU Test - CUDA-aware build configuration

cuda_sources = []
cuda_includes = []
cuda_compile_args = []
cuda_link_args = []

# Try to find CUDA (optional)
nvcc = find_program('nvcc', required : false)

if nvcc.found()
  message('CUDA (nvcc) found: @0@'.format(nvcc.full_path()))

  # Compile CUDA kernels
  cuda_kernel_obj = custom_target(
    'cxl_gpu_kernels',
    input: 'cxl_gpu_kernels.cu',
    output: 'cxl_gpu_kernels.o',
    command: [nvcc, '-c', '-O3', '--std=c++17',
              '-arch=sm_70',  # Volta or newer for atomicAdd_system
              '-I' + meson.project_source_root() / 'lib' / 'lock',
              '-I' + meson.project_source_root() / 'lib' / 'utils',
              '-DCUDA_GPU',
              '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true,
  )

  cuda_sources = [cuda_kernel_obj]
  cuda_compile_args = ['-DCUDA_GPU']
  cuda_link_args = ['-lcudart', '-L/usr/local/cuda/lib64', '-Wl,-rpath,/usr/local/cuda/lib64']

  message('Building with CUDA support (SM_70+)')
else
  message('CUDA not found; building CPU-only simulation mode')
endif

# Main executable
cxl_gpu_test = executable(
  'cxl_gpu_test',
  ['cxl_gpu_test.cpp'] + cuda_sources,
  cpp_args: cuda_compile_args,
  link_args: cuda_link_args,
  dependencies: [lock_dep, utils_dep],
)

message('Built: cxl_gpu_test (CUDA=@0@)'.format(nvcc.found() ? 'enabled' : 'disabled'))
