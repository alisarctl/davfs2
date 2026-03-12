mandata = configuration_data()

mandata.set_quoted('PACKAGE', cdata.get('PACKAGE'))

mandata.set_quoted('PACKAGE_STRING', cdata.get('PACKAGE_STRING'))
mandata.set_quoted('PROGRAM_NAME', cdata.get('PROGRAM_NAME'))
mandata.set_quoted('PACKAGE_BUGREPORT', cdata.get('PACKAGE_BUGREPORT'))

mandata.set_quoted('USER', get_option('dav_user'))
mandata.set_quoted('GROUP', get_option('dav_group'))
mandata.set_quoted('SYS_CONF_DIR', davfs2_sysconfdir)
mandata.set_quoted('SYS_RUN', davfs2_localstatedir)
mandata.set_quoted('SYS_CACHE_DIR', get_option('cachedir'))
mandata.set_quoted('CONFIGFILE', 'davfs2.conf')
mandata.set_quoted('SECRETSFILE', 'secrets')
mandata.set_quoted('CERTS_DIR', 'certs')
mandata.set_quoted('CLICERTS_DIR', 'private')


configure_file(
  input : 'davfs2.conf.5.in',
  output : 'davfs2.conf.5',
  configuration : mandata,
  install_dir: mandir + '/man5',
)

configure_file(
  input : 'mount.davfs.8.in',
  output : 'mount.davfs.8',
  configuration : mandata,
  install_dir: mandir + '/man8',
)

configure_file(
  input : 'umount.davfs.8.in',
  output : 'umount.davfs.8',
  configuration : mandata,
  install_dir: mandir + '/man8',
)

run_command('po4a', 'po4a.conf', check:true)

if get_variable('enable_nls')
  subdir('de')
  subdir('es')
endif
