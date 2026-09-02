/*
 * settopapp — a tiny root helper for the "foreground scheduling" feature.
 *
 * While the producer (the Linux desktop compositor, usually inside a PID
 * namespace/container) renders the display, the consumer wants its processes
 * moved into the Android "top-app" cpuset/cpuctl groups so they get foreground
 * CPU priority. The consumer cannot see the producer's pid directly: its
 * data/fence/audio sockets are socketpairs the consumer itself created and
 * passed along via SCM_RIGHTS, and a socketpair's SO_PEERCRED reports the
 * creator, not whoever holds the other end now.
 *
 * So the reverse lookup runs as root here instead, via NETLINK_INET_DIAG
 * (UNIX_DIAG): given the *inode* of the consumer's end of the data socketpair,
 * the kernel reports the peer socket's inode; scanning /proc for a process
 * whose fds include that inode finds the producer process holding the other
 * end.
 *
 * Two usages:
 *
 *   libsettopapp.so <data_socket_inode> [stops=n1:n2:...] noset
 *       -> resolve the producer and print the HOST pid of an "anchor"
 *          process: the top of the producer's session tree (found by walking
 *          PPIDs upward, stopping at container/superuser boundaries). The
 *          anchor's PID namespace is the container's; the printed host pid is
 *          what later "set" invocations use to enter that namespace. Nothing
 *          is moved. The optional custom stop-name list works as with the
 *          tree promote below.
 *
 *   libsettopapp.so set <anchor_host_pid> <container_pid> opt=on|off
 *                    [settree=true|false] [stops=n1:n2:...]
 *       -> O(1) foreground-scheduling switch. The helper enters the anchor's
 *          PID namespace with setns(), which remaps <container_pid> (a pid as
 *          seen INSIDE that namespace, e.g. the KWin-reported focused-app pid)
 *          to the host pid numbering understood by the cgroup files. The
 *          mount namespace is still the host's, so cgroup paths stay valid;
 *          only /proc lookups for child processes must go through the
 *          container's own procfs, /proc/<anchor>/root/proc/.
 *
 *          opt=on      move the process (settree=false: just it; settree=true:
 *                      its whole process subtree, collected via the container
 *                      procfs) into top-app.
 *          opt=off     move it/them back to the root groups ("/").
 *
 *   libsettopapp.so <data_socket_inode> [stops=n1:n2:...]   (no "noset")
 *       -> resolve the producer as above, then move its WHOLE session tree
 *          into top-app and print the tree-root pid on stdout, so the
 *          consumer can restore later without a second diag round-trip (the
 *          producer may be gone by then).
 *
 *   libsettopapp.so <pid> restore
 *       -> move the host tree rooted at <pid> back to "/".
 *
 * It is shipped inside the APK as lib*.so so Android extracts it into the app's
 * nativeLibraryDir with execute permission.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
/* The NDK sysroot ships only a subset of the kernel UAPI headers and lacks
 * socket_diag.h (and, on some releases, unix_diag.h). Both are trivial stable
 * UAPI definitions, so fall back to inline copies when they are missing. */
#if __has_include(<linux/socket_diag.h>)
#include <linux/socket_diag.h>
#else
#define SOCK_DIAG_BY_FAMILY 20
#endif
#if __has_include(<linux/unix_diag.h>)
#include <linux/unix_diag.h>
#else
struct unix_diag_req {
    __u8    sdiag_family;
    __u8    pad;
    __u16   udiag_states;
    __u32   udiag_ino;
    __u32   udiag_show;
    __u32   udiag_cookie[2];
};
struct unix_diag_msg {
    __u8    udiag_family;
    __u8    udiag_type;
    __u8    udiag_state;
    __u8    udiag_pad;
    __u32   udiag_cookie[2];
    __u32   udiag_ino;
    __u32   udiag_peer;
    __u32   udiag_rqueue;
    __u32   udiag_wqueue;
    __u32   udiag_cookie2[0];
};
#define UNIX_DIAG_PEER    3
#define UDIAG_SHOW_PEER   0x00000010
#endif

