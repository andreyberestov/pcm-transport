# Contributing to PCM Transport

PCM Transport welcomes contributions that fit the project's scope, architecture,
and compatibility requirements. There is no Contributor License Agreement (CLA)
or Developer Certificate of Origin (DCO) requirement.

## Contribution terms

By submitting a contribution to PCM Transport for inclusion in the project, you
confirm that:

- you created the contribution yourself, or otherwise have the right to submit
  and license it;
- you agree that the contribution may be distributed as part of PCM Transport
  under `GPL-3.0-only`;
- any applicable third-party copyright, attribution, and license notices are
  preserved;
- the contribution does not knowingly include material that is incompatible with
  the project's license.

You retain copyright in your contribution unless a separate written agreement
states otherwise.

## Technical requirements

Contributions should:

- use portable C++17 and avoid GNU-only C++ extensions;
- preserve `CMAKE_CXX_EXTENSIONS OFF`;
- use the GTK 3 C API from C++ code;
- preserve the supported baseline of GTK 3.16, GLib/GIO 2.52, FFmpeg 4.4 and libFLAC 1.3.3;
- use public FFmpeg APIs and explicit version/capability checks;
- do not replace direct FFmpeg library integration with ffmpeg/ffprobe subprocesses for playback or metadata probing;
- preserve `RangeLimitedDecoder` bounded transport for entries with known sample ranges;
- avoid unnecessary new dependencies;
- preserve existing behavior unless the change intentionally modifies it;
- follow the existing architecture and coding style;
- be reviewed and tested before acceptance.

A contribution may be declined even when technically valid if it does not fit the project's scope, architecture, dependency policy, or maintenance goals.

## AI-assisted contributions

AI coding tools may be used as development aids. Contributors remain responsible
for reviewing and testing their submissions, verifying the provenance of included
material, and ensuring compliance with applicable copyright and license terms.
Disclosure of the specific AI tool used is not required.
