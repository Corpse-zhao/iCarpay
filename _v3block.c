/* ── 从 dyld 共享缓存里抠出单个 dylib（v3）──────────────────────
 * v1 的坑：缓存里所有库的 __LINKEDIT 合并堆在缓存末尾，按
 *   (vmaddr - imageAddr) 平铺 → 出来 708MB 的空洞文件。
 * v2 的坑：只跳过 __LINKEDIT 还不够，__DATA/__DATA_CONST 在缓存里
 *   属于另一片地址区，与 __TEXT 相隔 1GB 以上，平铺依旧错位。
 * v3 干脆重排：把 __TEXT / __DATA* 紧排进 0x100000000 起的新地址
 *   空间（16KB 对齐），同步改写每个 segment 与 section 的
 *   addr/fileoff/offset；__LINKEDIT 以及所有 LINKEDIT 类 load
 *   command（symtab / dysymtab / dyld info / fixups / codesign…）
 *   一律清零。产物是"静态样本"——不能 load，但 IDA / objdump /
 *   capstone 能完整看到代码与数据，通常只有几百 KB。
 *   同时写一份 .map.txt 记录每段原始 vmaddr，便于对照回原缓存。只读。 */

#define ICARPAY_MAX_SEG 32

typedef struct {
    uint64_t vmaddr;
    uint64_t size;
    uint64_t fileOffset;
    int fileIndex;
} icarpay_mapping_t;

typedef struct {
    uint32_t cmdOff;
    char name[17];
    uint64_t srcVmaddr;
    uint64_t srcFileOff;
    uint64_t filesize;
    uint64_t newOff;
    uint32_t nsects;
} icarpay_seg_t;

static icarpay_mapping_t icarpay_maps[256];
static int icarpay_mapCount = 0;
static NSString *icarpay_files[8];
static int icarpay_fileCount = 0;

static void icarpay_parse_cache_mappings(NSString *path, int fileIndex) {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh) return;
    NSData *h = [fh readDataOfLength:0x200];
    if (h.length < 0x200 || memcmp(h.bytes, "dyld_v1", 7) != 0) {
        [fh closeFile];
        return;
    }
    const uint8_t *p = h.bytes;
    uint32_t mapOff = *(const uint32_t *)(p + 0x10);
    uint32_t mapCnt = *(const uint32_t *)(p + 0x14);
    if (mapCnt > 48) mapCnt = 48;
    if (!mapCnt) { [fh closeFile]; return; }
    [fh seekToFileOffset:mapOff];
    NSData *md = [fh readDataOfLength:(NSUInteger)mapCnt * 32];
    [fh closeFile];
    if (md.length < (NSUInteger)mapCnt * 32) return;
    for (uint32_t i = 0; i < mapCnt; i++) {
        const uint8_t *r = (const uint8_t *)md.bytes + i * 32;
        if (icarpay_mapCount >= 256) return;
        icarpay_maps[icarpay_mapCount].vmaddr = *(const uint64_t *)(r + 0);
        icarpay_maps[icarpay_mapCount].size = *(const uint64_t *)(r + 8);
        icarpay_maps[icarpay_mapCount].fileOffset =
            *(const uint64_t *)(r + 16);
        icarpay_maps[icarpay_mapCount].fileIndex = fileIndex;
        icarpay_mapCount++;
    }
}

/* 跨分片按 vmaddr 读，段跨 mapping 边界也能拼起来 */
static NSData *icarpay_read_vm(uint64_t vmaddr, uint64_t length) {
    if (!length || length > 64ull * 1024 * 1024) return nil;
    NSMutableData *acc = [NSMutableData dataWithCapacity:(NSUInteger)length];
    uint64_t done = 0;
    while (done < length) {
        uint64_t cur = vmaddr + done;
        int hit = -1;
        for (int i = 0; i < icarpay_mapCount; i++) {
            if (cur >= icarpay_maps[i].vmaddr &&
                cur < icarpay_maps[i].vmaddr + icarpay_maps[i].size) {
                hit = i;
                break;
            }
        }
        if (hit < 0) return nil;
        uint64_t room = icarpay_maps[hit].vmaddr + icarpay_maps[hit].size - cur;
        uint64_t want = length - done;
        if (want > room) want = room;
        NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:
                            icarpay_files[icarpay_maps[hit].fileIndex]];
        if (!fh) return nil;
        [fh seekToFileOffset:icarpay_maps[hit].fileOffset +
                             (cur - icarpay_maps[hit].vmaddr)];
        NSData *d = [fh readDataOfLength:(NSUInteger)want];
        [fh closeFile];
        if (d.length != (NSUInteger)want) return nil;
        [acc appendData:d];
        done += want;
    }
    return acc;
}

