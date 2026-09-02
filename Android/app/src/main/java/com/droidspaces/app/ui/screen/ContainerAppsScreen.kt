package com.droidspaces.app.ui.screen

import android.widget.Toast
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import android.graphics.BitmapFactory
import com.droidspaces.app.util.ContainerManager
import com.droidspaces.app.util.DesktopEntry
import com.droidspaces.app.util.DesktopEntryParser
import com.droidspaces.app.util.ShortcutHelper
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ContainerAppsScreen(
    containerName: String,
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var isLoading by remember { mutableStateOf(true) }
    var apps by remember { mutableStateOf<List<DesktopEntry>>(emptyList()) }
    var rootfsPath by remember { mutableStateOf("") }
    var containerUuid by remember { mutableStateOf("") }

    LaunchedEffect(containerName) {
        withContext(Dispatchers.IO) {
            val container = ContainerManager.getContainers().find { it.name == containerName }
            if (container != null) {
                containerUuid = container.uuid
                val rootfs = File(context.filesDir, "rootfs/${container.uuid}").absolutePath
                rootfsPath = rootfs
                val entries = DesktopEntryParser.getApplications(rootfs)
                withContext(Dispatchers.Main) {
                    apps = entries
                    isLoading = false
                }
            } else {
                withContext(Dispatchers.Main) {
                    isLoading = false
                }
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Applications") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        if (isLoading) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
        } else if (apps.isEmpty()) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("No applications found.", style = MaterialTheme.typography.bodyLarge)
            }
        } else {
            LazyVerticalGrid(
                columns = GridCells.Fixed(4),
                contentPadding = PaddingValues(16.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding)
            ) {
                items(apps) { app ->
                    AppItem(
                        app = app,
                        rootfsPath = rootfsPath,
                        onClick = {
                            // Launch the app
                            scope.launch(Dispatchers.IO) {
                                val container = ContainerManager.getContainers().find { it.name == containerName }
                                if (container != null) {
                                    if (!container.isRunning) {
                                        ContainerManager.startContainer(container.name)
                                    }
                                    com.topjohnwu.superuser.Shell.cmd("${com.droidspaces.app.util.Constants.BIN_DIR}/droidspaces exec ${container.name} -- ${app.exec} &").submit()
                                    
                                    val socketPath = "/data/local/tmp/anland-${container.uuid}.sock"
                                    withContext(Dispatchers.Main) {
                                        com.droidspaces.app.util.AnlandUtils.launchWindow(context, container.name, socketPath)
                                    }
                                }
                            }
                        },
                        onAddShortcut = {
                            val iconPath = if (app.icon != null) {
                                DesktopEntryParser.findIconPath(rootfsPath, app.icon)
                            } else null

                            ShortcutHelper.createShortcut(
                                context = context,
                                containerUuid = containerUuid,
                                containerName = containerName,
                                appName = app.name,
                                appExec = app.exec,
                                iconPath = iconPath
                            )
                            Toast.makeText(context, "Shortcut added", Toast.LENGTH_SHORT).show()
                        }
                    )
                }
            }
        }
    }
}

@Composable
fun AppItem(
    app: DesktopEntry,
    rootfsPath: String,
    onClick: () -> Unit,
    onAddShortcut: () -> Unit
) {
    var iconBitmap by remember { mutableStateOf<android.graphics.Bitmap?>(null) }
    
    LaunchedEffect(app) {
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            if (app.icon != null) {
                val iconPath = DesktopEntryParser.findIconPath(rootfsPath, app.icon)
                if (iconPath != null && iconPath.endsWith(".png", ignoreCase = true)) {
                    iconBitmap = BitmapFactory.decodeFile(iconPath)
                }
            }
        }
    }

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
    ) {
        Column(
            modifier = Modifier.padding(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            if (iconBitmap != null) {
                Image(
                    bitmap = iconBitmap!!.asImageBitmap(),
                    contentDescription = app.name,
                    modifier = Modifier.size(48.dp)
                )
            } else {
                Box(
                    modifier = Modifier
                        .size(48.dp)
                        .padding(8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        app.name.take(1).uppercase(),
                        style = MaterialTheme.typography.titleLarge
                    )
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = app.name,
                style = MaterialTheme.typography.bodySmall,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
            IconButton(
                onClick = onAddShortcut,
                modifier = Modifier.size(32.dp)
            ) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = "Add Shortcut",
                    modifier = Modifier.size(16.dp)
                )
            }
        }
    }
}
