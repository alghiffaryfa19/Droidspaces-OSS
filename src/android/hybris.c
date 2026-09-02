/*
 * Droidspaces v6 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"
#include <sys/stat.h>

/* Setup libhybris support by mounting Android host partitions into the container.
 * This is required for libhybris to access Android graphics/media drivers.
 */
int setup_libhybris(const char *rootfs) {
    if (!is_android())
        return 0;

    const char *partitions[] = {
        "/vendor",
        "/system",
        "/apex",
        "/odm"
    };

    int num_partitions = sizeof(partitions) / sizeof(partitions[0]);

    for (int i = 0; i < num_partitions; i++) {
        char dest_path[PATH_MAX];
        snprintf(dest_path, sizeof(dest_path), "%s/android%s", rootfs, partitions[i]);

        /* Ensure destination directory exists */
        if (mkdir_p(dest_path, 0755) != 0 && errno != EEXIST) {
            ds_warn("libhybris: failed to create mount point %s: %s", dest_path, strerror(errno));
            continue;
        }

        /* Bind mount the partition */
        if (bind_mount(partitions[i], dest_path) == 0) {
            ds_log("libhybris: mounted %s to /android%s", partitions[i], partitions[i]);
            /* Libhybris typically expects these to be read-only */
            domount_silent(NULL, dest_path, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL);
        } else {
            /* /odm and /apex might not exist on all devices */
            if (errno != ENOENT) {
                ds_warn("libhybris: failed to mount %s: %s", partitions[i], strerror(errno));
            }
        }
    }

    return 0;
}