#define TAG "AnlandSetTopApp"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Tree capacity: a desktop session (kwin + plasmashell + helpers) is a few
 * dozen processes; allow generous headroom for Wayland clients. */
#define MAX_PIDS 512

static int write_file(const char *path, const char *content)
{
    /* O_APPEND so repeated pid writes accumulate in the file (real cgroupfs
     * treats each write as a command and ignores the offset, but the flag
     * makes the helper's behaviour identical against a plain file too). */
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t len = (ssize_t)strlen(content);
    ssize_t n = write(fd, content, (size_t)len);
    close(fd);
    return (n == len) ? 0 : -1;
}

/* Append "pid\n" to a cgroup.procs file. */
static int write_cgroup_proc(const char *group_procs, pid_t pid)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    return write_file(group_procs, buf) == 0 && len > 0 ? 0 : -1;
}

/* True if pid is already in pids[0..n). */
static bool pid_seen(const pid_t *pids, int n, pid_t pid)
{
    for (int i = 0; i < n; i++)
        if (pids[i] == pid)
            return true;
    return false;
}

/* Depth-first walk of the process tree rooted at root, collecting every pid
 * (root included) exactly once. `proc_root` is the procfs prefix ("/proc" for
 * host pids, "/proc/<anchor>/root/proc" once we setns()ed into the container's
 * PID namespace, whose pid COLUMN of /proc is that namespace's while the
 * mount namespace -- and with it the cgroup paths -- stays the host's). The
 * tree is snapshotted before any cgroup write: a child that dies or reparents
 * mid-walk is skipped here but is not left boosted either (its cgroup
 * membership dies with it). Returns the count, or -1 if the table overflowed. */
static int collect_tree(const char *proc_root, pid_t root, pid_t *pids, int max_pids)
{
    int n = 0;
    int stack[MAX_PIDS];
    int top = 0;
    stack[top++] = root;

    while (top > 0) {
        pid_t cur = stack[--top];
        if (pid_seen(pids, n, cur))
            continue;
        pids[n++] = cur;
        if (n >= max_pids)
            return -1;

        /* children of every thread of cur; dedup against both the collected
         * table and the pending stack entries, else a grandchild gets pushed
         * twice and the second pop is (correctly, but wastefully) skipped */
        char tpath[160];
        snprintf(tpath, sizeof(tpath), "%s/%d/task", proc_root, (int)cur);
        DIR *d = opendir(tpath);
        if (!d)
            continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            char cpath[256];
            snprintf(cpath, sizeof(cpath), "%s/%d/task/%s/children",
                     proc_root, (int)cur, de->d_name);
            FILE *f = fopen(cpath, "r");
            if (!f)
                continue;
            int child;
            while (fscanf(f, "%d", &child) == 1) {
                if (child <= 0 || pid_seen(pids, n, (pid_t)child))
                    continue;
                if (n >= max_pids || top >= MAX_PIDS) {
                    fclose(f);
                    closedir(d);
                    return -1;
                }
                stack[top++] = (pid_t)child;
            }
            fclose(f);
        }
        closedir(d);
    }
    return n;
}

/* Read /proc/<pid>/stat and return the parent pid (field 4). The comm field
 * (2nd) is parenthesised and may contain spaces or parens itself, so locate
 * the LAST ')' first and parse after it. Returns 0 on failure. */
static pid_t get_ppid(const char *proc_root, pid_t pid)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%d/stat", proc_root, (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    char *rp = strrchr(line, ')');
    if (!rp)
        return 0;
    /* fields after comm: state ppid ... */
    char state_c = 0;
    int ppid = 0;
    if (sscanf(rp + 1, " %c %d", &state_c, &ppid) != 2)
        return 0;
    return (pid_t)ppid;
}

