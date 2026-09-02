package com.droidspaces.app.util

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.Icon
import androidx.core.content.pm.ShortcutInfoCompat
import androidx.core.content.pm.ShortcutManagerCompat
import androidx.core.graphics.drawable.IconCompat
import com.droidspaces.app.MainActivity
import java.io.File
import java.util.UUID

object ShortcutHelper {

    fun createShortcut(
        context: Context,
        containerUuid: String,
        containerName: String,
        appName: String,
        appExec: String,
        iconPath: String?
    ) {
        val intent = Intent(context, MainActivity::class.java).apply {
            action = "com.droidspaces.app.action.LAUNCH_APP"
            putExtra("container_uuid", containerUuid)
            putExtra("app_exec", appExec)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
        }

        var iconCompat: IconCompat? = null
        if (iconPath != null) {
            val file = File(iconPath)
            if (file.exists()) {
                if (file.extension.lowercase() == "svg") {
                    // Note: In a full implementation, you'd want to render the SVG.
                    // For now, we fallback to default if it's SVG, or assume it's pre-converted.
                } else {
                    val bitmap = BitmapFactory.decodeFile(iconPath)
                    if (bitmap != null) {
                        iconCompat = IconCompat.createWithBitmap(bitmap)
                    }
                }
            }
        }
        
        if (iconCompat == null) {
            // Fallback to a default launcher icon or app icon
            iconCompat = IconCompat.createWithResource(context, com.droidspaces.app.R.mipmap.ic_launcher)
        }

        val shortcutId = "ds_app_${containerUuid}_${UUID.randomUUID().toString().substring(0, 8)}"

        val shortcut = ShortcutInfoCompat.Builder(context, shortcutId)
            .setShortLabel(appName)
            .setLongLabel("$appName ($containerName)")
            .setIcon(iconCompat)
            .setIntent(intent)
            .build()

        ShortcutManagerCompat.requestPinShortcut(context, shortcut, null)
    }
}
