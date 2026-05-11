install_data('rdii-networkd.service', install_dir : systemunitdir)
install_data('rdii-set-default-loader-entry.service', install_dir : systemunitdir)
install_data('rdii-ssh-setup.service', install_dir : systemunitdir)
install_data('rdii-fetch-config.service', install_dir : systemunitdir)
install_data('rdii-fetch-config-early.service', install_dir : systemunitdir)

rdi_installer = configure_file(
  input: 'rdii-mount-img-part.service.in',
  output: 'rdii-mount-img-part.service',
  configuration: {
    'RDIIDIR': rdiidir,
  },
  install: true,
  install_dir: systemunitdir
)