/* Names that mark the TOP of the producer session: container init processes /
 * Android framework roots / the su chain. The walk stops ON one of these and
 * uses IT as the tree root, so every process it spawned -- the compositor AND
 * the apps it activated (KDE launches konsole etc. as direct children of
 * `systemd --user`, not of kwin) -- is included in the move. Writing the
 * init's own pid into a cgroup file is harmless: it stays put (its cgroup is
 * pinned/immutable or it just moves with the tree, which is exactly what we
 * want for the session manager).
 *
 * Overridable by the caller: `settopapp <inode> stops=name1:name2:...`
 * (":"-separated comm names). A custom list REPLACES the default; include
 * whatever you need from it explicitly. */
static const char *const default_stop_names[] = {
    "init", "systemd", "zygote", "zygote64", "app_process",
    "magisk", "magiskd", "su", "daemonsu", "supersu",
    NULL
};

/* Active stop-name list: points at default_stop_names, or at the parsed
 * custom list set once in main() before any lookup. Read-only afterwards. */
static const char *const *stop_names = default_stop_names;

/* Parse "a:b:c" into a NULL-terminated malloc'd vector. Returns NULL on
 * failure (caller falls back to the default list). Empty segments between
 * separators are skipped. */
static const char *const *parse_stop_names(const char *spec)
{
    size_t cap = 8, n = 0;
    const char **v = malloc(cap * sizeof(char *));
    if (!v)
        return NULL;

    const char *p = spec;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0) {
            char *name = strndup(p, len);
            if (!name)
                break;
            if (n + 1 >= cap) {
                const char **nv = realloc(v, (cap *= 2) * sizeof(char *));
                if (!nv)
                    break;
                v = nv;
            }
            v[n++] = name;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    if (n == 0) {
        free(v);
        return NULL;
    }
    v[n] = NULL;
    return v;
}

/* True when `pid` itself carries one of the stop names (comm, 15 chars max). */
static bool is_stop_name(const char *proc_root, pid_t pid)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%d/comm", proc_root, (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char comm[24];
    bool hit = false;
    if (fgets(comm, sizeof(comm), f)) {
        char *nl = strchr(comm, '\n');
        if (nl) *nl = '\0';
        for (const char *const *p = stop_names; *p; p++)
            if (strcmp(comm, *p) == 0) { hit = true; break; }
    }
    fclose(f);
    return hit;
}

/* Walk up from `pid` to the session's init process (init/systemd/zygote/su
 * or pid 1's direct child) and return THAT as the tree root, so the whole
 * desktop session -- compositor plus every app the session manager spawned
 * -- is one tree. Guards against loops (broken /proc) with a depth cap.
 * Runs on the HOST procfs: the resulting pid is a HOST pid even though the
 * walk crosses into the container (a container init still has a host
 * parent, e.g. the container runtime). */
static pid_t find_tree_root(const char *proc_root, pid_t pid)
{
    pid_t cur = pid;
    for (int depth = 0; depth < 32; depth++) {
        if (is_stop_name(proc_root, cur))
            return cur;
        pid_t ppid = get_ppid(proc_root, cur);
        if (ppid <= 1)
            return cur;   /* orphaned / reparented: this is the best root */
        cur = ppid;
    }
    return cur;
}

/*
 * NETLINK_INET_DIAG (UNIX_DIAG) lookup: given the inode of our end of the
 * data socketpair, find the peer socket's inode. AF_UNIX diag has no exact
 * get-by-inode op (a plain request returns NLMSG_ERROR/-ESTALE), so like ss(1)
 * we dump every unix socket with UDIAG_SHOW_PEER and match udiag_ino locally.
 * Returns the peer inode, or 0 on failure.
 */
