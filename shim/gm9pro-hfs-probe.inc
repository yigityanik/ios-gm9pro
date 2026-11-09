/* Read-only HFS metadata fingerprint. No CMD24, filesystem mount or repairs. */
static uint8_t hfs_gpt[32768], hfs_sector[512], hfs_repeat[512], hfs_vh[512];
static uint32_t hfs_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
        (uint32_t)p[2] << 8 | p[3];
}
static uint64_t hfs_le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static int hfs_read_twice(uint32_t lba, uint8_t *out)
{
    hfs_quiet = 1;
    int error = read_block(17, lba, out) || read_block(17, lba, hfs_repeat);
    hfs_quiet = 0;
    if (error) { hex("HFS read failed LBA", lba); return -1; }
    for (unsigned i = 0; i < 512; i++) if (out[i] != hfs_repeat[i]) {
        hex("HFS unstable read LBA", lba); return -1;
    }
    return 0;
}
static void hfs_probe(void)
{
    puts_uart("GM9 HFS METADATA: read-only, repeated reads, no filesystem mount\n");
    if (read32(EMMC + 0x24) & 3) return;
    uint32_t vendor = read32(EMMC + 0x250) & ~(1U << 10);
    write32(EMMC + 0x250, vendor);
    __asm__ volatile("dsb sy" ::: "memory");
    if (read32(EMMC + 0x250) != vendor) return;
    if (hfs_read_twice(1, hfs_sector)) return;
    const char magic[] = "EFI PART";
    for (unsigned i=0;i<8;i++) if(hfs_sector[i]!=(uint8_t)magic[i]) return;
    uint32_t hs=le32(hfs_sector+12), hc=le32(hfs_sector+16);
    if (hs<92 || hs>512) return;
    for(unsigned i=16;i<20;i++) hfs_sector[i]=0;
    if(crc32(hfs_sector,hs)!=hc) return;
    uint64_t table=hfs_le64(hfs_sector+72);
    uint32_t entries=le32(hfs_sector+80), es=le32(hfs_sector+84);
    uint32_t ac=le32(hfs_sector+88);
    uint64_t usable_first=hfs_le64(hfs_sector+40), usable_last=hfs_le64(hfs_sector+48);
    if(!entries || entries>256 || es!=128 || table<2 || table>0xffffffffU ||
        usable_first>=usable_last || usable_last>=0xffffffffU ||
        table+(entries+3)/4>usable_first) return;
    for(unsigned i=0;i<(entries+3)/4;i++)
        if(hfs_read_twice((uint32_t)table+i,hfs_gpt+i*512)) return;
    if(crc32(hfs_gpt,entries*128)!=ac) return;
    uint64_t first=0,last=0;unsigned found=0;
    const char name[]="userdata";
    for(unsigned i=0;i<entries;i++) {
        uint8_t *e=hfs_gpt+i*128;unsigned match=1;
        for(unsigned j=0;j<sizeof(name);j++)
            if(e[56+j*2]!=(uint8_t)name[j] || e[57+j*2]) match=0;
        if(match) { first=hfs_le64(e+32);last=hfs_le64(e+40);found++; }
    }
    if(found!=1 || first<usable_first || last>usable_last || last-first<4 || first>last) return;
    hex("userdata first",first);hex("userdata sectors",last-first+1);
    if(hfs_read_twice((uint32_t)first+2,hfs_vh)) return;
    if(hfs_vh[0]!='H' || (hfs_vh[1]!='X' && hfs_vh[1]!='+')) return;
    hex("HFS header CRC32",crc32(hfs_vh,512));
    hex("HFS attributes",hfs_be32(hfs_vh+4));
    hex("HFS last mounted version",hfs_be32(hfs_vh+8));
    uint32_t bs=hfs_be32(hfs_vh+40), total=hfs_be32(hfs_vh+44);
    hex("HFS block size",bs);hex("HFS total blocks",total);
    hex("HFS free blocks",hfs_be32(hfs_vh+48));
    hex("HFS write count",hfs_be32(hfs_vh+68));
    if(bs!=4096 || (uint64_t)total*8>last-first+1) return;
    uint32_t alloc_start=hfs_be32(hfs_vh+128), alloc_count=hfs_be32(hfs_vh+132);
    hex("allocation extent first",alloc_start);hex("allocation extent blocks",alloc_count);
    const uint32_t target=3059768, count=576;
    uint32_t map_sector=target/4096, bit=target%4096;
    if(bit+count>4096 || !alloc_count || alloc_start>=total ||
        alloc_count>total-alloc_start || map_sector>=(uint64_t)alloc_count*8) return;
    uint64_t lba=first+(uint64_t)alloc_start*8+map_sector;
    if(lba>last || hfs_read_twice((uint32_t)lba,hfs_sector)) return;
    hex("allocation bitmap sector CRC32",crc32(hfs_sector,512));
    unsigned allocated=0;
    for(unsigned n=bit;n<bit+count;n++) allocated+=!!(hfs_sector[n/8]&(0x80U>>(n%8)));
    hex("checked allocation block",target);hex("checked count",count);
    hex("allocated in checked range",allocated);
    puts_uart("HFS METADATA READ PASS; no physical sector was written\n");
}
