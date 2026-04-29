#include <stdio.h>
#include "utils.h"
#include "fat16.h"

/* Known string to write and verify */
#define TEST_MSG  "Hello FAT16 from DRAM!"

/* Forward declaration */
static void fat_list_dir(FAT16 *f, uint16_t start_cluster, int depth);

static void fat_print_entry(FAT16 *f, uint8_t *e, int depth) {
    /* Build 8.3 name string */
    char name[13];
    int n = 0;
    for (int j = 0; j < 8 && e[j] != ' '; j++) name[n++] = e[j];
    if (e[8] != ' ') {
        name[n++] = '.';
        for (int j = 8; j < 11 && e[j] != ' '; j++) name[n++] = e[j];
    }
    name[n] = '\0';

    uint16_t cluster = fat16_dir_cluster(e);
    uint32_t size    = fat16_dir_size(e);
    uint8_t  attr    = e[11];
    int      is_dir  = (attr & 0x10) != 0;

    /* Indent by depth */
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s%-12s  cluster=%u  size=%u\r\n",
           is_dir ? "[D] " : "    ", name, cluster, size);

    /* Recurse into subdirectory (max 3 levels deep) */
    if (is_dir && cluster >= 2 && depth < 3)
        fat_list_dir(f, cluster, depth + 1);
}

static void fat_list_dir(FAT16 *f, uint16_t start_cluster, int depth) {
    uint16_t cluster = start_cluster;
    while (cluster >= 2 && cluster < 0xFFF7) {
        uint8_t *data = fat16_cluster_ptr(f, cluster);
        uint32_t entries_per_cluster = ((uint32_t)f->spc * f->bps) / 32;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t *e = data + i * 32;
            if (e[0] == 0x00) return;        /* end of directory */
            if (e[0] == 0xE5) continue;      /* deleted          */
            if (e[11] & 0x08) continue;      /* volume label     */
            if (e[0] == '.')  continue;      /* dot / dotdot     */
            fat_print_entry(f, e, depth);
        }
        cluster = fat16_next_cluster(f, cluster);
    }
}

static void fat_list_root(FAT16 *f) {
    uint8_t *dir = fat16_sector(f, f->root_sec);
    printf("\r\n--- Root Directory ---\r\n");
    int found = 0;
    for (uint16_t i = 0; i < f->rdcnt; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if (e[11] & 0x08) continue;        /* volume label */
        fat_print_entry(f, e, 0);
        found++;
    }
    if (!found) printf("  (empty)\r\n");
    printf("----------------------\r\n");
}

static void fat_sanity_test(void) {
    FAT16 vol;

    printf("\r\n=== FAT16 RAM Sanity Test ===\r\n");

    /* ---- 1. Mount ---- */
    if (fat16_mount(&vol) != 0) {
        printf("FAIL [mount]: bad boot sector signature at 0x82100000\r\n");
        return;
    }
    printf("PASS [mount]: bps=%u spc=%u fat_sec=%u root_sec=%u data_sec=%u\r\n",
           vol.bps, vol.spc, vol.fat_sec, vol.root_sec, vol.data_sec);

    fat_list_root(&vol);

    /* ---- 2. Create HELLO.TXT ---- */
    const char name11[11] = "HELLO   TXT";   /* 8.3, space-padded */
    uint16_t first_cluster;
    uint8_t *dirent = fat16_create(&vol, name11, &first_cluster);
    if (!dirent) {
        printf("FAIL [create]: no free dir entry or cluster\r\n");
        return;
    }
    printf("PASS [create]: HELLO.TXT allocated at cluster %u\r\n", first_cluster);

    /* ---- 3. Write known string ---- */
    uint32_t len = (uint32_t)strlen(TEST_MSG) + 1;   /* include null */
    uint8_t *cdata = fat16_cluster_ptr(&vol, first_cluster);
    memcpy(cdata, TEST_MSG, len);

    /* ---- 4. Close: update file size in directory entry ---- */
    fat16_set_size(dirent, len);
    printf("PASS [write]:  wrote %u bytes: \"%s\"\r\n", len, TEST_MSG);

    /* ---- 5. Open again: search root dir for the file ---- */
    uint8_t *found = fat16_find(&vol, name11);
    if (!found) {
        printf("FAIL [open]:  file not found after create\r\n");
        return;
    }

    /* ---- 6. Read back ---- */
    uint16_t fc    = fat16_dir_cluster(found);
    uint32_t fsize = fat16_dir_size(found);
    uint8_t *fdata = fat16_cluster_ptr(&vol, fc);

    /* ---- 7. Compare ---- */
    if (fsize == len && memcmp(fdata, TEST_MSG, len) == 0) {
        printf("PASS [read]:  \"%s\" (%u bytes) matches\r\n", (char *)fdata, fsize);
        printf("=== RESULT: ALL PASS ===\r\n");
    } else {
        printf("FAIL [read]:  size=%u (exp %u) data=\"%s\"\r\n",
               fsize, len, (char *)fdata);
        printf("=== RESULT: FAIL ===\r\n");
    }
}

int main(void) {
    fat_sanity_test();
    while (1) {}
    return 0;
}