static unsigned long long peer_inode_of(unsigned long long inode)
{
    struct {
        struct nlmsghdr nlh;
        struct unix_diag_req udr;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(req.udr));
    req.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.udr.sdiag_family = AF_UNIX;
    req.udr.udiag_states = ~0U;   /* all states; 0 matches nothing */
    req.udr.udiag_show   = UDIAG_SHOW_PEER;

    int nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_INET_DIAG);
    if (nl < 0) {
        LOGE("netlink socket failed: %s", strerror(errno));
        return 0;
    }

    if (send(nl, &req, req.nlh.nlmsg_len, 0) < 0) {
        LOGE("netlink send failed: %s", strerror(errno));
        close(nl);
        return 0;
    }

    unsigned long long peer = 0;
    char buf[8192];
    while (!peer) {
        ssize_t n = recv(nl, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE)
                goto out;
            if (h->nlmsg_type == NLMSG_ERROR) {
                LOGE("netlink error reply");
                goto out;
            }
            struct unix_diag_msg *m = NLMSG_DATA(h);
            if (m->udiag_ino != (__u32)inode)
                continue;
            int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
            struct rtattr *ra = (struct rtattr *)(m + 1);
            while (RTA_OK(ra, len)) {
                if (ra->rta_type == UNIX_DIAG_PEER) {
                    peer = *(unsigned int *)RTA_DATA(ra);
                    break;
                }
                ra = RTA_NEXT(ra, len);
            }
        }
    }
out:
    close(nl);
    return peer;
}

/*
 * Scan the fd tables of every process for one holding open a socket with the
 * given inode.
 * Returns the pid, or 0 if none (or on error).
 */
static pid_t holder_of_inode(unsigned long long inode)
{
    char target[64];
    snprintf(target, sizeof(target), "socket:[%llu]", inode);

    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;

    pid_t found = 0;
    struct dirent *de;
    while ((de = readdir(proc)) && !found) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        int pid = atoi(de->d_name);
        if (pid == getpid())
            continue;

        char fdpath[48];
        snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd", pid);
        DIR *fd = opendir(fdpath);
        if (!fd)
            continue;   /* not ours to read (kernel threads, other UIDs...) */
        struct dirent *fde;
        while ((fde = readdir(fd))) {
            if (fde->d_name[0] == '.')
                continue;
            char linkpath[128], link[128];
            snprintf(linkpath, sizeof(linkpath), "/proc/%d/fd/%s", pid, fde->d_name);
            ssize_t l = readlink(linkpath, link, sizeof(link) - 1);
            if (l <= 0)
                continue;
            link[l] = '\0';
            if (strcmp(link, target) == 0) {
                found = (pid_t)pid;
                break;
            }
        }
        closedir(fd);
    }
    closedir(proc);
    return found;
}

/* Move every pid of the tree into the two top-app groups (or back to the root
 * groups when group paths are the "/" ones). `proc_root` selects the procfs
 * the tree is collected from (host /proc, or the container's
 * /proc/<anchor>/root/proc after setns into the container's PID namespace);
 * the pids it yields are already in the caller's current PID namespace, which
 * is what the cgroup files expect. Errors on individual pids are logged and
 * skipped (a process may have died between collect and write). */
static int move_tree(const char *proc_root, pid_t root,
                     const char *cpu_procs, const char *cpuset_procs,
                     const char *what)
{
    pid_t pids[MAX_PIDS];
    int n = collect_tree(proc_root, root, pids, MAX_PIDS);
    if (n <= 0) {
        LOGE("%s: collect_tree(%d) failed (%d)", what, (int)root, n);
        return -1;
    }
    LOGI("%s: %d processes from root %d", what, n, (int)root);

    int moved = 0;
    for (int i = 0; i < n; i++) {
        int ok = 0;
        if (write_cgroup_proc(cpu_procs, pids[i]) == 0)
            ok++;
        if (write_cgroup_proc(cpuset_procs, pids[i]) == 0)
            ok++;
        if (ok > 0)
            moved++;
        else
            LOGI("%s: pid %d skipped (dead?)", what, (int)pids[i]);
    }

    /* Processes forked after the snapshot inherit their parent's cgroup, so
     * they follow the move; nothing left to sweep. */
    LOGI("%s: moved %d/%d processes", what, moved, n);
    return moved > 0 ? 0 : -1;
}

