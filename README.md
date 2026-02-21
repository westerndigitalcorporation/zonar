# Zonar - Zoned Filesystem Viewer

Zonar is a GTK4-based graphical viewer for monitoring and visualising a zoned
filesystem. It provides real-time visualization of a zoned filesystem's
blockgroups. Zonar supports zoned filesystems on both regular and zoned block
devices.

Zonar supports both local and remote operation through a client-server
architecture, allowing inspecting file systems on remote systems.

Currently, Zonar supports only the XFS file system.

## Zonar in action (XFS on a zloop device)

Below is a demonstration of using Zonar in auto-refresh mode to inspect file
writes to an XFS file system on a zloop device. It also demonstrates the
file search feature which shows the files extents on the respective blockgroups.

The XFS allocation groups (AGs) which use conventional zones on the disk are
shown in pink, and XFS real-time groups (RGs) which use sequential write
required zones on the disk are shown in green. The evolving red shading shows
file data being written into XFS RGs blockgroups in real time.

![Zonar Preview](Docs/preview/znr_preview.gif)

The demonstration below visualises garbage collection in action on XFS. Note
that this demonstration is sped up for most of the write sequence up until
garbage collection occurs towards the end.

![Zonar GC Preview](Docs/preview/znr_gc.gif)

## Requirements

### System Requirements

- Linux kernel with zoned block device support
- XFS filesystem with zoned storage support (kernel v6.15 and above)
- Root privileges (required for accessing the block devices and file system
  information)

### Build Dependencies

Building Zonar requires the GTK4 and Adwaita libraries and their
development header files. XFS support requires the xfsprogs development header
files.

- **GTK 4**: GUI toolkit
- **libadwaita**: Building blocks for modern adaptive GNOME applications
- **GLib 2.0**: Core application building blocks
- **Math library**: For mathematical operations
- **xfsprogs development headers**: Version 6.15 and above

Zonar server can optionally be built independently without these requirements.

#### Fedora/RHEL

```bash
$ sudo dnf install gcc make autoconf automake libtool pkgconfig gtk4-devel \
     glib2-devel libadwaita-devel xfsprogs-devel
```

#### Ubuntu/Debian

```bash
$ sudo apt install gcc make autoconf automake libtool pkg-config libgtk-4-dev \
     libadwaita-dev libglib2.0-dev xfslibs-dev
```

#### xfs-progs

For XFS support, Zonar requires xfsprogs-devel 6.15 or higher. You may need to
manually install it if the system package is an older version. See xfsprogs
README for more installation options.

```bash
$ git clone git://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git

$ cd xfsprogs-dev
$ make
$ sudo make install-dev
```

## Building

To build zonar from source, navigate to the project directory and execute the
following.

```bash
$ cd zonar
$ ./autogen.sh
$ ./configure
$ make
```

Two binaries will be created:
 - src/zonar      (GUI client)
 - src/zonar_srv  (Server daemon)

For systems where the GUI dependencies are not installed, the build can be
limited to Zonar server only by configuring with the option *--disable-gui*.

```bash
$ ./configure --disable-gui
$ make
```

For debugging (gdb), Zonar can be built with the *--enable-debug* option.
```bash
$ ./configure --enable-debug
$ make
```

To clean the object files from the source directory, run:

```bash
$ make clean
```

To remove all generated files including configure, run:

```bash
$ make distclean
```

## Installation

To install both *zonar* and *zonar_srv*, run:

```bash
$ sudo make install
```

To install only *zonar_srv* on a machine without the GUI dependencies installed,
run:

```bash
$ sudo make install-srv
```

If the build was configured with zonar GUI disabled, the regular *make install*
can also be used but will be limited to just installing *zonar_srv*.

## Usage

To run Zonar GUI to inspect a local file system, simply run:

```bash
$ sudo zonar /mnt
```

To inspect a file system on a remote machine (or on a VM), run Zonar server
daemon on the remote system or the VM owning the mounted file system:

```bash
$ sudo zonar_srv /mnt
```

The default connection port used to listen for client connections is port 49152.
The option `--port` can be used to specify a custom port:

```bash
$ sudo zonar_srv --port 50000 /mnt
```

A local run of Zonar client GUI can then connect to the server (IP address
x.y.z.s as an example).

```bash
$ zonar --connect x.y.z.s
```

For cases where the remote server running the file system to inspect does not
have a routable IP address (e.g. a class C address), the reverse connection
mode can be used. With this, the local Zonar GUI client waits for the remote
Zonar server to connect to it.

On the local machine, first start Zonar GUI client in listen mode:

```bash
$ zonar --listen
```

On the remote server running owning the file system, the connection can be
initiated:

