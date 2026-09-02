/*
 * inputgrab -- the root helper behind "immersive mode".
 *
 * Immersive mode hands the device over to the Linux desktop: the touchscreen,
 * the keyboard and any pointer stop reaching Android entirely and are streamed
 * to the app instead, which replays them onto the remote desktop.
 *
 * The app cannot read /dev/input itself (untrusted_app is denied
 * input_device:chr_file), so it launches this helper through `su -c`. The
 * helper opens and EVIOCGRABs the devices in the root context and forwards a
 * fixed-size record stream over a unix socket the app listens on -- the same
 * bridge pattern fd_helper.c uses to hand back the daemon connection. It is
 * shipped inside the APK as lib*.so so Android extracts it into the app's
 * nativeLibraryDir with execute permission.
 *
 *   usage: libinputgrab.so <bridge_socket_path> <toggle_scancode>
 *
 * Grabbing every input device is a good way to brick a tablet, so the escape
 * hatches are the design, in the order they matter:
 *
 *   1. EVIOCGRAB hangs off the open file description, so the kernel drops every
 *      grab as soon as this process dies -- including on SIGKILL.
 *   2. <toggle_scancode> is mandatory and is watched on every device: pressing
 *      it ungrabs and exits, so the user gets out even if the app is wedged.
 *   3. Records are sent non-blocking and dropped under back-pressure. A stalled
 *      app can therefore never park this process inside send() and stop it from
 *      ever reaching the toggle key -- which is what actually locks a device up.
 *   4. The app heartbeats over the same socket. Silence for HEARTBEAT_TIMEOUT_MS
 *      (or EOF, which the kernel guarantees when the app dies) releases
 *      everything. The app only heartbeats while its main thread is running, so
 *      an ANR frees the input too.
 *   5. Pure Android hardware-button nodes, and every non-keyboard node that
 *      reports Power, are watched but never grabbed. That preserves Android's
 *      lock/wake path even on panels that expose KEY_POWER alongside contacts.
 *      Full keyboards remain grabbable because their ordinary keys cannot leak.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "input_grab.h"
#include "socket_utils.h"

#define TAG "AnlandGrab"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* No heartbeat for this long => the app is gone or frozen; release everything. */
#define HEARTBEAT_TIMEOUT_MS 6000
/* How often to look for input devices that appeared since the session started.
 * Bluetooth mice and keyboards sleep and reconnect as brand-new nodes
 * mid-session; a one-time scan at start would leave them out of the grab and
 * at Android's mercy. */
#define RESCAN_INTERVAL_MS 2000
/* Consecutive dropped records before we give the input back rather than keep
 * grabbing for an app that is plainly not reading. */
#define MAX_DROPS 2000
/* Poll slice; also how often the timeout above is re-checked. */
#define POLL_SLICE_MS 250

#define BITS_PER_LONG  (8 * (int)sizeof(unsigned long))
#define NBITS(x)       ((((x) - 1) / BITS_PER_LONG) + 1)
#define TEST_BIT(bit, arr) \
    (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

struct dev_entry {
    int  fd;
    int  cls;        /* IGRAB_CLASS_* */
    int  grabbed;    /* 0 => watched only (toggle detection), events not forwarded */
    int  announced;  /* the app has received this entry's DEVICE record */
    int  multitouch;
    int  clickpad;   /* one button under the pad: BTN_LEFT alone means "a click" */
    int  min_x, max_x, min_y, max_y;
    char name[80];
};

static struct dev_entry g_devs[IGRAB_MAX_DEVICES];
static int g_ndevs = 0;
static volatile sig_atomic_t g_quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Connect to the app's listening socket, which lives in the abstract namespace
 * (that is what android.net.LocalServerSocket creates). Abstract means there is
 * no file to find, chmod or clean up, and root needs one SELinux permission
 * fewer than for a socket inside the app's data directory. socket_utils'
 * connect_unix() only speaks filesystem paths, hence this one.
 */
static int connect_abstract(const char *name)
{
    size_t len = strlen(name);
    struct sockaddr_un addr;
    if (len + 1 > sizeof(addr.sun_path))
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';            /* leading NUL == abstract namespace */
    memcpy(addr.sun_path + 1, name, len);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);

    if (connect(fd, (struct sockaddr *)&addr, alen) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Release every grab and close. Called on every exit path; the kernel would do
 * it anyway when the process dies, but doing it explicitly keeps the window
 * where Android sees no input as short as possible. */
static void release_all(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            continue;
        if (g_devs[i].grabbed)
            ioctl(g_devs[i].fd, EVIOCGRAB, 0);
        close(g_devs[i].fd);
        g_devs[i].fd = -1;
    }
    g_ndevs = 0;
}

/* ---------------- record transport ---------------- */

/* Tail of a record that only went out partially. A stream socket can accept
 * fewer than 32 bytes, and losing the rest would desync the framing for good,
 * so the remainder is held here and flushed before anything else. */
static uint8_t g_pending[IGRAB_REC_SIZE];
static size_t  g_pending_len = 0;
static int     g_drops = 0;
static int     g_need_resync = 0;

static int writable(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLOUT };
    return poll(&p, 1, 0) > 0 && (p.revents & POLLOUT);
}

