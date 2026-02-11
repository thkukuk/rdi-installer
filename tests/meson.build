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
test('tst_ip_networkd_2', find_program('tst-ip-networkd-2.sh'))
test('tst_ip_networkd_3', find_program('tst-ip-networkd-3.sh'))
test('tst_ip_networkd_4', find_program('tst-ip-networkd-4.sh'))
test('tst_ip_networkd_5', find_program('tst-ip-networkd-5.sh'))
test('tst_ip_networkd_6', find_program('tst-ip-networkd-6.sh'))
test('tst_ip_networkd_7', find_program('tst-ip-networkd-7.sh'))
test('tst_ip_networkd_8', find_program('tst-ip-networkd-8.sh'))
test('tst_ip_networkd_9', find_program('tst-ip-networkd-9.sh'))
test('tst_ip_networkd_10', find_program('tst-ip-networkd-10.sh'))
test('tst_ip_networkd_11', find_program('tst-ip-networkd-11.sh'))
test('tst_ip_networkd_12', find_program('tst-ip-networkd-12.sh'))
test('tst_ip_networkd_13', find_program('tst-ip-networkd-13.sh'))
test('tst_ip_networkd_14', find_program('tst-ip-networkd-14.sh'))
test('tst_ip_networkd_15', find_program('tst-ip-networkd-15.sh'))
test('tst_ip_networkd_16', find_program('tst-ip-networkd-16.sh'))

test('tst_route_networkd_1', find_program('tst-route-networkd-1.sh'))
test('tst_route_networkd_2', find_program('tst-route-networkd-2.sh'))
test('tst_route_networkd_3', find_program('tst-route-networkd-3.sh'))
test('tst_route_networkd_4', find_program('tst-route-networkd-4.sh'))

test('tst_nameserver_networkd_1', find_program('tst-nameserver-networkd-1.sh'))

test('tst_peerdns_networkd_1', find_program('tst-peerdns-networkd-1.sh'))
test('tst_peerdns_networkd_2', find_program('tst-peerdns-networkd-2.sh'))