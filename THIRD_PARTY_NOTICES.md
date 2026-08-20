# Third-Party Notices

PCM Transport itself is licensed under the GNU General Public License version
3.0 only (`GPL-3.0-only`). Third-party software used by or distributed with PCM
Transport remains under the licenses of its respective copyright holders.
PCM Transport does not claim ownership of third-party software.

This document describes the primary direct dependencies of PCM Transport and
how third-party notices are handled for the official AppImage distribution. It
is informational and does not replace the license texts or notices that must
accompany a particular third-party component when its license requires them.

## Source and normal system builds

The PCM Transport source tree does not vendor the libraries listed below.
Normal Linux builds dynamically link against libraries supplied by the target
system.

### GTK 3

- Use in PCM Transport: graphical user interface.
- Upstream: https://www.gtk.org/
- Source: https://gitlab.gnome.org/GNOME/gtk
- License: GNU Library/Lesser General Public License, version 2 or later.

### GLib / GObject / GIO

- Use in PCM Transport: main-loop integration, object and signal handling,
  Unicode/string utilities, files and URIs, D-Bus/MPRIS, subprocess handling,
  resources, and platform helpers.
- Upstream: https://gitlab.gnome.org/GNOME/glib
- License: GNU Lesser General Public License version 2.1 or later.

### Pango

- Use in PCM Transport: text layout, wrapping, and ellipsizing in the GTK
  interface.
- Upstream: https://www.pango.org/
- Source: https://gitlab.gnome.org/GNOME/pango
- License: GNU Lesser General Public License version 2.1 or later.

### GdkPixbuf

- Use in PCM Transport: loading embedded application icons and image resources.
- Upstream: https://gitlab.gnome.org/GNOME/gdk-pixbuf
- License: GNU Library/Lesser General Public License, version 2 or later.

### Cairo

- Use in PCM Transport: custom GTK drawing.
- Upstream: https://www.cairographics.org/
- License: dual-licensed under the GNU Lesser General Public License version
  2.1 or the Mozilla Public License version 1.1.

### ALSA library (alsa-lib)

- Use in PCM Transport: direct PCM output and ALSA device/control access.
- Upstream: https://www.alsa-project.org/
- Source: https://github.com/alsa-project/alsa-lib
- License: GNU Lesser General Public License version 2.1 or later.

### libFLAC

- Use in PCM Transport: native FLAC decoding and FLAC-related operations.
- Upstream: https://xiph.org/flac/
- Source: https://github.com/xiph/flac
- License for `libFLAC`: the Xiph.Org BSD-like license in upstream
  `COPYING.Xiph`.

The FLAC project contains components under other licenses. PCM Transport links
against `libFLAC`; the separate `flac` command-line utility is not part of
`libFLAC` and has different licensing terms.

### FFmpeg libraries

PCM Transport links directly to these FFmpeg libraries:

- `libavformat`
- `libavcodec`
- `libavutil`
- `libswresample`

Upstream: https://ffmpeg.org/

FFmpeg licensing depends on the exact build configuration. FFmpeg states that
its default licensing is GNU Lesser General Public License version 2.1 or later,
while builds that enable GPL-covered components are subject to the GNU General
Public License version 2 or later. Other configuration options and external
components can affect the licensing of a particular build.

FFmpeg license information: https://ffmpeg.org/legal.html

Release distributors must determine and comply with the license of the exact
FFmpeg libraries they distribute.

### SoX Resampler (libsoxr)

- Use in PCM Transport: optional high-quality resampling requested through
  FFmpeg `libswresample` when the FFmpeg build exposes SoXR support.
- PCM Transport does not link directly to `libsoxr`.
- Upstream: https://github.com/chirlu/soxr
- License: GNU Lesser General Public License version 2.1 or later.

## Separate optional programs

PCM Transport may invoke programs already installed on the user's system for
specific diagnostics or system integration. These programs are separate works
and remain subject to their own licenses.

The optional FLAC bit-perfect diagnostic can invoke the `flac` command-line
program. PCM Transport does not bundle that program as part of its normal source
build.

## Official AppImage distribution

Official PCM Transport AppImage releases are self-contained binary bundles and
therefore include selected third-party shared libraries and runtime support
files in addition to the PCM Transport executable. The exact dependency set is
build-specific.

The AppImage release process preserves Debian/Ubuntu package copyright notices
for bundled distribution libraries under:

    usr/share/doc/<package>/copyright

It also includes the distribution's common license texts under:

    usr/share/common-licenses/

The release build records AppImage build and FFmpeg information in:

    usr/share/doc/pcm-transport/APPIMAGE_BUILD_INFO.txt

and includes the PCM Transport license and this notice file in the same
`usr/share/doc/pcm-transport/` directory.

The release packaging process also includes the upstream license notices for:

- the AppImage Type 2 runtime, as `APPIMAGE_RUNTIME_LICENSE.txt`;
- `linuxdeploy-plugin-gtk`, as `LINUXDEPLOY_GTK_PLUGIN_LICENSE.txt`.

The AppImage Type 2 runtime is embedded into every Type 2 AppImage. The runtime
license file also identifies third-party code statically linked into the
runtime. The GTK linuxdeploy plugin installs a runtime hook into the AppImage,
so its license notice is retained with the distribution.

The AppImage build process generates a component inventory and SHA-256 checksum
for release verification. The inventory is intended to identify the exact
contents of the release bundle; it is not itself a license notice.

The official Ubuntu-based AppImage build leaves the ALSA runtime library to the
host system when linuxdeploy excludes `libasound`. Bundled FFmpeg, libFLAC, GTK
and other libraries are governed by the licenses and notices applicable to the
exact files included in that release.

## Transitive dependencies

GTK, FFmpeg and other libraries have their own dependency trees. PCM Transport
does not maintain a handwritten exhaustive list of every transitive library,
because the actual AppImage dependency closure can change with the build
system. For bundled Debian/Ubuntu components, package copyright information is
preserved in the AppImage where available, and the release inventory records
what was actually included.

A distributor creating a different self-contained binary bundle must review
that bundle's actual dependency closure and satisfy all applicable license,
notice, source-code, relinking and redistribution requirements for the
components it includes.
