#
# Copyright (c) 2021-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

cmake_minimum_required(VERSION 3.10)

project(DOCA_APPLICATIONS C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

include(CheckFunctionExists)
include(CheckSymbolExists)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

set(DOCA_PREFIX "doca_")
set(GPU_SUFFIX "_gpu")
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SAMPLES_DIR "${REPO_ROOT}/samples")

set(BASE_C_ARGS
	-Wno-missing-braces
	-Wno-missing-field-initializers
)
set(BASE_CPP_ARGS
	-Wno-missing-field-initializers
)

option(install_apps "Install DOCA applications." OFF)
set(app_install_dir "" CACHE PATH "Install directory for DOCA applications.")

option(enable_all_applications "Enable all DOCA applications." ON)
option(enable_cross_compilation_to_dpu "Enable cross-compilation mode from host to DPU." OFF)
option(enable_deprecated_features "Silence compilation warnings regarding deprecated DOCA features." OFF)
option(enable_trace_log "Enable trace log." OFF)
option(enable_pcc_application_tx_counter_sampling "Enable sampling of TX bytes counter in PCC application." OFF)
option(enable_pcc_application_np_rx_rate "Enable PCC application CC rate update via notification point RX bytes." OFF)
option(upstream_grpc "Are we compiling using upstream gRPC?" ON)
set(DOCA_BUILD_PLATFORM "DPU" CACHE STRING "Building DOCA applications for DPU or HOST")
set_property(CACHE DOCA_BUILD_PLATFORM PROPERTY STRINGS DPU HOST)
message(STATUS "Building DOCA applications for ${DOCA_BUILD_PLATFORM}")

set(spdk_prefix "/opt/mellanox/spdk" CACHE PATH "SPDK installation prefix.")
set(spdk_dpdk_prefix "/opt/mellanox/spdk" CACHE PATH "Installation prefix of DPDK used by SPDK.")
set(spdk_isal_prefix "/opt/mellanox/spdk/lib" CACHE PATH "SPDK ISA-L installation prefix.")
set(dpacc_mcpu "nv-dpa-bf3,nv-dpa-cx7,nv-dpa-cx8,nv-dpa-cx9" CACHE STRING "Set the supported architecture for DPA related application.")
set(b_sanitize "none" CACHE STRING "Sanitizer option passed to DPA helper scripts.")

option(enable_app_shield_agent "Enable the App Shield Agent application." OFF)
option(enable_bifurcated_driver_model "Enable the Bifurcated Driver Model application." OFF)
option(enable_dma_copy "Enable the DMA copy application." OFF)
option(enable_dpa_all_to_all "Enable DPA All to All application." OFF)
option(enable_dpu_gpu_remote_offload "Enable DPU GPU remote offload application." OFF)
option(enable_eth_l2_fwd "Enable the Ethernet L2 Forwarding application." OFF)
option(enable_file_compression "Enable the File Compression application." OFF)
option(enable_file_integrity "Enable the File Integrity application." OFF)
option(enable_gpu_packet_processing "Enable GPUNetIO packet processing application." OFF)
option(enable_ip_frag "Enable IP fragmentation application." OFF)
option(enable_ipsec_security_gw "Enable IPsec Security Gateway application." OFF)
option(enable_psp_gateway "Enable PSP Gateway application." OFF)
option(enable_nvme_emulation "Enable NVMe Emulation application." OFF)
option(enable_pcc "Enable PCC application." OFF)
option(enable_secure_channel "Enable Secure Channel application." OFF)
option(enable_simple_fwd_vnf "Enable Simple Forward VNF application." OFF)
option(enable_sta_offload "Enable STA Offload application." OFF)
option(enable_storage "Enable storage reference applications." OFF)
option(enable_stream_receive_perf "Enable the Stream Receive Performance application." OFF)
option(enable_switch "Enable Switch application." OFF)
option(enable_telemetry "Enable Telemetry applications." OFF)
option(enable_telemetry_traceback "Enable Telemetry Traceback application." ON)
option(enable_telemetry_opentelemetry "Enable Telemetry OpenTelemetry application." ON)
option(enable_time_sync "Enable Time Sync application." OFF)
option(enable_upf_accel "Enable UPF Acceleration application." OFF)
option(enable_urom_rdmo "Enable UROM RDMO application." OFF)
option(enable_vblk_pci_dev "Enable VirtIO Block PCI Device application." OFF)
option(enable_virtiofs "Enable Virtiofs application." OFF)
option(enable_vnet_pci_dev "Enable VirtIO Net PCI Device application." OFF)
option(enable_yara_inspection "Enable Yara Inspection application." OFF)

function(doca_pkg_check PKG OUT_TARGET OUT_FOUND)
	string(MAKE_C_IDENTIFIER "${PKG}" PKG_ID)
	string(TOUPPER "${PKG_ID}" PKG_ID)
	set(PKG_PREFIX "DOCA_PKG_${PKG_ID}")

	if(NOT TARGET "PkgConfig::${PKG_PREFIX}")
		pkg_check_modules(${PKG_PREFIX} QUIET IMPORTED_TARGET "${PKG}")
	endif()

	if(TARGET "PkgConfig::${PKG_PREFIX}")
		set(${OUT_TARGET} "PkgConfig::${PKG_PREFIX}" PARENT_SCOPE)
		set(${OUT_FOUND} TRUE PARENT_SCOPE)
	else()
		set(${OUT_TARGET} "" PARENT_SCOPE)
		set(${OUT_FOUND} FALSE PARENT_SCOPE)
	endif()
endfunction()

function(doca_pkg_variable OUT_VAR PKG VAR)
	pkg_get_variable(PKG_VAR_VALUE "${PKG}" "${VAR}")
	set(${OUT_VAR} "${PKG_VAR_VALUE}" PARENT_SCOPE)
endfunction()

function(doca_apply_compile_options TARGET LANG)
	foreach(OPT IN LISTS ARGN)
		target_compile_options(${TARGET} PRIVATE "$<$<COMPILE_LANGUAGE:${LANG}>:${OPT}>")
	endforeach()
endfunction()