/* Flush a partial record. 1 = nothing pending anymore, 0 = still pending. */
static int flush_pending(int fd)
{
    while (g_pending_len > 0) {
        if (!writable(fd))
            return 0;
        ssize_t n = send(fd, g_pending + (IGRAB_REC_SIZE - g_pending_len),
                         g_pending_len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            g_pending_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        return -1;
    }
    return 1;
}

/*
 * Send one record, never blocking. Under back-pressure the record is dropped
 * and counted: input that cannot be delivered is worth less than the ability to
 * keep polling for the toggle key. Returns 0 on success or a drop, -1 when the
 * socket is dead.
 */
static int send_rec(int fd, const struct igrab_rec *rec)
{
    int fp = flush_pending(fd);
    if (fp < 0)
        return -1;
    if (fp == 0) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }

    if (!writable(fd)) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }

    ssize_t n = send(fd, rec, IGRAB_REC_SIZE, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == IGRAB_REC_SIZE) {
        g_drops = 0;
        return 0;
    }
    if (n > 0) {
        /* Partial: stash the tail so the next call restores the framing. */
        memcpy(g_pending, rec, IGRAB_REC_SIZE);
        g_pending_len = IGRAB_REC_SIZE - (size_t)n;
        g_drops = 0;
        return 0;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        g_drops++;
        g_need_resync = 1;
        return 0;
    }
    return -1;
}

/* Tell the app that records were lost, so it can release every key/contact it
 * still believes is held instead of leaving them stuck on the desktop. Mirrors
 * what the kernel does with SYN_DROPPED. */
static int send_resync(int fd)
{
    struct igrab_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_EVENT;
    rec.dev   = IGRAB_DEV_ALL;
    rec.etype = EV_SYN;
    rec.code  = SYN_DROPPED;
    if (send_rec(fd, &rec) < 0)
        return -1;
    /* Only clear once it actually left: a dropped resync marker is worse than
     * useless, it would hide the drop it was meant to report. */
    if (g_drops == 0 && g_pending_len == 0)
        g_need_resync = 0;
    return 0;
}

/*
 * Finish a partial record and, after any loss, deliver the global SYN_DROPPED
 * marker before normal traffic resumes. This is also driven by POLLOUT while
 * idle: waiting for another input event leaves a final KEY_UP or SYN_REPORT
 * stranded forever when the user stops moving.
 *
 *  1: normal records may be sent; 0: socket still back-pressured; -1: dead.
 */
static int recover_output(int fd)
{
    int flushed = flush_pending(fd);
    if (flushed < 0)
        return -1;
    if (flushed == 0)
        return 0;
    if (!g_need_resync)
        return 1;
    if (send_resync(fd) < 0)
        return -1;
    return (!g_need_resync && g_pending_len == 0) ? 1 : 0;
}

static int send_bye(int fd, int reason)
{
    /* Finish any half-written record first: leaving one truncated would put the
     * app's parser out of step with the stream, and this is the one record it
     * really has to be able to read. */
    if (g_pending_len > 0) {
        send_all(fd, g_pending + (IGRAB_REC_SIZE - g_pending_len), g_pending_len);
        g_pending_len = 0;
    }

    struct igrab_rec rec;
    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_BYE;
    rec.value = reason;
    /* Blocking on purpose: this is the last record and the socket has a send
     * timeout, so it cannot hang. Losing it would leave the app waiting for a
     * stream that has already ended. */
    return send_all(fd, &rec, sizeof(rec));
}

