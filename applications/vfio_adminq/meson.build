vfio_adminq_inc_dirs = include_directories('.', 'common', 'dev-be', 'pci-fe')
vfio_adminq_c_args = base_c_args + ['-Wall', '-Wextra', '-Werror']

dependency_dpa = dependency('doca-dpa')
dpa_build_script = find_program(meson.current_source_dir() / '../nvme_emulation/build_device_code.sh')
dpa_include_dir = dependency_dpa.get_variable(pkgconfig : 'includedir')
dpa_lib_dir = dependency_dpa.get_variable(pkgconfig : 'libdir')

dpa_code = custom_target(
	'vfio_adminq_doorbell',
	input: files('dev-be/dpa/doorbell_dev.c'),
	command: [
		dpa_build_script,
		'-I' + dpa_include_dir + ' -I' + meson.current_source_dir() / 'dev-be',
		'@OUTDIR@',
		'nv-dpa-bf3',
		'-L' + dpa_lib_dir + ' -ldoca_dpa_dev -ldoca_dpa_dev_comm',
		'srdma_doorbell_app',
		files('dev-be/dpa/attributes.yaml'),
		'@OUTPUT@',
		'@INPUT@',
	],
	depfile: '@BASENAME@.d',
	output: ['vfio_adminq_doorbell.a'],
)

executable(
	'doca_vfio_adminq_dev_be',
	files('dev-be/main.c', 'dev-be/gemini_client.c', 'dev-be/srdma_backend.c',
	      'dev-be/srdma_admin.c', 'common/srdma_uar_ipc.c') + dpa_code,
	c_args: vfio_adminq_c_args,
	dependencies: app_dependencies,
	include_directories: vfio_adminq_inc_dirs,
	install: install_apps,
)

executable(
	'doca_vfio_adminq_pci_fe',
	files('pci-fe/main.c', 'pci-fe/pci_fe.c', 'pci-fe/gemini_server.c',
	      'common/srdma_uar_ipc.c'),
	c_args: vfio_adminq_c_args,
	dependencies: app_dependencies,
	include_directories: vfio_adminq_inc_dirs,
	install: install_apps,
)

executable(
	'vfio_adminq_host_emu',
	files('host-emu/host_emu.c'),
	c_args: vfio_adminq_c_args,
	dependencies: [dependency('threads')],
	include_directories: vfio_adminq_inc_dirs,
	install: install_apps,
)

srdma_control_test = executable(
	'vfio_adminq_srdma_control_test',
	files('tests/srdma_control_test.c', 'dev-be/srdma_admin.c',
	      'common/srdma_uar_ipc.c'),
	c_args: vfio_adminq_c_args,
	include_directories: vfio_adminq_inc_dirs,
	install: false,
)

test('vfio_adminq_srdma_control', srdma_control_test)