function(doca_finish_target TARGET)
	cmake_parse_arguments(ARG "CPP17;CUDA" "" "INCLUDE_DIRS;DEPS;C_OPTIONS;CXX_OPTIONS;CUDA_OPTIONS;LINK_OPTIONS;COMPILE_DEFINITIONS" ${ARGN})

	target_include_directories(${TARGET} PRIVATE ${BASE_APP_INCLUDES} ${ARG_INCLUDE_DIRS})
	target_link_libraries(${TARGET} PRIVATE ${ARG_DEPS})
	target_compile_definitions(${TARGET} PRIVATE ${ARG_COMPILE_DEFINITIONS})

	doca_apply_compile_options(${TARGET} C ${BASE_C_ARGS} ${ARG_C_OPTIONS})
	doca_apply_compile_options(${TARGET} CXX ${BASE_CPP_ARGS} ${ARG_CXX_OPTIONS})
	if(ARG_CUDA)
		doca_apply_compile_options(${TARGET} CUDA ${GPU_CUDA_ARGS} ${ARG_CUDA_OPTIONS})
		set_target_properties(${TARGET} PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
	endif()

	if(ARG_CPP17)
		set_property(TARGET ${TARGET} PROPERTY CXX_STANDARD 17)
	endif()

	if(ARG_LINK_OPTIONS)
		target_link_options(${TARGET} PRIVATE ${ARG_LINK_OPTIONS})
	endif()

	if(install_apps)
		install(TARGETS ${TARGET} RUNTIME DESTINATION "${app_install_dir}" LIBRARY DESTINATION "${app_install_dir}")
	endif()
endfunction()

function(doca_add_executable TARGET)
	cmake_parse_arguments(ARG "CPP17;CUDA" "" "SOURCES;INCLUDE_DIRS;DEPS;C_OPTIONS;CXX_OPTIONS;CUDA_OPTIONS;LINK_OPTIONS;COMPILE_DEFINITIONS" ${ARGN})
	add_executable(${TARGET} ${ARG_SOURCES})
	set(FINISH_OPTIONS "")
	if(ARG_CPP17)
		list(APPEND FINISH_OPTIONS CPP17)
	endif()
	if(ARG_CUDA)
		list(APPEND FINISH_OPTIONS CUDA)
	endif()
	doca_finish_target(${TARGET}
		${FINISH_OPTIONS}
		INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
		DEPS ${ARG_DEPS}
		C_OPTIONS ${ARG_C_OPTIONS}
		CXX_OPTIONS ${ARG_CXX_OPTIONS}
		CUDA_OPTIONS ${ARG_CUDA_OPTIONS}
		LINK_OPTIONS ${ARG_LINK_OPTIONS}
		COMPILE_DEFINITIONS ${ARG_COMPILE_DEFINITIONS}
	)
endfunction()

function(doca_add_shared_library TARGET)
	cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE_DIRS;DEPS;C_OPTIONS;CXX_OPTIONS;LINK_OPTIONS;COMPILE_DEFINITIONS" ${ARGN})
	add_library(${TARGET} SHARED ${ARG_SOURCES})
	doca_finish_target(${TARGET}
		INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
		DEPS ${ARG_DEPS}
		C_OPTIONS ${ARG_C_OPTIONS}
		CXX_OPTIONS ${ARG_CXX_OPTIONS}
		LINK_OPTIONS ${ARG_LINK_OPTIONS}
		COMPILE_DEFINITIONS ${ARG_COMPILE_DEFINITIONS}
	)
	if(install_apps)
		install(TARGETS ${TARGET} LIBRARY DESTINATION "${app_install_dir}")
	endif()
endfunction()

function(doca_app_sources APP OUT_VAR)
	set(SOURCES "")
	foreach(SRC IN LISTS ARGN)
		if(IS_ABSOLUTE "${SRC}")
			list(APPEND SOURCES "${SRC}")
		else()
			list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${APP}/${SRC}")
		endif()
	endforeach()
	set(${OUT_VAR} ${SOURCES} PARENT_SCOPE)
endfunction()

macro(doca_set_app_dependencies APP)
	cmake_parse_arguments(ARG "" "" "DOCA;DRIVER" ${ARGN})
	set(APP_${APP}_DOCA_DEPS ${ARG_DOCA})
	set(APP_${APP}_DRIVER_DEPS ${ARG_DRIVER})
endmacro()

set(DOCA_DRIVER_dpdk "libdpdk")
set(DOCA_DRIVER_flexio "libflexio")
set(DOCA_DRIVER_ucx "ucx")
set(DPACC_DEPENDENCIES dpa pcc flexio)

doca_set_app_dependencies(app_shield_agent DOCA argp apsh telemetry_exporter)
doca_set_app_dependencies(bifurcated_driver_model DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(dma_copy DOCA argp dma comch)
doca_set_app_dependencies(dpa_all_to_all DOCA argp dpa rdma)
doca_set_app_dependencies(dpu_gpu_remote_offload DOCA argp comch gpunetio)
doca_set_app_dependencies(eth_l2_fwd DOCA argp eth flow)
doca_set_app_dependencies(file_compression DOCA argp comch compress)
doca_set_app_dependencies(file_integrity DOCA argp comch sha)
doca_set_app_dependencies(gpu_packet_processing DOCA argp gpunetio eth flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(ip_frag DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(ipsec_security_gw DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(nvme_emulation DOCA argp dma comch dpa devemu DRIVER flexio)
doca_set_app_dependencies(pcc DOCA argp pcc DRIVER flexio)
doca_set_app_dependencies(psp_gateway DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(secure_channel DOCA argp comch)
doca_set_app_dependencies(simple_fwd_vnf DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(storage DOCA argp comch compress dma erasure_coding rdma)
doca_set_app_dependencies(stream_receive_perf DOCA argp rmax)
doca_set_app_dependencies(switch DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(telemetry DOCA argp telemetry)
doca_set_app_dependencies(time_sync DOCA argp comch)
doca_set_app_dependencies(upf_accel DOCA argp flow dpdk_bridge DRIVER dpdk)
doca_set_app_dependencies(urom_rdmo DOCA argp urom DRIVER ucx)
doca_set_app_dependencies(vblk_pci_dev DOCA argp dma dpa devemu DRIVER flexio)
doca_set_app_dependencies(virtiofs DOCA argp dma devemu)
doca_set_app_dependencies(vnet_pci_dev DOCA argp devemu)
doca_set_app_dependencies(yara_inspection DOCA argp apsh telemetry_exporter)

doca_pkg_check("doca-common" DOCA_COMMON_TARGET DOCA_COMMON_FOUND)
if(NOT DOCA_COMMON_FOUND)
	message(FATAL_ERROR "Missing required DOCA library doca-common")
endif()

set(BASE_APP_DEPS Threads::Threads ${DOCA_COMMON_TARGET})
set(BASE_APP_INCLUDES "${COMMON_DIR}" "${REPO_ROOT}")

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_function_exists(strlcpy HAVE_LIBC_STRLCPY)
unset(CMAKE_REQUIRED_DEFINITIONS)
if(HAVE_LIBC_STRLCPY)
	add_compile_definitions(DOCA_USE_LIBC_STRING_L_FUNC)
else()
	doca_pkg_check("libbsd" LIBBSD_TARGET LIBBSD_FOUND)
	if(LIBBSD_FOUND)
		list(APPEND BASE_APP_DEPS ${LIBBSD_TARGET})
		add_compile_definitions(DOCA_USE_LIBBSD RTE_USE_LIBBSD)
	endif()
endif()

if(DOCA_BUILD_PLATFORM STREQUAL "DPU")
	set(is_dpu TRUE)
	set(is_host FALSE)
else()
	set(is_dpu FALSE)
	set(is_host TRUE)
endif()
message(STATUS "Building for ${DOCA_BUILD_PLATFORM}")

doca_pkg_check("libdpdk" DPDK_TARGET DPDK_FOUND)
if(DPDK_FOUND)
	pkg_check_modules(DPDK_CHECK QUIET "libdpdk")
	set(CMAKE_REQUIRED_INCLUDES ${DPDK_CHECK_INCLUDE_DIRS})
	check_symbol_exists(rte_pmd_mlx5_driver_disable_steering "rte_pmd_mlx5.h" HAVE_DOCA_BUNDLE_DPDK)
	check_symbol_exists(rte_pmd_mlx5_disable_steering "rte_pmd_mlx5.h" HAVE_UPSTREAM_DPDK)
	unset(CMAKE_REQUIRED_INCLUDES)
	if(HAVE_DOCA_BUNDLE_DPDK)
		add_compile_definitions(DOCA_BUNDLE_DPDK_FOUND)
		message(STATUS "Found rte_pmd_mlx5_driver_disable_steering(), using DOCA-Bundle DPDK")
	elseif(HAVE_UPSTREAM_DPDK)
		add_compile_definitions(UPSTREAM_DPDK_FOUND)
		message(STATUS "Found rte_pmd_mlx5_disable_steering(), using Upstream DPDK")
	else()
		message(WARNING "Unable to find rte_pmd_mlx5_driver_disable_steering() or rte_pmd_mlx5_disable_steering()")
	endif()
endif()

add_compile_definitions(DOCA_ALLOW_EXPERIMENTAL_API)
if(enable_deprecated_features)
	add_compile_definitions(DOCA_ALLOW_DEPRECATED_API)
endif()
if(enable_trace_log)
	add_compile_definitions(DOCA_LOGGING_ALLOW_TRACE)
endif()
if(enable_cross_compilation_to_dpu)
	include_directories("/root/doca-cross/usr/include")
endif()
add_compile_options(-Wno-format-zero-length)

set(flag_enable_gpu_support TRUE)
find_program(NVLINK_EXECUTABLE nvlink)
if(NOT NVLINK_EXECUTABLE)
	message(WARNING "CUDA was not found (nvlink) - Disabling GPU support!")
	set(flag_enable_gpu_support FALSE)
endif()
if(flag_enable_gpu_support)
	check_language(CUDA)
	if(CMAKE_CUDA_COMPILER)
		enable_language(CUDA)
		find_package(CUDAToolkit QUIET)
	else()
		message(WARNING "CUDA was not found (compiler) - Disabling GPU support!")
		set(flag_enable_gpu_support FALSE)
	endif()
endif()
if(flag_enable_gpu_support)
	if(NOT CUDAToolkit_FOUND)
		message(WARNING "CUDA was not found (dependency) - Disabling GPU support!")
		set(flag_enable_gpu_support FALSE)
	elseif(CUDAToolkit_VERSION VERSION_LESS "12.2")
		message(WARNING "CUDA version ${CUDAToolkit_VERSION} is lower than the expected version of 12.2 - Disabling GPU support!")
		set(flag_enable_gpu_support FALSE)
	endif()
endif()
if(flag_enable_gpu_support)
	set(CMAKE_CUDA_ARCHITECTURES 80 90)
	if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
		list(APPEND CMAKE_CUDA_ARCHITECTURES 100)
	endif()
	set(GPU_COMPILE_DEFINITIONS GPU_SUPPORT)
	set(GPU_CUDA_ARGS -forward-unknown-to-host-compiler -rdc=true)
	set(GPU_DEPS CUDA::cudart CUDA::cuda_driver)
endif()

set(flag_enable_grpc_support TRUE)
find_program(PROTOC_EXECUTABLE protoc)
if(NOT PROTOC_EXECUTABLE)
	message(WARNING "Failed to find an up-to-date gRPC compiler (protoc) - Disabling gRPC support!")
	set(flag_enable_grpc_support FALSE)
endif()
if(flag_enable_grpc_support)
	find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
	if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
		message(WARNING "Failed to find a gRPC C++ plugin - Disabling gRPC support!")
		set(flag_enable_grpc_support FALSE)
	endif()
endif()
if(flag_enable_grpc_support)
	find_program(GRPC_PY_PLUGIN_EXECUTABLE grpc_python_plugin)
	if(NOT GRPC_PY_PLUGIN_EXECUTABLE)
		message(WARNING "Failed to find a gRPC Python plugin (grpc_python_plugin) - Disabling gRPC support!")
		set(flag_enable_grpc_support FALSE)
	endif()
endif()
if(flag_enable_grpc_support)
	doca_pkg_check("protobuf" PROTOBUF_TARGET PROTOBUF_FOUND)
	if(NOT PROTOBUF_FOUND)
		message(WARNING "Failed to find protobuf (gRPC) - Disabling gRPC support!")
		set(flag_enable_grpc_support FALSE)
	endif()
endif()
if(flag_enable_grpc_support)
	doca_pkg_check("grpc++" GRPC_TARGET GRPC_FOUND)
	if(NOT GRPC_FOUND)
		message(WARNING "Failed to find gRPC - Disabling gRPC support!")
		set(flag_enable_grpc_support FALSE)
	endif()
endif()
if(flag_enable_grpc_support)
	set(GRPC_DEPS ${PROTOBUF_TARGET} ${GRPC_TARGET})
	set(GRPC_C_OPTIONS
		-Wno-deprecated-declarations
		-Wno-missing-declarations
		-Wno-redundant-decls
		-Wno-null-dereference
		-Wno-array-bounds
	)
	set(GRPC_CXX_OPTIONS
		${GRPC_C_OPTIONS}
		-fno-permissive
	)
	set(GRPC_LINK_OPTIONS "")
	pkg_check_modules(GRPC_VERSION_CHECK QUIET "grpc++")
	if(GRPC_VERSION_CHECK_VERSION VERSION_LESS "1.28.0")
		list(APPEND GRPC_LINK_OPTIONS -lgrpc)
	endif()
endif()

set(flag_enable_dpa_support TRUE)
find_program(DPACC_EXECUTABLE dpacc HINTS "/opt/mellanox/doca/tools")
if(NOT DPACC_EXECUTABLE)
	message(WARNING "Missing DPACC tool needed by DOCA DPA, DOCA PCC and FlexIO")
	set(flag_enable_dpa_support FALSE)
endif()
if(enable_cross_compilation_to_dpu)
	message(WARNING "DPACC tool is not supported during cross compilation")
	set(flag_enable_dpa_support FALSE)
endif()

function(doca_prepare_app APP OUT_ENABLED OUT_DEPS OUT_INCLUDES)
	if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${APP}")
		set(${OUT_ENABLED} FALSE PARENT_SCOPE)
		set(${OUT_DEPS} "" PARENT_SCOPE)
		set(${OUT_INCLUDES} "" PARENT_SCOPE)
		return()
	endif()

	set(ENABLE_OPTION "enable_${APP}")
	set(APP_REQUESTED FALSE)
	if(DEFINED ${ENABLE_OPTION} AND ${${ENABLE_OPTION}})
		set(APP_REQUESTED TRUE)
	endif()
	if(NOT enable_all_applications AND NOT APP_REQUESTED)
		message(WARNING "Skipping compilation of DOCA Application - ${APP}.")
		set(${OUT_ENABLED} FALSE PARENT_SCOPE)
		set(${OUT_DEPS} "" PARENT_SCOPE)
		set(${OUT_INCLUDES} "" PARENT_SCOPE)
		return()
	endif()

	set(APP_DEPS ${BASE_APP_DEPS})
	set(APP_DOCA_DEPS ${APP_${APP}_DOCA_DEPS} common)
	foreach(DOCA_DEP IN LISTS APP_DOCA_DEPS)
		string(REPLACE "_" "-" DOCA_DEP_PKG "${DOCA_DEP}")
		set(DOCA_DEP_PKG "doca-${DOCA_DEP_PKG}")
		doca_pkg_check("${DOCA_DEP_PKG}" CUR_TARGET CUR_FOUND)
		if(NOT CUR_FOUND)
			message(WARNING "Skipping compilation of DOCA Application - ${APP} - Missing DOCA library doca_${DOCA_DEP}")
			set(${OUT_ENABLED} FALSE PARENT_SCOPE)
			set(${OUT_DEPS} "" PARENT_SCOPE)
			set(${OUT_INCLUDES} "" PARENT_SCOPE)
			return()
		endif()
		list(FIND DPACC_DEPENDENCIES "${DOCA_DEP}" DPACC_DEP_INDEX)
		if(DPACC_DEP_INDEX GREATER -1 AND NOT flag_enable_dpa_support)
			message(WARNING "Skipping compilation of DOCA Application - ${APP} - Missing DPACC support")
			set(${OUT_ENABLED} FALSE PARENT_SCOPE)
			set(${OUT_DEPS} "" PARENT_SCOPE)
			set(${OUT_INCLUDES} "" PARENT_SCOPE)
			return()
		endif()
		list(APPEND APP_DEPS ${CUR_TARGET})
	endforeach()

	foreach(DRIVER_DEP IN LISTS APP_${APP}_DRIVER_DEPS)
		set(DRIVER_PKG "${DOCA_DRIVER_${DRIVER_DEP}}")
		doca_pkg_check("${DRIVER_PKG}" CUR_TARGET CUR_FOUND)
		if(NOT CUR_FOUND)
			message(WARNING "Skipping compilation of DOCA Application - ${APP} - Missing DOCA driver ${DRIVER_DEP}")
			set(${OUT_ENABLED} FALSE PARENT_SCOPE)
			set(${OUT_DEPS} "" PARENT_SCOPE)
			set(${OUT_INCLUDES} "" PARENT_SCOPE)
			return()
		endif()
		list(FIND DPACC_DEPENDENCIES "${DRIVER_DEP}" DPACC_DEP_INDEX)
		if(DPACC_DEP_INDEX GREATER -1 AND NOT flag_enable_dpa_support)
			message(WARNING "Skipping compilation of DOCA Application - ${APP} - Missing DPACC support")
			set(${OUT_ENABLED} FALSE PARENT_SCOPE)
			set(${OUT_DEPS} "" PARENT_SCOPE)
			set(${OUT_INCLUDES} "" PARENT_SCOPE)
			return()
		endif()
		list(APPEND APP_DEPS ${CUR_TARGET})
	endforeach()

	set(${OUT_ENABLED} TRUE PARENT_SCOPE)
	set(${OUT_DEPS} ${APP_DEPS} PARENT_SCOPE)
	set(${OUT_INCLUDES} ${BASE_APP_INCLUDES} PARENT_SCOPE)
endfunction()

function(doca_debug_mode OUT_VAR)
	if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
		set(${OUT_VAR} "true" PARENT_SCOPE)
	else()
		set(${OUT_VAR} "none" PARENT_SCOPE)
	endif()
endfunction()

function(doca_simple_c_app APP)
	cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE_DIRS;EXTRA_DEPS" ${ARGN})
	doca_prepare_app(${APP} APP_ENABLED APP_DEPS APP_INCLUDES)
	if(NOT APP_ENABLED)
		return()
	endif()
	doca_app_sources(${APP} APP_SOURCES ${ARG_SOURCES})
	doca_add_executable(${DOCA_PREFIX}${APP}
		SOURCES ${APP_SOURCES}
		INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
		DEPS ${APP_DEPS} ${ARG_EXTRA_DEPS}
	)
endfunction()

doca_simple_c_app(app_shield_agent
	SOURCES
		app_shield_agent_core.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		app_shield_agent.c
)
if(is_host AND TARGET doca_app_shield_agent)
	message(WARNING "app_shield_agent is only supported on the DPU, and is not supported on the Host")
endif()

doca_simple_c_app(bifurcated_driver_model
	SOURCES
		bifurcated_driver_model.c
		bifurcated_driver_model_core.c
		${COMMON_DIR}/dpdk_utils.c
		${COMMON_DIR}/utils.c
		${COMMON_DIR}/flow_parser.c
		${COMMON_DIR}/flow_pipes_manager.c
		${SAMPLES_DIR}/common.c
		${SAMPLES_DIR}/doca_flow/flow_common.c
		${SAMPLES_DIR}/doca_flow/flow_switch_common.c
	INCLUDE_DIRS
		${SAMPLES_DIR}
		${SAMPLES_DIR}/doca_flow
)

doca_simple_c_app(dma_copy
	SOURCES
		dma_copy_core.c
		${COMMON_DIR}/comch_utils.c
		${COMMON_DIR}/pack.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		dma_copy.c
)

doca_prepare_app(dpa_all_to_all DPA_ALL_TO_ALL_ENABLED DPA_ALL_TO_ALL_DEPS DPA_ALL_TO_ALL_INCLUDES)
if(DPA_ALL_TO_ALL_ENABLED)
	find_program(MPICC_EXECUTABLE mpicc)
	if(NOT MPICC_EXECUTABLE)
		message(WARNING "Skipping compilation of dpa_all_to_all - missing mpicc")
	else()
		doca_pkg_variable(DOCA_DPA_LIBDIR "doca-dpa" "libdir")
		doca_debug_mode(DPA_DEBUG_MODE)
		set(DPA_ALL_TO_ALL_OUTPUT "${CMAKE_BINARY_DIR}/dpa_all_to_all/${DOCA_PREFIX}dpa_all_to_all")
		add_custom_command(
			OUTPUT "${DPA_ALL_TO_ALL_OUTPUT}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dpa_all_to_all"
			COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/build_dpa_all_to_all.sh"
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all"
				"${CMAKE_BINARY_DIR}"
				"${b_sanitize}"
				"${DPA_DEBUG_MODE}"
				"${dpacc_mcpu}"
				"${DOCA_DPA_LIBDIR}"
			DEPENDS
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/build_dpa_all_to_all.sh"
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/build_device_code.sh"
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/host/dpa_all_to_all.c"
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/host/dpa_all_to_all_core.c"
				"${CMAKE_CURRENT_SOURCE_DIR}/dpa_all_to_all/device/dpa_all_to_all_dev.c"
			VERBATIM
		)
		add_custom_target(${DOCA_PREFIX}dpa_all_to_all ALL DEPENDS "${DPA_ALL_TO_ALL_OUTPUT}")
	endif()
endif()

doca_prepare_app(dpu_gpu_remote_offload DGRO_ENABLED DGRO_DEPS DGRO_INCLUDES)
if(DGRO_ENABLED)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
		message(WARNING "DPU GPU remote_offload does not currently support this operating system")
	else()
		set(DGRO_COMMON
			remote_offload_common/control_message.cpp
			remote_offload_common/doca_utils.cpp
			remote_offload_common/posix/os_utils.cpp
			remote_offload_common/posix/tcp_socket.cpp
		)
		doca_app_sources(dpu_gpu_remote_offload DGRO_CLIENT_SOURCES
			client/main.cpp
			client/application.cpp
			client/thread.cpp
			${DGRO_COMMON}
		)
		doca_add_executable(${DOCA_PREFIX}dpu_gpu_remote_offload_client
			CPP17
			SOURCES ${DGRO_CLIENT_SOURCES}
			INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/dpu_gpu_remote_offload"
			DEPS ${DGRO_DEPS}
		)

		if(is_host AND flag_enable_gpu_support)
			doca_pkg_check("doca-gpunetio" GPUNETIO_TARGET GPUNETIO_FOUND)
			if(GPUNETIO_FOUND)
				doca_pkg_variable(GPUNETIO_LIBDIR "doca-gpunetio" "libdir")
				doca_app_sources(dpu_gpu_remote_offload DGRO_ORCH_SOURCES
					orchestrator/application.cpp
					orchestrator/comch_control_channel.cpp
					orchestrator/comch_datapath.cpp
					orchestrator/main.cpp
					orchestrator/gpu_processing.cu
					${DGRO_COMMON}
				)
				doca_add_executable(${DOCA_PREFIX}dpu_gpu_remote_offload_orchestrator
					CPP17
					CUDA
					SOURCES ${DGRO_ORCH_SOURCES}
					INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/dpu_gpu_remote_offload"
					DEPS ${DGRO_DEPS} ${GPU_DEPS}
					LINK_OPTIONS "-L${GPUNETIO_LIBDIR}" -ldoca_gpunetio_device
				)
			else()
				message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}dpu_gpu_remote_offload_orchestrator - Missing library doca-gpunetio")
			endif()
		endif()

		if(is_dpu)
			doca_app_sources(dpu_gpu_remote_offload DGRO_SERVER_SOURCES
				server/application.cpp
				server/comch_control_channel.cpp
				server/main.cpp
				server/thread.cpp
				${DGRO_COMMON}
			)
			doca_add_executable(${DOCA_PREFIX}dpu_gpu_remote_offload_server
				CPP17
				SOURCES ${DGRO_SERVER_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/dpu_gpu_remote_offload"
				DEPS ${DGRO_DEPS}
			)
		endif()
	endif()
endif()

doca_simple_c_app(eth_l2_fwd
	SOURCES
		eth_l2_fwd.c
		eth_l2_fwd_core.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		${SAMPLES_DIR}/doca_eth/eth_flow_common.c
)

doca_pkg_check("zlib" ZLIB_TARGET ZLIB_FOUND)
if(ZLIB_FOUND)
	doca_simple_c_app(file_compression
		SOURCES
			file_compression_core.c
			${COMMON_DIR}/comch_utils.c
			${COMMON_DIR}/pack.c
			${COMMON_DIR}/utils.c
			${SAMPLES_DIR}/common.c
			${SAMPLES_DIR}/doca_compress/compress_common.c
			file_compression.c
		INCLUDE_DIRS
			${SAMPLES_DIR}/doca_compress
		EXTRA_DEPS
			${ZLIB_TARGET}
	)
elseif(enable_all_applications OR enable_file_compression)
	message(WARNING "Skipping compilation of DOCA Application - file_compression - Missing zlib")
endif()

doca_simple_c_app(file_integrity
	SOURCES
		file_integrity_core.c
		${COMMON_DIR}/comch_utils.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		file_integrity.c
)

doca_prepare_app(gpu_packet_processing GPP_ENABLED GPP_DEPS GPP_INCLUDES)
if(GPP_ENABLED)
	if(NOT flag_enable_gpu_support)
		message(WARNING "Skipping compilation of DOCA Application - gpu_packet_processing - Missing GPU support.")
	else()
		doca_pkg_variable(GPUNETIO_LIBDIR "doca-gpunetio" "libdir")
		doca_app_sources(gpu_packet_processing GPP_SOURCES
			gpu_packet_processing.c
			args.c
			device.c
			queues/flow.c
			queues/http_txbuf.c
			queues/udp_queues.c
			queues/tcp_queues.c
			queues/icmp_queues.c
			tcp_cpu/tcp_cpu_rss_func.c
			tcp_cpu/tcp_session_table.c
			${COMMON_DIR}/utils.c
			${SAMPLES_DIR}/common.c
			gpu_kernels/receive_tcp.cu
			gpu_kernels/receive_udp.cu
			gpu_kernels/receive_icmp.cu
			gpu_kernels/http_server.cu
		)
		doca_add_executable(${DOCA_PREFIX}gpu_packet_processing
			CUDA
			SOURCES ${GPP_SOURCES}
			INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/gpu_packet_processing/queues"
			DEPS ${GPP_DEPS} ${GPU_DEPS} resolv
			LINK_OPTIONS "-L${GPUNETIO_LIBDIR}" -ldoca_gpunetio_device
			COMPILE_DEFINITIONS ${GPU_COMPILE_DEFINITIONS}
		)
	endif()
endif()

doca_simple_c_app(ip_frag
	SOURCES
		ip_frag.c
		ip_frag_dp.c
		${COMMON_DIR}/dpdk_utils.c
		${COMMON_DIR}/packet_parser.c
		${SAMPLES_DIR}/common.c
		${SAMPLES_DIR}/doca_flow/flow_common.c
	INCLUDE_DIRS
		${SAMPLES_DIR}
		${SAMPLES_DIR}/doca_flow
)

doca_pkg_check("json-c" JSON_C_TARGET JSON_C_FOUND)
if(JSON_C_FOUND)
	doca_simple_c_app(ipsec_security_gw
		SOURCES
			config.c
			flow_common.c
			flow_decrypt.c
			flow_encrypt.c
			ipsec_ctx.c
			policy.c
			${COMMON_DIR}/dpdk_utils.c
			${COMMON_DIR}/pack.c
			${COMMON_DIR}/utils.c
			${COMMON_DIR}/flow_parser.c
			${SAMPLES_DIR}/common.c
			ipsec_security_gw.c
		EXTRA_DEPS
			${JSON_C_TARGET}
	)
elseif(enable_all_applications OR enable_ipsec_security_gw)
	message(WARNING "Skipping compilation of DOCA Application - ipsec_security_gw - Missing json-c support.")
endif()

doca_prepare_app(nvme_emulation NVME_ENABLED NVME_DEPS NVME_INCLUDES)
if(NVME_ENABLED)
	doca_pkg_check("uuid" UUID_TARGET UUID_FOUND)
	if(NOT UUID_FOUND)
		message(WARNING "Skipping compilation of DOCA Application - nvme_emulation - Missing UUID support.")
	elseif(NOT dpacc_mcpu MATCHES "nv-dpa-bf3")
		message(WARNING "Skipping compilation of DOCA Application - nvme_emulation - only BF3 compilation is supported.")
	else()
		set(SPDK_LIBS
			spdk_event spdk_event_accel spdk_event_bdev spdk_bdev spdk_bdev_aio spdk_bdev_delay
			spdk_bdev_error spdk_bdev_ftl spdk_bdev_gpt spdk_lvol spdk_bdev_lvol
			spdk_bdev_malloc spdk_bdev_null spdk_nvme spdk_bdev_nvme spdk_bdev_passthru
			spdk_bdev_raid spdk_bdev_split spdk_bdev_uring spdk_bdev_virtio spdk_bdev_zone_block
			spdk_blob_bdev spdk_blobfs_bdev spdk_rdma spdk_json spdk_rpc spdk_jsonrpc
			spdk_log spdk_thread spdk_util spdk_init spdk_conf spdk_trace spdk_notify spdk_sock
			spdk_env_dpdk spdk_dma spdk_sock_posix spdk_accel spdk_accel_mlx5 spdk_bdev_crypto
			spdk_nvmf spdk_event_nvmf rte_eal rte_ring rte_pci rte_bus_pci rte_mempool
			rte_mempool_ring rte_kvargs rte_ethdev rte_cmdline rte_telemetry rte_net rte_mbuf
			rte_meter rte_telemetry rte_net rte_mbuf
		)
		set(NVME_SPDK_LIBS "")
		set(NVME_SPDK_OK TRUE)
		foreach(SPDK_LIB IN LISTS SPDK_LIBS)
			string(MAKE_C_IDENTIFIER "${SPDK_LIB}" SPDK_LIB_ID)
			find_library(SPDK_${SPDK_LIB_ID}_LIBRARY "${SPDK_LIB}" PATHS "${spdk_prefix}/lib" "${spdk_dpdk_prefix}/lib" NO_DEFAULT_PATH)
			if(SPDK_${SPDK_LIB_ID}_LIBRARY)
				list(APPEND NVME_SPDK_LIBS "${SPDK_${SPDK_LIB_ID}_LIBRARY}")
			else()
				message(WARNING "Skipping compilation of DOCA Application - nvme_emulation - Failed to find ${SPDK_LIB} required by SPDK.")
				set(NVME_SPDK_OK FALSE)
				break()
			endif()
		endforeach()

		if(NOT EXISTS "${spdk_isal_prefix}/libisal.a")
			message(WARNING "Skipping compilation of DOCA Application - nvme_emulation - Failed to find SPDK ISA-L required by SPDK.")
			set(NVME_SPDK_OK FALSE)
		endif()

		if(NVME_SPDK_OK)
			if(is_host)
				message(WARNING "nvme_emulation is only supported on the DPU, and is not supported on the Host")
			endif()
			doca_pkg_variable(DOCA_DPA_INCLUDEDIR "doca-dpa" "includedir")
			doca_pkg_variable(DOCA_DPA_LIBDIR "doca-dpa" "libdir")
			set(NVME_DPA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/nvme_emulation")
			set(NVME_DPA_ARCHIVE "${NVME_DPA_OUTPUT_DIR}/nvmf_doca_transport_device.a")
			set(NVME_DPA_INCLUDE_FLAGS "-I${DOCA_DPA_INCLUDEDIR} -I${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/common")
			set(NVME_DPA_LINK_FLAGS "-L${DOCA_DPA_LIBDIR} -ldoca_dpa_dev -ldoca_dpa_dev_comm")
			add_custom_command(
				OUTPUT "${NVME_DPA_ARCHIVE}"
				COMMAND ${CMAKE_COMMAND} -E make_directory "${NVME_DPA_OUTPUT_DIR}"
				COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/build_device_code.sh"
					"${NVME_DPA_INCLUDE_FLAGS}"
					"${NVME_DPA_OUTPUT_DIR}"
					"nv-dpa-bf3"
					"${NVME_DPA_LINK_FLAGS}"
					"nvmf_doca_transport_app"
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/device/doca_transport_attributes.yaml"
					"${NVME_DPA_ARCHIVE}"
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/device/doca_transport_dev.c"
				DEPENDS
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/build_device_code.sh"
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/device/doca_transport_dev.c"
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/device/doca_transport_attributes.yaml"
				VERBATIM
			)
			doca_app_sources(nvme_emulation NVME_SOURCES
				nvme_emulation.c
				host/doca_transport.c
				host/nvmf_doca_io.c
				host/nvmf_rpc.c
				host/nvme_pci_common.c
			)
			doca_add_executable(${DOCA_PREFIX}nvme_emulation
				SOURCES ${NVME_SOURCES}
				INCLUDE_DIRS
					"${CMAKE_CURRENT_SOURCE_DIR}/nvme_emulation/common"
					"${spdk_prefix}/include"
				DEPS
					${NVME_DEPS}
					${UUID_TARGET}
					"${NVME_DPA_ARCHIVE}"
					${NVME_SPDK_LIBS}
					"${spdk_isal_prefix}/libisal.a"
				LINK_OPTIONS
					-Wl,-no-as-needed
					"-Wl,-rpath=${spdk_prefix}/lib"
					"-Wl,-rpath-link=${spdk_prefix}/lib"
					"-Wl,-rpath=${spdk_dpdk_prefix}/lib"
					"-Wl,-rpath-link=${spdk_dpdk_prefix}/lib"
					-Wl,-u,needed_symbol
					-Wl,--no-whole-archive
			)
			add_custom_target(nvme_emulation_dpa_code DEPENDS "${NVME_DPA_ARCHIVE}")
			add_dependencies(${DOCA_PREFIX}nvme_emulation nvme_emulation_dpa_code)
		endif()
	endif()
endif()

doca_prepare_app(pcc PCC_ENABLED PCC_DEPS PCC_INCLUDES)
if(PCC_ENABLED)
	doca_pkg_variable(DOCA_COMMON_LIBDIR "doca-common" "libdir")
	set(PCC_DEVICE_BUILD_DIR "${CMAKE_BINARY_DIR}/pcc/device/build_dpacc")
	set(PCC_DEVICE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pcc/device")
	if(enable_pcc_application_tx_counter_sampling)
		set(PCC_ENABLE_TX_COUNTER_SAMPLING "true")
	else()
		set(PCC_ENABLE_TX_COUNTER_SAMPLING "false")
	endif()
	function(doca_add_pcc_device_archive PCC_APP_NAME OUT_ARCHIVE)
		set(PCC_KEEP_DIR "${PCC_DEVICE_BUILD_DIR}/${PCC_APP_NAME}")
		set(PCC_ARCHIVE "${PCC_KEEP_DIR}/${PCC_APP_NAME}.a")
		add_custom_command(
			OUTPUT "${PCC_ARCHIVE}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${PCC_KEEP_DIR}"
			COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/pcc/build_device_code.sh"
				"${PCC_DEVICE_SRC_DIR}/"
				"${PCC_DEVICE_BUILD_DIR}"
				"${DOCA_COMMON_LIBDIR}"
				"${PCC_APP_NAME}"
				"${PCC_KEEP_DIR}"
				"${PCC_ENABLE_TX_COUNTER_SAMPLING}"
				"${dpacc_mcpu}"
			DEPENDS
				"${CMAKE_CURRENT_SOURCE_DIR}/pcc/build_device_code.sh"
			VERBATIM
		)
		set(${OUT_ARCHIVE} "${PCC_ARCHIVE}" PARENT_SCOPE)
	endfunction()
	doca_add_pcc_device_archive(pcc_rp_rtt_template_app PCC_RP_RTT_TEMPLATE_ARCHIVE)
	doca_add_pcc_device_archive(pcc_rp_switch_telemetry_app PCC_RP_SWITCH_TELEMETRY_ARCHIVE)
	doca_add_pcc_device_archive(pcc_np_switch_telemetry_app PCC_NP_SWITCH_TELEMETRY_ARCHIVE)
	add_custom_target(pcc_device_archives
		DEPENDS
			"${PCC_RP_RTT_TEMPLATE_ARCHIVE}"
			"${PCC_RP_SWITCH_TELEMETRY_ARCHIVE}"
			"${PCC_NP_SWITCH_TELEMETRY_ARCHIVE}"
	)
	doca_app_sources(pcc PCC_SOURCES
		host/pcc.c
		host/pcc_core.c
	)
	doca_add_executable(${DOCA_PREFIX}pcc
		SOURCES ${PCC_SOURCES}
		INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/pcc/host"
		DEPS
			${PCC_DEPS}
			"${PCC_RP_RTT_TEMPLATE_ARCHIVE}"
			"${PCC_RP_SWITCH_TELEMETRY_ARCHIVE}"
			"${PCC_NP_SWITCH_TELEMETRY_ARCHIVE}"
	)
	add_dependencies(${DOCA_PREFIX}pcc pcc_device_archives)
endif()

doca_prepare_app(psp_gateway PSP_ENABLED PSP_DEPS PSP_INCLUDES)
if(PSP_ENABLED)
	if(NOT flag_enable_grpc_support)
		message(WARNING "Skipping compilation of DOCA Application - psp_gateway - Missing gRPC support.")
	elseif(NOT JSON_C_FOUND)
		message(WARNING "Skipping compilation of DOCA Application - psp_gateway - Missing json-c support.")
	else()
		set(PSP_GRPC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/psp_gateway/grpc")
		set(PSP_GRPC_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/psp_gateway/grpc")
		file(MAKE_DIRECTORY "${PSP_GRPC_BINARY_DIR}")
		set(PSP_PROTO "${PSP_GRPC_SOURCE_DIR}/psp_gateway.proto")
		set(PSP_PROTO_CC "${PSP_GRPC_BINARY_DIR}/psp_gateway.pb.cc")
		set(PSP_PROTO_H "${PSP_GRPC_BINARY_DIR}/psp_gateway.pb.h")
		set(PSP_GRPC_CC "${PSP_GRPC_BINARY_DIR}/psp_gateway.grpc.pb.cc")
		set(PSP_GRPC_H "${PSP_GRPC_BINARY_DIR}/psp_gateway.grpc.pb.h")
		set(PSP_PROTO_PY "${PSP_GRPC_BINARY_DIR}/psp_gateway_pb2.py")
		set(PSP_GRPC_PY "${PSP_GRPC_BINARY_DIR}/psp_gateway_pb2_grpc.py")
		add_custom_command(
			OUTPUT "${PSP_PROTO_CC}" "${PSP_PROTO_H}"
			COMMAND "${PROTOC_EXECUTABLE}" "--cpp_out=${PSP_GRPC_BINARY_DIR}" "--proto_path=${PSP_GRPC_SOURCE_DIR}" "${PSP_PROTO}"
			DEPENDS "${PSP_PROTO}"
			VERBATIM
		)
		add_custom_command(
			OUTPUT "${PSP_GRPC_CC}" "${PSP_GRPC_H}"
			COMMAND "${PROTOC_EXECUTABLE}" "--grpc_out=${PSP_GRPC_BINARY_DIR}" "--proto_path=${PSP_GRPC_SOURCE_DIR}" "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}" "${PSP_PROTO}"
			DEPENDS "${PSP_PROTO}"
			VERBATIM
		)
		add_custom_command(
			OUTPUT "${PSP_PROTO_PY}"
			COMMAND "${PROTOC_EXECUTABLE}" "--python_out=${PSP_GRPC_BINARY_DIR}" "--proto_path=${PSP_GRPC_SOURCE_DIR}" "${PSP_PROTO}"
			DEPENDS "${PSP_PROTO}"
			VERBATIM
		)
		add_custom_command(
			OUTPUT "${PSP_GRPC_PY}"
			COMMAND "${PROTOC_EXECUTABLE}" "--grpc_out=${PSP_GRPC_BINARY_DIR}" "--proto_path=${PSP_GRPC_SOURCE_DIR}" "--plugin=protoc-gen-grpc=${GRPC_PY_PLUGIN_EXECUTABLE}" "${PSP_PROTO}"
			DEPENDS "${PSP_PROTO}"
			VERBATIM
		)
		add_library(psp_gateway_grpc STATIC "${PSP_PROTO_CC}" "${PSP_GRPC_CC}")
		target_include_directories(psp_gateway_grpc PUBLIC "${PSP_GRPC_BINARY_DIR}")
		target_compile_options(psp_gateway_grpc PRIVATE ${GRPC_CXX_OPTIONS})
		target_link_libraries(psp_gateway_grpc PRIVATE ${GRPC_DEPS})
		set_property(TARGET psp_gateway_grpc PROPERTY CXX_STANDARD 17)
		add_custom_target(psp_gateway_py_proto ALL DEPENDS "${PSP_PROTO_PY}")
		add_custom_target(psp_gateway_py_grpc ALL DEPENDS "${PSP_GRPC_PY}")

		doca_app_sources(psp_gateway PSP_SOURCES
			psp_gw_flows.cpp
			psp_gw_params.cpp
			psp_gw_svc_impl.cpp
			psp_gw_pkt_rss.cpp
			psp_gw_utils.cpp
			${COMMON_DIR}/dpdk_utils.c
			${SAMPLES_DIR}/common.c
			psp_gateway.cpp
		)
		doca_add_executable(${DOCA_PREFIX}psp_gateway
			CPP17
			SOURCES ${PSP_SOURCES}
			INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/psp_gateway"
			DEPS ${PSP_DEPS} psp_gateway_grpc ${GRPC_DEPS} ${JSON_C_TARGET}
			C_OPTIONS ${GRPC_C_OPTIONS}
			CXX_OPTIONS ${GRPC_CXX_OPTIONS}
			LINK_OPTIONS ${GRPC_LINK_OPTIONS}
		)
	endif()
endif()

doca_simple_c_app(secure_channel
	SOURCES
		secure_channel_core.c
		${COMMON_DIR}/comch_utils.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		secure_channel.c
)

doca_simple_c_app(simple_fwd_vnf
	SOURCES
		simple_fwd_vnf.c
		simple_fwd.c
		simple_fwd_ft.c
		simple_fwd_pkt.c
		simple_fwd_port.c
		simple_fwd_vnf_core.c
		${SAMPLES_DIR}/common.c
		${SAMPLES_DIR}/doca_flow/flow_common.c
		${COMMON_DIR}/dpdk_utils.c
		${COMMON_DIR}/utils.c
	INCLUDE_DIRS
		${SAMPLES_DIR}
		${SAMPLES_DIR}/doca_flow
)

doca_prepare_app(storage STORAGE_ENABLED STORAGE_DEPS STORAGE_INCLUDES)
if(STORAGE_ENABLED)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
		message(WARNING "Storage application does not currently support this operating system")
	else()
		set(STORAGE_COMMON
			storage_common/binary_content.cpp
			storage_common/buffer_utils.cpp
			storage_common/control_channel.cpp
			storage_common/control_message.cpp
			storage_common/doca_utils.cpp
			storage_common/file_utils.cpp
			storage_common/io_message.cpp
			storage_common/ip_address.cpp
			storage_common/posix/os_utils.cpp
			storage_common/posix/tcp_socket.cpp
		)
		set(STORAGE_DEFS STORAGE_APP_LZ4_SW_LIB_AVAILABLE=0)
		doca_pkg_check("liblz4" LZ4_TARGET LZ4_FOUND)
		if(LZ4_FOUND)
			list(APPEND STORAGE_COMMON storage_common/lz4_sw_context.cpp)
			set(STORAGE_DEFS STORAGE_APP_LZ4_SW_LIB_AVAILABLE=1)
			doca_app_sources(storage STORAGE_GGA_SOURCES
				gga_offload_sbc_generator.cpp
				${STORAGE_COMMON}
			)
			doca_add_executable(${DOCA_PREFIX}storage_gga_offload_sbc_generator
				CPP17
				SOURCES ${STORAGE_GGA_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/storage"
				DEPS ${STORAGE_DEPS} ${LZ4_TARGET}
				COMPILE_DEFINITIONS ${STORAGE_DEFS}
			)
			list(APPEND STORAGE_DEPS ${LZ4_TARGET})
		else()
			message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}storage_gga_offload_sbc_generator - Missing library liblz4")
		endif()

		if(is_host)
			doca_app_sources(storage STORAGE_INITIATOR_SOURCES
				initiator_comch.cpp
				${STORAGE_COMMON}
			)
			doca_add_executable(${DOCA_PREFIX}storage_initiator_comch
				CPP17
				SOURCES ${STORAGE_INITIATOR_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/storage"
				DEPS ${STORAGE_DEPS}
				COMPILE_DEFINITIONS ${STORAGE_DEFS}
			)
		endif()
		if(is_dpu)
			doca_app_sources(storage STORAGE_ZERO_COPY_SOURCES
				comch_to_rdma_zero_copy.cpp
				${STORAGE_COMMON}
			)
			doca_add_executable(${DOCA_PREFIX}storage_comch_to_rdma_zero_copy
				CPP17
				SOURCES ${STORAGE_ZERO_COPY_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/storage"
				DEPS ${STORAGE_DEPS}
				COMPILE_DEFINITIONS ${STORAGE_DEFS}
			)
			doca_app_sources(storage STORAGE_GGA_OFFLOAD_SOURCES
				comch_to_rdma_gga_offload.cpp
				${STORAGE_COMMON}
			)
			doca_add_executable(${DOCA_PREFIX}storage_comch_to_rdma_gga_offload
				CPP17
				SOURCES ${STORAGE_GGA_OFFLOAD_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/storage"
				DEPS ${STORAGE_DEPS}
				COMPILE_DEFINITIONS ${STORAGE_DEFS}
			)
		endif()
		doca_app_sources(storage STORAGE_TARGET_SOURCES
			target_rdma.cpp
			${STORAGE_COMMON}
		)
		doca_add_executable(${DOCA_PREFIX}storage_target_rdma
			CPP17
			SOURCES ${STORAGE_TARGET_SOURCES}
			INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/storage"
			DEPS ${STORAGE_DEPS}
			COMPILE_DEFINITIONS ${STORAGE_DEFS}
		)
	endif()
endif()

doca_simple_c_app(stream_receive_perf
	SOURCES
		stream_receive_perf_core.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		stream_receive_perf.c
)

doca_simple_c_app(switch
	SOURCES
		switch_core.c
		${COMMON_DIR}/dpdk_utils.c
		${COMMON_DIR}/utils.c
		${COMMON_DIR}/flow_parser.c
		${COMMON_DIR}/flow_pipes_manager.c
		${SAMPLES_DIR}/common.c
		${SAMPLES_DIR}/doca_flow/flow_common.c
		${SAMPLES_DIR}/doca_flow/flow_switch_common.c
		switch.c
	INCLUDE_DIRS
		${SAMPLES_DIR}
		${SAMPLES_DIR}/doca_flow
)

doca_prepare_app(telemetry TELEMETRY_ENABLED TELEMETRY_DEPS TELEMETRY_INCLUDES)
if(TELEMETRY_ENABLED)
	if(enable_telemetry_traceback)
		if(JSON_C_FOUND)
			doca_app_sources(telemetry TELEMETRY_TRACEBACK_SOURCES
				traceback/traceback.cpp
				traceback/traceback_core.cpp
			)
			doca_add_executable(${DOCA_PREFIX}telemetry_traceback
				CPP17
				SOURCES ${TELEMETRY_TRACEBACK_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/telemetry/traceback"
				DEPS ${TELEMETRY_DEPS} ${JSON_C_TARGET}
			)
		else()
			message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}telemetry_traceback - Missing library json-c")
		endif()
	else()
		message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}telemetry_traceback")
	endif()

	if(enable_telemetry_opentelemetry)
		find_package(opentelemetry-cpp QUIET CONFIG PATHS "/usr/lib/cmake/opentelemetry-cpp")
		doca_pkg_check("libcurl" CURL_TARGET CURL_FOUND)
		if(NOT TARGET opentelemetry-cpp::api)
			message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}telemetry_opentelemetry - Missing OpenTelemetry-cpp SDK")
		elseif(NOT CURL_FOUND)
			message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}telemetry_opentelemetry - Missing Curl support")
		else()
			doca_app_sources(telemetry TELEMETRY_OTEL_SOURCES
				opentelemetry/opentelemetry.cpp
				opentelemetry/opentelemetry_core.cpp
			)
			doca_add_executable(${DOCA_PREFIX}telemetry_opentelemetry
				CPP17
				SOURCES ${TELEMETRY_OTEL_SOURCES}
				INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/telemetry/opentelemetry"
				DEPS
					${TELEMETRY_DEPS}
					opentelemetry-cpp::api
					opentelemetry-cpp::otlp_http_metric_exporter
					opentelemetry-cpp::http_client_curl
					${CURL_TARGET}
			)
		endif()
	else()
		message(STATUS "Skipping compilation of DOCA Application - ${DOCA_PREFIX}telemetry_opentelemetry")
	endif()
endif()

doca_prepare_app(time_sync TIME_SYNC_ENABLED TIME_SYNC_DEPS TIME_SYNC_INCLUDES)
if(TIME_SYNC_ENABLED)
	if(is_dpu)
		find_program(MPICC_EXECUTABLE mpicc)
		if(NOT MPICC_EXECUTABLE)
			message(WARNING "Skipping compilation of time_sync - missing mpicc")
		else()
			doca_pkg_variable(DOCA_DPA_LIBDIR "doca-dpa" "libdir")
			doca_debug_mode(TIME_SYNC_DEBUG_MODE)
			set(TIME_SYNC_OUTPUT "${CMAKE_BINARY_DIR}/time_sync/${DOCA_PREFIX}time_sync_dpu")
			add_custom_command(
				OUTPUT "${TIME_SYNC_OUTPUT}"
				COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/time_sync"
				COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/build_time_sync.sh"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu"
					"${CMAKE_BINARY_DIR}"
					"${b_sanitize}"
					"${TIME_SYNC_DEBUG_MODE}"
					"${dpacc_mcpu}"
					"${DOCA_DPA_LIBDIR}"
				DEPENDS
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/build_time_sync.sh"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/build_device_code.sh"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/time_sync_dpu.c"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/time_sync_dpu_core.c"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/common/time_sync_common.c"
					"${CMAKE_CURRENT_SOURCE_DIR}/time_sync/dpu/device/time_sync_dev.c"
				VERBATIM
			)
			add_custom_target(${DOCA_PREFIX}time_sync ALL DEPENDS "${TIME_SYNC_OUTPUT}")
		endif()
	else()
		doca_app_sources(time_sync TIME_SYNC_HOST_SOURCES
			common/time_sync_common.c
			host/time_sync_host.c
			host/time_sync_host_core.c
		)
		doca_add_executable(${DOCA_PREFIX}time_sync_host
			SOURCES ${TIME_SYNC_HOST_SOURCES}
			INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/time_sync"
			DEPS ${TIME_SYNC_DEPS}
		)
	endif()
endif()

if(JSON_C_FOUND)
	doca_simple_c_app(upf_accel
		SOURCES
			upf_accel.c
			upf_accel_pipeline.c
			upf_accel_json_parser.c
			upf_accel_flow_processing.c
			upf_accel_print_header.c
			upf_accel_smf_default_init.c
			${COMMON_DIR}/dpdk_utils.c
			${COMMON_DIR}/packet_parser.c
			${SAMPLES_DIR}/common.c
			${SAMPLES_DIR}/doca_flow/flow_common.c
		INCLUDE_DIRS
			${SAMPLES_DIR}
			${SAMPLES_DIR}/doca_flow
		EXTRA_DEPS
			${JSON_C_TARGET}
	)
elseif(enable_all_applications OR enable_upf_accel)
	message(WARNING "Skipping compilation of DOCA Application - upf_accel - Missing json-c")
endif()

doca_prepare_app(urom_rdmo UROM_ENABLED UROM_DEPS UROM_INCLUDES)
if(UROM_ENABLED)
	if(is_dpu)
		doca_app_sources(urom_rdmo UROM_DPU_SOURCES
			dpu/worker_rdmo.c
			dpu/worker_rdmo_ops.c
		)
		doca_add_shared_library(worker_rdmo
			SOURCES ${UROM_DPU_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/urom_rdmo/common"
			DEPS ${UROM_DEPS}
		)
		set_target_properties(worker_rdmo PROPERTIES PREFIX "")
	else()
		doca_app_sources(urom_rdmo UROM_HOST_SOURCES
			host/urom_rdmo.c
			host/urom_rdmo_core.c
			host/worker_rdmo.c
			${SAMPLES_DIR}/common.c
			${SAMPLES_DIR}/doca_urom/urom_common.c
			${COMMON_DIR}/utils.c
		)
		doca_add_executable(${DOCA_PREFIX}urom_rdmo
			SOURCES ${UROM_HOST_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/urom_rdmo/common"
				"${SAMPLES_DIR}/doca_urom"
			DEPS ${UROM_DEPS}
		)
	endif()
endif()

if(is_dpu)
	doca_prepare_app(vblk_pci_dev VBLK_ENABLED VBLK_DEPS VBLK_INCLUDES)
	if(VBLK_ENABLED)
		doca_pkg_check("libbsd" VBLK_LIBBSD_TARGET VBLK_LIBBSD_FOUND)
		if(VBLK_LIBBSD_FOUND)
			list(APPEND VBLK_DEPS ${VBLK_LIBBSD_TARGET})
		endif()
		doca_app_sources(vblk_pci_dev VBLK_SOURCES
			vblk_pci.c
			pci_spec_tlp.c
			vblk_ctrl.c
			vblk_io_ctx.c
			vblk_tlp_ctx.c
			vblk_pci_dev.c
			vblk_pci_dev_core.c
			vblk_mpool.c
			${COMMON_DIR}/utils.c
			${SAMPLES_DIR}/common.c
		)
		doca_add_executable(${DOCA_PREFIX}vblk_pci_dev
			SOURCES ${VBLK_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/vblk_pci_dev"
				"${SAMPLES_DIR}"
			DEPS ${VBLK_DEPS}
		)
		doca_app_sources(vblk_pci_dev VBLK_LU_COMMON
			vblk_pci_lu.c
			pci_spec_tlp_lu.c
			vblk_ctrl_lu.c
			vblk_mpool_lu.c
			vblk_ipc.c
			vblk_handover.c
			${COMMON_DIR}/utils.c
			${SAMPLES_DIR}/common.c
		)
		doca_app_sources(vblk_pci_dev VBLK_LU_SOURCES
			${VBLK_LU_COMMON}
			vblk_pci_dev_lu.c
			vblk_pci_dev_tlp.c
			vblk_tlp_ctx_lu.c
		)
		doca_add_executable(${DOCA_PREFIX}vblk_pci_dev_lu
			SOURCES ${VBLK_LU_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/vblk_pci_dev"
				"${SAMPLES_DIR}"
			DEPS ${VBLK_DEPS}
		)
		doca_app_sources(vblk_pci_dev VBLK_EMU_SOURCES
			${VBLK_LU_COMMON}
			vblk_pci_dev_emu.c
			vblk_pci_dev_core_lu.c
			vblk_io_ctx_lu.c
		)
		doca_add_executable(${DOCA_PREFIX}vblk_pci_dev_emu
			SOURCES ${VBLK_EMU_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/vblk_pci_dev"
				"${SAMPLES_DIR}"
			DEPS ${VBLK_DEPS}
		)
	endif()
endif()

doca_prepare_app(virtiofs VIRTIOFS_ENABLED VIRTIOFS_DEPS VIRTIOFS_INCLUDES)
if(VIRTIOFS_ENABLED)
	doca_pkg_check("libnfs" LIBNFS_TARGET LIBNFS_FOUND)
	doca_pkg_check("glib-2.0" GLIB_TARGET GLIB_FOUND)
	if(NOT LIBNFS_FOUND)
		message(WARNING "Skipping compilation of DOCA Application - virtiofs - Missing libnfs package")
	elseif(NOT GLIB_FOUND)
		message(WARNING "Skipping compilation of DOCA Application - virtiofs - Missing glib-2.0 package")
	else()
		doca_app_sources(virtiofs VIRTIOFS_SOURCES
			virtiofs.c
			virtiofs_core.c
			virtiofs_thread.c
			virtiofs_mpool.c
			virtiofs_manager.c
			virtiofs_device.c
			virtiofs_request.c
			nfs_fsdev/nfs_fsdev.c
			nfs_fsdev/nfs_fsdev_pipe.cpp
			nfs_fsdev/nfs_fsdev_io_device.cpp
			nfs_fsdev/nfs_fsdev_recovery.c
		)
		doca_add_executable(${DOCA_PREFIX}virtiofs
			CPP17
			SOURCES ${VIRTIOFS_SOURCES}
			INCLUDE_DIRS
				"${CMAKE_CURRENT_SOURCE_DIR}/virtiofs/nfs_fsdev"
				"${CMAKE_CURRENT_SOURCE_DIR}/virtiofs"
			DEPS ${VIRTIOFS_DEPS} ${LIBNFS_TARGET} ${GLIB_TARGET}
		)
	endif()
endif()

if(is_dpu)
	doca_prepare_app(vnet_pci_dev VNET_ENABLED VNET_DEPS VNET_INCLUDES)
	if(VNET_ENABLED)
		doca_pkg_check("libbsd" VNET_LIBBSD_TARGET VNET_LIBBSD_FOUND)
		if(VNET_LIBBSD_FOUND)
			list(APPEND VNET_DEPS ${VNET_LIBBSD_TARGET})
		endif()
		doca_pkg_check("libibverbs" IBVERBS_TARGET IBVERBS_FOUND)
		if(NOT IBVERBS_FOUND)
			message(WARNING "Skipping compilation of DOCA Application - vnet_pci_dev - Missing libibverbs")
		else()
			doca_app_sources(vnet_pci_dev VNET_SOURCES
				vnet_pci_dev.c
				vnet_pci_dev_core.c
				vnet_pci_device.c
				vnet_pci_dev_lu.c
				${COMMON_DIR}/utils.c
				${SAMPLES_DIR}/common.c
			)
			doca_add_executable(${DOCA_PREFIX}vnet_pci_dev
				SOURCES ${VNET_SOURCES}
				INCLUDE_DIRS
					"${CMAKE_CURRENT_SOURCE_DIR}/vnet_pci_dev"
					"${SAMPLES_DIR}"
				DEPS ${VNET_DEPS} ${IBVERBS_TARGET}
			)
		endif()
	endif()
endif()

doca_simple_c_app(yara_inspection
	SOURCES
		yara_inspection_core.c
		${COMMON_DIR}/utils.c
		${SAMPLES_DIR}/common.c
		yara_inspection.c
)
if(is_host AND TARGET doca_yara_inspection)
	message(WARNING "yara_inspection is only supported on the DPU, and is not supported on the Host")
endif()
