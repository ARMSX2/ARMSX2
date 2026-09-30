package kr.co.iefriends.pcsx2;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Locale;

/** Finds actual siblings of a CHD inside its granted SAF tree. */
final class ContentChdFiles {
    private static final String[] PROJECTION = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE
    };

    private static final class Directory {
        boolean containsTarget;
        final ArrayList<String> chds = new ArrayList<>();
        final ArrayList<String> subdirectories = new ArrayList<>();
    }

    static String[] findSiblings(ContentResolver resolver, Uri file) {
        try {
            if (!DocumentsContract.isTreeUri(file))
                return new String[0];
            String targetId = DocumentsContract.getDocumentId(file);
            String treeId = DocumentsContract.getTreeDocumentId(file);

            // Only ExternalStorageProvider defines IDs as volume:path. Other
            // providers may use opaque IDs, even ones containing slashes.
            if ("com.android.externalstorage.documents".equals(file.getAuthority())) {
                int slash = targetId.lastIndexOf('/');
                if (slash > targetId.indexOf(':')) {
                    String parentId = targetId.substring(0, slash);
                    // Never derive a directory outside the selected tree.
                    if (parentId.equals(treeId) || parentId.startsWith(treeId + "/")) {
                        Directory parent = readDirectory(resolver, file, parentId, targetId);
                        if (parent.containsTarget)
                            return parent.chds.toArray(new String[0]);
                    }
                }
            }

            // SAF has no generic get-parent API. For opaque document IDs, walk
            // the granted tree until we find the directory containing the child.
            // Only that directory's CHDs are eligible, not CHDs elsewhere in it.
            ArrayDeque<String> pending = new ArrayDeque<>();
            HashSet<String> visited = new HashSet<>();
            pending.add(treeId);
            while (!pending.isEmpty()) {
                String directoryId = pending.removeFirst();
                if (!visited.add(directoryId))
                    continue;
                Directory directory;
                try {
                    directory = readDirectory(resolver, file, directoryId, targetId);
                } catch (Exception e) {
                    // An inaccessible unrelated branch must not hide the parent.
                    continue;
                }
                if (directory.containsTarget)
                    return directory.chds.toArray(new String[0]);
                pending.addAll(directory.subdirectories);
            }
        } catch (Exception e) {
            Log.w("ContentChdFiles", "Could not enumerate CHD siblings", e);
        }
        return new String[0];
    }

    private static Directory readDirectory(ContentResolver resolver, Uri tree,
                                           String directoryId, String targetId) {
        Directory result = new Directory();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, directoryId);
        try (Cursor cursor = resolver.query(children, PROJECTION, null, null, null)) {
            if (cursor == null)
                return result;
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                if (id == null)
                    continue;
                if (Document.MIME_TYPE_DIR.equals(mime)) {
                    result.subdirectories.add(id);
                } else if (id.equals(targetId)) {
                    result.containsTarget = true;
                } else if (name != null && name.toLowerCase(Locale.ROOT).endsWith(".chd")) {
                    result.chds.add(DocumentsContract.buildDocumentUriUsingTree(tree, id).toString());
                }
            }
        }
        return result;
    }
}