/* 某个缓存分片是否带镜像表（只有主缓存有） */
static BOOL icarpay_cache_images(NSString *path, uint64_t *tblOff,
                                 uint32_t *count) {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh) return NO;
    NSData *h = [fh readDataOfLength:0x200];
    [fh closeFile];
    if (h.length < 0x200 || memcmp(h.bytes, "dyld_v1", 7) != 0) return NO;
    const uint8_t *p = h.bytes;
    *tblOff = *(const uint32_t *)(p + 0x18);   /* imagesOffsetOld */
    *count = *(const uint32_t *)(p + 0x1c);    /* imagesCountOld  */
    return (*count > 0 && *count < 40000 && *tblOff >= 0x20);
}

static NSString *dump_cache_image(NSString *cacheDir, NSString *wantedSuffix,
                                  NSString *outputName) {
    icarpay_mapCount = 0;
    icarpay_fileCount = 0;

    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *names = [[fm contentsOfDirectoryAtPath:cacheDir error:NULL]
        filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"SELF BEGINSWITH %@",
                @"dyld_shared_cache_arm64"]];
    if (!names.count) return @"(no cache files)";
    names = [names sortedArrayUsingSelector:
                 @selector(localizedStandardCompare:)];

    for (NSString *n in names) {
        if (icarpay_fileCount >= 8) break;
        NSString *full = [cacheDir stringByAppendingPathComponent:n];
        NSDictionary *attr = [fm attributesOfItemAtPath:full error:NULL];
        if (!attr || [attr fileSize] < 0x200) continue;
        icarpay_files[icarpay_fileCount] = full;
        icarpay_parse_cache_mappings(full, icarpay_fileCount);
        icarpay_fileCount++;
    }
    if (!icarpay_mapCount) return @"(no usable cache mappings)";

    /* 在带镜像表的那一片里按路径后缀找目标库 */
    uint64_t imageAddr = 0;
    NSString *imagePath = nil;
    for (int f = 0; f < icarpay_fileCount && !imageAddr; f++) {
        uint64_t tblOff = 0;
        uint32_t imgCount = 0;
        if (!icarpay_cache_images(icarpay_files[f], &tblOff, &imgCount))
            continue;
        NSFileHandle *fh =
            [NSFileHandle fileHandleForReadingAtPath:icarpay_files[f]];
        if (!fh) continue;
        [fh seekToFileOffset:tblOff];
        NSData *imgData = [fh readDataOfLength:(NSUInteger)imgCount * 32];
        if (imgData.length < (NSUInteger)imgCount * 32) {
            [fh closeFile];
            continue;
        }
        for (uint32_t i = 0; i < imgCount; i++) {
            const uint8_t *rec = (const uint8_t *)imgData.bytes + i * 32;
            uint64_t addr = *(const uint64_t *)(rec + 0);
            uint32_t pathOff = *(const uint32_t *)(rec + 24);
            [fh seekToFileOffset:pathOff];
            NSData *pd = [fh readDataOfLength:300];
            if (!pd.length) continue;
            char buf[300];
            const char *ps = pd.bytes;
            NSUInteger j = 0;
            while (j < pd.length && j < 299 && ps[j]) { buf[j] = ps[j]; j++; }
            buf[j] = 0;
            NSString *p = [NSString stringWithUTF8String:buf];
            if (p && [p hasSuffix:wantedSuffix]) {
                imageAddr = addr;
                imagePath = p;
                break;
            }
        }
        [fh closeFile];
    }
    if (!imageAddr) return @"(image not found in cache)";

    /* 读 mach 头 + load commands */
    NSData *mhData = icarpay_read_vm(imageAddr, 0x1000);
    if (!mhData || mhData.length < 32) return @"(macho header unreadable)";
    if (*(const uint32_t *)mhData.bytes != 0xfeedfacf) return @"(not mach64)";
    uint32_t ncmds = *(const uint32_t *)((const uint8_t *)mhData.bytes + 16);
    uint32_t sizeofcmds =
        *(const uint32_t *)((const uint8_t *)mhData.bytes + 20);
    NSUInteger hdrLen = 32 + (NSUInteger)sizeofcmds;
    if (ncmds > 512 || hdrLen > 0x20000) return @"(load cmds implausible)";
    if (hdrLen > mhData.length) {
        mhData = icarpay_read_vm(imageAddr, hdrLen);
        if (!mhData || mhData.length < hdrLen) return @"(load cmds unreadable)";
    }

    NSMutableData *hdr = [NSMutableData dataWithBytes:mhData.bytes
                                               length:hdrLen];
    uint8_t *hp = hdr.mutableBytes;

    icarpay_seg_t segs[ICARPAY_MAX_SEG];
    int segCount = 0;
    uint32_t off = 32;
    for (uint32_t c = 0; c < ncmds; c++) {
        if (off + 8 > hdrLen) break;
        uint8_t *lc = hp + off;
        uint32_t cmd = *(uint32_t *)lc;
        uint32_t cmdsize = *(uint32_t *)(lc + 4);
        if (cmdsize < 8 || off + cmdsize > hdrLen) break;

        if (cmd == 0x19) {                        /* LC_SEGMENT_64 */
            char nm[17];
            memcpy(nm, lc + 8, 16);
            nm[16] = 0;
            uint64_t filesize = *(uint64_t *)(lc + 48);
            uint32_t nsects = *(uint32_t *)(lc + 64);
            if (strcmp(nm, "__LINKEDIT") == 0 || filesize == 0) {
                /* 不搬：地址与大小清零，否则指向缓存末尾的巨块 */
                uint64_t segVm = *(uint64_t *)(lc + 24);
                *(uint64_t *)(lc + 24) = 0;
                *(uint64_t *)(lc + 32) = 0;
                *(uint64_t *)(lc + 40) = 0;
                *(uint64_t *)(lc + 48) = 0;
                for (uint32_t s = 0; s < nsects; s++) {
                    uint8_t *sc = lc + 72 + s * 80;
                    if (sc + 80 > hp + hdrLen) break;
                    (void)segVm;
                    *(uint64_t *)(sc + 32) = 0;   /* section addr   */
                    *(uint64_t *)(sc + 40) = 0;   /* section size   */
                    *(uint32_t *)(sc + 44) = 0;   /* section offset */
                    *(uint32_t *)(sc + 52) = 0;   /* reloff         */
                    *(uint32_t *)(sc + 56) = 0;   /* nreloc         */
                }
            } else if (segCount < ICARPAY_MAX_SEG) {
                segs[segCount].cmdOff = off;
                memcpy(segs[segCount].name, nm, 17);
                segs[segCount].srcVmaddr = *(uint64_t *)(lc + 24);
                segs[segCount].srcFileOff = *(uint64_t *)(lc + 40);
                segs[segCount].filesize = filesize;
                segs[segCount].newOff = 0;
                segs[segCount].nsects = nsects;
                segCount++;
            }
        } else if (cmd == 0x2 || cmd == 0xb || cmd == 0x22 ||
                   cmd == 0x80000022 || cmd == 0x1d || cmd == 0x1e ||
                   cmd == 0x26 || cmd == 0x29 || cmd == 0x2b ||
                   cmd == 0x2e || cmd == 0x80000033 ||
                   cmd == 0x80000034) {
            /* symtab/dysymtab/dyld_info/codesign/function_starts/… 清零 */
            memset(lc + 8, 0, cmdsize - 8);
        }
        off += cmdsize;
    }
    if (!segCount) return @"(no file-backed segments)";

    /* 让含 mach 头的那一段排第一（fileoff 必须是 0） */
    for (int i = 0; i < segCount; i++) {
        if (segs[i].srcVmaddr != imageAddr) continue;
        if (i != 0) {
            icarpay_seg_t t = segs[0];
            segs[0] = segs[i];
            segs[i] = t;
        }
        break;
    }
    if (segs[0].srcVmaddr != imageAddr) return @"(no segment at image base)";

    /* 重排：0x100000000 起，16KB 对齐，紧挨着放 */
    const uint64_t kBase = 0x100000000ull;
    const uint64_t kAlign = 0x4000ull;
    uint64_t cursor = 0;
    NSMutableString *map = [NSMutableString string];
    for (int i = 0; i < segCount; i++) {
        cursor = (cursor + kAlign - 1) & ~(kAlign - 1);
        uint8_t *lc = hp + segs[i].cmdOff;
        *(uint64_t *)(lc + 24) = kBase + cursor;      /* vmaddr   */
        *(uint64_t *)(lc + 32) = segs[i].filesize;    /* vmsize   */
        *(uint64_t *)(lc + 40) = cursor;              /* fileoff  */
        *(uint64_t *)(lc + 48) = segs[i].filesize;    /* filesize */
        for (uint32_t s = 0; s < segs[i].nsects; s++) {
            uint8_t *sc = lc + 72 + s * 80;
            if (sc + 80 > hp + hdrLen) break;
            uint64_t sa = *(uint64_t *)(sc + 32);
            uint64_t rel = (sa >= segs[i].srcVmaddr)
                               ? sa - segs[i].srcVmaddr : 0;
            *(uint64_t *)(sc + 32) = kBase + cursor + rel;      /* addr   */
            *(uint32_t *)(sc + 44) = (uint32_t)(cursor + rel);  /* offset */
        }
        [map appendFormat:
            @"%s src=0x%llx size=0x%llx -> off=0x%llx\n",
            segs[i].name, segs[i].srcVmaddr, segs[i].filesize, cursor];
        segs[i].newOff = cursor;
        cursor += segs[i].filesize;
    }
    uint64_t total = cursor;
    if (total > 48ull * 1024 * 1024) return @"(repacked image too big, abort)";

    NSMutableData *output = [NSMutableData dataWithData:hdr];
    if (total > hdrLen) [output increaseLengthBy:(NSUInteger)(total - hdrLen)];
    for (int i = 0; i < segCount; i++) {
        NSData *seg = icarpay_read_vm(segs[i].srcVmaddr, segs[i].filesize);
        if (!seg || seg.length != (NSUInteger)segs[i].filesize)
            return [NSString stringWithFormat:
                @"(segment %s unreadable)", segs[i].name];
        [output replaceBytesInRange:
            NSMakeRange((NSUInteger)segs[i].newOff,
                        (NSUInteger)segs[i].filesize)
                          withBytes:seg.bytes];
    }
    /* 第一段的数据从 fileoff 0 开始，会盖掉刚改好的头，最后再压一次 */
    [output replaceBytesInRange:NSMakeRange(0, hdrLen) withBytes:hdr.bytes];

    NSString *docs = @"/var/mobile/Documents";
    NSString *dst = [docs stringByAppendingPathComponent:outputName];
    if (![output writeToFile:dst atomically:YES]) return @"(write failed)";
    chmod(dst.fileSystemRepresentation, 0644);
    chown(dst.fileSystemRepresentation, 501, 501);

    NSString *mapName = [[outputName stringByDeletingPathExtension]
        stringByAppendingString:@".map.txt"];
    NSString *mapPath = [docs stringByAppendingPathComponent:mapName];
    NSString *body = [NSString stringWithFormat:
        @"image=%s\nimage_addr=0x%llx\nsegments=%d\ntotal=0x%llx\n%@",
        imagePath.UTF8String, imageAddr, segCount, total, map];
    [body writeToFile:mapPath atomically:YES
             encoding:NSUTF8StringEncoding error:NULL];
    chmod(mapPath.fileSystemRepresentation, 0644);
    chown(mapPath.fileSystemRepresentation, 501, 501);

    return [NSString stringWithFormat:
        @"%@ ok (%lu bytes, %d segs) image@0x%llx",
        outputName, (unsigned long)output.length, segCount, imageAddr];
}

