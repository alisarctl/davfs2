# 1 INTRODUCTION

davfs2 is a Linux and FreeBSD file system in userspace (FUSE) driver that
allows you to mount a WebDAV resource into your Unix file system tree. So - and
that is what makes davfs2 different - applications can use it without knowing
about WebDAV.  You may edit WebDAV resources using standard applications that
interact with the file system as usual.

davfs2 supports SSL and proxy, HTTP authentication (basic and digest)
and client certificates.

## 1.1 WHAT DAVFS2 IS INTENDED FOR

- If you have documents you want to access from different locations, store
  them on a WebDAV server accessible via Internet. Mount them with davfs2
  from wherever you want.

- Use a WebDAV server as workspace for a geographically distributed work group.

- A web site may be made accessible to the developers via WebDAV. So they
  can mount with davfs2 and edit in place.

## 1.2 WHAT DAVFS2 IS NOT INTENDED FOR

davfs2 is not intended as a replacement for distributed file systems like
nfs, coda, cifs and similar.

When davfs2 mounts a resource, it authenticates with the server using the
user-name and password it got from the mounting user. All requests to the
server are done on behalf of this WebDAV user. davfs2 does not handle different
WebDAV users within one mount. But this would be required for a distributed
file system.

davfs2 is not a generic WebDAV client. davfs2 maps a WebDAV resource to a file
system. But as the file system interface and the WebDAV protocol are quite
different, this is not possible without losses. As a file system davfs2 cannot
use all the possibilities of WebDAV, and most WebDAV servers do not provide all
the information a file system usually requires.

A specialised application with built-in WebDAV capabilities should be able to
make better use of the WebDAV protocol. Whether it really does, depends on the
implementation. But if a free WebDAV enabled application is available, you
might try it first.

davfs2 can't (always) replace local disk space. Due to the nature of WebDAV
davfs2 can't directly redirect reading and writing to the WebDAV server. davfs2
always needs local copies of all open files. So if you have not enough disk
space to hold these local copies, davfs2 will not help.

# 2 SECURITY CONSIDERATIONS

To allow non-root users mounting of WebDAV resources, mount.davfs is run
setuid root. To prevent inexperienced (or even malicious) users from introducing
dangerous content into system directories or other users home directory,
the administrator must have control over user mounts.

- Non-root users can only mount using the normal mount program. There must
  also be an entry in /etc/fstab. This can only be done by root.

- To mount a WebDAV resource users must be member of dav_group (default is
  group 'davfs2'). The administrator may use group membership to allow or
  disallow mounting of WebDAV resources.

mount.davfs starts with effective user-id 'root' to be able to mount. After
mounting it changes its id permanently to that of the mounting user. When
the mounting user is root, the mount.davfs daemon will run as user 'davfs2'.


# 3 MOUNTING

davfs2 comes with three manuals: mount.davfs, umount.davfs and davfs2.conf.

When a normal user mounts a davfs2 file system for the first time, there
is not yet a user configuration file and a secrets file. So you will be asked
for the credentials. mount.davfs will create a hidden directory .davfs2 in
the users home directory, that holds configuration files, the cache and
certificates. You will want to edit this files afterwards.

If you update from an older version, these files already exist and davfs2
will not touch them. To allow mount.davfs installation of newer versions,
you might rename davfs2.conf and secrets and merge your changes into the
new versions.

GUIs like Gnome and KDE provide means to mount file systems listed in
fstab. But at the moment there is no means to ask the user for credentials
etc. You must configure your davfs2 mounts, using davfs2.conf and secrets,
to allow mounting without user interaction for this to work.

davfs2 needs a network connection to mount and also to unmount cleanly. So
automatic mounting at boot time and unmounting at shut down may not work
reliably. By default davfs2 mounts with option '_netdev' to inform the
operating system about this and allow correct handling. Whether this really
works depends on the details of the start-up and shut-down process and will
be different on different systems. So please test before you rely on this.


# 4 TLS / SSL

