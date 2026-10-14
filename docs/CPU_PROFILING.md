# Geometry CPU investigation

The sampled CPU preparation total is now split into setup, scene/effect
classification, diagnostics, vertex bindings, constant upload/binding,
resources/materials, and pixel shader/state lookup/binding. A separate stage
measures geometry buffer uploads (Unlock and UP helper paths). It is not a
subset of PrepareDraw. These are elapsed CPU times including driver waits,
not GPU execution or a whole-game thread profile.

Detailed measurements run on one frame in 30. Extra timestamp calls add some
overhead to sampled frames; compare builds with identical profiling settings.
CSV averages use sampled frames only. Cumulative counters track duplicate
constant writes, successful VS/PS uploads and submitted geometry upload bytes.
Use differences between CSV rows, not cumulative totals, for rates.

The optimization only skips bit-identical constant writes. Embedded vertex
shader declaration constants use the same comparison. Pending dirty flags
and written-register tracking are preserved. Geometry buffer upload semantics
and shader math remain unchanged. Failed mappings do not count as uploads.

Large partial dynamic geometry writes may still copy the complete buffer on
discard; no unsafe partial-discard change is made without measurements.

Shader declaration kind and water bytecode classification are now computed
when their shader objects are created. Draw, material and water paths read
immutable fields, never re-decode the bytecode. Shader handles are append-only;
deletion destroys the classification with the shader object.

Draw-path, sun-material and normal-map diagnostics use numeric VS/PS keys and
independent event bits. Repeated combinations no longer format strings or
take the log mutex. The diagnostic cache has a last-pair fast path and a
4096-pair cap; unknown pairs past that cap are not logged. This affects only
diagnostics, not rendering. Explicit frame traces remain available.

Base-renderer state optimization: identical D3D8 render/texture-stage state
setters return before updating PS constants or diagnostics. Existing pending
dirty flags are not cleared. Per-stage sampler selections compare all ten
effective descriptor words (including forced-filter configuration) before
building a string and searching the shared sampler object cache. This caches
selection, not GPU bindings: all original binding calls remain in place, so
helper passes cannot invalidate this optimization. Objects remain owned by
the existing sampler cache through device Reset. New cumulative CSV counters
record repeated state writes and sampler selection hits/lookups.

The automatic shadow frame trace is cancelled, including pending scheduling,
when the master effects switch is off. Previously a trace scheduled by a
shadowed frame could start later in base mode and format every draw forever:
completion required a shadow draw, impossible with effects disabled. Explicit
CPU/GPU profiling and CSV collection remain enabled independently of effects.