/* Resolve the producer (holder of the peer of data socket `inode`) to its
 * session-tree-root HOST pid. Returns 0 on failure. */
static pid_t resolve_producer_anchor(unsigned long long inode)
{
    unsigned long long peer = peer_inode_of(inode);
    if (!peer) {
        LOGE("no peer socket found for inode %llu (diag unavailable?)", inode);
        return 0;
    }

    pid_t producer = holder_of_inode(peer);
    if (!producer) {
        LOGE("no process holds peer socket inode %llu", peer);
        return 0;
    }

    pid_t root = find_tree_root("/proc", producer);
    LOGI("producer pid %d, tree root %d", (int)producer, (int)root);
    return root;
}

/*
 * Raw syscalls (inline asm, no libc): once the seccomp filter below is up,
 * the worker must not execute any bionic code -- no wrappers, no errno, no
 * stdio, no logging. Everything it writes is pre-formatted by the parent,
 * so its entire life is: write() the ready-made "pid\n" lines to the
 * pre-opened cgroup fds, then exit_group(). The parent's own post-filter
 * syscalls (clone / wait4 / close / exit) are raw too, so the allowlist
 * stays minimal: write, clone, wait4, close, exit, exit_group.
 * The build is arm64-v8a only (and this dev box is aarch64 itself).
 */
static long raw_syscall1(long nr, long a)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}
static long raw_syscall3(long nr, long a, long b, long c)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}
static long raw_syscall5(long nr, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ __volatile__("svc 0" : "+r"(x0)
                 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                 : "memory", "cc");
    return x0;
}

/* Plain fork() semantics via clone(SIGCHLD, ...): child_stack NULL keeps the
 * COW-stack fork behaviour, and no CLONE_CHILD_* flags means the tid/tls
 * pointer arguments are never dereferenced. */
#define RAW_FORK() \
    raw_syscall5(__NR_clone, SIGCHLD, 0, 0, 0, 0)
#define RAW_WRITE(fd, buf, len) \
    raw_syscall3(__NR_write, (long)(fd), (long)(buf), (long)(len))
#define RAW_EXIT(code) \
    raw_syscall1(__NR_exit_group, (long)(code))

/*
 * Sandbox installed by the PARENT right before the raw fork(): the forked
 * worker is visible inside the container's PID namespace and runs as host
 * root, so a hostile container process could ptrace it or SIGSTOP it.
 * Everything the worker needs is prepared beforehand -- cgroup fds
 * pre-opened, pid list pre-collected (the container procfs pid column is
 * plain text, so the numbers the parent reads are the worker's
 * container-numbered pids), every write payload pre-formatted. seccomp
 * filters and the mount namespace are inherited across fork(), so
 * installing them pre-fork leaves no unfiltered window: the worker is born
 * with an empty read-only / and nothing but write() left, running zero
 * bionic code. No logging after this returns -- logd is not allowed.
 */
#define ALLOW_NR(nr)                                                    \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 1, (nr) },                          \
    { BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW }

/* Argument-checked allow for the parent's one and only clone() call:
 * clone(SIGCHLD, 0, 0, 0, 0). Any other flags (CLONE_NEWUSER, CLONE_NEWNS,
 * thread spawning, ...) or a nonzero stack/tid argument is killed, so the
 * clone allowance cannot be repurposed. args are 64-bit; flags are checked
 * in both halves, the pointer args in their low halves (they only matter as
 * "zero or not" -- a hostile 64-bit pointer with a zero low half gives the
 * attacker nothing the filter cares about). */
