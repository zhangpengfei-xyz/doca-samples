#
# Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

project(DOCA_DEVEMU_SAMPLES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

include(CheckFunctionExists)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

set(DOCA_BUILD_PLATFORM "DPU" CACHE STRING "Build DOCA DevEmu samples for DPU or HOST")
set_property(CACHE DOCA_BUILD_PLATFORM PROPERTY STRINGS DPU HOST)
message(STATUS "Building DOCA DevEmu samples for ${DOCA_BUILD_PLATFORM}")
if(DOCA_BUILD_PLATFORM STREQUAL "DPU")
	add_compile_definitions(DOCA_ARCH_DPU)
endif()

set(DOCA_PREFIX "doca_")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SAMPLES_DIR "${REPO_ROOT}/samples")
set(APPLICATIONS_COMMON_DIR "${REPO_ROOT}/applications/common")

set(DEVEMU_SAMPLE_INCLUDE_DIRS
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${SAMPLES_DIR}"
	"${REPO_ROOT}"
	"${APPLICATIONS_COMMON_DIR}"
)

set(DEVEMU_SAMPLE_C_OPTIONS
	-Wno-missing-braces
)

function(doca_devemu_pkg_check PKG OUT_TARGET)
	string(MAKE_C_IDENTIFIER "${PKG}" PKG_ID)
	string(TOUPPER "${PKG_ID}" PKG_ID)
	set(PKG_PREFIX "DOCA_DEVEMU_PKG_${PKG_ID}")

	if(NOT TARGET "PkgConfig::${PKG_PREFIX}")
		pkg_check_modules(${PKG_PREFIX} REQUIRED IMPORTED_TARGET "${PKG}")
	endif()

	set(${OUT_TARGET} "PkgConfig::${PKG_PREFIX}" PARENT_SCOPE)
endfunction()

function(doca_devemu_pkg_variable OUT_VAR PKG VAR)
	pkg_get_variable(PKG_VAR_VALUE "${PKG}" "${VAR}")
	if(NOT PKG_VAR_VALUE)
		message(FATAL_ERROR "Failed to get pkg-config variable ${VAR} for ${PKG}")
	endif()
	set(${OUT_VAR} "${PKG_VAR_VALUE}" PARENT_SCOPE)
endfunction()

doca_devemu_pkg_check("doca-common" DOCA_COMMON_TARGET)
doca_devemu_pkg_check("doca-devemu" DOCA_DEVEMU_TARGET)
doca_devemu_pkg_check("doca-argp" DOCA_ARGP_TARGET)
if(DOCA_BUILD_PLATFORM STREQUAL "DPU")
	doca_devemu_pkg_check("doca-dpa" DOCA_DPA_TARGET)
	doca_devemu_pkg_check("doca-dma" DOCA_DMA_TARGET)
	doca_devemu_pkg_check("libflexio" LIBFLEXIO_TARGET)
	doca_devemu_pkg_check("libibverbs" LIBIBVERBS_TARGET)
endif()

set(DEVEMU_BASE_DEPS
	Threads::Threads
	${DOCA_COMMON_TARGET}
	${DOCA_DEVEMU_TARGET}
	${DOCA_ARGP_TARGET}
)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_function_exists(strlcpy HAVE_LIBC_STRLCPY)
unset(CMAKE_REQUIRED_DEFINITIONS)
if(HAVE_LIBC_STRLCPY)
	add_compile_definitions(DOCA_USE_LIBC_STRING_L_FUNC)
else()
	pkg_check_modules(LIBBSD QUIET IMPORTED_TARGET "libbsd")
	if(TARGET PkgConfig::LIBBSD)
		list(APPEND DEVEMU_BASE_DEPS PkgConfig::LIBBSD)
		add_compile_definitions(DOCA_USE_LIBBSD RTE_USE_LIBBSD)
	endif()
endif()

add_compile_definitions(DOCA_ALLOW_EXPERIMENTAL_API)
add_compile_options(-Wno-format-zero-length)

