/*
 * fault_probe.c - is the 256KB data zone costing a minor fault per 4K page,
 *                 every time a scrap page is recycled?
 *
 * WHY THIS EXISTS. The 2026-08-26 soak measured 39.75M minor faults against
 * 51.26M foreground writes (PERFORMANCE.md §4.6) -- 0.78 faults per write,
 * with sys time running 3:1 over user time. Arithmetic points at the page
 * lifecycle rather than at the workload:
 *
 *     147.4 GB written / 262144 B per page = 562238 page cycles
 *     562238 cycles x 64 pages of 4K        =  36.0 M faults
 *     measured                              =  39.8 M faults   (ratio 1.10)
 *
 * That is close enough to accuse scrap_page_alloc/scrap_page_free, but not
 * close enough to convict them: the engine does many other things, and a
 * coincidence at 10% is still a coincidence. This probe removes the engine
 * from the question entirely. It performs ONLY the allocation pattern, three
 * ways, and counts the faults each way costs.
 *
 * The three arms, and what each one would prove:
 *
 *   A. alloc / touch / free, every cycle       <- what noxdb does today
 *   B. alloc once, touch, reuse                <- what a free-list would do
 *   C. same as A, but with glibc told to keep  <- whether the fix can be a
 *      the memory (mallopt)                       one-line allocator tuning
 *                                                 instead of a page pool
 *
 * If A costs ~64 faults per cycle and B costs ~0, the hypothesis is confirmed
 * and the faults are structural, not workload. If A and B are both expensive,
 * the hypothesis is WRONG and the faults come from somewhere else -- which is
 * a result worth having before anyone writes a pool that fixes nothing.
 *
 * C is the arm that decides how much work the fix is. glibc serves any
 * allocation at or above mmap_threshold (128 KB by default) with mmap, and
 * munmaps it on free; a 256 KB zone sits above that line. glibc also RAISES
 * the threshold dynamically when it sees such a chunk freed, so it is
 * genuinely unclear from reading the source whether the steady state keeps
 * faulting. That is exactly why this is measured rather than argued.
 *
 * Deliberately NOT linked against the engine: no noxdb headers, no O_DIRECT,
 * no device. It is pure user-space RAM and runs anywhere glibc runs.
 *
 *   cc -std=c11 -O2 -o bench/fault_probe bench/fault_probe.c
 *   ./bench/fault_probe [cycles]        # default 20000
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <sys/resource.h>

/* Mirrors src/noxdb_config.h. Duplicated on purpose: this probe must not
 * depend on the engine's headers, or it stops being an independent check. */
#define ZONE_SIZE   262144u
#define BLOCK_SIZE    4096u
#define PAGES_PER_ZONE (ZONE_SIZE / BLOCK_SIZE)   /* 64 */

static long minflt(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Touch every 4K page of the zone, which is what the engine effectively does:
 * Stage-1 fills the holes from disk across the whole zone, and Stage-2 writes
 * the whole zone out. `volatile` so -O2 cannot delete the stores. */
static void touch_zone(void *zone)
{
    volatile uint8_t *z = zone;
    for (uint32_t off = 0; off < ZONE_SIZE; off += BLOCK_SIZE)
        z[off] = (uint8_t)off;
}

struct arm_result { long faults; double seconds; };

/* A: allocate and free per cycle -- today's scrap_page_alloc/free. */
static struct arm_result arm_alloc_free(uint32_t cycles)
{
    long f0 = minflt();
    double t0 = now_s();
    for (uint32_t i = 0; i < cycles; i++) {
        void *zone = NULL;
        if (posix_memalign(&zone, BLOCK_SIZE, ZONE_SIZE) != 0)
            exit(fprintf(stderr, "posix_memalign failed\n"));
        touch_zone(zone);
        free(zone);
    }
    return (struct arm_result){ minflt() - f0, now_s() - t0 };
}

/* B: allocate once, reuse -- the counterfactual a free-list would produce.
 * The touch loop is identical, so any difference is the allocator, not the
 * memory traffic. */
static struct arm_result arm_reuse(uint32_t cycles)
{
    void *zone = NULL;
    if (posix_memalign(&zone, BLOCK_SIZE, ZONE_SIZE) != 0)
        exit(fprintf(stderr, "posix_memalign failed\n"));
    touch_zone(zone);                 /* warm it before measuring */

    long f0 = minflt();
    double t0 = now_s();
    for (uint32_t i = 0; i < cycles; i++)
        touch_zone(zone);
    struct arm_result r = { minflt() - f0, now_s() - t0 };
    free(zone);
    return r;
}

int main(int argc, char **argv)
{
    uint32_t cycles = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 20000u;

    printf("fault_probe: %u cycles of a %u B zone (%u pages of %u B)\n\n",
           cycles, ZONE_SIZE, PAGES_PER_ZONE, BLOCK_SIZE);

    struct arm_result a = arm_alloc_free(cycles);
    struct arm_result b = arm_reuse(cycles);

    /* C: same as A, but forbid glibc from using mmap for this size and from
     * returning heap memory to the kernel. mallopt() also switches off the
     * dynamic threshold adaptation, which is what makes this arm a controlled
     * comparison rather than a second sample of A. */
    mallopt(M_MMAP_THRESHOLD, (int)(ZONE_SIZE * 4));
    mallopt(M_TRIM_THRESHOLD, (int)(ZONE_SIZE * 64));
    struct arm_result c = arm_alloc_free(cycles);

    printf("  arm                                 faults   per-cycle    seconds\n");
    printf("  --------------------------------  --------   ---------   --------\n");
    printf("  A alloc+free each cycle           %8ld   %9.2f   %8.3f\n",
           a.faults, (double)a.faults / cycles, a.seconds);
    printf("  B allocate once, reuse            %8ld   %9.2f   %8.3f\n",
           b.faults, (double)b.faults / cycles, b.seconds);
    printf("  C alloc+free, mallopt tuned       %8ld   %9.2f   %8.3f\n",
           c.faults, (double)c.faults / cycles, c.seconds);

    printf("\n  expected per-cycle cost if every zone is fresh memory: %u\n",
           PAGES_PER_ZONE);
    printf("\nVERDICT\n");
    if ((double)a.faults / cycles > PAGES_PER_ZONE * 0.5 &&
        (double)b.faults / cycles < 1.0)
        printf("  H1 CONFIRMED: the per-cycle allocation is the fault source.\n"
               "  Reusing the zone removes it. A page pool would too.\n");
    else if ((double)a.faults / cycles < 1.0)
        printf("  H1 WRONG: alloc+free is already cheap here, so the engine's\n"
               "  39.8M faults come from somewhere else. Do NOT write a pool.\n");
    else
        printf("  INCONCLUSIVE: neither arm matches the predicted shape.\n"
               "  Read the numbers above before concluding anything.\n");

    if ((double)c.faults / cycles < (double)a.faults / cycles * 0.5)
        printf("  mallopt alone fixes it -> the change may be two lines, not a pool.\n");
    else
        printf("  mallopt does NOT fix it -> a real page pool is required.\n");
    return 0;
}