/* ---------------- device discovery ---------------- */

static int any_key_bit(const unsigned long *key)
{
    for (int i = 0; i <= KEY_MAX; i++) {
        if (TEST_BIT(i, key))
            return 1;
    }
    return 0;
}

/* A full keyboard is allowed to advertise KEY_POWER: many docks expose that
 * capability on their ordinary keyboard node while the tablet's real power
 * switch is a separate node. Non-keyboard nodes have no safe way to split one
 * evdev fd after EVIOCGRAB, so a Power-bearing panel/mouse/controller stays
 * watch-only and Android retains its lock/wake path. */
static int has_alpha_keys(const unsigned long *key)
{
    static const int letters[] = {
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y,
        KEY_A, KEY_S, KEY_D, KEY_F, KEY_G,
        KEY_Z, KEY_X, KEY_C, KEY_V,
    };
    for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); i++) {
        if (!TEST_BIT(letters[i], key))
            return 0;
    }
    return 1;
}

static int has_power_key(const unsigned long *key)
{
    if (TEST_BIT(KEY_POWER, key))
        return 1;
#ifdef KEY_POWER2
    if (TEST_BIT(KEY_POWER2, key))
        return 1;
#endif
    return 0;
}

static int is_physical_android_key(int code)
{
    switch (code) {
    case KEY_POWER:
#ifdef KEY_POWER2
    case KEY_POWER2:
#endif
    case KEY_WAKEUP:
    case KEY_SLEEP:
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN:
    case KEY_MUTE:
        return 1;
    default:
        return 0;
    }
}

static int has_only_physical_android_keys(const unsigned long *key)
{
    int any = 0;
    for (int i = 0; i <= KEY_MAX; i++) {
        if (!TEST_BIT(i, key))
            continue;
        any = 1;
        if (!is_physical_android_key(i))
            return 0;
    }
    return any;
}

static void read_abs_range(int fd, int axis, int *min, int *max)
{
    struct input_absinfo info;
    if (ioctl(fd, EVIOCGABS(axis), &info) == 0 && info.maximum > info.minimum) {
        *min = info.minimum;
        *max = info.maximum;
    }
}

/*
 * Open one /dev/input node and decide what it is. Returns 0 when the node was
 * taken (entry filled in, fd owned by the caller), -1 when it was skipped.
 *
 * Classification is by capability bits, never by event number: the numbering
 * shifts as soon as a keyboard or dock is attached.
 */
