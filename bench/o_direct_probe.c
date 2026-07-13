/*
 * o_direct_probe.c - What does O_DIRECT *actually* demand? Ask the hardware.
 *
 * Standalone teaching artifact for C0. NO engine dependency: raw open / pwrite /
 * mincore / statx + sysfs only. Run the SAME binary against a file on two
 * different disks and the output diverges -- because the O_DIRECT alignment
 * contract is dictated by the BLOCK DEVICE, not by the syscall.
 *
 * It prints four sections, designed to be screenshotted for the write-up:
 *
 *   1. what the DEVICE demands   - the backing disk's logical_block_size and
 *      dma_alignment (from sysfs) + the kernel-declared STATX_DIOALIGN values.
 *   2. does O_DIRECT bypass?     - buffered write() lands in the page cache;
 *      posix_memalign + O_DIRECT does not. Proven with mincore residency.
 *   3. where is the REAL wall?   - sweep buffer address / file offset / transfer
 *      length one knob at a time and show exactly which values pwrite accepts
 *      and which return EINVAL. The measured wall must match section 1.
 *   4. verdict                   - this disk's contract, and why NoxDB aligns
 *      everything to 4096: a conservative SUPERSET that is legal on any disk.
 *
 * THE FOLKLORE THIS CORRECTS:
 *   "malloc + O_DIRECT gives you EINVAL" and "you must align to 4096" are both
 *   device-specific half-truths. A modern NVMe reports dma_alignment=3 (a buffer
 *   only needs 4-byte alignment) and logical_block_size=512 (offset/length need
 *   512, not 4096). A USB/SATA transport commonly reports dma_alignment=511 (the
 *   buffer must be 512-aligned) -- so the exact same misaligned malloc buffer is
 *   accepted on the NVMe and rejected with EINVAL on the slower disk. Same
 *   kernel, same code; only /sys/block/<dev>/queue/dma_alignment changed.
 *
 * mincore detail: after a write we mmap the file read-only and call mincore(),
 * which reports per-page cache residency WITHOUT faulting the pages in (we never
 * dereference the mapping -- one read would taint the measurement). O_DIRECT
 * leaves the data pages out of the cache; buffered I/O leaves them in.
 *
 * Needs a real block-backed filesystem (XFS/EXT4 on the disk). Will NOT behave
 * correctly on macOS / tmpfs / overlay.
 *
 *   usage: ./o_direct_probe [backing-file]     (default: ./probe.dat)
 *          run it once per disk, e.g.
 *              ./o_direct_probe /mnt/nvme/probe.dat   (WD SN530 NVMe)
 *              ./o_direct_probe /tmp/probe.dat        (OS disk)
 *
 * Exit status: 0 if the two invariants held (buffered=cached, aligned
 * O_DIRECT=bypassed); 1 otherwise (wrong FS? O_DIRECT ignored? not a real disk?).
 */
#define _GNU_SOURCE          /* O_DIRECT, mincore, statx, major()/minor() gating */
#include "noxdb_config.h"    /* NOX_BLOCK_SIZE (4096), NOX_IS_ALIGNED */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>         /* PATH_MAX */
#include <dirent.h>         /* opendir/readdir -- follow dm slaves */
#include <unistd.h>
#include <sys/stat.h>        /* stat, statx */
#include <sys/mman.h>        /* mmap, mincore */
#include <sys/sysmacros.h>   /* major, minor */

/* 10 MB residency payload. 10*1024*1024 == 2560 * 4096, so length is trivially
 * block-aligned -- section 2 isolates the page-cache question, not alignment. */
#define PROBE_SIZE (10u * 1024u * 1024u)

/* 1 MB aligned scratch transfer for the section-3 sweeps: big enough to be a real
 * I/O, always a clean multiple of 4096 so only the knob under test can misalign. */
#define SWEEP_SIZE (1u * 1024u * 1024u)

/* ANSI styling, populated only when stdout is a TTY (see main). Keeping them as
 * runtime strings means a piped/redirected run stays plain for clean logs. */
static const char *DIM = "", *BOLD = "", *GRN = "", *RED = "",
                  *CYN = "", *YEL = "", *RST = "";

