#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include <jni.h>
#include <stdbool.h>
#include <stdint.h>

#include "display_consumer.h"

#define MAX_CAMERAS 8

/*
 * Camera service (SERVICE_TYPE_CAMERA).
 *
 * Lifecycle (see also the design doc): the resources a camera needs -- one shared
 * control socketpair plus one stream socketpair per physical camera -- are created
 * once, at app start, by camera_service_init() and live independently of the
 * transport layer's connect/fallback churn. Allocating the service to a producer
 * therefore just hands the pre-created remote fd ends back over SCM_RIGHTS; no
 * camera is opened until the producer asks to record over the control channel.
 *
 *   ctrl_fd   : 1, shared by all cameras, bidirectional. Producer sends
 *               GET_INFO / START_RECORD / STOP_RECORD; consumer replies INFO.
 *   stream_fd : 1 per camera, SEQPACKET, bidirectional control channel. Frame PIXELS
 *               are NOT sent here -- they go through a per-camera shared-memory double
 *               buffer (ashmem). stream_fd only carries small pacing messages and the
 *               shm fd hand-off (see struct cam_stream_msg).
 *
 * The fd order handed to the producer is: { ctrl, stream_0, .., stream_{N-1} }.
 *
 * The camera is driven entirely in C via the Camera2 NDK (libcamera2ndk /
 * libmediandk): discovery, device open, capture session, and per-frame YUV delivery
 * all happen natively. The Java side only triggers init/destroy.
 *
 * Frame transport (shared memory, zero socket copy):
 *   Each camera owns one ashmem region of CAMERA_SLOTS * slot_bytes, created once at
 *   init and sized for the camera's max resolution (slot_bytes = maxW*maxH*3/2) and
 *   SHARED by all recorders. The AImageReader callback packs each frame as NV21 straight
 *   into a slot, then notifies every recorder over its stream_fd. The two ends ping-pong
 *   the two slots:
 *     0. consumer writes slot s, sends READY(s, w, h) to every recorder
 *     1. consumer advances to the other slot for the next frame
 *     2. each producer copies slot s out, sends DONE(s)
 *     3. consumer waits for DONE from ALL recorders before reusing slot s (1s timeout)
 */

/* ---- control-channel protocol (over ctrl_fd) ---- */

struct camera_ctrl_msg {
    uint8_t  type;       /* CAMERA_CTRL_* */
    uint8_t  reserved;
    uint16_t len;        /* payload length in bytes */
    uint8_t  payload[];
} __attribute__((packed));

#define CAMERA_CTRL_GET_INFO     0x01  /* no payload */
#define CAMERA_CTRL_START_RECORD 0x02  /* payload: id(1B) w(2B,LE) h(2B,LE) */
#define CAMERA_CTRL_STOP_RECORD  0x03  /* payload: id(1B) */
#define CAMERA_CTRL_INFO_REPLY   0x81  /* payload: num_cameras(1B) + per-cam w,h */

/* ---- stream-channel control protocol (over stream_fd[i], SEQPACKET) ---- */

#define CAMERA_SLOTS 2

struct cam_stream_msg {
    uint8_t  type;       /* CAM_STREAM_* */
    uint8_t  slot;       /* 0..CAMERA_SLOTS-1 */
    uint16_t fmt;        /* READY: CAM_FMT_* pixel format; else 0 */
    uint32_t a;          /* SHM_OFFER: slot_bytes; READY: width  */
    uint32_t b;          /* READY: height                       */
} __attribute__((packed));   /* 12 bytes */

#define CAM_STREAM_GET_SHM   1  /* producer->consumer: please offer the shm fd          */
#define CAM_STREAM_SHM_OFFER 2  /* consumer->producer: + shm fd (SCM_RIGHTS), a=slot_bytes */
#define CAM_STREAM_READY     3  /* consumer->producer: slot ready; fmt, a=w, b=h        */
#define CAM_STREAM_DONE      4  /* producer->consumer: slot copied out, free to reuse   */

/* Pixel format of the data in a slot (carried in READY.fmt). The consumer ships the
 * camera's native layout and the producer announces the matching PipeWire format, so
 * neither side has to de-interleave chroma. All three are w*h*3/2 bytes, Y-stride = w. */
#define CAM_FMT_I420 0  /* planar:       Y plane, then U plane, then V plane      */
#define CAM_FMT_NV12 1  /* semi-planar:  Y plane, then interleaved U,V            */
#define CAM_FMT_NV21 2  /* semi-planar:  Y plane, then interleaved V,U            */

/* ---- service_info callbacks (wired into do_connect via allocate_services) ----
 *
 * Multi-instance: one physical camera is captured once and its frames are fanned
 * out to every connected window (a "camera client"). Each window is one-to-one with
 * its producer and owns its own ctrl/stream channels; `userdata` is the instance token
 * (consumer_state*) that tells clients apart. The FRAME BUFFER, however, is shared: all
 * recorders of a camera map the one per-camera ashmem region and every recorder gets
 * the real frame (there is no blank/focus fast-path).
 *
 * Because the buffer is shared, the reader packs each frame once and then blocks before
 * overwriting a slot until every producer that received the previous frame in it has
 * DONE'd (concurrent reads are safe, overwrites are not). The wait is bounded (1s) so a
 * stuck producer can't freeze capture. */

struct resources camera_allocate_resource(uint32_t *args, void *userdata);
void             camera_free_resource(struct resources res, void *userdata);

/* Retained no-op: focus no longer affects frame delivery (all recorders get the real
 * frame from the shared buffer). Still called from JNI on window focus change. */
void camera_set_focus(void *owner);

/*
 * Destroy the client owned by `owner` and close its fds. The client (and its fds) is
 * bound to the WINDOW lifetime, not the connection: allocate/free only add or remove
 * it from the cameras' recording lists. This is the only call that frees the
 * channels -- invoke it when the instance is torn down (nativeDestroy). Clears focus
 * if `owner` held it. No-op if it has no client. */
void camera_release_client(void *owner);

/* ---- lifecycle, driven from JNI (MainActivity) ---- */

/*
 * Discover cameras via the Camera2 NDK, create the control + per-camera stream
 * socketpairs and ashmem buffers, and start the control thread. Idempotent: a second
 * call while already initialised is a no-op. Returns 0 on success, -1 on failure.
 * Needs no Context -- the NDK camera manager is process-global.
 */
int  camera_service_init(void);

/* Stop the control thread, stop all recording, and close every fd. */
void camera_service_destroy(void);

/*
 * True once init() succeeded and the fds are live. do_connect() registers the
 * camera service only when this is true, so the settings toggle (which gates the
 * init call) is the single source of truth for whether the producer ever sees it.
 */
bool camera_service_is_ready(void);

#endif /* CAMERA_SERVICE_H */
