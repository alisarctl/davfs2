project('davfs2', 'c',
  version : '1.8.0',
  license: 'GPL3+',
  meson_version : '>= 0.58.0',
  default_options : ['warning_level=1'])

cc = meson.get_compiler('c')

# Use GNU extensions if available
add_project_arguments('-D_GNU_SOURCE', language: 'c')

sys = build_machine.system()

if sys != 'freebsd' and sys != 'linux'
  error('Unsupported Operating system ', sys)
endif

cdata = configuration_data()

prefix = get_option('prefix')
davfs2_sbindir = prefix / get_option('sbindir')
davfs2_sysconfdir = prefix / get_option('sysconfdir') / meson.project_name()
davfs2_localstatedir = get_option('statedir')
davfs2_datadir = prefix / get_option('datadir')
davfs2_sharedir = prefix / davfs2_datadir / meson.project_name()
mandir = prefix / get_option('mandir')

davfs2_certdir = get_option('certdir')

cdata.set_quoted('DAV_SYS_CONF_DIR', davfs2_sysconfdir)
cdata.set_quoted('DAV_SYS_RUN', davfs2_localstatedir)
cdata.set_quoted('DAV_DATA_DIR', davfs2_datadir)

cdata.set_quoted('DAV_SYS_CACHE', get_option('cachedir'))
cdata.set_quoted('PROGRAM_NAME', 'mount.davfs')
cdata.set_quoted('PACKAGE_STRING', 'davfs2 ' / meson.project_version())
cdata.set_quoted('PACKAGE_VERSION', meson.project_version())
cdata.set_quoted('PACKAGE_TARNAME', 'davfs2')
cdata.set_quoted('PACKAGE', 'davfs2')
cdata.set_quoted('DAV_USER', get_option('dav_user'))
cdata.set_quoted('DAV_GROUP', get_option('dav_group'))
cdata.set_quoted('DAV_LOCALSTATE_DIR', davfs2_localstatedir)
cdata.set_quoted('DAV_CLICERTS_DIR', 'private')
cdata.set_quoted('DAV_CONFIG', 'davfs2.conf')
cdata.set_quoted('DAV_CERTS_DIR', davfs2_certdir)
cdata.set_quoted('DAV_SECRETS', 'secrets')
cdata.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/alisarctl/davfs2')

enable_nls = false

if get_option('nls')
  msgfmt = find_program('msgfmt', required: false)
  if msgfmt.found()
    enable_nls = true
    localedir = join_paths(get_option('datadir'), 'locale')
    cdata.set_quoted('LOCALEDIR', localedir)
  endif
endif

set_variable('enable_nls', enable_nls)
cdata.set('ENABLE_NLS', enable_nls)

system_headers = [
  'dirent.h',
  'fcntl.h',
  'iconv.h',
  'libintl.h',
  'langinfo.h',
  'limits.h',
  'locale.h',
  'mntent.h',
  'stddef.h',
  'stdint.h',
  'stdlib.h',
  'string.h',
  'sys/file.h',
  'sys/stat.h',
  'sys/mount.h',
  'sys/time.h',
  'sys/select.h',
  'sys/types.h',
  'syslog.h',
  'termios.h',
  'unistd.h',
  'utime.h',
  'fstab.h',
  'paths.h'
]

foreach h : system_headers
  if cc.has_header(h)
    cdata.set('HAVE_' + h.underscorify().to_upper(), 1)
  endif
endforeach

required_functions = [
  'ftruncate',
  'mkdir',
  'select',
  'setlocale',
  'fopencookie',
  'strtoull',
  'strerror',
  'strcasecmp',
  'strdup',
  'strchr',
  'strpbrk',
  'utime'
]

if meson.version().version_compare('>= 1.3.0')
  foreach f : required_functions
    cc.has_function(f, required : true)
  endforeach

  cc.has_function('canonicalize_file_name')

  if cc.has_header('error.h') and cc.has_function('error', prefix: '#include <error.h>')
    # has error.h and -lerror (if on musl) is not required
    cdata.set('HAVE_ERROR_H', 1)
  endif

endif

configure_file(
  output : 'config.h',
  configuration: cdata,
)

config_inc = include_directories('.')

neon_dep = dependency('neon', required : true)
run_command('po4a', '--version', check : true)

subdir('src')
subdir('etc')
if enable_nls
subdir('po')
endif
if get_option('man') == true
subdir('man')
endif

doc_files = ['AUTHORS',
  'BUGS',
  'COPYING',
  'ChangeLog',
  'FAQ',
  'INSTALL',
  'README.md',
  'README.translators',
  'THANKS',
  'TODO'
]

if get_option('doc') == true

  docdir_option = get_option('docdir')

  if docdir_option == ''
    davfs2_docdir = davfs2_datadir / 'doc' / meson.project_name()
  else
    davfs2_docdir = docdir_option
  endif

  davfs2_doc_files = install_data(
    doc_files,
    install_dir : davfs2_docdir
  )
endif

summary({
  'Prefix' : prefix,
  'CC'     : cc.get_id(),
  'Neon'   : neon_dep.version(),
  'Host'   : sys
})