#define ALLOW_CLONE_FORK()                                                  \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 13, __NR_clone },                       \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[0]) }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 11, SIGCHLD },                          \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[0]) + 4 }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 9, 0 },                                 \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[1]) }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 7, 0 },                                 \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[2]) }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 5, 0 },                                 \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[3]) }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 3, 0 },                                 \
    { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[4]) }, \
    { BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0 },                                 \
    { BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW }

static int sandbox_before_fork(void)
{
    if (unshare(CLONE_NEWNS) < 0)
        return -1;
    if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) < 0)
        return -1;
    if (mount("tmpfs", "/", "tmpfs", MS_RDONLY, "size=4k,mode=0") < 0)
        return -1;

    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return -1;

    /* [ 0..3] arch gate + load syscall nr
     * [ 4..13] plain per-syscall allows
     * [14..28] clone, argument-matched to the parent's exact call
     * [29] default kill */
    struct sock_filter filter[] = {
        { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, arch) },
        { BPF_JMP | BPF_JEQ | BPF_K, 1, 0, AUDIT_ARCH_AARCH64 },
        { BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL },
        { BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr) },
        ALLOW_NR(__NR_write),       /* worker: cgroup fds */
        ALLOW_NR(__NR_wait4),       /* parent: reap the worker */
        ALLOW_NR(__NR_close),       /* parent: drop the fds */
        ALLOW_NR(__NR_exit),        /* parent: return from main */
        ALLOW_NR(__NR_exit_group),
        ALLOW_CLONE_FORK(),         /* parent: fork, exact args only */
        { BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL },
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
}

/*
 * "set" subcommand: O(1) foreground-scheduling switch for one container
 * process (or its whole subtree with settree=true).
 *
 *   set <anchor_host_pid> <container_pid> opt=on|off [settree=true|false]
 *
 * The anchor is a process in the producer's PID namespace, discovered once at
 * connect time via the UNIX_DIAG lookup (see the "noset" mode above); its
 * /proc/<pid>/ns/pid is the namespace file we setns() into. After setns the
 * calling thread translates container pids to host pids -- so the container
 * pid the producer reported can be written straight into the host's cgroup
 * files. The mount namespace is NOT entered: cgroup paths remain the host's.
 * The container's OWN procfs (reachable at /proc/<anchor>/root/proc while the
 * anchor lives) is the only procfs whose pid column is the container's, so
 * the settree scan runs against it -- in the parent, before the sandbox.
 */
