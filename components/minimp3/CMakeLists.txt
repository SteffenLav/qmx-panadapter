# minimp3 (lieff/minimp3), CC0 1.0 - vendored. See README.md for why in-tree.
#
# Header only. The implementation is instantiated exactly once, in
# main/audio/minimp3_impl.c, which is where MINIMP3_SCRATCH_STORAGE is set so
# the 16,256 B scratch lands in PSRAM. Nothing to compile here.
idf_component_register(
    INCLUDE_DIRS "."
)
