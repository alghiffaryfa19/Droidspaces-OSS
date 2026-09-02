#define _GNU_SOURCE
#include "camera_service.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImageReader.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "tracy_zones.h"

#define TAG "AnlandCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Fallback slot size when a camera's max resolution is unknown (1080p I420). */
#define CAM_FALLBACK_BYTES (1920 * 1080 * 3 / 2)

/*
 * Per-client channel set: one per consumer window (one-to-one with its producer),
 * bound to the WINDOW lifetime. Created lazily on first allocate, torn down only by
 * camera_release_client() (nativeDestroy). Owns its own ctrl socketpair and per-camera
 * stream socketpair; the frame buffer itself is NOT per client -- all recorders of a
 * camera share the one ashmem region in g_cam (see below). `recording[cam]` marks
 * membership in camera `cam`'s recording list (producer START_RECORD adds it).
 */
struct camera_client {
    void *owner;                 /* instance token (consumer_state*) from userdata */

    int   ctrl_local, ctrl_remote;
    int   stream_local[MAX_CAMERAS], stream_remote[MAX_CAMERAS];
    int   alloc_fds[1 + MAX_CAMERAS];   /* { ctrl_remote, stream_remote[0..N-1] } */

    bool recording[MAX_CAMERAS];   /* in camera cam's recording list */

    int  busy;   /* threads currently referencing this client via a snapshot */
};

/*
 * Global state: the physical capture pipeline (opened once per camera, shared) plus
 * a growable registry of the live per-client channel sets.
 *
 * Frame buffer sharing: each camera owns ONE ashmem region (shm_fd/shm_ptr, sized for
 * its max resolution) shared by every recorder. The reader packs each frame ONCE into
 * cur_slot, sends READY to every recording client, and records how many were sent in
 * slot_pending[cam][slot]. Every producer maps the same region read-only and DONEs the
 * slot when it has copied it out; the DONE handler decrements slot_pending, and the
 * slot only becomes writable again once it hits 0 (all readers finished). The reader
 * blocks on slot_cond until then -- concurrent reads are safe, overwrites are not.
 */
static struct camera_hw {
    ACameraManager *cam_mgr;
    char           *cam_ids[MAX_CAMERAS];
    int             max_w[MAX_CAMERAS];
    int             max_h[MAX_CAMERAS];
    int             num_cameras;

    pthread_mutex_t cap_lock[MAX_CAMERAS];      /* guard start/stop races */
    ACameraDevice               *device[MAX_CAMERAS];
    ACameraCaptureSession       *session[MAX_CAMERAS];
    AImageReader                *reader[MAX_CAMERAS];
    ANativeWindow               *window[MAX_CAMERAS];
    ACaptureSessionOutputContainer *out_container[MAX_CAMERAS];
    ACaptureSessionOutput       *session_output[MAX_CAMERAS];
    ACameraOutputTarget         *out_target[MAX_CAMERAS];
    ACaptureRequest             *request[MAX_CAMERAS];
    AImageReader_ImageListener      img_listener[MAX_CAMERAS];
    ACameraDevice_StateCallbacks    dev_cb[MAX_CAMERAS];
    ACameraCaptureSession_stateCallbacks ses_cb[MAX_CAMERAS];
    int  rec_count[MAX_CAMERAS];   /* #clients recording cam; capture runs while >0 */

    /* Shared per-camera frame buffer + slot accounting. */
    int      shm_fd[MAX_CAMERAS];
    uint8_t *shm_ptr[MAX_CAMERAS];
    size_t   slot_bytes[MAX_CAMERAS];
    int      cur_slot[MAX_CAMERAS];                    /* reader write cursor (reader only) */
    int      last_sent[MAX_CAMERAS];                   /* #recorders the last frame went to */
    int      slot_pending[MAX_CAMERAS][CAMERA_SLOTS];  /* outstanding DONEs per slot */
    pthread_mutex_t slot_lock[MAX_CAMERAS];            /* guards slot_pending */
    pthread_cond_t  slot_cond[MAX_CAMERAS];            /* signalled when a slot hits 0 */

    pthread_mutex_t clients_lock;  /* guards the array + every client's busy/recording */
    pthread_cond_t  clients_cond;  /* signalled when a client's busy drops */
    struct camera_client **clients;
    size_t clients_cap;

    pthread_t      io_thread;
    volatile bool  running;
    volatile bool  ready;
} g_cam;

static void cam_stop_recording(int cam);

/* Per-camera frame-mark names for Tracy (one frame sequence per reader thread). */
static const char *const cam_framemark[MAX_CAMERAS] = {
    "cam0", "cam1", "cam2", "cam3", "cam4", "cam5", "cam6", "cam7",
};

/* ---------------------------------------------------------------
 * Client registry. clients_lock guards the array and every client's
 * busy/recording fields. A snapshot bumps busy on each client so worker
 * threads can use them without holding clients_lock across the (brief,
 * non-blocking) per-frame work; camera_release_client waits for busy==0.
 * --------------------------------------------------------------- */

/* Insert into the first free slot, growing the array if full (no fixed cap).
 * Caller holds clients_lock. */
