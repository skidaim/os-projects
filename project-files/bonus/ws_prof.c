// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAGE_SHIFT 12UL
#define PAGE_SIZE  (1UL << PAGE_SHIFT)
#define PMD_SHIFT  21UL
#define PMD_SIZE   (1UL << PMD_SHIFT)
#define PTE_PER_PT 512UL  // 4096/8, for 4KB pages 

#define PERR_EXIT(msg) do { perror(msg); exit(1); } while (0)

static inline uint64_t ns_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) PERR_EXIT("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline size_t div_up(size_t x, size_t y) {
    return (x + y - 1) / y;
}

// Map only the EPT window covering [start, start+len) 
static unsigned long *map_ept_window(uintptr_t start, size_t len, size_t *map_len, unsigned long *first_chunk)
{
    if (len == 0) {
        errno = EINVAL;
        PERR_EXIT("len==0");
    }

    uintptr_t end = start + len - 1;

    *first_chunk = (unsigned long)(start >> PMD_SHIFT);
    unsigned long last_chunk = (unsigned long)(end >> PMD_SHIFT);
    unsigned long chunks = last_chunk - *first_chunk + 1;

    *map_len = (size_t)chunks * PAGE_SIZE;
    off_t offset = (off_t)(*first_chunk) * (off_t)PAGE_SIZE;

    int fd = open("/dev/ept", O_RDONLY);
    if (fd < 0) PERR_EXIT("open /dev/ept");

    void *ptr = mmap(NULL, *map_len, PROT_READ, MAP_PRIVATE, fd, offset);
    if (ptr == MAP_FAILED) PERR_EXIT("mmap ept window");

    close(fd);
    return (unsigned long *)ptr;
}

// EPT scan: count pages with Present bit set 
static size_t scan_ept(unsigned long *ept, uintptr_t start, size_t len, unsigned long first_chunk)
{
    size_t pages = div_up(len, PAGE_SIZE);
    size_t present = 0;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t va = start + i * PAGE_SIZE;

        unsigned long chunk = (unsigned long)(va >> PMD_SHIFT);
        unsigned long chunk_off = chunk - first_chunk;

        unsigned long pte_idx = (unsigned long)((va >> PAGE_SHIFT) & (PTE_PER_PT - 1));

        // EPT layout: one 4KB EPT page per chunk; each holds 512 PTE values 
        unsigned long pte = ept[chunk_off * PTE_PER_PT + pte_idx];

        if (pte & 1UL)
            present++;
    }
    return present;
}

// Baseline: mincore() scan 
static size_t scan_mincore(void *start, size_t len, unsigned char *vec)
{
    size_t pages = div_up(len, PAGE_SIZE);
    if (mincore(start, len, vec) != 0) PERR_EXIT("mincore");

    size_t resident = 0;
    for (size_t i = 0; i < pages; i++)
        if (vec[i] & 1)
            resident++;

    return resident;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [MB=1024] [touch_pct=10] [iters=50]\n"
        "  MB        : region size in MiB (default 1024)\n"
        "  touch_pct : percent of pages to touch (0..100, default 10)\n"
        "  iters     : timed iterations (default 50)\n"
        "Run as root.\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    size_t mb = 1024;
    int touch_pct = 10;
    int iters = 50;

    if (argc > 1) mb = (size_t)atoll(argv[1]);
    if (argc > 2) touch_pct = atoi(argv[2]);
    if (argc > 3) iters = atoi(argv[3]);
    if (argc > 4) usage(argv[0]);

    if (touch_pct < 0 || touch_pct > 100 || iters <= 0 || mb == 0)
        usage(argv[0]);

    size_t len = mb * 1024ULL * 1024ULL;
    size_t pages = div_up(len, PAGE_SIZE);

    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) PERR_EXIT("mmap region");
    madvise(p, len, MADV_NOHUGEPAGE);

    // touch roughly touch_pct% of pages (uniform stride) 
    if (touch_pct > 0) {
        size_t touch_pages = (pages * (size_t)touch_pct) / 100;
        if (touch_pages == 0) touch_pages = 1;

        size_t stride = pages / touch_pages;
        if (stride == 0) stride = 1;

        for (size_t i = 0; i < pages; i += stride)
            p[i * PAGE_SIZE] = 1;
    }

    // map the EPT window for this region 
    size_t ept_len = 0;
    unsigned long first_chunk = 0;
    unsigned long *ept = map_ept_window((uintptr_t)p, len, &ept_len, &first_chunk);

    unsigned char *vec = (unsigned char *)malloc(pages);
    if (!vec) PERR_EXIT("malloc vec");

    // warm up both methods once (avoid first-fault noise) 
    (void)scan_ept(ept, (uintptr_t)p, len, first_chunk);
    (void)scan_mincore(p, len, vec);

    // timed EPT scan 
    uint64_t t_ept_sum = 0;
    size_t count_ept = 0;
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = ns_now();
        count_ept = scan_ept(ept, (uintptr_t)p, len, first_chunk);
        uint64_t t1 = ns_now();
        t_ept_sum += (t1 - t0);
    }

    // timed mincore scan 
    uint64_t t_min_sum = 0;
    size_t count_min = 0;
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = ns_now();
        count_min = scan_mincore(p, len, vec);
        uint64_t t1 = ns_now();
        t_min_sum += (t1 - t0);
    }

    double avg_ept = (double)t_ept_sum / (double)iters;
    double avg_min = (double)t_min_sum / (double)iters;

    // throughput in Mpages/s: pages / ns * 1e9 / 1e6 = pages * 1000 / ns 
    double thr_ept = (avg_ept > 0.0) ? ((double)pages * 1000.0 / avg_ept) : 0.0;
    double thr_min = (avg_min > 0.0) ? ((double)pages * 1000.0 / avg_min) : 0.0;

    printf("Region: %zu MiB (%zu pages), touched: %d%%\n", mb, pages, touch_pct);
    printf("EPT window mapped: %zu KiB (offset chunk=%lu)\n", ept_len / 1024, first_chunk);
    printf("\nMethod    | Avg Time (ns) | Throughput (Mpages/s) | Count\n");
    printf("----------+---------------+------------------------+--------\n");
    printf("EPT read  | %-13.0f | %-22.2f | %zu\n", avg_ept, thr_ept, count_ept);
    printf("mincore() | %-13.0f | %-22.2f | %zu\n", avg_min, thr_min, count_min);

    printf("\nSpeedup: %.2fx\n", (avg_min > 0.0) ? (avg_min / avg_ept) : 0.0);

    munmap(ept, ept_len);
    munmap(p, len);
    free(vec);
    return 0;
}