static int cmd_set(char **argv, int argc)
{
    if (argc < 5) {
        LOGE("usage: %s set <anchor_host_pid> <container_pid> opt=on|off "
             "[settree=true|false]", argv[0]);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    pid_t anchor = (pid_t)strtoll(argv[2], &end, 10);
    if (errno != 0 || !end || *end != '\0' || anchor <= 0) {
        LOGE("set: bad anchor pid '%s'", argv[2]);
        return 1;
    }
    errno = 0;
    end = NULL;
    pid_t target = (pid_t)strtoll(argv[3], &end, 10);
    if (errno != 0 || !end || *end != '\0' || target <= 0) {
        LOGE("set: bad container pid '%s'", argv[3]);
        return 1;
    }

    bool on;
    if (strcmp(argv[4], "opt=on") == 0)
        on = true;
    else if (strcmp(argv[4], "opt=off") == 0)
        on = false;
    else {
        LOGE("set: bad opt '%s' (want opt=on|off)", argv[4]);
        return 1;
    }

    bool settree = false;
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "settree=true") == 0)
            settree = true;
        else if (strcmp(argv[i], "settree=false") == 0)
            settree = false;
        else {
            LOGE("set: unknown option '%s'", argv[i]);
            return 1;
        }
    }

    /* Enter the anchor's PID namespace. setns(CLONE_NEWPID) does NOT change
     * the calling thread's own pid translations -- only children forked
     * afterwards are registered in the new namespace. So the actual cgroup
     * work happens in a fork()ed child, which resolves <container_pid> to
     * the process the producer meant. The mount namespace (and with it the
     * cgroup paths and the anchor's /proc/<pid>/root link) stays the
     * host's. */
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/pid", (int)anchor);
    int ns_fd = open(ns_path, O_RDONLY | O_CLOEXEC);
    if (ns_fd < 0) {
        LOGE("set: cannot open %s: %s", ns_path, strerror(errno));
        return 2;
    }
    if (setns(ns_fd, CLONE_NEWPID) < 0) {
        LOGE("set: setns(%s) failed: %s", ns_path, strerror(errno));
        close(ns_fd);
        return 2;
    }
    close(ns_fd);

    /* The anchor pins the container's root; its procfs is the one whose pid
     * column matches the worker's (container) pid translations. The parent
     * collects the subtree here, BEFORE the sandbox: the pid column is plain
     * text, so the numbers it reads are already the container-numbered pids
     * the worker will resolve. If the anchor died, refuse rather than walk
     * the host /proc with container pids. */
    pid_t pids[MAX_PIDS];
    int n_pids = 1;
    pids[0] = target;
    if (settree) {
        char proc_root[96];
        snprintf(proc_root, sizeof(proc_root), "/proc/%d/root/proc", (int)anchor);
        struct stat proc_stat;
        if (stat(proc_root, &proc_stat) < 0 || !S_ISDIR(proc_stat.st_mode)) {
            LOGE("set: container procfs %s unavailable (anchor gone?)", proc_root);
            return 2;
        }
        n_pids = collect_tree(proc_root, target, pids, MAX_PIDS);
        if (n_pids <= 0) {
            LOGE("set: collect_tree(%d) failed (%d)", (int)target, n_pids);
            return 2;
        }
        LOGI("set-tree: %d processes from root %d", n_pids, (int)target);
    }

    /* Pre-open the destination cgroup files and pre-format every write:
     * after the sandbox goes up neither parent nor worker resolves a path
     * or formats a string ever again. */
    const char *cpu_procs    = on ? "/dev/cpuctl/top-app/cgroup.procs" : "/dev/cpuctl/cgroup.procs";
    const char *cpuset_procs = on ? "/dev/cpuset/top-app/cgroup.procs" : "/dev/cpuset/cgroup.procs";
    int fd_cpu = open(cpu_procs, O_WRONLY | O_APPEND | O_CLOEXEC);
    int fd_cpuset = open(cpuset_procs, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd_cpu < 0 || fd_cpuset < 0) {
        LOGE("set: cannot open cgroup files (%s, %s): %s",
             cpu_procs, cpuset_procs, strerror(errno));
        if (fd_cpu >= 0) close(fd_cpu);
        if (fd_cpuset >= 0) close(fd_cpuset);
        return 2;
    }

    /* Pre-formatted writes AND their lengths: the worker must not call any
     * bionic code, strlen() included. */
    static char lines[MAX_PIDS][16];
    static long line_len[MAX_PIDS];
    for (int i = 0; i < n_pids; i++) {
        int len = snprintf(lines[i], sizeof(lines[i]), "%d\n", (int)pids[i]);
        line_len[i] = len > 0 ? len : 0;
    }

    /* Sandbox LAST: from here on -- raw syscalls only, no bionic, no logs.
     * The worker is born masked, filtered, and with everything it needs
     * already open and formatted. Failures past this point exit raw too:
     * returning through libc would run bionic's atexit cleanup under the
     * filter and die of SIGSYS after all. */
    if (sandbox_before_fork() < 0)
        RAW_EXIT(2);

    long worker = RAW_FORK();
    if (worker < 0)
        RAW_EXIT(2);
    if (worker == 0) {
        /* Worker: registered in the container's PID namespace, so the
         * pre-formatted pid lines resolve to the processes the producer
         * meant. Raw write()s to the two pre-opened fds, then raw exit. */
        long ok = 0;
        for (int i = 0; i < n_pids; i++) {
            if (RAW_WRITE(fd_cpu, lines[i], line_len[i]) == line_len[i])
                ok++;
            if (RAW_WRITE(fd_cpuset, lines[i], line_len[i]) == line_len[i])
                ok++;
        }
        /* raw exit_group never returns; the loop below is unreachable, but
         * keeps the compiler from believing control falls out of the if */
        for (;;)
            RAW_EXIT(ok > 0 ? 0 : 3);
    }

    /* Parent: reaps the worker and passes its exit code through, raw.
     * A signalled worker (SIGSYS = it hit the filter; SIGSTOP-hostile
     * tampering would leave wait4 hanging, the consumer's su timeout
     * reaps the helper then) reports failure, not a bogus 0. */
    long status = 0;
    while (raw_syscall5(__NR_wait4, worker, (long)&status, 0, 0, 0) == -EINTR) {
    }
    raw_syscall1(__NR_close, fd_cpu);
    raw_syscall1(__NR_close, fd_cpuset);
    if ((status & 0x7f) != 0)
        RAW_EXIT(3);
    for (;;)
        RAW_EXIT((int)(((unsigned)status >> 8) & 0xff));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        LOGE("usage: %s <data_socket_inode> [stops=n1:n2:...] [noset]", argv[0]);
        LOGE("       %s <data_socket_inode> [stops=...] (promote whole tree)", argv[0]);
        LOGE("       %s <pid> restore", argv[0]);
        LOGE("       %s set <anchor_host_pid> <container_pid> opt=on|off [settree=...]", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "set") == 0)
        return cmd_set(argv, argc);

    /* Optional custom stop-name list ("stops=init:my_init:..."), replacing the
     * built-in one. Bounds the PPID-upward scan in every mode: promote,
     * noset (anchor discovery), and restore all use the resulting session
     * tree root. Parsed before anything else so failures here fail fast. */
    bool noset = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "noset") == 0) {
            noset = true;
        } else if (strncmp(argv[i], "stops=", 6) == 0) {
            const char *const *custom = parse_stop_names(argv[i] + 6);
            if (custom)
                stop_names = custom;
            else
                LOGE("bad stops= spec '%s', using defaults", argv[i] + 6);
        }
    }

    if (argc >= 3 && strcmp(argv[2], "restore") == 0) {
        pid_t root = (pid_t)atoi(argv[1]);
        if (root <= 0) {
            LOGE("restore: bad pid '%s'", argv[1]);
            return 1;
        }
        /* "/" (the root cgroup of each hierarchy) == the top of the mounted
         * controller: /dev/cpuctl/cgroup.procs and /dev/cpuset/cgroup.procs. */
        int rc = move_tree("/proc", root, "/dev/cpuctl/cgroup.procs",
                           "/dev/cpuset/cgroup.procs", "restore");
        if (rc < 0)
            return 2;
        printf("%d\n", (int)root);
        return 0;
    }

    unsigned long long inode = strtoull(argv[1], NULL, 10);
    if (inode == 0) {
        LOGE("bad inode '%s'", argv[1]);
        return 1;
    }

    pid_t root = resolve_producer_anchor(inode);
    if (!root)
        return 4;

    if (noset) {
        /* Anchor discovery only: hand the host pid back so the consumer can
         * later "set ..." against it without a second diag round-trip. */
        printf("%d\n", (int)root);
        return 0;
    }

    if (move_tree("/proc", root, "/dev/cpuctl/top-app/cgroup.procs",
                  "/dev/cpuset/top-app/cgroup.procs", "top-app") < 0)
        return 5;

    /* Hand the tree root back so the consumer can restore without a second
     * diag round-trip (the producer may be gone by then). */
    printf("%d\n", (int)root);
    return 0;
}