The key question when using TLS/SSL is whether you can trust in the certificate
the server presents. There is no gain in security when you use strong
encryption for your communication with an attacker. There are also different
opinions on whether you can really trust in certificates issued by the well
known certificate 'authorities'.

Nevertheless davfs2 insists on verification of server certificates. There
are three ways to do this:

- davfs2 will use the CA-certificates of your system to verify the server
  certificate. The server's certificate must be valid and host-name of the
   server must match the subject-alt-name or the common name of the certificate.

- You may store a top-level CA-certificate in the certs directory and set
  option trust_ca_cert in the davfs2.conf directory. This CA-certificate will
  be used instead of the CA-certificates provided by your system. he server's
  certificate must be valid and host-name of the server must match the
  subject-alt-name or the common name of the certificate.
  This is useful when the service provider uses a private CA or the server
  certificate is self-signed.

- You may store the certificate of the server and set option trust_server_cert
  in the davfs2.conf file. In this case the certificate of the server must
  exactly match this certificate, but it does not matter whether it is valid,
  outdated or does not match the server's host-name.

When you use option trust_ca_cert or trust_server_cert it is your responsibility
to get the certificate in a reliable way and care for certificate revocation.
If you can do this it is more secure then relying of well known certificate
authorities (considering recent events).

If a certificate can not be verified, mount.davfs will print information about
the certificate and ask the user. This will only be done before mount.davfs
changes into daemon mode.


# 5 CACHE

davfs2 will store a local copy of all open files in its cache. So make sure
there is enough local disk space available in the cache directory.

There are two reasons for caching:

- The coda kernel file system expects a local copy of the file to act on.

- Many applications, especially those with graphical user interfaces, think
  of file system calls as cheap and quick, which is not true when using a slow
  connection to the Internet. Some graphical interfaces for file handling even
  open every file in every directory they list, forcing davfs2 to download them
  from the server.

To avoid excessive network traffic, davfs2 now saves all downloaded files in a
cache directory and will hold the files, even when the file system is
unmounted. When the same file system is mounted again, it will reuse this
cached files.

To avoid inconsistencies, davfs2 will do a conditional GET whenever a file is
opened (it will ask the server if there is a newer version, and download only
if there is).

Many application use temporary files that will be deleted just after they have
been closed. So whenever a file is newly created or changed, davfs2 will wait
until it is closed and then wait another short period (configurable, default
is 10 seconds) before it will upload the changed version to server. This saves
a lot of unnecessary traffic, but the strategy still has to be enhanced. If
there are many files to be uploaded (e.g. after copying a directory)
mount.davfs may block quite some time, as it has to upload all the files.


# 6 TROUBLESHOOTING

In case davfs2 does not behave as you expect, there is some very useful free
software, to search for the reason:

- Use any browser, telnet and 'openssl s_client' to test whether you can
  connect to the server at all.

- Cadaver is a WebDAV-client with an FTP-like interface. Besides the usual
  FTP-commands it allows you to display and manipulate WebDAV-properties, e.g.
  you can remove stale locks. (http://www.webdav.org/cadaver/)

- You may set option 'debug most" in the davfs2.conf file. This will print a
  lot of debug messages in one of your log files.

- Wireshark will log and analyse the traffic between davfs2 and the server.
  (http://www.wireshark.org/)

- If you have access to the server's log files, they contain valuable
  information.

When sending a bug report, please include

- the exact version of davfs2 and the source where you got it from.

- a complete description of the bug and the actions that lead to the buggy
  behaviour (please note: I usually do not know the acronyms of your favourite
  applications, operating system and server. In many cases I never used them).
  The exact commands you issued on the command line and the messages you got
  from davfs2 are necessary to understand what's going on.
  Please always send the original error and debug messages in full. Don't
  replace them by your interpretation.

- if possible, output from the above mentioned tools.


# 7 KNOWN ISSUES

- If the server does not support RFC 4331 (most servers don't), davfs2 cannot
  calculate the free disk space on the server. But some applications
  (e.g. nautilus) insist on this. So davfs can't help but lie. I tried to
  make the numbers look funny, so you will notice they are faked.

