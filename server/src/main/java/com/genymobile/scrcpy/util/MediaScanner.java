package com.genymobile.scrcpy.util;

import android.content.Intent;
import android.net.Uri;

import com.genymobile.scrcpy.FakeContext;

import java.util.HashSet;
import java.util.Set;

/**
 * Makes files transferred from the computer visible to the media store (gallery)
 * immediately. Called from native code when upload tasks complete.
 */
public final class MediaScanner {

    private MediaScanner() {
        // not instantiable
    }

    // Send one MEDIA_SCANNER_SCAN_FILE broadcast per distinct parent directory
    // (scanning a directory covers the files in it), so bulk uploads do not
    // flood the system with one broadcast per file. The bind-service based
    // MediaScannerConnection.scanFile() is blocked on some ROMs (e.g. ColorOS),
    // while the broadcast works there.
    // Any failure is logged and swallowed: refreshing the media store must
    // never break the file transfer flow.
    public static void scanFiles(String[] paths) {
        if (paths == null || paths.length == 0) {
            return;
        }
        try {
            Set<String> dirs = new HashSet<>();
            for (String path : paths) {
                int slash = path != null ? path.lastIndexOf('/') : -1;
                dirs.add(slash > 0 ? path.substring(0, slash) : path);
            }
            for (String dir : dirs) {
                Intent intent = new Intent(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE, Uri.parse("file://" + dir));
                FakeContext.get().sendBroadcast(intent);
            }
            Ln.i("MediaScanner: scan broadcast sent for " + dirs.size() + " dir(s)");
        } catch (Throwable t) {
            Ln.w("MediaScanner: scan failed: " + t);
        }
    }
}