static int inspect_device(const char *path, struct dev_entry *out)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    unsigned long ev[NBITS(EV_MAX)];
    unsigned long key[NBITS(KEY_MAX)];
    unsigned long abs[NBITS(ABS_MAX)];
    unsigned long rel[NBITS(REL_MAX)];
    unsigned long prop[NBITS(INPUT_PROP_MAX)];
    memset(ev, 0, sizeof(ev));
    memset(key, 0, sizeof(key));
    memset(abs, 0, sizeof(abs));
    memset(rel, 0, sizeof(rel));
    memset(prop, 0, sizeof(prop));

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) {
        close(fd);
        return -1;
    }
    if (TEST_BIT(EV_KEY, ev))
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
    if (TEST_BIT(EV_ABS, ev))
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    if (TEST_BIT(EV_REL, ev))
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    ioctl(fd, EVIOCGPROP(sizeof(prop)), prop);

    memset(out, 0, sizeof(*out));
    out->fd = fd;
    if (ioctl(fd, EVIOCGNAME(sizeof(out->name)), out->name) < 0)
        out->name[0] = '\0';
    out->name[sizeof(out->name) - 1] = '\0';

    int mt      = TEST_BIT(ABS_MT_POSITION_X, abs) && TEST_BIT(ABS_MT_POSITION_Y, abs);
    int abs_xy  = TEST_BIT(ABS_X, abs) && TEST_BIT(ABS_Y, abs);
    int rel_xy  = TEST_BIT(REL_X, rel) && TEST_BIT(REL_Y, rel);
    int direct  = TEST_BIT(INPUT_PROP_DIRECT, prop);
    int pointer = TEST_BIT(INPUT_PROP_POINTER, prop);
    int touch   = TEST_BIT(BTN_TOUCH, key);
    int click   = TEST_BIT(BTN_LEFT, key);
    int pen     = TEST_BIT(BTN_TOOL_PEN, key) || TEST_BIT(BTN_STYLUS, key);
    int alpha   = has_alpha_keys(key);
    int power   = has_power_key(key);
    int keys    = any_key_bit(key);

    /* A stylus digitizer overlaps the panel; forwarding it as a second finger
     * only confuses the gesture engine, and its barrel keys are useless here. */
    if (pen) {
        close(fd);
        return -1;
    }

    /* A single EVIOCGRAB applies to the whole node. Do not take a touchscreen,
     * touchpad, mouse or consumer-control node that carries Power: forwarding
     * its KEY_POWER to the desktop cannot make Android lock the device, so the
     * ACTION_SCREEN_OFF safety receiver would never run. Full keyboards retain
     * the existing exception above because their ordinary keys must not leak. */
    if (power && !alpha) {
        out->cls = IGRAB_CLASS_KEYBOARD;
        out->grabbed = 0;
        return 0;
    }

    /* Contacts, not axes: a gamepad also reports ABS_X/ABS_Y, and its sticks
     * must not be mistaken for a finger. */
    int contacts = mt || (abs_xy && (direct || pointer || touch));

    if (contacts) {
        /* The property bits say it outright when they are set. Panels that set
         * neither are told apart by the clickpad button: a touchscreen has no
         * BTN_LEFT, an integrated pad almost always does. */
        if (direct)
            out->cls = IGRAB_CLASS_TOUCHSCREEN;
        else if (pointer || click)
            out->cls = IGRAB_CLASS_TOUCHPAD;
        else
            out->cls = IGRAB_CLASS_TOUCHSCREEN;
    } else if (rel_xy) {
        out->cls = IGRAB_CLASS_MOUSE;
    } else if (keys) {
        /* Physical Android buttons must remain available to Android while
         * immersive mode is active. Keep these fds open watch-only so the bound
         * toggle key can still release the session, but do not EVIOCGRAB them.
         * Non-keyboard Power nodes returned above; this path protects the rest
         * of the dedicated Android button devices. */
        if (has_only_physical_android_keys(key)) {
            out->cls = IGRAB_CLASS_KEYBOARD;
            out->grabbed = 0;
            return 0;
        }
        out->cls = IGRAB_CLASS_KEYBOARD;
    } else {
        close(fd);
        return -1;
    }

    out->multitouch = mt;
    /* A clickpad has one button under the whole surface, so every press comes in
     * as BTN_LEFT and the app has to work out left vs right from where the
     * finger is. INPUT_PROP_BUTTONPAD is the authoritative bit; a pad that
     * offers no BTN_RIGHT at all is one in practice too. */
    out->clickpad = TEST_BIT(INPUT_PROP_BUTTONPAD, prop)
            || (click && !TEST_BIT(BTN_RIGHT, key));
    if (out->cls == IGRAB_CLASS_TOUCHSCREEN || out->cls == IGRAB_CLASS_TOUCHPAD) {
        if (mt) {
            read_abs_range(fd, ABS_MT_POSITION_X, &out->min_x, &out->max_x);
            read_abs_range(fd, ABS_MT_POSITION_Y, &out->min_y, &out->max_y);
        }
        /* Single-touch panels only have ABS_X/ABS_Y; multitouch ones still use
         * them as a fallback when the MT axes report nothing usable. */
        if (out->max_x <= out->min_x)
            read_abs_range(fd, ABS_X, &out->min_x, &out->max_x);
        if (out->max_y <= out->min_y)
            read_abs_range(fd, ABS_Y, &out->min_y, &out->max_y);
        if (out->max_x <= out->min_x || out->max_y <= out->min_y) {
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        /* Someone else already owns it exclusively. Watching it would double up
         * with Android's own delivery, so let it go entirely. */
        LOGE("grab '%s' failed: %s", out->name, strerror(errno));
        close(fd);
        return -1;
    }
    out->grabbed = 1;
    return 0;
}

static int scan_devices(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        LOGE("opendir(/dev/input): %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int grabbed = 0;
    while ((ent = readdir(dir)) != NULL && g_ndevs < IGRAB_MAX_DEVICES) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        if (inspect_device(path, &g_devs[g_ndevs]) < 0)
            continue;
        LOGI("%s '%s' class=%d %s", path, g_devs[g_ndevs].name,
             g_devs[g_ndevs].cls, g_devs[g_ndevs].grabbed ? "GRABBED" : "watch-only");
        if (g_devs[g_ndevs].grabbed)
            grabbed++;
        g_ndevs++;
    }
    closedir(dir);
    return grabbed;
}

