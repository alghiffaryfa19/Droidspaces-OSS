package com.anland.consumer;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.widget.Toast;

import java.util.Arrays;

/**
 * Immersive mode: the whole device is handed over to the Linux desktop.
 *
 * A root helper takes an exclusive grab (EVIOCGRAB) on the touchscreen, the
 * keyboard and any pointer, so Android stops seeing them entirely — no gesture
 * navigation, no notification shade, no accidental Home. This class turns the
 * raw evdev stream that comes back into the input the app already knows how to
 * forward, so nothing about the desktop side changes:
 *
 * <ul>
 *   <li>a grabbed touchscreen is replayed as an ordinary {@link MotionEvent} and
 *       routed exactly like a real touch — through {@link Touchpad} when
 *       touchpad (relative) mode is on, straight to the remote when it is off,
 *       which is why the two features compose;</li>
 *   <li>a grabbed physical touchpad gets its own {@link Touchpad}, so the same
 *       gestures, thresholds and pointer sensitivity apply as when Android
 *       captures the pad itself;</li>
 *   <li>a grabbed mouse drives the shared cursor through the host's relative
 *       motion path, which is where the sensitivity setting is applied;</li>
 *   <li>keys are already evdev scan codes on the wire, so they pass straight
 *       through.</li>
 * </ul>
 *
 * The session is bound to one key, chosen in Settings. Pressing it enters and
 * pressing it again leaves — the helper watches for that key itself, so the way
 * out never depends on this process being healthy. The Settings switch is only a
 * safety gate: with it off the key does nothing at all, and a session never
 * survives a pause, a focus change, a lost surface or a locked screen — the
 * keyguard needs the touchscreen, so the grab ends the moment the screen turns
 * off (see {@link #registerScreenOff}).
 */
final class ImmersiveMode implements InputGrab.Listener {
    private static final String TAG = "Anland";
    private static final String PREFS_NAME = "anland_settings";

    static final String KEY_ENABLED  = "immersive_enabled";
    static final String KEY_KEYCODE  = "immersive_keycode";
    /** Raw evdev scan code of the bound key: what the helper actually compares. */
    static final String KEY_SCANCODE = "immersive_scancode";

    // evdev event types / codes (linux/input-event-codes.h).
    private static final int EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03;
    private static final int SYN_REPORT = 0, SYN_DROPPED = 3;
    private static final int ABS_X = 0x00, ABS_Y = 0x01;
    private static final int ABS_MT_SLOT = 0x2f;
    private static final int ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36;
    private static final int ABS_MT_TRACKING_ID = 0x39;
    private static final int REL_X = 0x00, REL_Y = 0x01, REL_HWHEEL = 0x06;
    private static final int REL_WHEEL = 0x08;
    private static final int REL_WHEEL_HI_RES = 0x0b, REL_HWHEEL_HI_RES = 0x0c;
    private static final int BTN_FIRST = 0x110, BTN_LAST = 0x117;   // BTN_LEFT..BTN_TASK
    private static final int BTN_LEFT = 0x110;
    private static final int BTN_TOOL_FIRST = 0x140, BTN_TOOL_LAST = 0x14f;
    private static final int BTN_TOUCH = 0x14a;
    private static final int KEY_CODE_LIMIT = 0x300;
    /** One wheel detent, matching the ±1 AXIS_VSCROLL the captured-mouse path sees. */
    private static final float WHEEL_STEP = 10f;
    private static final float HI_RES_PER_DETENT = 120f;

    /** MotionEvent tops out at 16 pointers, and so does every panel worth caring about. */
    private static final int MAX_SLOTS = 16;
    /** Matches IGRAB_MAX_DEVICES in jni/input_grab.h. */
    private static final int MAX_DEVICES = 32;

    /** Everything this class needs from the activity that owns the pipeline. */
    interface Host {
        Context context();
        /** Size of the area touches are mapped into, in view pixels. */
        int outputWidth();
        int outputHeight();
        /** Its top-left corner, which is not the origin when the display is letterboxed. */
        float outputOriginX();
        float outputOriginY();
        /** The panel's current refresh rate, for the session's telemetry line. */
        float displayRefreshHz();
        /** Cached {@link android.view.Display#getRotation()}. */
        int displayRotation();
        /**
         * A grabbed touchscreen frame, already in view pixels: routed exactly
         * like a real touch, so touchpad mode keeps working.
         */
        void onGrabbedTouch(MotionEvent ev);
        /** A {@link Touchpad} wired to the host's cursor, for a grabbed physical pad. */
        Touchpad newGrabbedPad();
        /** Relative cursor motion in view pixels, with the sensitivity setting applied. */
        void movePointerRelative(float dx, float dy);
        void sendKey(int action, int evdev);
        void sendMouseButton(int button, boolean pressed);
        void sendMouseScroll(int axis, float value);
        /** Session started or ended; the host re-syncs pointer capture. */
        void onImmersiveChanged(boolean active);
    }

