package com.droidspaces.app.util

import java.io.File

data class DesktopEntry(
    val name: String,
    val exec: String,
    val icon: String?,
    val file: File
)

object DesktopEntryParser {

    /**
     * Parses all .desktop files in the container's /usr/share/applications directory.
     */
    fun getApplications(rootfsPath: String): List<DesktopEntry> {
        val appsDir = File(rootfsPath, "usr/share/applications")
        if (!appsDir.exists() || !appsDir.isDirectory) {
            return emptyList()
        }

        val entries = mutableListOf<DesktopEntry>()
        
        appsDir.listFiles { file -> file.name.endsWith(".desktop") }?.forEach { file ->
            try {
                val lines = file.readLines()
                var name = ""
                var exec = ""
                var icon: String? = null
                var isNoDisplay = false
                var isHidden = false
                
                var inDesktopEntrySection = false
                
                for (line in lines) {
                    val trimmed = line.trim()
                    if (trimmed == "[Desktop Entry]") {
                        inDesktopEntrySection = true
                        continue
                    } else if (trimmed.startsWith("[")) {
                        inDesktopEntrySection = false
                        continue
                    }
                    
                    if (!inDesktopEntrySection || trimmed.isEmpty() || trimmed.startsWith("#")) {
                        continue
                    }
                    
                    if (trimmed.startsWith("Name=") && name.isEmpty()) {
                        name = trimmed.substring(5)
                    } else if (trimmed.startsWith("Exec=") && exec.isEmpty()) {
                        exec = trimmed.substring(5)
                    } else if (trimmed.startsWith("Icon=") && icon == null) {
                        icon = trimmed.substring(5)
                    } else if (trimmed.startsWith("NoDisplay=true") || trimmed.startsWith("NoDisplay=1")) {
                        isNoDisplay = true
                    } else if (trimmed.startsWith("Hidden=true") || trimmed.startsWith("Hidden=1")) {
                        isHidden = true
                    }
                }
                
                if (name.isNotEmpty() && exec.isNotEmpty() && !isNoDisplay && !isHidden) {
                    // Clean up exec string (remove %F, %U, etc.)
                    val cleanExec = exec.replace(Regex("%[a-zA-Z]"), "").trim()
                    entries.add(DesktopEntry(name, cleanExec, icon, file))
                }
            } catch (e: Exception) {
                // Ignore parse errors for individual files
            }
        }
        
        return entries.sortedBy { it.name.lowercase() }
    }
    
    /**
     * Finds the absolute path to an icon file given its name.
     */
    fun findIconPath(rootfsPath: String, iconName: String): String? {
        if (iconName.isEmpty()) return null
        
        // If it's already an absolute path
        if (iconName.startsWith("/")) {
            val absoluteFile = File(rootfsPath, iconName.substring(1))
            if (absoluteFile.exists()) return absoluteFile.absolutePath
        }
        
        // Search common icon directories
        val pixmapsDir = File(rootfsPath, "usr/share/pixmaps")
        if (pixmapsDir.exists()) {
            val exts = listOf(".png", ".svg", ".xpm", "")
            for (ext in exts) {
                val f = File(pixmapsDir, "$iconName$ext")
                if (f.exists()) return f.absolutePath
            }
        }
        
        // Simplified hicolor search (just fallback to hicolor/48x48 or scalable)
        val hicolorDir = File(rootfsPath, "usr/share/icons/hicolor")
        if (hicolorDir.exists()) {
            val commonSizes = listOf("48x48/apps", "scalable/apps", "64x64/apps", "128x128/apps", "256x256/apps", "32x32/apps")
            for (size in commonSizes) {
                val ext = if (size.startsWith("scalable")) ".svg" else ".png"
                val f = File(hicolorDir, "$size/$iconName$ext")
                if (f.exists()) return f.absolutePath
            }
        }
        
        return null
    }
}