/* ---------------- mid-session device hotplug ---------------- */

/* Device number of an event node; -1 when it is gone or not a device. */
static int node_identity(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
        return -1;
    return (int)st.st_rdev;
}

/* Whether a tracked device still owns this node (dead fds do not count: the
 * device was unplugged, and a reconnect with the same number must be treated
 * as new). */
static int already_tracked(int rdev)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            continue;
        struct stat st;
        if (fstat(g_devs[i].fd, &st) == 0 && (int)st.st_rdev == rdev)
            return 1;
    }
    return 0;
}

static void fill_device_record(struct igrab_rec *rec, int idx)
{
    const struct dev_entry *de = &g_devs[idx];
    memset(rec, 0, sizeof(*rec));
    rec->rtype = IGRAB_REC_DEVICE;
    rec->dev   = (uint16_t)idx;
    rec->etype = (uint16_t)de->cls;
    rec->aux[IGRAB_AUX_MIN_X] = de->min_x;
    rec->aux[IGRAB_AUX_MAX_X] = de->max_x;
    rec->aux[IGRAB_AUX_MIN_Y] = de->min_y;
    rec->aux[IGRAB_AUX_MAX_Y] = de->max_y;
    rec->aux[IGRAB_AUX_FLAGS] =
        (de->grabbed ? IGRAB_DEV_GRABBED : 0) |
        (de->multitouch ? IGRAB_DEV_MULTITOUCH : 0) |
        (de->clickpad ? IGRAB_DEV_CLICKPAD : 0);
}

/* Reuse a detached device id before growing the table. The Java side only sees
 * a replacement DEVICE record after the global resync sent on removal, so the
 * old state cannot be confused with the new node. */
static int find_free_device_slot(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0)
            return i;
    }
    return g_ndevs < IGRAB_MAX_DEVICES ? g_ndevs : -1;
}

/* A hotplugged device is grabbed before it can leak input to Android, but its
 * events stay local until its DEVICE record is definitely on the stream. The
 * non-blocking transport may drop a record under pressure, so retry on every
 * writable turn instead of treating that one announcement as expendable. */
static int announce_pending_devices(int sock)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd < 0 || g_devs[i].announced)
            continue;

        int ready = recover_output(sock);
        if (ready <= 0)
            return ready;

        struct igrab_rec rec;
        fill_device_record(&rec, i);
        if (send_rec(sock, &rec) < 0)
            return -1;
        if (g_pending_len != 0 || g_need_resync)
            return 0;
        g_devs[i].announced = 1;
        LOGI("announced device %d '%s'", i, g_devs[i].name);
    }
    return 1;
}

static int has_unannounced_devices(void)
{
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].fd >= 0 && !g_devs[i].announced)
            return 1;
    }
    return 0;
}

/*
 * Grab any input device that appeared since the last scan. A Bluetooth mouse
 * that went to sleep reconnects as a brand-new node mid-session; without this
 * it would keep feeding Android, and the app would forward it unaccelerated
 * and ungated. New devices get the same classification rules as at session
 * start, a DEVICE record announces them to the app, and the toggle key is
 * watched on them like on everything else.
 */
static void scan_new_devices(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int rdev = node_identity(path);
        if (rdev < 0 || already_tracked(rdev))
            continue;

        int idx = find_free_device_slot();
        if (idx < 0)
            break;

        struct dev_entry de;
        if (inspect_device(path, &de) < 0)
            continue;   /* uninteresting, ungrabbable, or already grabbed by us */
        g_devs[idx] = de;
        if (idx == g_ndevs)
            g_ndevs++;
        LOGI("new device %s '%s' class=%d %s", path, de.name, de.cls,
             de.grabbed ? "GRABBED (awaiting DEVICE)" : "watch-only (awaiting DEVICE)");
    }
    closedir(dir);
}

