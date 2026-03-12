mount_davfs_sources = [
  'mount_davfs.c',
  'dav_fuse.c',
  'cache.c',
  'webdav.c',
  'kernel_interface.c'
]

umount_davfs_sources = [
  'umount_davfs.c'
]

executable('mount.davfs',
  mount_davfs_sources,
  include_directories : config_inc,
  dependencies : [ neon_dep, intl_dep],
  install : true,
  install_dir : davfs2_sbindir
)

executable('umount.davfs',
  umount_davfs_sources,
  include_directories : config_inc,
  dependencies : [ neon_dep, intl_dep],
  install : true,
  install_dir : davfs2_sbindir
)