static void registry_add_locked(struct camera_client *c)
{
    for (size_t i = 0; i < g_cam.clients_cap; i++) {
        if (!g_cam.clients[i]) { g_cam.clients[i] = c; return; }
    }
    size_t old = g_cam.clients_cap;
    size_t ncap = old ? old * 2 : 4;
    struct camera_client **n = realloc(g_cam.clients, ncap * sizeof(*n));
    if (!n) { LOGE("registry: realloc to %zu failed", ncap); return; }
    for (size_t i = old; i < ncap; i++) n[i] = NULL;
    n[old] = c;
    g_cam.clients = n;
    g_cam.clients_cap = ncap;
}

static void registry_remove_locked(struct camera_client *c)
{
    for (size_t i = 0; i < g_cam.clients_cap; i++)
        if (g_cam.clients[i] == c) { g_cam.clients[i] = NULL; return; }
}

static struct camera_client *registry_find_locked(void *owner)
{
    for (size_t i = 0; i < g_cam.clients_cap; i++)
        if (g_cam.clients[i] && g_cam.clients[i]->owner == owner)
            return g_cam.clients[i];
    return NULL;
}

/* Snapshot all live clients, bumping busy so they stay alive until release. */
static struct camera_client **snapshot_clients(int *out_n)
{
    pthread_mutex_lock(&g_cam.clients_lock);
    int n = 0;
    for (size_t i = 0; i < g_cam.clients_cap; i++)
        if (g_cam.clients[i]) n++;
    struct camera_client **arr = n ? malloc((size_t)n * sizeof(*arr)) : NULL;
    int k = 0;
    if (arr) {
        for (size_t i = 0; i < g_cam.clients_cap && k < n; i++)
            if (g_cam.clients[i]) { g_cam.clients[i]->busy++; arr[k++] = g_cam.clients[i]; }
    }
    pthread_mutex_unlock(&g_cam.clients_lock);
    *out_n = k;
    return arr;
}

static void release_snapshot(struct camera_client **arr, int n)
{
    if (!arr) return;
    pthread_mutex_lock(&g_cam.clients_lock);
    for (int i = 0; i < n; i++) arr[i]->busy--;
    pthread_cond_broadcast(&g_cam.clients_cond);
    pthread_mutex_unlock(&g_cam.clients_lock);
    free(arr);
}

/* Send a fixed stream-control message, optionally with an fd attached (SCM_RIGHTS). */
static void send_stream(int sock, uint8_t type, uint8_t slot, uint16_t fmt,
                        uint32_t a, uint32_t b, int fd)
{
    struct cam_stream_msg m = { .type = type, .slot = slot, .fmt = fmt, .a = a, .b = b };
    struct iovec iov = { .iov_base = &m, .iov_len = sizeof(m) };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } cmsg;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT) < 0)
        LOGE("stream: send type=%u failed: %s", type, strerror(errno));
}

/* ---------------------------------------------------------------
 * NV21 packing into the shared per-camera slot (Y + interleaved V,U),
 * always w*h*3/2 bytes with Y-stride = w.
 * --------------------------------------------------------------- */

static void cam_frame_ready(struct camera_client *c, int cam, int slot, int w, int h, int fmt)
{
    send_stream(c->stream_local[cam], CAM_STREAM_READY, (uint8_t)slot,
                (uint16_t)fmt, (uint32_t)w, (uint32_t)h, -1);
}

