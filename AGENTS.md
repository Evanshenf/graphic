# graphic architecture redesign

Develop in the existing `/home/virgo/graphic` source directory. The local
checkout is a synchronized editing and evidence copy. Preserve baseline
`a483dd0` and first working driver `18c1161` as historical versions.

The user requests high readability and learning value, so redesign module
boundaries instead of preserving incidental i915/Loongson scaffolding.
Separate generic DRM/KMS, memory/resource ownership, and a manual-based
7A2000 hardware backend. Document the data flow and lifetime contracts.

Read-only comparison with old Loongson KMS implementations is explicitly
allowed for provenance analysis only. Do not use them as implementation
references or copy their hardware algorithms into the redesign. New code
must use the official LS7A2000 V1.3 manual and generic kernel interfaces.
Preserve proper attribution for retained code; generic DRM API use alone
is not evidence of copying. Follow target 7.3 kernel style and APIs.
