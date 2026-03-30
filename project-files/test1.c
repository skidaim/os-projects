// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define STR_SIZE (1ULL << 8)
#define EPT_SIZE (1ULL << 39)
#define MEM_SIZE (STR_SIZE << 15) /* 2^8 * 2^15 = 2^23 = 8MB */

#define PERR_RET(cond, str) \
    do {                    \
        if (cond) {         \
            perror(str);    \
            return 1;       \
        }                   \
    } while (0)             \

void decode(char *msg, char *ptr, unsigned long *ept)
{
    memset(msg, 0, STR_SIZE);

    for (size_t i = 0; i < STR_SIZE; i++) {
        for (int j = 0; j < 8; j++) {
            unsigned long va = (unsigned long)&ptr[(8 * i + j) << 12];
            unsigned long pte = ept[va >> 12];

            if (pte & 0x1)
                msg[i] |= (1 << j);
        }
    }
}

void encode(char *msg, char *mem) {
    for (size_t i = 0; i < STR_SIZE; i++)
        for (int j = 0; j < 8; j++)
            if (msg[i] & (1 << j))
                mem[(8 * i + j) << 12] = 0x10;
}

void encode2(char *msg, char *mem) {
    for (size_t i = 0; i < STR_SIZE; i++) {
        for (int j = 0; j < 8; j++) {
            mem[(8 * i + j) << 12] = 0x10;
            if ((msg[i] & (1 << j)) == 0)
                munmap(&mem[(8 * i + j) << 12], 1ULL << 12);
        }
    }
}

void init(char *msg, size_t len) {
    srand(time(NULL));
    for (int i = 0; i < len; i++)
        msg[i] = rand() % 255;
}

int main(void) {
    char buf[STR_SIZE] = {0}, buf2[STR_SIZE] = {0}, buf3[STR_SIZE] = {0};

    char *ptr1 = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    PERR_RET(ptr1 == MAP_FAILED, "mmap");

    char *ptr2 = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    PERR_RET(ptr2 == MAP_FAILED, "mmap");

    int fd_ept = open("/dev/ept", O_RDONLY);
    PERR_RET(fd_ept < 0, "open");

    unsigned long *ept = mmap(NULL, EPT_SIZE, PROT_READ, MAP_PRIVATE, fd_ept, 0);
    PERR_RET(ept == MAP_FAILED, "mmap");
    PERR_RET(close(fd_ept), "close");

    PERR_RET(mprotect(ept, EPT_SIZE, PROT_NONE), "mprotect");
    PERR_RET(mprotect(ept, EPT_SIZE, PROT_READ), "mprotect");

    init(buf, STR_SIZE);

    encode(buf, ptr1);
    decode(buf2, ptr1, ept);

    encode2(buf, ptr2);
    decode(buf3, ptr2, ept);

    if (memcmp(buf, buf2, STR_SIZE) || memcmp(buf, buf3, STR_SIZE))
        printf("error\n");
    else
        printf("success\n");

    PERR_RET(munmap(ept, EPT_SIZE), "munmap");
    PERR_RET(munmap(ptr1, MEM_SIZE), "munmap");
    PERR_RET(munmap(ptr2, MEM_SIZE), "munmap");

    return 0;
}
