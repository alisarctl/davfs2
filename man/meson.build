install_man('davfs2.conf.5',
  'mount.davfs.8',
  'umount.davfs.8'
)

run_command('po4a', 'po4a.conf', check:true)

if get_variable('enable_nls')
  subdir('de')
  subdir('es')
endif