/* ---- section framing helpers (box + rule), kept dumb on purpose ----------- */
static void frame_top(const char *title)
{
    printf("%s%s+== %s ", BOLD, CYN, title);
    for (int i = (int)strlen(title); i < 54; i++) putchar('=');
    printf("+%s\n", RST);
}
static void frame_bottom(void)
{
    printf("%s%s+", BOLD, CYN);
    for (int i = 0; i < 59; i++) putchar('=');
    printf("+%s\n", RST);
}
static void rule(int n, const char *title)
{
    printf("\n%s-- %d - %s%s ", BOLD, n, title, RST);
    for (int i = (int)strlen(title); i < 46; i++) putchar('-');
    putchar('\n');
}

/*
 * Loud EINVAL banner mandated by CLAUDE.md §2 / docs/02. In this probe an EINVAL
 * in the section-3 sweep is EXPECTED (we are hunting the wall), so we do NOT
 * shout there. We only fire this if the *aligned* control write unexpectedly
 * fails -- that would be a genuine "your assumptions are broken" signal.
 */
static void report_einval(const char *ctx)
{
    fprintf(stderr,
            "\n%s!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
            "!! O_DIRECT alignment violation  (%s)\n"
            "!! errno=EINVAL on an ALIGNED write -- this should never happen.\n"
            "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!%s\n",
            RED, ctx, RST);
}

/* ---- sysfs: resolve the backing disk of `path` and read two queue knobs ---- */

static int read_sysfs_long(const char *p, long *out)
{
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%ld", out) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

/* Read a single trimmed line from a sysfs attribute. Returns 0 on success. */
static int read_sysfs_str(const char *p, char *out, size_t osz)
{
    if (!out || !osz) return -1;
    out[0] = '\0';
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int ok = (fgets(out, (int)osz, f) != NULL);
    fclose(f);
    if (!ok) return -1;
    size_t n = strlen(out);                 /* trim trailing space/newline */
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = '\0';
    return 0;
}

/* Bus/transport tag inferred from a realpath'd /sys/block/<disk> path. The
 * physical transport lives in the sysfs ancestry: a USB-bridged disk sits under
 * .../usbN/..., an NVMe under .../nvme/..., a SATA disk under .../ataN/... */
static const char *transport_of(const char *realdir)
{
    if (strstr(realdir, "/usb"))  return "USB";
    if (strstr(realdir, "/nvme")) return "NVMe/PCIe";
    if (strstr(realdir, "/ata"))  return "SATA";
    return "";
}

/*
 * Fill `name` with the backing disk (e.g. "nvme0n1" / "sda") and read its
 * logical_block_size + dma_alignment from /sys. Works whether the filesystem
 * sits on a whole disk or a partition:
 *   - stat() the target (or its parent dir if it does not exist yet) for st_dev.
 *   - /sys/dev/block/MAJ:MIN is the partition/disk node; its queue/ dir lives on
 *     the node itself for a whole disk, or one level up for a partition.
 * Returns 0 on success, -1 if anything could not be resolved (caller prints n/a).
 */
static int get_disk_info(const char *path, char *name, size_t nsz,
                         long *lbs, long *dma, char *model, size_t msz)
{
    if (model && msz) model[0] = '\0';
    struct stat st;
    if (stat(path, &st) != 0) {
        /* File may not exist yet: fall back to the directory that will hold it. */
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else strcpy(dir, ".");
        if (stat(dir[0] ? dir : "/", &st) != 0) return -1;
    }

    unsigned maj = major(st.st_dev), mn = minor(st.st_dev);
    char node[64];
    snprintf(node, sizeof node, "/sys/dev/block/%u:%u", maj, mn);

    /* Whole disk: queue/ is right here. Partition: queue/ is one level up. */
    char qpath[PATH_MAX], diskdir[PATH_MAX];
    snprintf(qpath, sizeof qpath, "%s/queue/logical_block_size", node);
    if (read_sysfs_long(qpath, lbs) == 0) {
        if (!realpath(node, diskdir)) return -1;
    } else {
        snprintf(qpath, sizeof qpath, "%s/../queue/logical_block_size", node);
        if (read_sysfs_long(qpath, lbs) != 0) return -1;
        char up[PATH_MAX];
        snprintf(up, sizeof up, "%s/..", node);
        if (!realpath(up, diskdir)) return -1;
    }

    /* diskdir is a realpath'd /sys node (always short); the +32 headroom just
     * keeps -Wformat-truncation quiet about the appended suffix. */
    char dpath[PATH_MAX + 32];
    snprintf(dpath, sizeof dpath, "%s/queue/dma_alignment", diskdir);
    if (read_sysfs_long(dpath, dma) != 0) *dma = -1;

    /* Bounded manual copy of the disk basename -> no truncation warning. */
    const char *base = strrchr(diskdir, '/');
    base = base ? base + 1 : diskdir;
    size_t k = 0;
    while (base[k] && k + 1 < nsz) { name[k] = base[k]; k++; }
    name[k] = '\0';

    /* Human identity for the device line: model/vendor + transport tag. For a
     * device-mapper (LVM) volume the node itself exposes no model, so we follow
     * its first slave down to the underlying PHYSICAL disk -- that is what proves
     * to a skeptical reader that two runs really hit two distinct disks. */
    if (model && msz) {
        const char *physdir = diskdir;         /* where we mine model/transport */
        char physbuf[PATH_MAX];
        char lead[96] = "";                    /* e.g. "lvm -> sda  .  " prefix */

        if (strncmp(name, "dm-", 3) == 0) {
            char sldir[PATH_MAX + 16];
            snprintf(sldir, sizeof sldir, "%s/slaves", diskdir);
            DIR *d = opendir(sldir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (e->d_name[0] == '.') continue;   /* skip . / .. */
                    char slpath[PATH_MAX + 512];
                    snprintf(slpath, sizeof slpath, "%s/%s", sldir, e->d_name);
                    if (realpath(slpath, physbuf)) {
                        /* physbuf = .../block/sda/sda3 -> strip to the whole disk */
                        char *s = strrchr(physbuf, '/');
                        if (s) *s = '\0';
                        const char *pn = strrchr(physbuf, '/');
                        pn = pn ? pn + 1 : physbuf;
                        char pdisk[48];                  /* bounded -> no warning */
                        size_t j = 0;
                        while (pn[j] && j + 1 < sizeof pdisk) { pdisk[j] = pn[j]; j++; }
                        pdisk[j] = '\0';
                        snprintf(lead, sizeof lead, "lvm -> %s  .  ", pdisk);
                        physdir = physbuf;
                    }
                    break;                               /* first slave is enough */
                }
                closedir(d);
            }
        }

        /* Model, falling back to vendor -- a USB bridge often hides the drive's
         * model and exposes only the bridge vendor (e.g. "JMicron"). */
        char id[96] = "", vendor[64] = "", p[PATH_MAX + 32];
        snprintf(p, sizeof p, "%s/device/model", physdir);
        read_sysfs_str(p, id, sizeof id);
        if (!id[0]) {
            snprintf(p, sizeof p, "%s/device/vendor", physdir);
            read_sysfs_str(p, vendor, sizeof vendor);
        }
        const char *idp = id[0] ? id : vendor;
        const char *tr  = transport_of(physdir);

        snprintf(model, msz, "%s%s%s%s", lead, idp,
                 (idp[0] && tr[0]) ? "  .  " : "", tr);
    }
    return 0;
}

