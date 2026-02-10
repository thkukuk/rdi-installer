shellcheck_exe = find_program('shellcheck', required : false)

if shellcheck_exe.found()
test('shellcheck-rdi-installer',
  shellcheck_exe,
  args: ['-a', '-x', rdi_installer]
)

test('shellcheck-rdii-ssh-setup',
  shellcheck_exe,
  args: [files('../scripts/rdii-ssh-setup.sh')]
)
endif

test('tst_ifcfg_networkd_1', find_program('tst-ifcfg-networkd-1.sh'))
test('tst_ifcfg_networkd_2', find_program('tst-ifcfg-networkd-2.sh'))
test('tst_ip_networkd_1', find_program('tst-ip-networkd-1.sh'))
