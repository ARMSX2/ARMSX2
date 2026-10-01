package kr.co.iefriends.pcsx2;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.filters.SdkSuppress;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;

@RunWith(AndroidJUnit4.class)
@SdkSuppress(minSdkVersion = 29) // ContentResolver.wrap() for the in-process test provider.
public class ContentChdFilesTest {
    private static final String STORAGE = "com.android.externalstorage.documents";

    private static class Provider extends ContentProvider {
        final Map<String, Object[][]> directories = new HashMap<>();
        final List<String> queried = new ArrayList<>();

        @Override public boolean onCreate() { return true; }
        @Override public String getType(Uri uri) { return Document.MIME_TYPE_DIR; }
        @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
        @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
        @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }

        @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
            String directory = DocumentsContract.getDocumentId(uri);
            queried.add(directory);
            MatrixCursor cursor = new MatrixCursor(projection);
            for (Object[] row : directories.getOrDefault(directory, new Object[0][]))
                cursor.addRow(row);
            return cursor;
        }

        ContentResolver resolver(String authority) {
            ProviderInfo info = new ProviderInfo();
            info.authority = authority;
            attachInfo(InstrumentationRegistry.getInstrumentation().getTargetContext(), info);
            return ContentResolver.wrap(this);
        }
    }

    private static Object[] file(String id, String name) { return new Object[] {id, name, "application/octet-stream"}; }
    private static Object[] dir(String id) { return new Object[] {id, id, Document.MIME_TYPE_DIR}; }
    private static Uri document(String authority, String root, String id) {
        return DocumentsContract.buildDocumentUriUsingTree(DocumentsContract.buildTreeDocumentUri(authority, root), id);
    }

    @Test public void encodedNestedPathFindsOnlySameDirectory() {
        Provider provider = new Provider();
        String root = "primary:Download/PS2";
        String folder = root + "/original";
        String child = folder + "/translation.chd";
        String parent = folder + "/original.chd";
        provider.directories.put(folder, new Object[][] {file(child, "translation.chd"), file(parent, "original.chd"),
            file(folder + "/notes", "notes.txt"), dir(folder + "/nested.chd")});
        Uri uri = document(STORAGE, root, child);
        assertArrayEquals(new String[] {document(STORAGE, root, parent).toString()},
            ContentChdFiles.findSiblings(provider.resolver(STORAGE), uri));
        assertEquals(List.of(folder), provider.queried);
    }

    @Test public void opaqueIdsUseDisplayNamesAndFindNestedParent() {
        Provider provider = new Provider();
        String authority = "test.documents";
        provider.directories.put("root", new Object[][] {dir("folder/opaque"), file("wrong-parent", "original.chd")});
        provider.directories.put("folder/opaque", new Object[][] {file("123", "translation.chd"), file("456", "ORIGINAL.CHD")});
        assertArrayEquals(new String[] {document(authority, "root", "456").toString()},
            ContentChdFiles.findSiblings(provider.resolver(authority), document(authority, "root", "123")));
        assertEquals(List.of("root", "folder/opaque"), provider.queried);
    }

    @Test public void fileAtTreeRootFindsSibling() {
        Provider provider = new Provider();
        String root = "primary:Download/PS2";
        provider.directories.put(root, new Object[][] {file(root + "/child.chd", "child.chd"), file(root + "/parent.chd", "parent.chd")});
        assertArrayEquals(new String[] {document(STORAGE, root, root + "/parent.chd").toString()},
            ContentChdFiles.findSiblings(provider.resolver(STORAGE), document(STORAGE, root, root + "/child.chd")));
    }

    @Test public void neverQueriesDerivedDirectoryOutsideGrantedTree() {
        Provider provider = new Provider();
        String root = "primary:Download/PS2";
        provider.directories.put(root, new Object[][] {file("another", "original.chd")});
        assertArrayEquals(new String[0], ContentChdFiles.findSiblings(provider.resolver(STORAGE),
            document(STORAGE, root, "primary:Download/PS2-other/child.chd")));
        assertEquals(List.of(root), provider.queried);
    }

    @Test public void missingTargetAndDirectoryCyclesReturnNoUnrelatedParent() {
        Provider provider = new Provider();
        provider.directories.put("root", new Object[][] {dir("nested"), file("wrong", "original.chd")});
        provider.directories.put("nested", new Object[][] {dir("root")});
        assertArrayEquals(new String[0], ContentChdFiles.findSiblings(provider.resolver("test.documents"),
            document("test.documents", "root", "missing")));
        assertEquals(List.of("root", "nested"), provider.queried);
    }

    @Test public void inaccessibleBranchDoesNotHideTargetDirectory() {
        Provider provider = new Provider() {
            @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
                if ("denied".equals(DocumentsContract.getDocumentId(uri)))
                    throw new SecurityException("No access");
                return super.query(uri, projection, selection, args, sort);
            }
        };
        provider.directories.put("root", new Object[][] {dir("denied"), dir("allowed")});
        provider.directories.put("allowed", new Object[][] {file("child", "translation.chd"), file("parent", "original.chd")});
        assertArrayEquals(new String[] {document("test.documents", "root", "parent").toString()},
            ContentChdFiles.findSiblings(provider.resolver("test.documents"), document("test.documents", "root", "child")));
    }
}
