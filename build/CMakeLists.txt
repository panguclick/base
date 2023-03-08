
cmake_minimum_required(VERSION 3.13.0)

find_package(PythonInterp REQUIRED)

file(REMOVE_RECURSE ${PROJECT_BINARY_DIR}/gen)

set(py_file "./write_buildflag_header.py")
set(gen_dir "${PROJECT_BINARY_DIR}/gen")

set(
  file_paths
  base/android/library_loader
  base
  base/android/library_loader
  base/message_loop
  base
  base/power_monitor
  base/allocator/partition_allocator/partition_alloc_base/debug
  base/debug
  base/message_loop
  base
  base/profiler
  base
  base/allocator
  base
  base/allocator/partition_allocator
  base
  base
  base/allocator/partition_allocator
  base/allocator/partition_allocator
  base/synchronization
  build/rust
  build
  build/config/compiler
  build
  build
  base/win
  base/allocator/partition_allocator
)

set(
  file_names
  anchor_functions_buildflags.h
  logging_buildflags.h
  anchor_functions_buildflags.h
  message_pump_buildflags.h
  parsing_buildflags.h
  power_monitor_buildflags.h
  debugging_buildflags.h
  debugging_buildflags.h
  ios_cronet_buildflags.h
  cfi_buildflags.h
  profiler_buildflags.h
  clang_profiling_buildflags.h
  buildflags.h
  feature_list_buildflags.h
  chromeos_buildflags.h
  sanitizer_buildflags.h
  tracing_buildflags.h
  logging_buildflags.h
  chromecast_buildflags.h
  synchronization_buildflags.h
  rust_buildflags.h
  branding_buildflags.h
  compiler_buildflags.h
  chromecast_buildflags.h
  chromeos_buildflags.h
  base_win_buildflags.h
  partition_alloc_buildflags.h
)

set(
  flag_file_names
  __base_orderfile_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_logging_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_anchor_functions_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_message_pump_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_parsing_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_power_monitor_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_partition_allocator_debugging_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_debugging_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_ios_cronet_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_cfi_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_profiler_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_clang_profiling_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_feature_list_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_partition_allocator_chromeos_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_sanitizer_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_tracing_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_partition_allocator_logging_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_partition_allocator_chromecast_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_synchronization_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __build_rust_rust_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __build_branding_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __build_config_compiler_compiler_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __build_chromecast_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __build_chromeos_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_win_base_win_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
  __base_allocator_partition_allocator_partition_alloc_buildflags___build_toolchain_win_win_clang_x64__rule.rsp
)

set(
  rule_names
  //base:anchor_functions_buildflags
  //base:logging_buildflags
  //base:anchor_functions_buildflags
  //base:message_pump_buildflags
  //base:parsing_buildflags
  //base:power_monitor_buildflags
  //base/allocator/partition_allocator:debugging_buildflags
  //base:debugging_buildflags
  //base:ios_cronet_buildflags
  //base:cfi_buildflags
  //base:profiler_buildflags
  //base:clang_profiling_buildflags
  //base/allocator:buildflags
  //base:feature_list_buildflags
  //base/allocator/partition_allocator:chromeos_buildflags
  //base:sanitizer_buildflags
  //base:tracing_buildflags
  //base/allocator/partition_allocator:logging_buildflags
  //base/allocator/partition_allocator:chromecast_buildflags
  //base:synchronization_buildflags
  //build/rust:rust_buildflags
  //build/branding_buildflags
  //build/config/compiler:compiler_buildflags
  //build:chromecast_buildflags
  //build:chromeos_buildflags
  //base/win:base_win_buildflags
  //base/allocator/partition_allocator:partition_alloc_buildflags
)

foreach(file_path file_name rule_name flag_file_name IN ZIP_LISTS file_paths file_names rule_names flag_file_names)
  file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/gen/${file_path})
  execute_process(COMMAND ${PYTHON_EXECUTABLE}
    ${py_file}
    "--gen-dir=${gen_dir}"
    "--output=${file_path}/${file_name}"
    "--rulename=${rule_name}"
    "--definitions=./flags/${flag_file_name}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endforeach()

execute_process(COMMAND ${PYTHON_EXECUTABLE}
"./write_build_date_header.py"
"${gen_dir}/base/generated_build_date.h"
"1675573200"
WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
