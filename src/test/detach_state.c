/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "util_internal.h"

char path1[PATH_MAX];
char path2[PATH_MAX];

int main(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    pid_t pid = fork();
    if (pid == 0) {
        readlink("/proc/self/exe", path1, sizeof(path1));

        // Check that MAP_GROWSDOWN doesn't get erased
        size_t page_size = sysconf(_SC_PAGESIZE);

        // Let the kernel find us a 512 page gap that's free
        char *pbase = mmap(NULL, page_size * 512, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        test_assert(pbase != MAP_FAILED);
        munmap(pbase, 512*page_size);

        volatile char *p = (char *)mmap(pbase + 509 * page_size, page_size * 3, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_FIXED, -1, 0);
        test_assert(p != MAP_FAILED);

        // Check that /proc/self/mem gets remapped
        uintptr_t newval = 1;
        volatile uintptr_t stackval = 0;
        int fd = open("/proc/self/mem", O_RDWR);
        test_assert(fd >= 0);
        test_assert(sizeof(stackval) == pwrite64(fd, &newval, sizeof(stackval), (off64_t)(uintptr_t)&stackval));
        test_assert(stackval == 1);

        // Check that rlimits survive
        struct rlimit lim1, lim2;
        test_assert(0 == getrlimit(RLIMIT_STACK, &lim1));
        if (lim1.rlim_cur == RLIM_INFINITY) {
            lim1.rlim_cur = 2 * 1024 * 1024; // 2 MiB
        }
        lim1.rlim_cur = 2 * lim1.rlim_cur;
        if (lim1.rlim_max != RLIM_INFINITY && lim1.rlim_cur > lim1.rlim_max) {
            lim1.rlim_cur = lim1.rlim_max;
        }
        test_assert(0 == setrlimit(RLIMIT_STACK, &lim1));

        if (running_under_rr()) {
            rr_detach_teleport();
        }

        test_assert(0 == getrlimit(RLIMIT_STACK, &lim2));
        test_assert(0 == memcmp(&lim1, &lim2, sizeof(struct rlimit)));

        newval = 2;
        test_assert(sizeof(stackval) == pwrite64(fd, &newval, sizeof(stackval), (off64_t)(uintptr_t)&stackval));
        test_assert(stackval == 2);

        // Validate that rr reset the scheduler affinity mask.
        // Unfortunately, we can't just check that it matches that from
        // before the detach, because rr does not yet emulate the correct
        // scheduler mask after it changes it. XXX: Adjust this test when
        // that is fixed.
        cpu_set_t cpus;
        test_assert(0 == sched_getaffinity(0, sizeof(cpu_set_t), &cpus));
        if (CPU_COUNT(&cpus) == 1) {
            // If only one CPU is set either rr messed with us, or only one
            // cpu is available for environmental reasons (e.g. only one CPU
            // is online). To see if that is the case, try resetting the
            // affinity ourselves and if still only one CPU is allowed, let
            // that through.
            memset(&cpus, 0xFF, sizeof(cpu_set_t));
            test_assert(0 == sched_setaffinity(0, sizeof(cpu_set_t), &cpus));
            test_assert(0 == sched_getaffinity(0, sizeof(cpu_set_t), &cpus));
            test_assert(CPU_COUNT(&cpus) == 1);
        }

        atomic_printf("Ptr is %p\n", p);

        // The kernel is picky about MAP_GROWSDOWN. Whether our setup above
        // works depends on the stack_guard_gap cmdline parameter as well as
        // kernel versions (prior to 5.0 MAP_GROWSDOWN was disallowed for
        // access more than 64k bytes beyond the kernel pointer), so check
        // whether we can expect it to work by explicitly allocating a
        // MAP_GROWSDOWN page in a subprocess.
        if (0 == fork()) {
            test_assert(MAP_FAILED !=
                (char *)mmap(pbase + 509 * page_size, page_size * 3, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_FIXED, -1, 0));
            *p = 1;
            *(p - 1) = 1;
            return 0;
        }

        int status;
        wait(&status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            *p = 1;
            *(p - 1) = 1;
        } else {
            atomic_printf("Skipping MAP_GROWSDOWN due to kernel configuration");
        }

        readlink("/proc/self/exe", path2, sizeof(path2));
        test_assert(0 == strcmp(path1, path2));
        return 0;
    }

    int status;
    wait(&status);
    test_assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    atomic_puts("EXIT-SUCCESS");
    return 0;
}