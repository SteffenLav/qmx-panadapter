# minimp3 — vendored

Upstream: <https://github.com/lieff/minimp3> (single header, `minimp3.h`).

Licence: **CC0 1.0 Universal** (`LICENSE`, copied from upstream). Public-domain
dedication: no copyleft, no source-disclosure duty, no attribution
requirement, no notice requirement. That is why it is here — this firmware is
GPL-3 and the decoder it replaced, RealNetworks' Helix
(`chmorgan/esp-libhelix-mp3`), ships under the **RealNetworks Public Source
License 1.0**, which is copyleft over combined works (§4.1) and
GPL-incompatible.

Vendored 2026-11-18 from upstream `master`.
`sha256(upstream minimp3.h) = 1f7f6893987a69e3703f6a2ef24a3e91a5d7d67dae73893901800fe8bfb77eff`

## In-tree, not a registry dependency — deliberate

One CC0 header in the tree is the point. A registry component would put the
licence back behind a manifest label, and the label is exactly what was wrong
about Helix: the Component Registry calls `chmorgan/esp-libhelix-mp3`
Apache-2.0, which covers only the wrapper, not the RPSL submodule it pulls in.

It also keeps the project free of the standing-patch problem. The one change
this project needs is *inside* the decoder, so as a managed component it would
have needed an `apply_*.ps1` script re-run after every `idf.py fullclean` and
every release's `rm -r managed_components/`. In the tree it is just source.

## The one local modification

`mp3dec_decode_frame()`'s `mp3dec_scratch_t scratch` is `static` instead of a
plain local, behind the `MINIMP3_SCRATCH_STORAGE` macro. See the comment at
the site — it carries the measurements, the non-reentrancy cost and what is
still unverified. CC0 permits the edit without conditions.

`main/audio/minimp3_impl.c` is the single translation unit that instantiates
the implementation and sets that macro to put the 16,256 B scratch in PSRAM.

## Guarding the vendoring

`tools/mp3_ab/` holds the host A/B harness and the procedure. It decodes 15
Layer III ISO conformance vectors through this header and through pristine
upstream and requires the PCM to be bit-identical. Run it after any re-vendor.