```bash
$ sudo zonar_srv --connect x.y.z.c /mnt
```

### Command-Line Options

Zonar GUI Client (*zonar*) accepts the following options.

```
$ zonar --help
Options:
  -v, --verbose            Enable verbose output
  -V, --version            Display version information and exit
  -s, --connect <IP>       Connect to the specified server IP address
  -p, --port <port>        Specify the server connection port
  -l, --listen             Reverse connection mode: wait for a server to
                           connect
```

Zonar server daemon (*zonar_srv*) accepts the following options.

```
$ zonar_srv --help
Options:
  -h, --help               Print help and exit
  -V, --version            Print version and exit
  -v, --verbose            Enable verbose output
  -p, --port <port>        Specify connection port number (default: 49152)
  -c, --connect <ipaddr>   Reverse connection mode: connect to the client at
                           <ipaddr>.
```

## Architecture

### Key Components

- **Core Layer** (`znr.c`, `znr.h`):
  - Main data structures and global state management
  - Device initialization and cleanup
  - Filesystem detection and mounting
  - Mode selection (local, client, server)

- **Device Layer** (`znr_device.c`, `znr_device.h`):
  - Low-level interface to zoned block devices using Linux kernel ioctls
  - Zone reporting and management (BLKREPORTZONE ioctl)
  - Device geometry and capability queries
  - Block device operations

- **Blockgroups Layer** (`znr_bg.c`, `znr_bg.h`)
  - Abstraction layer over the device and filesystems
  - Handling mapping blockgroups to physical zones

- **Filesystem Layer** (`znr_fs.c`, `znr_fs.h`):
  - Filesystem abstraction interface
  - File extent retrieval coordination
  - Filesystem type detection
  - Currently supports XFS

- **XFS Layer** (`znr_xfs.c`):
  - XFS-specific extent mapping using GETBMAPX ioctl
  - Zone offset calculation for file extents (fsmap)

- **Network Layer** (`zonar_src.c`, `znr_net.c`, `znr_net.h`):
  - Client-server protocol implementation
  - TCP socket communication (default port: 49152)
  - Request/response handling for device info, zone reports, and extent queries
  - Server daemon mode and client connection management

- **GUI Layer** (`znr_gui.c`):
  - GTK4-based visualization and user interface
  - Real-time blockgroup monitoring
  - File extent visualization
  - Interactive blockgroup and extent inspection
  - Auto-refreshing blockgroups

- **Applications**:
  - `zonar.c`: GUI client with local or remote mode support
  - `zonar_srv.c`: Server daemon for remote device access

### Client-Server Protocol

The network protocol uses a custom binary format over TCP:

- **Magic Number**: `0x7a6f6e65` ("zone" in ASCII)
- **Request Types**:
  - `ZNR_NET_MNTDIR_INFO`: Get mount directory and filesystem type
  - `ZNR_NET_DEV_INFO`: Get device information (geometry, zone count, etc.)
  - `ZNR_NET_DEV_REP_ZONES`: Get zone report for a range of zones
  - `ZNR_NET_FILE_EXTENTS`: Get extent mapping for a specific file
  - `ZNR_NET_EXTENTS_IN_RANGE`: Get all file extents in the sector range
                                specified.
  - `ZNR_NET_BLOCKGROUPS`: Get the blockgroups of the mounted filesystem

- **Data Format**: All multi-byte integers are transmitted in network byte order
                   (big-endian)
- **Error Handling**: Server returns errno codes in responses for error
                      conditions

## Features

- **Blockgroup Visualization**: Real-time display of blockgroups
- **Write Pointer Tracking**: Visual representation of write pointer positions
                              within blockgroups (For zoned block devices)
- **File Extent Mapping**: Shows which files occupy which blockgroups.
- **Remote Inspection**: Inspect zoned filesystems on devices on remote systems
- **XFS Support**: Support for Zoned XFS
- **Interactive UI**: Click blockgroups to see detailed information and
                      associated files

## Contributing

We welcome contributions to Zonar! Please follow the Linux kernel coding style
and submit pull requests for review.

## Authors

- Wilfred Mallawa (wilfred.mallawa@wdc.com)
- Damien Le Moal (damien.lemoal@wdc.com)
- Alistair Francis (alistair.francis@wdc.com)

- Based on [libzbd](https://github.com/westerndigitalcorporation/libzbd)
- Based on [xfsprogs](https://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git)

## License

Zonar is licensed under the GNU General Public License v2.0 or later
(GPL-2.0-or-later).

See the SPDX license identifiers in the source files for details.

## Additional Resources

- [Zoned Storage Documentation](https://zonedstorage.io/)