    /** Per-device translation state. */
    private static final class Dev {
        int devIdx;
        int cls;
        boolean grabbed;
        boolean multitouch;
        /** One button under the whole pad, so BTN_LEFT alone just means "clicked". */
        boolean clickpad;
        int minX, maxX, minY, maxY;
        Touchpad pad;                       // CLASS_TOUCHPAD only, and never for twins

        /**
         * This pad is a twin: another CLASS_TOUCHPAD already covered these exact
         * ranges. Some docks expose one physical pad through two nodes (pogo +
         * HID), so this one may be the same pad a second time.
         */
        boolean twin;
        /**
         * A twin that provably delivers the same contacts as its primary: both
         * emitted real contact frames, so one of them is a duplicate path for
         * the same pad. Whichever keeps working is the same pad to the user.
         */
        boolean dup;
        /**
         * Frames with at least one real contact, for the twin decision. Only
         * frames that actually produced a contact count — a chip that reports
         * BTN_TOUCH without any MT data must never tip the decision.
         */
        int contactFrames;

        // Multi-touch protocol B: one contact per slot, alive while its tracking
        // id is >= 0. The slot index doubles as the MotionEvent pointer id, which
        // keeps ids stable for as long as the finger stays down.
        final int[] trackingId = new int[MAX_SLOTS];
        final float[] x = new float[MAX_SLOTS];
        final float[] y = new float[MAX_SLOTS];
        int curSlot = 0;
        boolean sawTrackingId = false;

        // Single-touch fallback for panels with no MT axes (BTN_TOUCH + ABS_X/Y).
        // stValid is set by actual ABS_X/Y data, never by the BTN_TOUCH hint
        // alone: a multitouch chip that only reports "someone is touching" must
        // not produce a phantom contact at stale coordinates.
        boolean stTouch = false;
        boolean stValid = false;
        float stX, stY;

        /** Contacts currently reported to the host, in pointer-index order. */
        final int[] active = new int[MAX_SLOTS];
        int activeCount = 0;
        long downTime = 0;
        boolean absDirty = false;

        // Relative axes, accumulated over a frame and emitted on SYN_REPORT.
        float relX, relY;
        float wheel, hwheel;
        boolean sawHiRes = false;

        /** Clickpad press latch: release exactly the button that was pressed. */
        int latchedButton = 0;

        Dev() {
            Arrays.fill(trackingId, -1);
        }
    }

    private final Host host;
    private final Context ctx;
    private final InputGrab grab;
    private final Dev[] devs = new Dev[MAX_DEVICES];

    /** Held keys and buttons, so a session can never leave one stuck on the desktop. */
    private final boolean[] keyDown = new boolean[KEY_CODE_LIMIT];

    // Scratch for MotionEvent synthesis; obtain() copies these out immediately,
    // so one set is reused for every event.
    private final MotionEvent.PointerProperties[] props =
            new MotionEvent.PointerProperties[MAX_SLOTS];
    private final MotionEvent.PointerCoords[] coords =
            new MotionEvent.PointerCoords[MAX_SLOTS];
    private final int[] desiredSlots = new int[MAX_SLOTS];
    private final int[] probeSlots = new int[MAX_SLOTS];
    private final float[] mapped = new float[2];

    private boolean active = false;
    private boolean starting = false;
    /** The user asked to leave, so the closing toast is theirs to see. */
    private boolean userExitPending = false;
    private boolean suppressToggleUntilUp = false;
    private long suppressToggleDeadlineMs = 0L;
    private static final long TOGGLE_SUPPRESS_MS = 2000L;

