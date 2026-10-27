package com.caligo.port;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipFile;

/**
 * First-run setup: the app ships no game files. The player picks their own Caligo Chaser APK and the Com2uS data folder with the system file picker (no storage permission needed),
 * they are copied into the app's private folder once, and the game starts. Skipped when the files are already there (copied in with adb, or bundled in a private test build).
 */
public class SetupActivity extends Activity {
    private static final int REQ_APK = 1, REQ_DIR = 2;
    private TextView status;
    private Button btnApk, btnDir;
    private boolean apkDone, dataDone, busy;

    private File privateDir() { return getFilesDir(); }
    private boolean haveFilesIn(File base) {
        return base != null && new File(base, "CaligoChaser.apk").isFile() && new File(base, "Com2uS/com.com2us.caligo/resources.csr").isFile();
    }
    private boolean ready() {
        if (haveFilesIn(privateDir())) return true;
        try { for (String s : getAssets().list("")) if (s.equals("files.txt")) return true; } catch (Exception ignored) { }      // a bundled test build unpacks its own files
        File ext = getExternalFilesDir(null);
        try { if (haveFilesIn(ext)) return true; } catch (Exception ignored) { }
        try { File[] l = ext == null ? null : ext.listFiles(); if (l != null) for (File f : l) if (f.getName().endsWith(".apk") && new File(ext, "Com2uS/com.com2us.caligo/resources.csr").isFile()) return true; } catch (Exception ignored) { }
        return false;
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (ready()) { startGame(); return; }
        apkDone = new File(privateDir(), "CaligoChaser.apk").isFile();
        dataDone = new File(privateDir(), "Com2uS/com.com2us.caligo/resources.csr").isFile();
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL); col.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density); col.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this); title.setText("Caligo Chaser - first-time setup"); title.setTextSize(24); col.addView(title);
        TextView info = new TextView(this);
        info.setText("This app does not contain the game. Choose your own copies of the Caligo Chaser APK (version 1.0.4) and of the Com2uS data folder (it contains com.com2us.caligo with resources.csr). They are copied into the app once and never leave your phone.");
        info.setPadding(0, pad / 2, 0, pad / 2); col.addView(info);
        btnApk = new Button(this); btnApk.setOnClickListener(v -> pick(REQ_APK)); col.addView(btnApk);
        btnDir = new Button(this); btnDir.setOnClickListener(v -> pick(REQ_DIR)); col.addView(btnDir);
        status = new TextView(this); status.setPadding(0, pad / 2, 0, 0); col.addView(status);
        ScrollView sv = new ScrollView(this); sv.addView(col); setContentView(sv);
        refresh("");
    }

    private void refresh(String msg) {
        btnApk.setText((apkDone ? "\u2714 " : "1. ") + "Choose the game APK" + (apkDone ? " (done)" : ""));
        btnDir.setText((dataDone ? "\u2714 " : "2. ") + "Choose the Com2uS folder" + (dataDone ? " (done)" : ""));
        btnApk.setEnabled(!busy); btnDir.setEnabled(!busy);
        status.setText(msg);
        if (apkDone && dataDone && !busy) startGame();
    }

    private void pick(int req) {
        Intent i;
        if (req == REQ_APK) { i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); }
        else i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, req);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        busy = true; refresh("Copying ...");
        new Thread(() -> {
            String err = null;
            try { if (req == REQ_APK) copyApk(uri); else copyData(uri); } catch (Exception e) { err = e.getMessage() == null ? e.toString() : e.getMessage(); }
            final String msg = err;
            runOnUiThread(() -> { busy = false; if (msg == null) { if (req == REQ_APK) apkDone = true; else dataDone = true; } refresh(msg == null ? "" : "Could not use that: " + msg); });
        }).start();
    }

    private void copyApk(Uri uri) throws Exception {
        File out = new File(privateDir(), "CaligoChaser.apk.part");
        copyStream(getContentResolver().openInputStream(uri), out);
        try (ZipFile z = new ZipFile(out)) { if (z.getEntry("lib/armeabi/libgame.so") == null) throw new Exception("that file is not the Caligo Chaser APK (no lib/armeabi/libgame.so inside)"); }
        File dst = new File(privateDir(), "CaligoChaser.apk"); dst.delete();
        if (!out.renameTo(dst)) throw new Exception("could not save the APK");
    }

    private static void copyStream(InputStream in, File out) throws Exception {
        if (in == null) throw new Exception("could not read the file");
        out.getParentFile().mkdirs();
        try (InputStream i = in; OutputStream o = new FileOutputStream(out)) { byte[] buf = new byte[65536]; int n; while ((n = i.read(buf)) > 0) o.write(buf, 0, n); }
    }

    private static class Doc { String id, name; boolean dir; }
    private List<Doc> children(Uri tree, String parentId) {
        List<Doc> list = new ArrayList<>();
        Uri q = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        try (Cursor c = getContentResolver().query(q, new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) { Doc d = new Doc(); d.id = c.getString(0); d.name = c.getString(1); d.dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)); list.add(d); }
        }
        return list;
    }
    private String nameOf(Uri tree, String id) {
        try (Cursor c = getContentResolver().query(DocumentsContract.buildDocumentUriUsingTree(tree, id), new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) { if (c != null && c.moveToFirst()) return c.getString(0); }
        return "";
    }
    /** The folder called com.com2us.caligo: the picked folder itself, or up to three levels below it. */
    private String findGameDir(Uri tree, String id, int depth) {
        if (nameOf(tree, id).equals("com.com2us.caligo")) return id;
        if (depth == 0) return null;
        for (Doc d : children(tree, id)) if (d.dir) { String r = findGameDir(tree, d.id, depth - 1); if (r != null) return r; }
        return null;
    }
    private int copied;
    private void copyTree(Uri tree, String id, File dst) throws Exception {
        dst.mkdirs();
        for (Doc d : children(tree, id)) {
            File f = new File(dst, d.name);
            if (d.dir) copyTree(tree, d.id, f);
            else { copyStream(getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id)), f); copied++; final int n = copied; runOnUiThread(() -> status.setText("Copying ... " + n + " files")); }
        }
    }
    private void copyData(Uri tree) throws Exception {
        String root = DocumentsContract.getTreeDocumentId(tree);
        String game = findGameDir(tree, root, 3);
        if (game == null) throw new Exception("the folder com.com2us.caligo was not found inside it. Pick the Com2uS folder that you copied from the phone.");
        File dst = new File(privateDir(), "Com2uS/com.com2us.caligo"); copied = 0;
        copyTree(tree, game, dst);
        if (!new File(dst, "resources.csr").isFile()) throw new Exception("resources.csr is missing in that folder.");
    }

    private void startGame() { startActivity(new Intent(this, CaligoActivity.class)); finish(); }
}