function(doca_devemu_add_sample TARGET)
	cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE_DIRS;DEPS" ${ARGN})

	add_executable(${TARGET} ${ARG_SOURCES})
	target_include_directories(${TARGET} PRIVATE
		${DEVEMU_SAMPLE_INCLUDE_DIRS}
		${ARG_INCLUDE_DIRS}
	)
	target_link_libraries(${TARGET} PRIVATE
		${DEVEMU_BASE_DEPS}
		${ARG_DEPS}
	)
	target_compile_options(${TARGET} PRIVATE
		"$<$<COMPILE_LANGUAGE:C>:${DEVEMU_SAMPLE_C_OPTIONS}>"
	)
endfunction()

function(doca_devemu_add_host_sample TARGET)
	if(NOT DOCA_BUILD_PLATFORM STREQUAL "HOST")
		return()
	endif()
	doca_devemu_add_sample(${TARGET} ${ARGN})
endfunction()

function(doca_devemu_add_dpu_sample TARGET)
	if(NOT DOCA_BUILD_PLATFORM STREQUAL "DPU")
		return()
	endif()
	doca_devemu_add_sample(${TARGET} ${ARGN})
endfunction()

function(doca_devemu_add_dpacc_sample SAMPLE_NAME TARGET OUT_ARCHIVE)
	doca_devemu_pkg_variable(DOCA_DPA_LIBDIR "doca-dpa" "libdir")
	doca_devemu_pkg_variable(DOCA_DPA_INCLUDEDIR "doca-dpa" "includedir")

	set(SAMPLE_PROGRAM_NAME "${SAMPLE_NAME}_program")
	set(SAMPLE_DEVICE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${SAMPLE_NAME}/dpu/device/${SAMPLE_NAME}_dpu_kernels_dev.c")
	set(SAMPLE_ATTRIBUTES "${CMAKE_CURRENT_SOURCE_DIR}/${SAMPLE_NAME}/dpu/device/${SAMPLE_NAME}_attributes.yaml")
	set(SAMPLE_DEVICE_BUILD_DIR "${CMAKE_BINARY_DIR}/${SAMPLE_NAME}/dpu/device/build_dpacc")
	set(SAMPLE_ARCHIVE "${SAMPLE_DEVICE_BUILD_DIR}/${SAMPLE_PROGRAM_NAME}.a")

	add_custom_command(
		OUTPUT "${SAMPLE_ARCHIVE}"
		COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/build_dpacc_samples.sh"
			"${CMAKE_BINARY_DIR}"
			"${DOCA_DPA_LIBDIR}"
			"${DOCA_DPA_INCLUDEDIR}"
			"${SAMPLE_DEVICE_SRC}"
			"${SAMPLE_NAME}"
			"${SAMPLE_PROGRAM_NAME}"
			"nv-dpa-bf3"
			"${SAMPLE_ATTRIBUTES}"
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/build_dpacc_samples.sh"
			"${SAMPLE_DEVICE_SRC}"
			"${SAMPLE_ATTRIBUTES}"
		VERBATIM
	)
	add_custom_target(${TARGET}_dpacc DEPENDS "${SAMPLE_ARCHIVE}")
	set(${OUT_ARCHIVE} "${SAMPLE_ARCHIVE}" PARENT_SCOPE)
endfunction()

doca_devemu_add_host_sample(${DOCA_PREFIX}devemu_pci_device_dma_host
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_dma/host/devemu_pci_device_dma_host_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_dma/host/devemu_pci_device_dma_host_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_host_common.c"
		"${SAMPLES_DIR}/common.c"
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_dma_dpu
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_dma/dpu/devemu_pci_device_dma_dpu_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_dma/dpu/devemu_pci_device_dma_dpu_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${DOCA_DMA_TARGET}
		${DOCA_DPA_TARGET}
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_hotplug
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_hotplug/devemu_pci_device_hotplug_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_hotplug/devemu_pci_device_hotplug_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${LIBFLEXIO_TARGET}
		${DOCA_DPA_TARGET}
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_list
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_list/devemu_pci_device_list_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_list/devemu_pci_device_list_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${LIBFLEXIO_TARGET}
		${DOCA_DPA_TARGET}
)

doca_devemu_add_host_sample(${DOCA_PREFIX}devemu_pci_device_db_host
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_db/host/devemu_pci_device_db_host_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_db/host/devemu_pci_device_db_host_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_host_common.c"
		"${SAMPLES_DIR}/common.c"
)

if(DOCA_BUILD_PLATFORM STREQUAL "DPU")
	doca_devemu_add_dpacc_sample(devemu_pci_device_db ${DOCA_PREFIX}devemu_pci_device_db_dpu DEVEMU_PCI_DEVICE_DB_DPA_ARCHIVE)
	doca_devemu_add_sample(${DOCA_PREFIX}devemu_pci_device_db_dpu
		SOURCES
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_db/dpu/host/devemu_pci_device_db_dpu_sample.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_db/dpu/devemu_pci_device_db_dpu_main.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
			"${SAMPLES_DIR}/common.c"
		DEPS
			${LIBFLEXIO_TARGET}
			${DOCA_DPA_TARGET}
			"${DEVEMU_PCI_DEVICE_DB_DPA_ARCHIVE}"
	)
	add_dependencies(${DOCA_PREFIX}devemu_pci_device_db_dpu ${DOCA_PREFIX}devemu_pci_device_db_dpu_dpacc)
endif()

doca_devemu_add_host_sample(${DOCA_PREFIX}devemu_pci_device_msix_host
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_msix/host/devemu_pci_device_msix_host_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_msix/host/devemu_pci_device_msix_host_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_host_common.c"
		"${SAMPLES_DIR}/common.c"
)

if(DOCA_BUILD_PLATFORM STREQUAL "DPU")
	doca_devemu_add_dpacc_sample(devemu_pci_device_msix ${DOCA_PREFIX}devemu_pci_device_msix_dpu DEVEMU_PCI_DEVICE_MSIX_DPA_ARCHIVE)
	doca_devemu_add_sample(${DOCA_PREFIX}devemu_pci_device_msix_dpu
		SOURCES
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_msix/dpu/host/devemu_pci_device_msix_dpu_sample.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_msix/dpu/devemu_pci_device_msix_dpu_main.c"
			"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
			"${SAMPLES_DIR}/common.c"
		DEPS
			${LIBFLEXIO_TARGET}
			${DOCA_DPA_TARGET}
			"${DEVEMU_PCI_DEVICE_MSIX_DPA_ARCHIVE}"
	)
	add_dependencies(${DOCA_PREFIX}devemu_pci_device_msix_dpu ${DOCA_PREFIX}devemu_pci_device_msix_dpu_dpacc)
endif()

doca_devemu_add_host_sample(${DOCA_PREFIX}devemu_pci_device_stateful_region_host
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_stateful_region/host/devemu_pci_device_stateful_region_host_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_stateful_region/host/devemu_pci_device_stateful_region_host_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_host_common.c"
		"${SAMPLES_DIR}/common.c"
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_stateful_region_dpu
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_stateful_region/dpu/devemu_pci_device_stateful_region_dpu_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_stateful_region/dpu/devemu_pci_device_stateful_region_dpu_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${LIBFLEXIO_TARGET}
		${DOCA_DPA_TARGET}
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_tlp_bridge_handler
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_bridge_handler/devemu_pci_device_tlp_bridge_handler_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_bridge_handler/devemu_pci_device_tlp_bridge_handler_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${DOCA_DPA_TARGET}
)

doca_devemu_add_host_sample(${DOCA_PREFIX}devemu_pci_device_tlp_handler_host
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_handler/host/devemu_pci_device_tlp_handler_host_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_handler/host/devemu_pci_device_tlp_handler_host_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_host_common.c"
		"${SAMPLES_DIR}/common.c"
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_pci_device_tlp_handler_dpu
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_handler/dpu/devemu_pci_device_tlp_handler_dpu_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_device_tlp_handler/dpu/devemu_pci_device_tlp_handler_dpu_main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_pci_common.c"
		"${SAMPLES_DIR}/common.c"
	DEPS
		${DOCA_DPA_TARGET}
		${LIBIBVERBS_TARGET}
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_vfs_device_hotplug_unplug
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_vfs_device_hotplug_unplug/devemu_vfs_device_hotplug_unplug_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_vfs_device_hotplug_unplug/devemu_vfs_device_hotplug_unplug_main.c"
		"${SAMPLES_DIR}/common.c"
)

doca_devemu_add_dpu_sample(${DOCA_PREFIX}devemu_vfs_list_devices
	SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_vfs_list_devices/devemu_vfs_list_devices_sample.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/devemu_vfs_list_devices/devemu_vfs_list_devices_main.c"
		"${SAMPLES_DIR}/common.c"
)