static int send_device_list(int sock, int grabbed)
{
    struct igrab_rec rec;

    memset(&rec, 0, sizeof(rec));
    rec.rtype  = IGRAB_REC_HELLO;
    rec.value  = IGRAB_PROTO_VERSION;
    rec.aux[0] = grabbed;
    rec.aux[1] = (int32_t)getpid();
    if (send_all(sock, &rec, sizeof(rec)) < 0)
        return -1;

    for (int i = 0; i < g_ndevs; i++) {
        fill_device_record(&rec, i);
        if (send_all(sock, &rec, sizeof(rec)) < 0)
            return -1;
        g_devs[i].announced = 1;
    }

    memset(&rec, 0, sizeof(rec));
    rec.rtype = IGRAB_REC_READY;
    rec.aux[0] = grabbed;
    return send_all(sock, &rec, sizeof(rec));
}

/* ---------------- main loop ---------------- */

/*
 * Drain one device. Returns 1 to keep going, 0 when the toggle key was pressed
 * and the session must end, -1 when the socket died.
 */
static int pump_device(int sock, int idx, int toggle)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(g_devs[idx].fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;   /* drained */
            return -2;      /* ENODEV: the device was unplugged */
        }
        if (n == 0)
            return -2;      /* would otherwise spin: poll keeps reporting POLLIN */

        int count = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count; i++) {
            struct input_event *e = &evs[i];

            if (e->type == EV_KEY && e->code == toggle) {
                /* Press ends the session; the release and any auto-repeat are
                 * swallowed so the key never reaches the desktop. Checked before
                 * the grabbed test so a watch-only node can still trigger it. */
                if (e->value == 1)
                    return 0;
                continue;
            }

            if (!g_devs[idx].grabbed)
                continue;   /* Android still owns it; forwarding would duplicate */

            /* A hotplugged node stays silent until the Java side has its DEVICE
             * descriptor. Discarding a transition here requires a global reset:
             * otherwise its first delivered record could be a KEY_UP for a key
             * the desktop never saw go down. */
            if (!g_devs[idx].announced) {
                g_need_resync = 1;
                continue;
            }

            int ready = recover_output(sock);
            if (ready < 0)
                return -1;
            if (ready == 0) {
                g_drops++;
                g_need_resync = 1;
                if (g_drops > MAX_DROPS) {
                    LOGE("app is not reading (%d dropped records); giving input back",
                         g_drops);
                    return -3;
                }
                continue;
            }

            struct igrab_rec rec;
            memset(&rec, 0, sizeof(rec));
            rec.rtype = IGRAB_REC_EVENT;
            rec.dev   = (uint16_t)idx;
            rec.etype = e->type;
            rec.code  = e->code;
            rec.value = e->value;
            if (send_rec(sock, &rec) < 0)
                return -1;
            if (g_drops > MAX_DROPS) {
                LOGE("app is not reading (%d dropped records); giving input back",
                     g_drops);
                return -3;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        LOGE("usage: %s <bridge_socket> <toggle_scancode>", argv[0]);
        return 1;
    }

    const char *bridge = argv[1];
    int toggle = atoi(argv[2]);
    /* A session with no way out is never started: the toggle key is the only
     * thing that guarantees the user can hand the input back. */
    if (toggle <= 0 || toggle > KEY_MAX) {
        LOGE("refusing to grab without a valid toggle scancode (%d)", toggle);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    int sock = connect_abstract(bridge);
    if (sock < 0) {
        LOGE("connect to bridge '%s' failed: %s", bridge, strerror(errno));
        return 3;
    }

    /* The hello/device records are sent blocking; bound them so a peer that
     * connects and then stops reading cannot wedge the handshake. A large send
     * buffer keeps the event stream from dropping during a UI hiccup. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int sndbuf = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int grabbed = scan_devices();
    if (grabbed <= 0) {
        LOGE("no grabbable input devices");
        send_bye(sock, IGRAB_BYE_ERROR);
        release_all();
        close(sock);
        return 4;
    }

    if (send_device_list(sock, grabbed) < 0) {
        LOGE("handshake failed: %s", strerror(errno));
        release_all();
        close(sock);
        return 5;
    }
    LOGI("immersive session started: %d grabbed device(s), toggle=%d",
         grabbed, toggle);

    struct pollfd pfds[IGRAB_MAX_DEVICES + 1];
    long last_beat = now_ms();
    long last_scan = now_ms();
    int reason = IGRAB_BYE_PEER_GONE;

    for (;;) {
        if (g_quit) {
            /* SIGTERM comes from the app's own last-resort kill, i.e. it has
             * already given up on the session. */
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }

        int nfds = 0;
        pfds[nfds].fd = sock;
        pfds[nfds].events = POLLIN;
        if (g_pending_len != 0 || g_need_resync || has_unannounced_devices())
            pfds[nfds].events |= POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;
        for (int i = 0; i < g_ndevs; i++) {
            if (g_devs[i].fd < 0)
                continue;
            pfds[nfds].fd = g_devs[i].fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int ret = poll(pfds, (nfds_t)nfds, POLL_SLICE_MS);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            reason = IGRAB_BYE_ERROR;
            break;
        }

        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            char beat[64];
            ssize_t n = recv(sock, beat, sizeof(beat), MSG_DONTWAIT);
            if (n == 0) {
                reason = IGRAB_BYE_PEER_GONE;   /* app closed / died */
                break;
            }
            if (n > 0)
                last_beat = now_ms();
        }

        /* A frozen app keeps its socket open but stops beating. Treat that the
         * same as death: the input has to go back to Android either way. */
        if (now_ms() - last_beat > HEARTBEAT_TIMEOUT_MS) {
            LOGE("no heartbeat for %d ms; releasing", HEARTBEAT_TIMEOUT_MS);
            reason = IGRAB_BYE_STALLED;
            break;
        }

        /* Finish any partial stream work even when the user has stopped moving.
         * POLLOUT wakes this promptly; the unconditional call also handles a
         * race where the socket became writable between poll() and here. */
        int output_ready = recover_output(sock);
        if (output_ready < 0) {
            reason = IGRAB_BYE_PEER_GONE;
            break;
        }

        /* Bluetooth devices wake and reconnect as new nodes. A device is held
         * locally until announce_pending_devices() has put its DEVICE record on
         * the stream, so Android and the desktop can never both own it. */
        if (now_ms() - last_scan > RESCAN_INTERVAL_MS) {
            scan_new_devices();
            last_scan = now_ms();
        }

        if (output_ready > 0) {
            int announced = announce_pending_devices(sock);
            if (announced < 0) {
                reason = IGRAB_BYE_PEER_GONE;
                break;
            }
        }

        int stop = 0;
        for (int p = 1; p < nfds && !stop; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int idx = -1;
            for (int i = 0; i < g_ndevs; i++) {
                if (g_devs[i].fd == pfds[p].fd) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
                continue;

            int r = pump_device(sock, idx, toggle);
            if (r == 0) {
                reason = IGRAB_BYE_TOGGLE;
                stop = 1;
            } else if (r == -1) {
                reason = IGRAB_BYE_PEER_GONE;
                stop = 1;
            } else if (r == -3) {
                reason = IGRAB_BYE_STALLED;
                stop = 1;
            } else if (r == -2) {
                /* Device vanished (unplugged dock/keyboard). Drop it and carry
                 * on -- the remaining devices are still grabbed. Release every
                 * remote state first; otherwise a key/button held at unplug stays
                 * pressed until the whole immersive session ends. */
                LOGI("device '%s' went away", g_devs[idx].name);
                if (g_devs[idx].grabbed && g_devs[idx].announced)
                    g_need_resync = 1;
                if (g_devs[idx].grabbed)
                    ioctl(g_devs[idx].fd, EVIOCGRAB, 0);
                close(g_devs[idx].fd);
                memset(&g_devs[idx], 0, sizeof(g_devs[idx]));
                g_devs[idx].fd = -1;
            }
        }
        if (stop)
            break;

        /* Every grabbed device is gone (dock unplugged mid-session). There is
         * nothing left to hold, and staying alive would only keep the app
         * believing it is still immersive. */
        int alive = 0;
        for (int i = 0; i < g_ndevs; i++) {
            if (g_devs[i].fd >= 0 && g_devs[i].grabbed)
                alive++;
        }
        if (alive == 0) {
            LOGI("all grabbed devices are gone; ending session");
            reason = IGRAB_BYE_ERROR;
            break;
        }
    }

    /* Ungrab first: the app is about to be told the session ended, and the
     * sooner Android has its input back the shorter the dead window. */
    release_all();
    send_bye(sock, reason);
    close(sock);
    LOGI("immersive session ended (reason=%d)", reason);
    return 0;
}