/* Pack a YUV_420_888 frame into camera cam's shared slot as NV21 (Y + interleaved V,U). */
static int cam_pack_frame(int cam, int slot,
                          const uint8_t *y, int yRow,
                          const uint8_t *u, int uRow, int uPix,
                          const uint8_t *v, int vRow, int vPix,
                          int w, int h)
{
    if (!y || !u || !v)
        return CAM_FMT_I420;
    uint8_t *dst = g_cam.shm_ptr[cam] + (size_t)slot * g_cam.slot_bytes[cam];
    size_t ySize = (size_t)w * h;
    if (ySize + ySize / 2 > g_cam.slot_bytes[cam])
        return CAM_FMT_I420;   /* shouldn't happen (slot sized for max) */

    uint8_t *p = dst;
    if (yRow == w) {
        memcpy(p, y, ySize);
        p += ySize;
    } else {
        for (int r = 0; r < h; r++) { memcpy(p, y + (size_t)r * yRow, (size_t)w); p += w; }
    }

    int chh = h / 2;
    int cw = w / 2;
    if (uPix == 2 && vPix == 2 && v < u) {
        /* NV21 source (V before U): already NV21. */
        for (int r = 0; r < chh; r++) { memcpy(p, v + (size_t)r * vRow, (size_t)w); p += w; }
    } else if (uPix == 2 && vPix == 2) {
        /* NV12 source: swap each pair to V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;
            for (int k = 0; k < cw; k++) { p[2*k] = su[2*k+1]; p[2*k+1] = su[2*k]; }
            p += w;
        }
    } else {
        /* Planar source: interleave V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;
            const uint8_t *sv = v + (size_t)r * vRow;
            for (int k = 0; k < cw; k++) { p[2*k] = sv[k]; p[2*k+1] = su[k]; }
            p += w;
        }
    }
    return CAM_FMT_NV21;
}

/*
 * AImageReader callback (NDK reader thread). Acquires the latest frame for physical
 * camera `cam`, waits until the slot it is about to reuse has been fully consumed
 * (every producer that got the previous frame there has DONE'd it -- the shared buffer
 * must not be overwritten while anyone is still reading), packs it ONCE, then sends
 * READY to every recording client and records the recipient count so the DONE handler
 * knows when the slot is free again. The wait is bounded so a dead producer can't
 * freeze capture forever.
 */
static void on_frame_available(void *context, AImageReader *reader)
{
    int cam = (int)(intptr_t)context;
    if (cam < 0 || cam >= g_cam.num_cameras)
        return;

    AImage *image = NULL;
    TracyCZoneN(zAcquire, "cam acquireLatestImage", 1);
    media_status_t acq = AImageReader_acquireLatestImage(reader, &image);
    TracyCZoneEnd(zAcquire);
    if (acq != AMEDIA_OK || image == NULL)
        return;

    int32_t numPlanes = 0;
    AImage_getNumberOfPlanes(image, &numPlanes);
    if (numPlanes < 3) { AImage_delete(image); return; }

    int32_t w = 0, h = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);

    uint8_t *yData = NULL, *uData = NULL, *vData = NULL;
    int yLen = 0, uLen = 0, vLen = 0;
    int32_t yRow = 0, uRow = 0, vRow = 0;
    int32_t yPix = 0, uPix = 0, vPix = 0;
    AImage_getPlaneData(image, 0, &yData, &yLen);
    AImage_getPlaneData(image, 1, &uData, &uLen);
    AImage_getPlaneData(image, 2, &vData, &vLen);
    AImage_getPlaneRowStride(image, 0, &yRow);
    AImage_getPlaneRowStride(image, 1, &uRow);
    AImage_getPlaneRowStride(image, 2, &vRow);
    AImage_getPlanePixelStride(image, 0, &yPix);
    AImage_getPlanePixelStride(image, 1, &uPix);
    AImage_getPlanePixelStride(image, 2, &vPix);
    (void)yPix; (void)yLen; (void)uLen; (void)vLen;

    if (yData && uData && vData) {
        int slot = g_cam.cur_slot[cam];

        /* Wait for every reader of the previous frame in this slot to finish. Bounded
         * (1s) so a stuck/dead producer can't stall capture indefinitely. */
        TracyCZoneN(zSlotWait, "cam wait slot free", 1);
        pthread_mutex_lock(&g_cam.slot_lock[cam]);
        while (g_cam.slot_pending[cam][slot] > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            if (pthread_cond_timedwait(&g_cam.slot_cond[cam], &g_cam.slot_lock[cam], &ts)
                    == ETIMEDOUT) {
                LOGE("cam %d slot %d: %d producer(s) never DONE'd; reclaiming",
                     cam, slot, g_cam.slot_pending[cam][slot]);
                g_cam.slot_pending[cam][slot] = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_cam.slot_lock[cam]);
        TracyCZoneEnd(zSlotWait);

        /* Pack the frame ONCE into the shared slot, then fan out READY to all
         * recorders. slot_pending is set under the lock before we drop it so a fast
         * producer's DONE can't race ahead of the count. */
        TracyCZoneN(zPack, "cam pack NV21", 1);
        int fmt = cam_pack_frame(cam, slot, yData, yRow, uData, uRow, uPix,
                                 vData, vRow, vPix, w, h);
        TracyCZoneEnd(zPack);

        int n = 0, sent = 0;
        struct camera_client **snap = snapshot_clients(&n);
        pthread_mutex_lock(&g_cam.slot_lock[cam]);
        TracyCZoneN(zFanout, "cam fan-out READY", 1);
        for (int i = 0; i < n; i++) {
            struct camera_client *c = snap[i];
            if (!c->recording[cam])
                continue;
            cam_frame_ready(c, cam, slot, w, h, fmt);
            sent++;
        }
        TracyCZoneEnd(zFanout);
        g_cam.slot_pending[cam][slot] = sent;
        g_cam.last_sent[cam] = sent;
        g_cam.cur_slot[cam] = slot ^ 1;
        pthread_mutex_unlock(&g_cam.slot_lock[cam]);
        release_snapshot(snap, n);
        TracyCPlot(cam_framemark[cam], (float)sent);
        TracyCFrameMarkNamed(cam_framemark[cam]);
    }

    AImage_delete(image);
}

/* ---------------------------------------------------------------
 * Camera2 NDK device/session callbacks (no-op / logging).
 * --------------------------------------------------------------- */

static void on_device_disconnected(void *context, ACameraDevice *dev)
{ (void)dev; LOGE("camera %d disconnected", (int)(intptr_t)context); }
static void on_device_error(void *context, ACameraDevice *dev, int error)
{ (void)dev; LOGE("camera %d device error=%d", (int)(intptr_t)context, error); }
static void on_session_closed(void *context, ACameraCaptureSession *s) { (void)context; (void)s; }
static void on_session_ready(void *context, ACameraCaptureSession *s) { (void)context; (void)s; }
static void on_session_active(void *context, ACameraCaptureSession *s) { (void)context; (void)s; }

/* ---------------------------------------------------------------
 * Physical capture pipeline (per camera, in g_cam). Opened on demand
 * when a camera's recording list becomes non-empty, closed when empty.
 * --------------------------------------------------------------- */

static void cam_stop_locked(int id);

static void cam_start_locked(int id, int width, int height)
{
    cam_stop_locked(id);   /* tear down any prior pipeline first */

    int w = (width  > 0) ? width  : g_cam.max_w[id];
    int h = (height > 0) ? height : g_cam.max_h[id];
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    media_status_t ms = AImageReader_newWithUsage(
            w, h, AIMAGE_FORMAT_YUV_420_888,
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 2, &g_cam.reader[id]);
    if (ms != AMEDIA_OK || !g_cam.reader[id]) {
        LOGE("start: AImageReader_new(%d) failed: %d", id, ms);
        cam_stop_locked(id);
        return;
    }
    g_cam.img_listener[id].context = (void *)(intptr_t)id;
    g_cam.img_listener[id].onImageAvailable = on_frame_available;
    AImageReader_setImageListener(g_cam.reader[id], &g_cam.img_listener[id]);
    if (AImageReader_getWindow(g_cam.reader[id], &g_cam.window[id]) != AMEDIA_OK
            || !g_cam.window[id]) {
        LOGE("start: AImageReader_getWindow(%d) failed", id);
        cam_stop_locked(id);
        return;
    }

    g_cam.dev_cb[id].context = (void *)(intptr_t)id;
    g_cam.dev_cb[id].onDisconnected = on_device_disconnected;
    g_cam.dev_cb[id].onError = on_device_error;
    camera_status_t cs = ACameraManager_openCamera(g_cam.cam_mgr, g_cam.cam_ids[id],
                                                   &g_cam.dev_cb[id], &g_cam.device[id]);
    if (cs != ACAMERA_OK || !g_cam.device[id]) {
        LOGE("start: openCamera(%d id=%s) failed: %d", id, g_cam.cam_ids[id], cs);
        cam_stop_locked(id);
        return;
    }

    if (ACaptureSessionOutputContainer_create(&g_cam.out_container[id]) != ACAMERA_OK ||
        ACaptureSessionOutput_create(g_cam.window[id], &g_cam.session_output[id]) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(g_cam.out_container[id],
                                           g_cam.session_output[id]) != ACAMERA_OK) {
        LOGE("start: session output setup(%d) failed", id);
        cam_stop_locked(id);
        return;
    }
    g_cam.ses_cb[id].context = (void *)(intptr_t)id;
    g_cam.ses_cb[id].onClosed = on_session_closed;
    g_cam.ses_cb[id].onReady = on_session_ready;
    g_cam.ses_cb[id].onActive = on_session_active;
    cs = ACameraDevice_createCaptureSession(g_cam.device[id], g_cam.out_container[id],
                                            &g_cam.ses_cb[id], &g_cam.session[id]);
    if (cs != ACAMERA_OK || !g_cam.session[id]) {
        LOGE("start: createCaptureSession(%d) failed: %d", id, cs);
        cam_stop_locked(id);
        return;
    }

    if (ACameraDevice_createCaptureRequest(g_cam.device[id], TEMPLATE_RECORD,
                                           &g_cam.request[id]) != ACAMERA_OK ||
        ACameraOutputTarget_create(g_cam.window[id], &g_cam.out_target[id]) != ACAMERA_OK ||
        ACaptureRequest_addTarget(g_cam.request[id], g_cam.out_target[id]) != ACAMERA_OK) {
        LOGE("start: capture request setup(%d) failed", id);
        cam_stop_locked(id);
        return;
    }
    uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
    uint8_t awbMode = ACAMERA_CONTROL_AWB_MODE_AUTO;
    uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
    ACaptureRequest_setEntry_u8(g_cam.request[id], ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
    ACaptureRequest_setEntry_u8(g_cam.request[id], ACAMERA_CONTROL_AWB_MODE, 1, &awbMode);
    ACaptureRequest_setEntry_u8(g_cam.request[id], ACAMERA_CONTROL_AF_MODE, 1, &afMode);

    cs = ACameraCaptureSession_setRepeatingRequest(g_cam.session[id], NULL, 1,
                                                   &g_cam.request[id], NULL);
    if (cs != ACAMERA_OK) {
        LOGE("start: setRepeatingRequest(%d) failed: %d", id, cs);
        cam_stop_locked(id);
        return;
    }
    LOGI("start: camera %d started %dx%d", id, w, h);
}

static void cam_stop_locked(int id)
{
    if (g_cam.session[id]) {
        ACameraCaptureSession_stopRepeating(g_cam.session[id]);
        ACameraCaptureSession_close(g_cam.session[id]);
        g_cam.session[id] = NULL;
    }
    if (g_cam.request[id]) { ACaptureRequest_free(g_cam.request[id]); g_cam.request[id] = NULL; }
    if (g_cam.out_target[id]) { ACameraOutputTarget_free(g_cam.out_target[id]); g_cam.out_target[id] = NULL; }
    if (g_cam.session_output[id]) { ACaptureSessionOutput_free(g_cam.session_output[id]); g_cam.session_output[id] = NULL; }
    if (g_cam.out_container[id]) { ACaptureSessionOutputContainer_free(g_cam.out_container[id]); g_cam.out_container[id] = NULL; }
    if (g_cam.reader[id]) {
        AImageReader_delete(g_cam.reader[id]);   /* invalidates its window */
        g_cam.reader[id] = NULL;
        g_cam.window[id] = NULL;
    }
    if (g_cam.device[id]) { ACameraDevice_close(g_cam.device[id]); g_cam.device[id] = NULL; }
}

static void cam_start_recording(int id, int w, int h)
{
    if (id < 0 || id >= g_cam.num_cameras) return;
    pthread_mutex_lock(&g_cam.cap_lock[id]);
    cam_start_locked(id, w, h);
    pthread_mutex_unlock(&g_cam.cap_lock[id]);
}

static void cam_stop_recording(int id)
{
    if (id < 0 || id >= g_cam.num_cameras) return;
    pthread_mutex_lock(&g_cam.cap_lock[id]);
    cam_stop_locked(id);
    pthread_mutex_unlock(&g_cam.cap_lock[id]);
}

/* ---------------------------------------------------------------
 * Recording-list membership. START_RECORD adds a client to camera cam's
 * list (opening the physical camera if it was the first); STOP / free /
 * release removes it (closing the camera when the list empties).
 * --------------------------------------------------------------- */

static void client_start_record(struct camera_client *c, int cam, int w, int h)
{
    if (cam < 0 || cam >= g_cam.num_cameras)
        return;
    bool first = false;
    pthread_mutex_lock(&g_cam.clients_lock);
    if (!c->recording[cam]) {
        c->recording[cam] = true;
        first = (++g_cam.rec_count[cam] == 1);
    }
    pthread_mutex_unlock(&g_cam.clients_lock);
    if (first) {
        /* First recorder: reset the shared slot state before the reader thread starts. */
        pthread_mutex_lock(&g_cam.slot_lock[cam]);
        g_cam.cur_slot[cam] = 0;
        g_cam.last_sent[cam] = 0;
        for (int s = 0; s < CAMERA_SLOTS; s++) g_cam.slot_pending[cam][s] = 0;
        pthread_mutex_unlock(&g_cam.slot_lock[cam]);
        cam_start_recording(cam, w, h);   /* on-demand: first recorder opens camera */
    }
}

static void client_stop_record(struct camera_client *c, int cam)
{
    if (cam < 0 || cam >= g_cam.num_cameras)
        return;
    bool last = false;
    pthread_mutex_lock(&g_cam.clients_lock);
    if (c->recording[cam]) {
        c->recording[cam] = false;
        last = (--g_cam.rec_count[cam] == 0);
    }
    pthread_mutex_unlock(&g_cam.clients_lock);
    if (last)
        cam_stop_recording(cam);   /* list empty: release the physical camera */
}

static void client_stop_all(struct camera_client *c)
{
    for (int cam = 0; cam < g_cam.num_cameras; cam++)
        client_stop_record(c, cam);
}

/* ---------------------------------------------------------------
 * Control + stream message handling (per client).
 * --------------------------------------------------------------- */

static void ctrl_handle_msg(struct camera_client *c,
                            const struct camera_ctrl_msg *hdr, const uint8_t *payload)
{
    switch (hdr->type) {
    case CAMERA_CTRL_GET_INFO: {
        _Alignas(uint16_t) uint8_t reply[sizeof(struct camera_ctrl_msg) + 1 + MAX_CAMERAS * 4];
        struct camera_ctrl_msg *rh = (struct camera_ctrl_msg *)reply;
        rh->type = CAMERA_CTRL_INFO_REPLY;
        rh->reserved = 0;
        uint8_t *pl = reply + sizeof(struct camera_ctrl_msg);
        int n = g_cam.num_cameras;
        pl[0] = (uint8_t)n;
        size_t off = 1;
        for (int i = 0; i < n; i++) {
            uint16_t w16 = (uint16_t)g_cam.max_w[i];
            uint16_t h16 = (uint16_t)g_cam.max_h[i];
            memcpy(pl + off, &w16, sizeof(w16)); off += sizeof(w16);
            memcpy(pl + off, &h16, sizeof(h16)); off += sizeof(h16);
        }
        rh->len = (uint16_t)off;
        if (send(c->ctrl_local, reply, sizeof(struct camera_ctrl_msg) + off, MSG_NOSIGNAL) < 0)
            LOGE("ctrl: INFO_REPLY send failed: %s", strerror(errno));
        break;
    }
    case CAMERA_CTRL_START_RECORD: {
        if (hdr->len < 5) { LOGE("ctrl: START_RECORD short payload (%u)", hdr->len); break; }
        uint8_t id = payload[0];
        uint16_t w, h;
        memcpy(&w, payload + 1, sizeof(w));
        memcpy(&h, payload + 3, sizeof(h));
        if (id >= g_cam.num_cameras) { LOGE("ctrl: START_RECORD bad id %u", id); break; }
        LOGI("ctrl: START_RECORD cam=%u %ux%u", id, w, h);
        client_start_record(c, id, w, h);
        break;
    }
    case CAMERA_CTRL_STOP_RECORD: {
        if (hdr->len < 1) { LOGE("ctrl: STOP_RECORD short payload"); break; }
        uint8_t id = payload[0];
        if (id >= g_cam.num_cameras) break;
        LOGI("ctrl: STOP_RECORD cam=%u", id);
        client_stop_record(c, id);
        break;
    }
    default:
        LOGE("ctrl: unknown msg type 0x%02x", hdr->type);
        break;
    }
}

static void stream_handle_msg(struct camera_client *c, int cam, const struct cam_stream_msg *m)
{
    switch (m->type) {
    case CAM_STREAM_GET_SHM:
        /* Every recorder of this camera is handed the SAME shared region. */
        send_stream(c->stream_local[cam], CAM_STREAM_SHM_OFFER, 0, 0,
                    (uint32_t)g_cam.slot_bytes[cam], 0, g_cam.shm_fd[cam]);
        LOGI("stream cam=%d: offered shared shm (%zu B/slot)", cam, g_cam.slot_bytes[cam]);
        break;
    case CAM_STREAM_DONE:
        /* One producer finished reading the slot; wake the reader once all have. */
        if (m->slot < CAMERA_SLOTS) {
            pthread_mutex_lock(&g_cam.slot_lock[cam]);
            if (g_cam.slot_pending[cam][m->slot] > 0 &&
                --g_cam.slot_pending[cam][m->slot] == 0)
                pthread_cond_signal(&g_cam.slot_cond[cam]);
            pthread_mutex_unlock(&g_cam.slot_lock[cam]);
        }
        break;
    default:
        LOGE("stream cam=%d: unknown msg type %u", cam, m->type);
        break;
    }
}

/*
 * I/O thread: polls the ctrl + per-camera stream sockets of ALL clients. Each loop
 * snapshots the client set (busy-refcounted so entries survive the poll and handling)
 * and dispatches. Reading DONE here -- a different thread from the reader -- decrements
 * the shared slot's pending count so the reader can overwrite it once all readers finish.
 */
static void *io_thread_func(void *arg)
{
    (void)arg;
    LOGI("io thread started");

    const int ncam = g_cam.num_cameras;
    while (g_cam.running) {
        int n = 0;
        struct camera_client **snap = snapshot_clients(&n);
        if (n == 0) { release_snapshot(snap, n); usleep(50000); continue; }

        int nfds = n * (1 + ncam);
        struct pollfd *pfds = malloc((size_t)nfds * sizeof(*pfds));
        if (!pfds) { release_snapshot(snap, n); usleep(50000); continue; }
        for (int i = 0; i < n; i++) {
            int base = i * (1 + ncam);
            pfds[base].fd = snap[i]->ctrl_local;
            pfds[base].events = POLLIN;
            for (int cam = 0; cam < ncam; cam++) {
                pfds[base + 1 + cam].fd = snap[i]->stream_local[cam];
                pfds[base + 1 + cam].events = POLLIN;
            }
        }

        int r = poll(pfds, nfds, 500);
        if (r > 0) {
            TracyCZoneN(zDispatch, "cam io dispatch", 1);
            for (int i = 0; i < n; i++) {
                struct camera_client *c = snap[i];
                int base = i * (1 + ncam);
                if (pfds[base].revents & POLLIN) {
                    struct camera_ctrl_msg hdr;
                    if (recv(c->ctrl_local, &hdr, sizeof(hdr), MSG_WAITALL) == (ssize_t)sizeof(hdr)) {
                        uint8_t pl[256];
                        uint16_t len = hdr.len;
                        if (len > sizeof(pl)) len = sizeof(pl);
                        if (len == 0 || recv(c->ctrl_local, pl, len, MSG_WAITALL) == (ssize_t)len)
                            ctrl_handle_msg(c, &hdr, pl);
                    }
                }
                for (int cam = 0; cam < ncam; cam++) {
                    if (!(pfds[base + 1 + cam].revents & POLLIN))
                        continue;
                    struct cam_stream_msg m;
                    if (recv(c->stream_local[cam], &m, sizeof(m), 0) == (ssize_t)sizeof(m))
                        stream_handle_msg(c, cam, &m);
                }
            }
            TracyCZoneEnd(zDispatch);
        }
        free(pfds);
        release_snapshot(snap, n);
    }

    LOGI("io thread stopped");
    return NULL;
}

/* ---------------------------------------------------------------
 * Shared per-camera frame buffer (created once at init, sized for max res).
 * --------------------------------------------------------------- */

static int cam_shm_create(int cam)
{
    int w = g_cam.max_w[cam];
    int h = g_cam.max_h[cam];
    size_t slot_bytes = (w > 0 && h > 0) ? (size_t)w * h * 3 / 2 : CAM_FALLBACK_BYTES;
    g_cam.slot_bytes[cam] = slot_bytes;

    int fd = ASharedMemory_create("anland-camera", CAMERA_SLOTS * slot_bytes);
    if (fd < 0) { LOGE("cam %d shm_create failed", cam); return -1; }
    g_cam.shm_fd[cam] = fd;

    void *p = mmap(NULL, CAMERA_SLOTS * slot_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { LOGE("cam %d mmap shm failed: %s", cam, strerror(errno)); return -1; }
    g_cam.shm_ptr[cam] = p;
    return 0;
}

static void cam_shm_destroy(int cam)
{
    if (g_cam.shm_ptr[cam]) {
        munmap(g_cam.shm_ptr[cam], CAMERA_SLOTS * g_cam.slot_bytes[cam]);
        g_cam.shm_ptr[cam] = NULL;
    }
    if (g_cam.shm_fd[cam] >= 0) { close(g_cam.shm_fd[cam]); g_cam.shm_fd[cam] = -1; }
}

/* ---------------------------------------------------------------
 * Client channel create / destroy (bound to window lifetime).
 * --------------------------------------------------------------- */

/* Free everything in a client. Caller guarantees it is unlinked and busy==0. */
static void client_destroy_now(struct camera_client *c)
{
    if (c->ctrl_local  >= 0) close(c->ctrl_local);
    if (c->ctrl_remote >= 0) close(c->ctrl_remote);
    for (int i = 0; i < g_cam.num_cameras; i++) {
        if (c->stream_local[i]  >= 0) close(c->stream_local[i]);
        if (c->stream_remote[i] >= 0) close(c->stream_remote[i]);
    }
    free(c);
}

static struct camera_client *camera_client_create(void *owner)
{
    struct camera_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->owner = owner;
    c->ctrl_local = c->ctrl_remote = -1;
    for (int i = 0; i < MAX_CAMERAS; i++)
        c->stream_local[i] = c->stream_remote[i] = -1;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        LOGE("client: ctrl socketpair failed: %s", strerror(errno));
        free(c);
        return NULL;
    }
    c->ctrl_local = sv[0];
    c->ctrl_remote = sv[1];
    c->alloc_fds[0] = c->ctrl_remote;

    for (int i = 0; i < g_cam.num_cameras; i++) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
            LOGE("client: stream socketpair[%d] failed: %s", i, strerror(errno));
            client_destroy_now(c);
            return NULL;
        }
        c->stream_local[i]  = sv[0];
        c->stream_remote[i] = sv[1];
        c->alloc_fds[1 + i] = c->stream_remote[i];
    }

    pthread_mutex_lock(&g_cam.clients_lock);
    registry_add_locked(c);
    pthread_mutex_unlock(&g_cam.clients_lock);
    LOGI("camera client %p created (%d cameras)", owner, g_cam.num_cameras);
    return c;
}

/* --- service_info callbacks (per instance via userdata = owner token) --- */

struct resources camera_allocate_resource(uint32_t *args, void *userdata)
{
    (void)args;
    struct resources res = { .service_type = SERVICE_TYPE_CAMERA, .type = -1, .num = 0, .fds = NULL };
    if (!g_cam.ready)
        return res;

    pthread_mutex_lock(&g_cam.clients_lock);
    struct camera_client *c = registry_find_locked(userdata);
    pthread_mutex_unlock(&g_cam.clients_lock);
    if (!c)
        c = camera_client_create(userdata);   /* first allocate: create the channels */
    if (!c)
        return res;

    res.type = 0;
    res.num = (uint32_t)(1 + g_cam.num_cameras);
    res.fds = c->alloc_fds;
    LOGI("allocate_resource(owner=%p): %u fds", userdata, res.num);
    return res;
}

void camera_free_resource(struct resources res, void *userdata)
{
    (void)res;
    if (!g_cam.ready)
        return;
    /* Producer gone: only drop this client from the recording lists -- its channels
     * (fds) stay alive for the window's lifetime. */
    pthread_mutex_lock(&g_cam.clients_lock);
    struct camera_client *c = registry_find_locked(userdata);
    if (c) c->busy++;
    pthread_mutex_unlock(&g_cam.clients_lock);
    if (!c)
        return;
    LOGI("free_resource(owner=%p): stopping recording", userdata);
    client_stop_all(c);
    pthread_mutex_lock(&g_cam.clients_lock);
    c->busy--;
    pthread_cond_broadcast(&g_cam.clients_cond);
    pthread_mutex_unlock(&g_cam.clients_lock);
}

/* Focus no longer affects frame delivery -- every recorder gets the real frame from
 * the shared buffer -- but the JNI/Java path (nativeSetFocused) still calls this, so
 * keep it as a harmless no-op. */
void camera_set_focus(void *owner)
{
    (void)owner;
}

void camera_release_client(void *owner)
{
    pthread_mutex_lock(&g_cam.clients_lock);
    struct camera_client *c = registry_find_locked(owner);
    if (c) registry_remove_locked(c);   /* no new snapshot will include it */
    pthread_mutex_unlock(&g_cam.clients_lock);
    if (!c)
        return;
    client_stop_all(c);   /* leave every recording list (may stop physical capture) */

    pthread_mutex_lock(&g_cam.clients_lock);
    while (c->busy > 0)
        pthread_cond_wait(&g_cam.clients_cond, &g_cam.clients_lock);
    pthread_mutex_unlock(&g_cam.clients_lock);

    client_destroy_now(c);
    LOGI("camera client %p released", owner);
}

/* --- JNI: camera service lifecycle (called from CameraServices, main thread) --- */

JNIEXPORT void JNICALL
Java_com_anland_consumer_CameraServices_nativeInitCameraService(
    JNIEnv *env, jclass clazz, jobject activity)
{ (void)env; (void)clazz; (void)activity; camera_service_init(); }

JNIEXPORT void JNICALL
Java_com_anland_consumer_CameraServices_nativeDestroyCameraService(
    JNIEnv *env, jclass clazz)
{ (void)env; (void)clazz; camera_service_destroy(); }

/* --- lifecycle --- */

bool camera_service_is_ready(void) { return g_cam.ready; }

/* Discover cameras via the NDK manager and cache each one's max YUV_420_888 output
 * size. Returns the camera count (capped at MAX_CAMERAS). */
static int discover_cameras(void)
{
    g_cam.cam_mgr = ACameraManager_create();
    if (!g_cam.cam_mgr) { LOGE("init: ACameraManager_create failed"); return 0; }

    ACameraIdList *idList = NULL;
    if (ACameraManager_getCameraIdList(g_cam.cam_mgr, &idList) != ACAMERA_OK || !idList) {
        LOGE("init: getCameraIdList failed");
        return 0;
    }

    int n = idList->numCameras;
    if (n < 0) n = 0;
    if (n > MAX_CAMERAS) n = MAX_CAMERAS;

    for (int i = 0; i < n; i++) {
        g_cam.cam_ids[i] = strdup(idList->cameraIds[i]);

        ACameraMetadata *meta = NULL;
        if (ACameraManager_getCameraCharacteristics(g_cam.cam_mgr,
                idList->cameraIds[i], &meta) != ACAMERA_OK || !meta)
            continue;

        ACameraMetadata_const_entry entry;
        if (ACameraMetadata_getConstEntry(meta,
                ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK) {
            long bestArea = 0;
            for (uint32_t k = 0; k + 3 < entry.count; k += 4) {
                int32_t fmt = entry.data.i32[k];
                int32_t w   = entry.data.i32[k + 1];
                int32_t h   = entry.data.i32[k + 2];
                int32_t io  = entry.data.i32[k + 3];
                if (fmt != AIMAGE_FORMAT_YUV_420_888 ||
                    io  != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
                    continue;
                long area = (long)w * h;
                if (area > bestArea) { bestArea = area; g_cam.max_w[i] = w; g_cam.max_h[i] = h; }
            }
        }
        ACameraMetadata_free(meta);
    }

    {
        char sb[256];
        int off = snprintf(sb, sizeof(sb), "init: cameras=%d", n);
        for (int i = 0; i < n && off < (int)sizeof(sb); i++)
            off += snprintf(sb + off, sizeof(sb) - off, " [%d id=%s max=%dx%d]",
                            i, g_cam.cam_ids[i] ? g_cam.cam_ids[i] : "?",
                            g_cam.max_w[i], g_cam.max_h[i]);
        LOGI("%s", sb);
    }

    ACameraManager_deleteCameraIdList(idList);
    return n;
}

int camera_service_init(void)
{
    if (g_cam.ready)
        return 0;                    /* idempotent */

    memset(&g_cam, 0, sizeof(g_cam));
    pthread_mutex_init(&g_cam.clients_lock, NULL);
    pthread_cond_init(&g_cam.clients_cond, NULL);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        pthread_mutex_init(&g_cam.cap_lock[i], NULL);
        pthread_mutex_init(&g_cam.slot_lock[i], NULL);
        pthread_cond_init(&g_cam.slot_cond[i], NULL);
        g_cam.shm_fd[i] = -1;
    }

    g_cam.num_cameras = discover_cameras();
    LOGI("init: %d camera(s)", g_cam.num_cameras);

    /* One shared frame buffer per camera, sized for its max resolution. */
    for (int i = 0; i < g_cam.num_cameras; i++)
        if (cam_shm_create(i) < 0)
            LOGE("init: shared shm for cam %d failed", i);

    g_cam.running = true;
    if (pthread_create(&g_cam.io_thread, NULL, io_thread_func, NULL) != 0) {
        LOGE("init: io thread create failed");
        g_cam.running = false;
        if (g_cam.cam_mgr) { ACameraManager_delete(g_cam.cam_mgr); g_cam.cam_mgr = NULL; }
        return -1;
    }

    g_cam.ready = true;
    LOGI("camera service initialised");
    return 0;
}

void camera_service_destroy(void)
{
    if (!g_cam.ready)
        return;
    LOGI("camera service destroying");

    g_cam.ready = false;
    g_cam.running = false;
    pthread_join(g_cam.io_thread, NULL);

    for (int i = 0; i < g_cam.num_cameras; i++)
        cam_stop_recording(i);

    pthread_mutex_lock(&g_cam.clients_lock);
    struct camera_client **all = g_cam.clients;
    size_t cap = g_cam.clients_cap;
    g_cam.clients = NULL;
    g_cam.clients_cap = 0;
    pthread_mutex_unlock(&g_cam.clients_lock);
    for (size_t i = 0; i < cap; i++)
        if (all[i]) client_destroy_now(all[i]);   /* io/reader threads are stopped */
    free(all);

    for (int i = 0; i < g_cam.num_cameras; i++)
        cam_shm_destroy(i);   /* reader/io threads are stopped -> safe to unmap */
    for (int i = 0; i < MAX_CAMERAS; i++) {
        pthread_mutex_destroy(&g_cam.cap_lock[i]);
        pthread_mutex_destroy(&g_cam.slot_lock[i]);
        pthread_cond_destroy(&g_cam.slot_cond[i]);
    }
    for (int i = 0; i < g_cam.num_cameras; i++)
        free(g_cam.cam_ids[i]);
    if (g_cam.cam_mgr) { ACameraManager_delete(g_cam.cam_mgr); g_cam.cam_mgr = NULL; }

    pthread_cond_destroy(&g_cam.clients_cond);
    pthread_mutex_destroy(&g_cam.clients_lock);

    memset(&g_cam, 0, sizeof(g_cam));
}
