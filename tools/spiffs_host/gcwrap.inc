static s32_t gc_watch(u32_t len, const char *who) {
    int ok0 = integrity_q(0); long fb = fs.free_blocks, al = fs.stats_p_allocated, de = fs.stats_p_deleted;
    s32_t r = SPIFFS_gc(&fs, len); SPIFFS_clearerr(&fs);
    int ok1 = integrity_q(0);
    if (ok0 && !ok1) printf("** %s gc(%u) BROKE the log: before free_blocks %ld alloc %ld deleted %ld; ret %d; after free_blocks %d\n",
                           who, (unsigned)len, fb, al, de, (int)r, (int)fs.free_blocks);
    return r; }
