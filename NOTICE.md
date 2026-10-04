# License and source notices

The current graphic kernel driver is licensed under GPL version 2, as stated
by its per-file SPDX identifiers. The complete text is in [LICENSE](LICENSE).
`tests/mouse-latency.c` is an independent MIT-licensed tool,
Copyright (c) 2026 Evanshenf; the permission
notice is in [LICENSES/MIT.txt](LICENSES/MIT.txt). SPDX declarations in individual
files remain authoritative; this notice does not relicense third-party work.
Project-authored documentation and the unmarked
`tests/check-architecture.py` script are distributed under GPL version 2.
Historical files keep their original declarations and source attribution.

Current development and redesign: Evanshenf <archwse@gmail.com>, 2026,
with OpenAI Codex assistance recorded in the commits. Existing project
statements such as `Original author: sw` and `Copyright (C) 2024Ltd. All rights
reserved.` are retained where applicable rather than silently replaced.

## Historical Loongson material

The imported baseline `a483dd089dfe5520fafb786568427364601f373d` and the
first working version `18c116156dead4d665ff216da8c5edd33bc80e0b` remain in Git
history. The baseline includes material matching Linux v6.7
`drivers/gpu/drm/loongson`, including the complete `lsdc_regs.h`, private KMS
structures, and adapted TTM placement, offset, pin and unpin functions.

Original attribution: **Copyright (C) 2023 Loongson Technology Corporation
Limited**. Original license: **GPL-2.0-or-later** (`GPL-2.0+` in those sources).
The GPL version 2 text is supplied in LICENSE; this notice preserves the
original recipients' later-version option for those historical portions.
Fixed upstream links and exact affected locations are recorded in
[docs/source-provenance.md](docs/source-provenance.md).

## Historical Intel material

The baseline's `lsdc_module.c` initialization/rollback loop matches
`drivers/gpu/drm/i915/i915_module.c` at kernel commit
`bca45af5998a05f34b13a2ef11e639bac9c62643`.

Original attribution: **Copyright © 2021 Intel Corporation**.
Original license: **MIT**. The full permission notice, including the warranty
disclaimer, is supplied in [LICENSES/MIT.txt](LICENSES/MIT.txt) and applies
together with this copyright notice to that historical material.

These historical implementations have left the current production source
tree. New hardware programming uses the LS7A2000 V1.3 manual; provenance
comparisons were read-only. Git history is preserved for review, and these
notices accompany its distribution without changing the original commits.
The bounded audit does not prove that every possible source was excluded.

## Other incomplete historical notices

The archived baseline `lsdc_params.c` contains MIT permission text but only
an incomplete `Copyright` line. Its original notice remains unchanged in
Git history; no missing copyright holder is guessed or attributed to Intel.
This file is absent from the current source tree.
