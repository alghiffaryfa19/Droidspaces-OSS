package com.anland.consumer;

/**
 * A secondary window. Behaviour is identical to {@link MainActivity} -- it inherits
 * everything -- but it is declared separately in the manifest with
 * {@code documentLaunchMode="always"} so each launch spins up a new task/window in
 * the same process. Launch it with the {@code EXTRA_SOCKET_PATH} / {@code
 * EXTRA_WINDOW_NAME} extras to point it at another daemon and title its window.
 *
 * The launcher icon keeps starting {@link MainActivity} (the default socket, window
 * name "anland"); this class is not exposed in the launcher.
 */
public class SecondaryActivity extends MainActivity {
}