/*
 * How many of the first `len` bytes of `path` are resident in the page cache.
 * mmap read-only + mincore; never dereference the mapping. -1 on error.
 */
static long resident_pages(const char *path, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open (mincore)"); return -1; }

    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) pgsz = NOX_BLOCK_SIZE;
    size_t npages = (len + (size_t)pgsz - 1) / (size_t)pgsz;

    void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap (mincore)"); close(fd); return -1; }

    unsigned char *vec = malloc(npages);
    if (!vec) { munmap(m, len); close(fd); return -1; }

    long resident = -1;
    if (mincore(m, len, vec) == 0) {
        resident = 0;
        for (size_t i = 0; i < npages; i++)
            if (vec[i] & 1u) resident++;   /* LSB = page resident in core */
    } else {
        perror("mincore");
    }

    free(vec);
    munmap(m, len);
    close(fd);
    return resident;
}

/* Full-length pwrite loop. pwrite (never write+lseek) keeps the file offset
 * untouched -- house rule, and it makes each probe independent. errno preserved. */
static int write_all(int fd, const void *buf, size_t len, off_t off)
{
    size_t done = 0;
    const char *p = buf;
    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, off + (off_t)done);
        if (n < 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* ===================== section 1: what the device demands ================== */
static void section_device(const char *path)
{
    rule(1, "what the DEVICE demands");

    char disk[64] = "?", model[128] = "";
    long lbs = -1, dma = -1;
    int have = (get_disk_info(path, disk, sizeof disk, &lbs, &dma,
                              model, sizeof model) == 0);

    if (have) {
        if (model[0])
            printf("  %-22s %s%s%s  %s.  %s%s\n", "device", BOLD, disk, RST,
                   DIM, model, RST);
        else
            printf("  %-22s %s%s%s\n", "device", BOLD, disk, RST);
        printf("  %-22s %s%ld%s   offset & length granularity\n",
               "logical_block_size", BOLD, lbs, RST);
        if (dma >= 0)
            printf("  %-22s %s%ld%s   buffer addr -> needs %ld-byte align\n",
                   "dma_alignment", BOLD, dma, RST, dma + 1);
        else
            printf("  %-22s %sn/a%s\n", "dma_alignment", DIM, RST);
    } else {
        printf("  %s(could not resolve backing disk via sysfs)%s\n", DIM, RST);
    }

    /* Kernel-declared direct-I/O alignment, straight from the file (statx,
     * STATX_DIOALIGN, Linux 6.1+). Cross-checks the sysfs numbers above. */
#if defined(STATX_DIOALIGN)
    struct statx stx;
    if (statx(AT_FDCWD, path, AT_STATX_SYNC_AS_STAT, STATX_DIOALIGN, &stx) == 0
        && (stx.stx_mask & STATX_DIOALIGN)) {
        printf("  %-22s %s%u%s   kernel-declared (statx)\n",
               "dio_mem_align", BOLD, stx.stx_dio_mem_align, RST);
        printf("  %-22s %s%u%s\n",
               "dio_offset_align", BOLD, stx.stx_dio_offset_align, RST);
    } else {
        printf("  %-22s %sn/a (file not created yet / <6.1)%s\n",
               "statx dio_align", DIM, RST);
    }
#else
    printf("  %-22s %sn/a (built without STATX_DIOALIGN)%s\n",
           "statx dio_align", DIM, RST);
#endif

    printf("  %sfolklore says \"align to 4096\".%s  this disk: %s%ld / %ld-byte%s.\n",
           DIM, RST, YEL, lbs > 0 ? lbs : (long)NOX_BLOCK_SIZE,
           dma >= 0 ? dma + 1 : 1, RST);
}

/* ================ section 2: does O_DIRECT bypass the cache? ================ */
/* Returns 0 if both invariants held (buffered cached, aligned direct bypassed). */
static int section_residency(const char *path)
{
    rule(2, "does O_DIRECT actually bypass the cache?");

    long pgsz = sysconf(_SC_PAGESIZE);
    if (pgsz <= 0) pgsz = NOX_BLOCK_SIZE;
    long total = (long)(PROBE_SIZE / (size_t)pgsz);
    int bad = 0;

    /* (a) plain buffered write -> must populate the page cache. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    void *b = malloc(PROBE_SIZE);
    if (fd >= 0 && b) {
        memset(b, 0xA1, PROBE_SIZE);
        if (write_all(fd, b, PROBE_SIZE, 0) != 0) perror("write_all buffered");
        close(fd);
    }
    free(b);
    long r_cached = resident_pages(path, PROBE_SIZE);

    /* (b) aligned O_DIRECT write -> must bypass; pages stay out of the cache. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    void *ab = NULL;
    if (fd >= 0 && posix_memalign(&ab, NOX_BLOCK_SIZE, PROBE_SIZE) == 0) {
        memset(ab, 0xC3, PROBE_SIZE);
        errno = 0;
        if (write_all(fd, ab, PROBE_SIZE, 0) != 0) {
            if (errno == EINVAL) report_einval("aligned control write");
            else perror("write_all O_DIRECT");
            bad = 1;
        }
        close(fd);
    }
    free(ab);
    long r_direct = resident_pages(path, PROBE_SIZE);

    int cached_ok = (r_cached  > total / 2);
    int bypass_ok = (r_direct <= total / 2);
    printf("  %-28s %s%4ld/%ld%s pages cached   %s%s CACHED%s\n",
           "malloc + write()", BOLD, r_cached, total, RST,
           cached_ok ? GRN : RED, cached_ok ? "*" : "!", RST);
    printf("  %-28s %s%4ld/%ld%s pages cached   %s%s BYPASS%s\n",
           "posix_memalign + O_DIRECT", BOLD, r_direct, total, RST,
           bypass_ok ? GRN : RED, bypass_ok ? "o" : "!", RST);

    return (cached_ok && bypass_ok && !bad) ? 0 : 1;
}

/* ================ section 3: where is the real alignment wall? ============== */

/*
 * One O_DIRECT write attempt. A page-aligned base buffer is nudged by `buf_shift`
 * to control the buffer's alignment exactly; `off`/`len` set the file offset and
 * transfer length. Returns 0 on success, else the errno pwrite/open reported.
 */
static int try_write(const char *path, size_t buf_shift, off_t off, size_t len)
{
    void *base = NULL;
    if (posix_memalign(&base, NOX_BLOCK_SIZE, len + NOX_BLOCK_SIZE) != 0)
        return ENOMEM;
    char *buf = (char *)base + buf_shift;
    memset(buf, 0xD4, len);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) { int e = errno; free(base); return e; }

    errno = 0;
    int rc = write_all(fd, buf, len, off);
    int e = rc == 0 ? 0 : errno;
    close(fd);
    free(base);
    return e;
}

/* Print one "<label> <verdict>" cell. Green tick on success, red EINVAL/errno. */
static void cell(const char *label, int e)
{
    if (e == 0)
        printf("%s %s%s%s   ", label, GRN, "ok", RST);
    else if (e == EINVAL)
        printf("%s %sEINVAL%s   ", label, RED, RST);
    else
        printf("%s %se%d%s   ", label, RED, e, RST);
}

static void section_matrix(const char *path)
{
    rule(3, "where is the REAL alignment wall?");
    const size_t S = SWEEP_SIZE;

    /* Buffer address: offset 0 and length S are held aligned, so only the
     * buffer's alignment varies. The pass/fail split reveals dma_alignment. */
    printf("  %-8s", "buffer");
    cell("+0",  try_write(path, 0, 0, S));
    cell("+4",  try_write(path, 4, 0, S));
    cell("+8",  try_write(path, 8, 0, S));
    cell("+1",  try_write(path, 1, 0, S));
    cell("+2",  try_write(path, 2, 0, S));
    putchar('\n');

    /* File offset: buffer + length aligned; only the offset varies. */
    printf("  %-8s", "offset");
    cell("512",  try_write(path, 0, 512,  S));
    cell("4096", try_write(path, 0, 4096, S));
    cell("100",  try_write(path, 0, 100,  S));
    cell("511",  try_write(path, 0, 511,  S));
    putchar('\n');

    /* Transfer length: buffer aligned, offset 0; only the length varies. */
    printf("  %-8s", "length");
    cell("512",  try_write(path, 0, 0, 512));
    cell("4096", try_write(path, 0, 0, 4096));
    cell("511",  try_write(path, 0, 0, 511));
    cell("4097", try_write(path, 0, 0, 4097));
    putchar('\n');
}

/* ============================ section 4: verdict =========================== */
static void section_verdict(const char *path)
{
    char disk[64] = "?";
    long lbs = -1, dma = -1;
    get_disk_info(path, disk, sizeof disk, &lbs, &dma, NULL, 0);

    rule(4, "verdict");
    printf("  O_DIRECT contract on %s%s%s:\n", BOLD, disk, RST);
    printf("    buffer  %s%ld-byte%s aligned      offset/length  %s%ld%s-multiple\n",
           YEL, dma >= 0 ? dma + 1 : 1, RST, YEL, lbs > 0 ? lbs : 512, RST);
    printf("  NoxDB aligns everything to %s%d%s -> safe superset on ANY disk.\n",
           BOLD, NOX_BLOCK_SIZE, RST);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "./probe.dat";

    if (isatty(STDOUT_FILENO)) {
        DIM = "\033[2m"; BOLD = "\033[1m"; GRN = "\033[32m"; RED = "\033[31m";
        CYN = "\033[36m"; YEL = "\033[33m"; RST = "\033[0m";
    }

    /* Create the backing file up front so section 1's statx has an inode to
     * report STATX_DIOALIGN on (statx of a not-yet-existing path yields nothing).
     * Sections 2/3 reopen it with O_TRUNC; main unlinks it at the end. */
    int cfd = open(path, O_WRONLY | O_CREAT, 0644);
    if (cfd >= 0) close(cfd);

    /* Host banner: kernel/fs come from the shell wrapper; here we anchor path. */
    frame_top("NoxDB . O_DIRECT alignment probe");
    printf("  target  %s%s%s\n", BOLD, path, RST);

    section_device(path);
    int inv = section_residency(path);
    section_matrix(path);
    section_verdict(path);
    frame_bottom();

    unlink(path);   /* scrap file, clean up */

    if (inv)
        fprintf(stderr, "\n%sPROBE FAILED%s: a residency invariant was violated "
                "(wrong FS? O_DIRECT ignored? not a real disk?)\n", RED, RST);
    return inv ? 1 : 0;
}
