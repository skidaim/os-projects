// k22test.c - user-space tester for k22tree syscall
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#include <linux/k22info.h>


static long k22tree_syscall(struct k22info *buf, int *ne)
{
    return syscall(467, buf, ne);
}

/*
 * Find the position of parent_pid in the stack (which stores the PIDs
 * along the current path). Search from top down.
 */
static int find_parent(pid_t *stack, int top, pid_t parent_pid)
{
    for (int i = top; i >= 0; i--) {
        if (stack[i] == parent_pid)
            return i;
    }
    return -1;
}

int main(void)
{
    struct k22info *buf = NULL;
    int size = 100; 
    int ne;
    long ret;

    /* find a sufficient buffer size by calling k22tree repeatedly, double each time (we could get return value but whatever) */
    for (;;) {
        free(buf);
        buf = malloc(size * sizeof(*buf));
        if (!buf) {
            return 1;
        }

        printf("- User-space buf. size: %d\n", size);

        ne = size;
        ret = k22tree_syscall(buf, &ne);
        if (ret < 0) {
            free(buf);
            return 1;
        }

        printf("- syscall return val:   %ld\n", ret);

        if (ret <= size) {
            printf("--- OK ---\n\n");
            break;
        }

        size *= 2;

    }

    if (ne <= 0) {
        free(buf);
        return 1;
    }

    /* pretty-print the tree using a stack for indentation */
    
    pid_t *stack;
    int top = 0;
    int i, j;
    struct k22info *p;

    stack = malloc(ne * sizeof(*stack));
    if (!stack) {
        free(buf);
        return 1;
    }

    printf("#comm,pid,ppid,fcldpid,nsblpid,nvcsw,nivcsw,stime\n");

    /* print root (first entry) without leading dashes */
    p = &buf[0];
    printf("%s,%d,%d,%d,%d,%lu,%lu,%lu\n",
            p->comm,
            p->pid,
            p->parent_pid,
            p->first_child_pid,
            p->next_sibling_pid,
            p->nvcsw,
            p->nivcsw,
            p->start_time/1000000000);

    stack[0] = p->pid;
    top = 0;

    for (i = 1; i < ne; i++) {
        int parent_pos;

        p = &buf[i];
        parent_pos = find_parent(stack, top, p->parent_pid);

        top = parent_pos + 1;
        stack[top] = p->pid;

        /* one dash per depth level */
        for (j = 0; j < top; j++)
            putchar('-');

        printf("%s,%d,%d,%d,%d,%lu,%lu,%lu\n",
                p->comm,
                p->pid,
                p->parent_pid,
                p->first_child_pid,
                p->next_sibling_pid,
                p->nvcsw,
                p->nivcsw,
                p->start_time/1000000000);
    }

    free(stack);
    

    free(buf);
    return 0;
}