    // Session telemetry: events per second, per-device event counts and the
    // panel's current rate, logged once a second while a session runs. It is the
    // fastest way to tell a bursty device (a duplicated touchpad streaming the
    // same contacts twice) from a healthy stream on a slow panel.
    private final android.os.Handler stats = new android.os.Handler(Looper.getMainLooper());
    private long statsLastAt = 0L;
    private int statsEvents = 0;
    private final int[] statsPerDev = new int[MAX_DEVICES];
    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            if (!active && !starting)
                return;
            long now = SystemClock.elapsedRealtime();
            if (statsLastAt != 0L && now - statsLastAt >= 900) {
                long dt = now - statsLastAt;
                StringBuilder sb = new StringBuilder("immersive stats: ")
                        .append((int) (statsEvents * 1000f / dt)).append(" ev/s, display ")
                        .append(host.displayRefreshHz()).append(" Hz, devs:");
                for (int i = 0; i < devs.length; i++) {
                    Dev d = devs[i];
                    if (d == null)
                        continue;
                    sb.append(' ').append(i).append(':').append(d.cls);
                    if (d.clickpad)
                        sb.append("(cp)");
                    if (d.twin)
                        sb.append(d.dup ? "(dup)" : "(twin)");
                    sb.append('=').append(statsPerDev[i]);
                }
                Log.i(TAG, sb.toString());
                statsEvents = 0;
                java.util.Arrays.fill(statsPerDev, 0);
                statsLastAt = now;
            } else if (statsLastAt == 0L) {
                statsLastAt = now;
            }
            stats.postDelayed(this, 500);
        }
    };

    ImmersiveMode(Host host) {
        this.host = host;
        this.ctx = host.context();
        this.grab = new InputGrab(ctx, this);
        for (int i = 0; i < MAX_SLOTS; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
        }
    }

    boolean isActive() {
        return active || starting;
    }

    // ---- settings --------------------------------------------------------

    private SharedPreferences prefs() {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    private boolean isEnabled() {
        return prefs().getBoolean(KEY_ENABLED, false);
    }

    /**
     * Evdev code of the bound key. Recorded straight from the KeyEvent at bind
     * time because {@link KeyCodeMapper} has no entry for the volume keys — the
     * obvious thing to bind on a tablet — and the helper compares evdev codes,
     * not Android key codes.
     */
    private int boundScanCode() {
        SharedPreferences p = prefs();
        int scan = p.getInt(KEY_SCANCODE, -1);
        if (scan > 0)
            return scan;
        int keycode = p.getInt(KEY_KEYCODE, -1);
        return keycode == -1 ? -1 : KeyCodeMapper.getScanCode(keycode);
    }

    private void suppressToggleTail() {
        suppressToggleUntilUp = true;
        suppressToggleDeadlineMs = SystemClock.uptimeMillis() + TOGGLE_SUPPRESS_MS;
    }

    private boolean consumeSuppressedToggle(KeyEvent event) {
        if (!suppressToggleUntilUp)
            return false;
        if (SystemClock.uptimeMillis() > suppressToggleDeadlineMs) {
            suppressToggleUntilUp = false;
            suppressToggleDeadlineMs = 0L;
            return false;
        }
        if (event.getAction() == KeyEvent.ACTION_UP) {
            suppressToggleUntilUp = false;
            suppressToggleDeadlineMs = 0L;
        }
        return true;
    }

    /**
     * Consume the bound key. Called from both key paths — the accessibility
     * service eats keys before the window when interception is on, so neither
     * path alone sees every press.
     *
     * @return true when the event was the toggle key and must go no further.
     */
    boolean handleKey(KeyEvent event) {
        if (!isEnabled())
            return false;
        SharedPreferences p = prefs();
        int scan = p.getInt(KEY_SCANCODE, -1);
        int keycode = p.getInt(KEY_KEYCODE, -1);
        if (scan <= 0 && keycode == -1)
            return false;
        boolean match = (scan > 0 && event.getScanCode() == scan)
                || (keycode != -1 && event.getKeyCode() == keycode);
        if (!match)
            return false;
        // A key with no evdev code is one the root helper could never recognise,
        // and the helper is what ends a session no matter what happens to this
        // process. Rather than swallow the key to no purpose, leave it alone;
        // Settings flags such a binding where the user can see it.
        if (boundScanCode() <= 0)
            return false;
        if (consumeSuppressedToggle(event))
            return true;

        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0)
            toggle();
        // Swallow the release too, so the key never reaches the desktop or the
        // soft-keyboard toggle bound to the same code.
        return true;
    }

    private void toggle() {
        if (active || starting) {
            userExitPending = true;
            suppressToggleTail();
            stop();
        } else {
            start();
        }
    }

    private void start() {
        int scan = boundScanCode();
        if (scan <= 0) {
            // Without an evdev code the helper cannot recognise the key that ends
            // the session, and it refuses to grab anything blind.
            toast(ctx.getString(R.string.immersive_no_scancode));
            return;
        }
        starting = true;
        userExitPending = false;
        if (!grab.start(scan)) {
            starting = false;
            toast(ctx.getString(R.string.immersive_failed));
            return;
        }
        registerScreenOff();
        statsLastAt = 0L;
        statsEvents = 0;
        java.util.Arrays.fill(statsPerDev, 0);
        stats.postDelayed(statsTick, 500);
        // Announced up front, and with the way out, because once the grab lands
        // the on-screen UI is unreachable by design.
        toast(ctx.getString(R.string.immersive_entering,
                KeyCodeMapper.keyName(ctx, prefs().getInt(KEY_KEYCODE, -1), scan)));
        host.onImmersiveChanged(true);
    }

    /** End the session. Safe to call at any time, including when not running. */
    void stop() {
        // The receiver is registered against the activity. Remove it before the
        // helper's asynchronous teardown can outlive that activity.
        unregisterScreenOff();
        if (!active && !starting)
            return;
        grab.stop();
    }

    // ---- screen-off safety ------------------------------------------------

    /**
     * When the screen locks, Android's keyguard is about to take over the
     * display — and the keyguard cannot be swiped while the touchscreen is
     * grabbed. The session must hand the input back the moment the screen goes
     * dark, so Android's dedicated power path (kept ungrabbed by input_grab.c)
     * can unlock again.
     * The receiver exists only for the duration of a session: registered in
     * {@link #start}, dropped synchronously in {@link #stop}; {@link #onEnded}
     * repeats that idempotent cleanup for helper-initiated exits.
     */
    private final BroadcastReceiver screenOffReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_SCREEN_OFF.equals(intent.getAction())) {
                Log.i(TAG, "screen off; leaving immersive mode");
                stop();
            }
        }
    };
    private boolean screenOffRegistered = false;

    private void registerScreenOff() {
        if (screenOffRegistered)
            return;
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            ctx.registerReceiver(screenOffReceiver, filter,
                    Context.RECEIVER_NOT_EXPORTED);
        else
            ctx.registerReceiver(screenOffReceiver, filter);
        screenOffRegistered = true;
    }

    private void unregisterScreenOff() {
        if (!screenOffRegistered)
            return;
        screenOffRegistered = false;
        try {
            ctx.unregisterReceiver(screenOffReceiver);
        } catch (IllegalArgumentException ignored) {
            // The activity auto-unregisters its receivers on destroy; a session
            // end delivered after that has nothing left to unregister.
        }
    }

    // ---- InputGrab.Listener (all on the main thread) ---------------------

    @Override
    public void onDevice(int dev, int cls, int minX, int maxX, int minY, int maxY,
                         int flags) {
        if (dev < 0 || dev >= devs.length)
            return;
        Dev d = new Dev();
        d.devIdx = dev;
        d.cls = cls;
        d.grabbed = (flags & InputGrab.DEV_GRABBED) != 0;
        d.multitouch = (flags & InputGrab.DEV_MULTITOUCH) != 0;
        d.clickpad = (flags & InputGrab.DEV_CLICKPAD) != 0;
        d.minX = minX;
        d.maxX = maxX;
        d.minY = minY;
        d.maxY = maxY;
        if (cls == InputGrab.CLASS_TOUCHPAD && maxX > minX && maxY > minY) {
            // Same ranges as an earlier pad: one physical pad behind two nodes.
            // Both get a Touchpad: until the duplicate decision, a pad's frames
            // must be interpreted as a pad, never as screen touches.
            for (Dev other : devs) {
                if (other != null && other.pad != null
                        && other.minX == minX && other.maxX == maxX
                        && other.minY == minY && other.maxY == maxY) {
                    d.twin = true;
                    break;
                }
            }
            d.pad = host.newGrabbedPad();
            d.pad.setInputBounds(minX, minY, maxX - minX, maxY - minY);
        }
        devs[dev] = d;
    }

    @Override
    public void onReady(int grabbedCount) {
        starting = false;
        active = true;
        Log.i(TAG, "immersive mode active (" + grabbedCount + " grabbed devices)");
        host.onImmersiveChanged(true);
    }

    @Override
    public void onEnded(int reason) {
        boolean wasRunning = active || starting;
        if (wasRunning && reason == InputGrab.REASON_TOGGLE)
            suppressToggleTail();
        unregisterScreenOff();
        active = false;
        starting = false;
        stats.removeCallbacks(statsTick);
        releaseEverything(true);
        Arrays.fill(devs, null);
        host.onImmersiveChanged(false);
        if (!wasRunning)
            return;

        switch (reason) {
            case InputGrab.REASON_TOGGLE:
                toast(ctx.getString(R.string.immersive_exited));
                break;
            case InputGrab.REASON_STOPPED:
                // Lifecycle teardown (pause / focus loss / surface gone) is not
                // worth a toast; a key-driven exit is.
                if (userExitPending)
                    toast(ctx.getString(R.string.immersive_exited));
                break;
            case InputGrab.REASON_NO_ROOT:
                toast(ctx.getString(R.string.immersive_failed));
                break;
            case InputGrab.REASON_HELPER_ERR:
                toast(ctx.getString(R.string.immersive_no_devices));
                break;
            default:
                toast(ctx.getString(R.string.immersive_interrupted));
                break;
        }
        userExitPending = false;
    }

    @Override
    public void onEvents(int[] batch, int count) {
        if (!active && !starting)
            return;
        statsEvents += count / 4;
        for (int i = 0; i + 3 < count; i += 4) {
            int dev = batch[i];
            if (dev >= 0 && dev < statsPerDev.length)
                statsPerDev[dev]++;
            handleEvent(dev, batch[i + 1], batch[i + 2], batch[i + 3]);
        }
    }

    // ---- evdev translation ----------------------------------------------

    private void handleEvent(int devIdx, int type, int code, int value) {
        if (devIdx == InputGrab.DEV_ALL) {
            // The helper had to drop records. Anything we still believe is held
            // may already be up, so let go of all of it rather than strand a key
            // or a contact on the desktop.
            if (type == EV_SYN && code == SYN_DROPPED)
                releaseEverything(false);
            return;
        }
        if (devIdx < 0 || devIdx >= devs.length)
            return;
        Dev d = devs[devIdx];
        if (d == null)
            return;

        switch (type) {
            case EV_SYN:
                if (code == SYN_REPORT)
                    endFrame(d);
                else if (code == SYN_DROPPED)
                    releaseEverything(false);
                break;
            case EV_KEY:
                handleKeyEvent(d, code, value);
                break;
            case EV_REL:
                handleRelEvent(d, code, value);
                break;
            case EV_ABS:
                handleAbsEvent(d, code, value);
                break;
            default:
                break;   // EV_MSC scan codes, EV_LED, EV_SW: nothing to forward
        }
    }

    private void handleKeyEvent(Dev d, int code, int value) {
        if (value == 2)
            return;   // auto-repeat: the compositor makes its own from the keymap
        boolean pressed = value != 0;

        if (code == BTN_TOUCH) {
            // Only meaningful for the single-touch fallback; protocol B devices
            // track contacts by slot instead.
            d.stTouch = pressed;
            d.absDirty = true;
            return;
        }
        if (code >= BTN_FIRST && code <= BTN_LAST) {
            handlePointerButton(d, code, pressed);
            return;
        }
        if (code >= BTN_TOOL_FIRST && code <= BTN_TOOL_LAST)
            return;   // BTN_TOOL_*: contact-shape hints, not real keys

        if (code < 0 || code >= KEY_CODE_LIMIT)
            return;
        if (keyDown[code] == pressed)
            return;   // a repeated state means a record was dropped, not a new press
        keyDown[code] = pressed;
        host.sendKey(pressed ? 0 : 1, code);
    }

    /**
     * Mouse and clickpad buttons. A single-button clickpad reports every press as
     * BTN_LEFT, so the pressing finger's position decides left vs right — the
     * same rule the Android-captured pad uses — and the choice is latched, so a
     * finger drifting across the midline cannot strand a held button.
     */
    private void handlePointerButton(Dev d, int code, boolean pressed) {
        int button = code;
        if (d.pad != null && d.clickpad && code == BTN_LEFT) {
            if (pressed) {
                syncPadOutput(d);
                MotionEvent probe = buildPadProbe(d);
                if (probe != null) {
                    button = d.pad.clickpadButton(probe);
                    probe.recycle();
                }
                d.latchedButton = button;
                // A physical press is not a gesture; drop whatever was in flight.
                d.pad.cancel();
            } else if (d.latchedButton != 0) {
                button = d.latchedButton;
                d.latchedButton = 0;
            }
        }
        if (button < 0 || button >= KEY_CODE_LIMIT)
            return;
        if (keyDown[button] == pressed)
            return;
        keyDown[button] = pressed;
        host.sendMouseButton(button, pressed);
    }

    /** A throwaway event holding the pad's current contacts, for clickpadButton(). */
    private MotionEvent buildPadProbe(Dev d) {
        int n = gatherContacts(d, probeSlots);
        if (n <= 0)
            return null;
        return buildMotionEvent(d, MotionEvent.ACTION_MOVE, -1, probeSlots, n,
                SystemClock.uptimeMillis());
    }

    private void handleRelEvent(Dev d, int code, int value) {
        switch (code) {
            case REL_X: d.relX += value; break;
            case REL_Y: d.relY += value; break;
            case REL_WHEEL_HI_RES:
                d.sawHiRes = true;
                d.wheel += value / HI_RES_PER_DETENT;
                break;
            case REL_HWHEEL_HI_RES:
                d.sawHiRes = true;
                d.hwheel += value / HI_RES_PER_DETENT;
                break;
            // Drivers that report hi-res send the legacy axis as well; taking
            // both would scroll twice as far.
            case REL_WHEEL:
                if (!d.sawHiRes) d.wheel += value;
                break;
            case REL_HWHEEL:
                if (!d.sawHiRes) d.hwheel += value;
                break;
            default:
                break;
        }
    }

    private void handleAbsEvent(Dev d, int code, int value) {
        if (d.cls != InputGrab.CLASS_TOUCHSCREEN && d.cls != InputGrab.CLASS_TOUCHPAD)
            return;   // a gamepad's sticks are not contacts
        switch (code) {
            case ABS_MT_SLOT:
                d.curSlot = (value >= 0 && value < MAX_SLOTS) ? value : MAX_SLOTS - 1;
                break;
            case ABS_MT_TRACKING_ID:
                d.sawTrackingId = true;
                d.trackingId[d.curSlot] = value;
                d.absDirty = true;
                break;
            case ABS_MT_POSITION_X:
                d.x[d.curSlot] = value;
                d.absDirty = true;
                break;
            case ABS_MT_POSITION_Y:
                d.y[d.curSlot] = value;
                d.absDirty = true;
                break;
            case ABS_X:
                d.stX = value;
                d.stValid = true;
                d.absDirty = true;
                break;
            case ABS_Y:
                d.stY = value;
                d.stValid = true;
                d.absDirty = true;
                break;
            default:
                break;
        }
    }

    /** SYN_REPORT: the frame is complete, so turn it into app input. */
    private void endFrame(Dev d) {
        if (d.relX != 0f || d.relY != 0f) {
            host.movePointerRelative(d.relX, d.relY);
            d.relX = 0f;
            d.relY = 0f;
        }
        if (d.wheel != 0f) {
            host.sendMouseScroll(0, -d.wheel * WHEEL_STEP);
            d.wheel = 0f;
        }
        if (d.hwheel != 0f) {
            host.sendMouseScroll(1, d.hwheel * WHEEL_STEP);
            d.hwheel = 0f;
        }
        if (d.absDirty) {
            d.absDirty = false;
            resolveTwin(d);
            if (!d.dup)
                dispatchContacts(d);
        }
    }

    /**
     * Once two identical-range pads have both provably delivered real contact
     * frames, they are two paths for one physical pad. The pad with more real
     * frames is the one actually carrying the stream, so it survives; a tie
     * keeps the lower dev id. A twin that never produces real contacts (a dead
     * duplicate node, or one that only reports BTN_TOUCH) never triggers this
     * and never gets dropped. An already-deduped peer is skipped, so a third
     * identical node can still resolve against a live one.
     */
    private void resolveTwin(Dev d) {
        if (!d.twin || d.dup || d.contactFrames <= 0)
            return;
        for (Dev other : devs) {
            if (other == null || other == d || other.cls != InputGrab.CLASS_TOUCHPAD
                    || other.minX != d.minX || other.maxX != d.maxX
                    || other.minY != d.minY || other.maxY != d.maxY)
                continue;
            if (other.contactFrames <= 0 || other.dup)
                continue;
            // Both deliver real contacts: keep the busier one.
            if (d.contactFrames > other.contactFrames
                    || (d.contactFrames == other.contactFrames && d.devIdx < other.devIdx)) {
                other.dup = true;
                Log.i(TAG, "touchpad " + other.devIdx + " (" + other.contactFrames
                        + " frames) duplicates " + d.devIdx + " (" + d.contactFrames
                        + "); ignoring its events");
            } else {
                d.dup = true;
                Log.i(TAG, "touchpad " + d.devIdx + " (" + d.contactFrames
                        + " frames) duplicates " + other.devIdx + " (" + other.contactFrames
                        + "); ignoring its events");
            }
            break;
        }
    }

    /**
     * Slots holding a live contact, in ascending order (== pointer index order).
     *
     * Protocol B devices are tracked by slot; their BTN_TOUCH is only a "someone
     * is touching" hint and produces no contact on its own — a chip that reports
     * the hint without MT data (a dock's pass-through) must not turn into a
     * phantom contact at stale coordinates. The single-touch fallback needs
     * real ABS_X/Y data; the hint alone is never enough, regardless of what the
     * device's capability flags claim.
     */
    private int gatherContacts(Dev d, int[] out) {
        int n = 0;
        if (d.sawTrackingId) {
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (d.trackingId[s] >= 0)
                    out[n++] = s;
            }
        } else if (d.stTouch && d.stValid) {
            // Device without usable MT data: one contact, wherever the last
            // position was.
            d.x[0] = d.stX;
            d.y[0] = d.stY;
            out[n++] = 0;
        }
        return n;
    }

    /**
     * Turn the new contact set into the MotionEvent sequence a real touchscreen
     * would have produced: releases first, then a move for whatever stayed down,
     * then the new contacts. That is the order Android's own input reader uses,
     * and it is what keeps the pointer indices valid inside each event.
     */
    private void dispatchContacts(Dev d) {
        int nDesired = gatherContacts(d, desiredSlots);
        if (nDesired > 0)
            d.contactFrames++;
        long now = SystemClock.uptimeMillis();

        // 1. contacts that lifted, one event each, highest index first so the
        //    removal below never disturbs an index still to be visited.
        for (int i = d.activeCount - 1; i >= 0; i--) {
            if (contains(desiredSlots, nDesired, d.active[i]))
                continue;
            int action = d.activeCount == 1
                    ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP;
            emit(d, action, i, d.active, d.activeCount, now);
            System.arraycopy(d.active, i + 1, d.active, i, d.activeCount - i - 1);
            d.activeCount--;
        }

        // 2. everything still down, moving together
        if (d.activeCount > 0)
            emit(d, MotionEvent.ACTION_MOVE, -1, d.active, d.activeCount, now);

        // 3. contacts that appeared
        for (int i = 0; i < nDesired; i++) {
            int slot = desiredSlots[i];
            if (contains(d.active, d.activeCount, slot))
                continue;
            if (d.activeCount >= MAX_SLOTS)
                break;
            int insert = d.activeCount++;
            d.active[insert] = slot;
            if (d.activeCount == 1) {
                d.downTime = now;
                emit(d, MotionEvent.ACTION_DOWN, -1, d.active, 1, now);
            } else {
                emit(d, MotionEvent.ACTION_POINTER_DOWN, insert, d.active,
                        d.activeCount, now);
            }
        }
    }

    private static boolean contains(int[] arr, int len, int value) {
        for (int i = 0; i < len; i++) {
            if (arr[i] == value)
                return true;
        }
        return false;
    }

    private void emit(Dev d, int action, int actionIndex, int[] slots, int count,
                      long eventTime) {
        if (d.dup)
            return;
        MotionEvent ev = buildMotionEvent(d, action, actionIndex, slots, count,
                eventTime);
        if (ev == null)
            return;
        try {
            if (d.pad != null) {
                syncPadOutput(d);
                d.pad.onTouch(ev);
            } else {
                host.onGrabbedTouch(ev);
            }
        } finally {
            ev.recycle();
        }
    }

    private void syncPadOutput(Dev d) {
        if (d.pad != null)
            d.pad.setOutputSize(host.outputWidth(), host.outputHeight());
    }

    private MotionEvent buildMotionEvent(Dev d, int action, int actionIndex,
                                         int[] slots, int count, long eventTime) {
        if (count <= 0 || count > MAX_SLOTS)
            return null;
        for (int i = 0; i < count; i++) {
            int slot = slots[i];
            props[i].id = slot;
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            mapContact(d, slot);
            coords[i].clear();
            coords[i].x = mapped[0];
            coords[i].y = mapped[1];
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }
        int act = action;
        if (actionIndex >= 0 && (action == MotionEvent.ACTION_POINTER_DOWN
                || action == MotionEvent.ACTION_POINTER_UP))
            act |= actionIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        // obtain() reads exactly `count` entries, so the full-size scratch arrays
        // can be handed over as they are. The source is deliberately
        // SOURCE_TOUCHSCREEN even for a pad: Touchpad normalises pad coordinates
        // with MotionEvent.transform(), which newer Android releases ignore for
        // SOURCE_CLASS_POSITION events.
        return MotionEvent.obtain(d.downTime, eventTime, act, count, props, coords,
                0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
    }

    /**
     * Raw panel coordinates to the space the consumer expects.
     *
     * A touchscreen reports in its own fixed, panel-native frame, which does not
     * turn with the display, so the rotation has to be undone here before scaling
     * into view pixels. A pad has no such relationship to the screen: its
     * coordinates stay as they are and {@link Touchpad#setInputBounds} does the
     * normalising.
     */
    private void mapContact(Dev d, int slot) {
        float rawX = d.x[slot];
        float rawY = d.y[slot];
        if (d.cls != InputGrab.CLASS_TOUCHSCREEN
                || d.maxX <= d.minX || d.maxY <= d.minY) {
            mapped[0] = rawX;
            mapped[1] = rawY;
            return;
        }
        float u = clamp01((rawX - d.minX) / (float) (d.maxX - d.minX));
        float v = clamp01((rawY - d.minY) / (float) (d.maxY - d.minY));
        float ou, ov;
        switch (host.displayRotation()) {
            case Surface.ROTATION_90:  ou = v;       ov = 1f - u; break;
            case Surface.ROTATION_180: ou = 1f - u;  ov = 1f - v; break;
            case Surface.ROTATION_270: ou = 1f - v;  ov = u;      break;
            default:                   ou = u;       ov = v;      break;
        }
        mapped[0] = host.outputOriginX() + ou * host.outputWidth();
        mapped[1] = host.outputOriginY() + ov * host.outputHeight();
    }

    private static float clamp01(float f) {
        if (!(f > 0f))
            return 0f;   // also catches NaN
        return f > 1f ? 1f : f;
    }

    // ---- teardown --------------------------------------------------------

    /**
     * Let go of everything this session put down: contacts, mouse buttons and
     * keys. A missed release would otherwise stay stuck on the desktop with no
     * input left to clear it.
     *
     * @param forgetContacts true when the session is over. On a mid-session
     *        resync the per-slot tracking state is kept instead, because a finger
     *        still on the glass will not have its tracking id announced again —
     *        keeping it lets the next frame bring the contact back as a fresh
     *        press rather than losing it until the user lifts.
     */
    private void releaseEverything(boolean forgetContacts) {
        long now = SystemClock.uptimeMillis();
        for (Dev d : devs) {
            if (d == null)
                continue;
            if (d.activeCount > 0) {
                emit(d, MotionEvent.ACTION_CANCEL, -1, d.active, d.activeCount, now);
                d.activeCount = 0;
            }
            if (forgetContacts) {
                Arrays.fill(d.trackingId, -1);
                d.stTouch = false;
                d.stValid = false;
            }
            d.absDirty = false;
            d.relX = d.relY = d.wheel = d.hwheel = 0f;
            d.latchedButton = 0;
            if (d.pad != null)
                d.pad.cancel();
        }
        for (int code = 0; code < keyDown.length; code++) {
            if (!keyDown[code])
                continue;
            keyDown[code] = false;
            if (code >= BTN_FIRST && code <= BTN_LAST)
                host.sendMouseButton(code, false);
            else
                host.sendKey(1, code);
        }
    }

    private void toast(String text) {
        Toast.makeText(ctx, text, Toast.LENGTH_SHORT).show();
    }
}
