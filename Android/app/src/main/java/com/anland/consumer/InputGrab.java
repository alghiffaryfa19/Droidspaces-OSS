package com.anland.consumer;

import android.content.Context;
import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Transport for immersive mode: runs the bundled {@code libinputgrab.so} helper as
 * root and turns its record stream into callbacks on the main thread.
 *
 * The app cannot open {@code /dev/input} itself (untrusted_app is denied
 * {@code input_device:chr_file}), so the helper does it in the root context and
 * streams the raw evdev events back. This is the same bridge shape
 * {@code fd_helper.c} uses for the daemon connection, except the app is the one
 * listening and the payload is events rather than a file descriptor.
 *
 * Because the helper holds an exclusive grab on the touchscreen while it runs,
 * every failure mode here has to end with that grab released:
 * <ul>
 *   <li>closing the socket makes the helper see EOF and ungrab — and the kernel
 *       closes it for us even if this process is killed outright;</li>
 *   <li>the heartbeat below is written only while the <em>main thread</em> is
 *       still running, so an ANR — not just a crash — also frees the input;</li>
 *   <li>the helper watches the toggle key itself, which is what rescues a
 *       session whose app went away without either of the above working;</li>
 *   <li>and if the helper is somehow still alive shortly after {@link #stop}, it
 *       is killed by the token this session put in its argv.</li>
 * </ul>
 */
final class InputGrab {
    private static final String TAG = "AnlandGrab";

    /** Wire record size; see jni/input_grab.h. */
    private static final int REC_SIZE = 32;
    /** Byte offset of aux[0] inside a record. */
    private static final int AUX0 = 12;

    // rtype
    private static final int REC_HELLO  = 1;
    private static final int REC_DEVICE = 2;
    private static final int REC_READY  = 3;
    private static final int REC_EVENT  = 4;
    private static final int REC_BYE    = 5;

    // DEVICE.etype (device class)
    static final int CLASS_TOUCHSCREEN = 1;
    static final int CLASS_TOUCHPAD    = 2;
    static final int CLASS_MOUSE       = 3;
    static final int CLASS_KEYBOARD    = 4;

    // DEVICE.aux[4] flags
    static final int DEV_GRABBED    = 1;
    static final int DEV_MULTITOUCH = 2;
    static final int DEV_CLICKPAD   = 4;

    /** Broadcast device id used by the helper's "records were dropped" marker. */
    static final int DEV_ALL = 0xFFFF;

    // Session end reasons. 1..4 mirror the helper's IGRAB_BYE_*; the rest are local.
    static final int REASON_TOGGLE     = 1;  // toggle key pressed on a grabbed device
    static final int REASON_PEER_GONE  = 2;
    static final int REASON_STALLED    = 3;
    static final int REASON_HELPER_ERR = 4;  // helper found nothing it could grab
    static final int REASON_NO_ROOT    = 5;  // helper never connected (su denied/missing)
    static final int REASON_IO         = 6;  // stream broke
    static final int REASON_STOPPED    = 7;  // stop() was called

    /** How long to wait for the helper to connect; a first-time su prompt is slow. */
    private static final int CONNECT_TIMEOUT_MS = 20000;
    private static final int HEARTBEAT_INTERVAL_MS = 1000;
    /** The main thread must have run this recently for a heartbeat to be sent. */
    private static final int MAIN_ALIVE_WINDOW_MS = 2500;
    private static final int MAIN_BEACON_INTERVAL_MS = 500;
    /** Grace period for the helper to notice a closed socket before we kill it. */
    private static final int KILL_FALLBACK_MS = 1500;

    interface Listener {
        /** One opened device, before {@link #onReady}. Ranges are raw ABS units. */
        void onDevice(int dev, int cls, int minX, int maxX, int minY, int maxY, int flags);
        /** Device list complete; from here on only events arrive. */
        void onReady(int grabbedCount);
        /**
         * A batch of evdev events as flat {dev, type, code, value} quadruples.
         * Batched per SYN_REPORT so a multi-touch frame is applied in one piece.
         */
        void onEvents(int[] batch, int count);
        /** The session ended, for any reason, exactly once per {@link #start}. */
        void onEnded(int reason);
    }

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private LocalSocket binder;          // owns the listening fd; closed via `server`
    private volatile LocalServerSocket server;
    private volatile LocalSocket client;
    private Thread heartbeat;

    private volatile boolean running = false;
    private volatile boolean stopping = false;
    /**
     * Token of a session whose helper is known to be gone, so the last-resort
     * kill is skipped. Per-token rather than a flag: a delayed kill from the
     * previous session must never target the one that replaced it.
     */
    private volatile String finishedToken = null;
    private volatile long lastMainAlive = 0L;
    private boolean endReported = false;
    private volatile String killToken;

    InputGrab(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    boolean isRunning() {
        return running;
    }

    /**
     * Launch a session. Returns false only when the listening socket could not be
     * created; every later failure arrives through {@link Listener#onEnded}.
     *
     * @param toggleScanCode evdev code of the key that ends the session. The helper
     *                       refuses to grab anything without it.
     */
    boolean start(int toggleScanCode) {
        if (running)
            return true;
        if (toggleScanCode <= 0)
            return false;

        killToken = Long.toHexString(SystemClock.elapsedRealtimeNanos())
                + Integer.toHexString(System.identityHashCode(this));
        final String token = killToken;
        final String socketName = "anland.igrab." + token;

        try {
            binder = new LocalSocket(LocalSocket.SOCKET_STREAM);
            // Abstract namespace (LocalServerSocket's own default): no file to
            // create, chmod or leave behind, and root needs one SELinux
            // permission fewer than for a socket in the app's cache dir. The name
            // carries a random token and the peer's uid is checked on accept, so
            // another app cannot slip into the event stream.
            binder.bind(new LocalSocketAddress(socketName,
                    LocalSocketAddress.Namespace.ABSTRACT));
            server = new LocalServerSocket(binder.getFileDescriptor());
        } catch (IOException e) {
            Log.e(TAG, "cannot listen on " + socketName, e);
            closeAll();
            return false;
        }

        running = true;
        stopping = false;
        finishedToken = null;
        endReported = false;
        lastMainAlive = SystemClock.uptimeMillis();
        main.postDelayed(mainBeacon, MAIN_BEACON_INTERVAL_MS);

        final String helper = context.getApplicationInfo().nativeLibraryDir
                + "/libinputgrab.so";
        Thread worker = new Thread(() -> run(helper, socketName, toggleScanCode, token),
                "anland-inputgrab");
        worker.setDaemon(true);
        worker.start();
        return true;
    }

    /** End the session. Idempotent; {@link Listener#onEnded} still fires once. */
    void stop() {
        if (!running)
            return;
        stopping = true;
        final String token = killToken;
        // Closing the socket is the release that matters: the helper's poll()
        // sees EOF and ungrabs. Everything else here is a backstop.
        closeAll();
        main.postDelayed(() -> killHelper(token), KILL_FALLBACK_MS);
    }

    // ---- main-thread liveness beacon -------------------------------------

    // Re-posts itself while a session is live. The heartbeat writer only sends
    // when this ran recently, so a wedged main thread stops the heartbeat and the
    // helper hands the input back without needing anything from us.
    private final Runnable mainBeacon = new Runnable() {
        @Override
        public void run() {
            if (!running)
                return;
            lastMainAlive = SystemClock.uptimeMillis();
            main.postDelayed(this, MAIN_BEACON_INTERVAL_MS);
        }
    };

    // ---- worker ----------------------------------------------------------

    private void run(String helperPath, String socketName, int toggleScanCode,
                     String token) {
        int reason = REASON_NO_ROOT;
        Process p = null;
        try {
            // The token is inert to the helper (it stops parsing after argv[2]);
            // it exists so this session can find its own process again later.
            String cmd = helperPath + " " + socketName + " " + toggleScanCode
                    + " igrabtoken=" + token + " >/dev/null 2>&1";
            try {
                p = new ProcessBuilder("su", "-c", cmd)
                        .redirectErrorStream(true).start();
            } catch (IOException e) {
                // `su` is missing or refused outright: nothing was launched, so
                // nothing can be holding a grab and no kill is needed.
                Log.w(TAG, "su failed: " + e);
                finishedToken = token;
                return;
            }
            drain(p);

            LocalSocket sock = acceptHelper();
            if (sock == null) {
                // Nothing was ever grabbed: a helper that starts late finds the
                // abstract name gone, fails to connect and exits before it
                // touches any device.
                finishedToken = token;
                return;
            }
            client = sock;
            if (stopping)
                return;
            startHeartbeat(sock);
            reason = readLoop(sock, token);
        } catch (IOException e) {
            // The stream broke with the helper possibly still grabbing; the kill
            // in the finally block is what covers that.
            Log.w(TAG, "immersive stream ended: " + e);
            reason = REASON_IO;
        } catch (Throwable t) {
            Log.e(TAG, "immersive session crashed", t);
            reason = REASON_IO;
        } finally {
            final int r = stopping ? REASON_STOPPED : reason;
            closeAll();
            killHelper(token);
            if (p != null)
                p.destroy();
            main.post(() -> finish(r));
        }
    }

    /** Discard the su pipe so a chatty `su` can never block on a full one. */
    private void drain(final Process p) {
        Thread t = new Thread(() -> {
            byte[] scratch = new byte[256];
            try (InputStream in = p.getInputStream()) {
                while (in.read(scratch) >= 0) {
                    // discarded on purpose: the helper logs through liblog
                }
            } catch (IOException ignored) {
            }
        }, "anland-inputgrab-drain");
        t.setDaemon(true);
        t.start();
    }

    /**
     * Wait for the helper's connection, verifying it really is root: the abstract
     * name is reachable process-wide, so an unprivileged peer is refused rather
     * than handed the device's entire input stream.
     */
    private LocalSocket acceptHelper() {
        final LocalServerSocket srv = server;
        if (srv == null)
            return null;
        // LocalServerSocket has no accept timeout; close it from a timer instead.
        // Closing an fd on Android signals threads blocked on it, so the pending
        // accept() returns. Guarded on identity so a timer left over from an
        // earlier session cannot close this one's listener.
        main.postDelayed(() -> {
            if (server == srv && client == null)
                closeServer();
        }, CONNECT_TIMEOUT_MS);
        try {
            LocalSocket sock = srv.accept();
            Credentials cred = sock.getPeerCredentials();
            if (cred == null || cred.getUid() != 0) {
                Log.e(TAG, "rejecting non-root peer uid="
                        + (cred == null ? -1 : cred.getUid()));
                sock.close();
                return null;
            }
            // One peer only. Dropping the listener also releases the abstract
            // name, so nothing else can connect for the rest of the session.
            closeServer();
            return sock;
        } catch (IOException e) {
            if (!stopping)
                Log.w(TAG, "helper did not connect: " + e);
            return null;
        }
    }

    private void startHeartbeat(final LocalSocket sock) {
        Thread hb = new Thread(() -> {
            try {
                OutputStream out = sock.getOutputStream();
                byte[] beat = {(byte) 0xA5};
                while (running && !stopping) {
                    Thread.sleep(HEARTBEAT_INTERVAL_MS);
                    if (SystemClock.uptimeMillis() - lastMainAlive
                            > MAIN_ALIVE_WINDOW_MS) {
                        // Withholding the beat is the point: a main thread that
                        // stopped running cannot end the session, so let the
                        // helper time out and release the devices itself.
                        Log.w(TAG, "main thread stalled; withholding heartbeat");
                        continue;
                    }
                    out.write(beat);
                    out.flush();
                }
            } catch (InterruptedException ignored) {
            } catch (IOException e) {
                // Socket already gone; the read loop reports the end.
            }
        }, "anland-inputgrab-hb");
        heartbeat = hb;
        hb.setDaemon(true);
        hb.start();
    }

    /**
     * Parse the fixed-size record stream. Records carry no length field precisely
     * so that the helper can drop them under back-pressure without ever
     * desyncing this parser.
     */
    private int readLoop(LocalSocket sock, String token) throws IOException {
        InputStream in = sock.getInputStream();
        byte[] buf = new byte[REC_SIZE * 64];
        ByteBuffer bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        int have = 0;
        int[] batch = new int[512];
        int batchLen = 0;

        while (!stopping) {
            int n = in.read(buf, have, buf.length - have);
            if (n < 0)
                return REASON_PEER_GONE;
            have += n;

            int off = 0;
            while (have - off >= REC_SIZE) {
                final int rec = off;
                off += REC_SIZE;

                int rtype = bb.getShort(rec) & 0xFFFF;
                int dev   = bb.getShort(rec + 2) & 0xFFFF;
                int etype = bb.getShort(rec + 4) & 0xFFFF;
                int code  = bb.getShort(rec + 6) & 0xFFFF;
                int value = bb.getInt(rec + 8);

                switch (rtype) {
                    case REC_HELLO:
                        Log.i(TAG, "helper v" + value + " pid=" + bb.getInt(rec + AUX0 + 4)
                                + " grabbed=" + bb.getInt(rec + AUX0));
                        break;
                    case REC_DEVICE: {
                        // A hotplug replacement is preceded by a global
                        // SYN_DROPPED. Post that batch before replacing devs[d],
                        // otherwise the main thread can cancel old state through
                        // the new device object when both records share a read.
                        batchLen = flush(batch, batchLen);
                        final int d = dev, cls = etype;
                        final int minX = bb.getInt(rec + AUX0);
                        final int maxX = bb.getInt(rec + AUX0 + 4);
                        final int minY = bb.getInt(rec + AUX0 + 8);
                        final int maxY = bb.getInt(rec + AUX0 + 12);
                        final int flags = bb.getInt(rec + AUX0 + 16);
                        main.post(() -> {
                            if (running)
                                listener.onDevice(d, cls, minX, maxX, minY, maxY, flags);
                        });
                        break;
                    }
                    case REC_READY: {
                        batchLen = flush(batch, batchLen);
                        final int grabbed = bb.getInt(rec + AUX0);
                        main.post(() -> {
                            if (running)
                                listener.onReady(grabbed);
                        });
                        break;
                    }
                    case REC_EVENT:
                        if (batchLen + 4 > batch.length)
                            batchLen = flush(batch, batchLen);
                        batch[batchLen++] = dev;
                        batch[batchLen++] = etype;
                        batch[batchLen++] = code;
                        batch[batchLen++] = value;
                        if (etype == 0 && code == 0)   // EV_SYN / SYN_REPORT
                            batchLen = flush(batch, batchLen);
                        break;
                    case REC_BYE:
                        batchLen = flush(batch, batchLen);
                        // The helper only sends this on its way out, so no kill
                        // is needed for this session.
                        finishedToken = token;
                        return (value >= REASON_TOGGLE && value <= REASON_HELPER_ERR)
                                ? value : REASON_PEER_GONE;
                    default:
                        Log.e(TAG, "bad record type " + rtype + "; ending session");
                        flush(batch, batchLen);
                        return REASON_IO;
                }
            }

            if (off > 0 && off < have)
                System.arraycopy(buf, off, buf, 0, have - off);
            have -= off;
            // Nothing left to parse: push what is pending rather than sit on it
            // waiting for a SYN that a key-only device never sends.
            batchLen = flush(batch, batchLen);
        }
        return REASON_STOPPED;
    }

    private int flush(int[] batch, int len) {
        if (len == 0)
            return 0;
        final int[] copy = new int[len];
        System.arraycopy(batch, 0, copy, 0, len);
        main.post(() -> {
            if (running)
                listener.onEvents(copy, copy.length);
        });
        return 0;
    }

    // ---- teardown --------------------------------------------------------

    private void finish(int reason) {
        if (endReported)
            return;
        endReported = true;
        running = false;
        main.removeCallbacks(mainBeacon);
        listener.onEnded(reason);
    }

    private void closeServer() {
        LocalServerSocket srv = server;
        server = null;
        if (srv != null) {
            try {
                srv.close();
            } catch (IOException ignored) {
            }
        }
        // `binder` shares the listening fd with `server`; closing it a second time
        // would close whatever fd number has been reused since. Drop the
        // reference only — it exists to keep the fd alive, not to own it.
        binder = null;
    }

    private void closeAll() {
        closeServer();
        LocalSocket c = client;
        client = null;
        if (c != null) {
            try {
                // Half-close first so the helper's read() sees EOF immediately
                // and starts ungrabbing, rather than waiting on the close.
                c.shutdownOutput();
            } catch (IOException ignored) {
            }
            try {
                c.close();
            } catch (IOException ignored) {
            }
        }
        Thread hb = heartbeat;
        heartbeat = null;
        if (hb != null)
            hb.interrupt();
    }

    /**
     * Last resort when the helper did not announce its own exit. Matched on the
     * token this session put in its argv rather than on a pid, so a recycled pid
     * can never be the target; the "[x]" prefix keeps the pattern from matching
     * the shell that runs pkill.
     */
    private void killHelper(final String token) {
        if (token == null || token.isEmpty() || token.equals(finishedToken))
            return;
        finishedToken = token;
        Thread t = new Thread(() -> {
            String pattern = "igrabtoken=[" + token.charAt(0) + "]" + token.substring(1);
            Process p = null;
            try {
                p = new ProcessBuilder("su", "-c",
                        "pkill -9 -f '" + pattern + "' >/dev/null 2>&1").start();
                p.waitFor();
            } catch (Exception ignored) {
            } finally {
                if (p != null)
                    p.destroy();
            }
        }, "anland-inputgrab-kill");
        t.setDaemon(true);
        t.start();
    }
}
