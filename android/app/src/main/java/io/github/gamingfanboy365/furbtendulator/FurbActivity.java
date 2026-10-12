// Furbtendulator for Android: the activity.  The emulator, the picture, the
// sound and the on-screen controls are native (android/native/main.cpp, on
// SDL 2); this class unpacks the data files, imports games and BIOS files
// through Android's file picker, and shows the menus.  The native side calls
// showMenu() with what is loaded ("key=value;..."), and every choice goes
// back to it as a text command through nativeCommand().
package io.github.gamingfanboy365.furbtendulator;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Display;
import android.view.HapticFeedbackConstants;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;

public class FurbActivity extends SDLActivity {
    static native void nativeCommand(String command);

    private static final int PICK_GAME = 1, PICK_BIOS = 2;
    // what Furbtendulator opens (NES.cpp: OpenFile)
    private static final String[] GAME_EXTENSIONS = {".nes", ".unf", ".unif", ".fds", ".qd", ".nsf", ".nsfe", ".tnes", ".stbx", ".bin", ".mfc", ".smc"};

    private Map<String, String> state = new HashMap<>();
    private int openDialogs = 0;
    private boolean picking = false;

    @Override
    protected String[] getLibraries() {
        // libmain.so loads the mapper packs (libfurb_*.so) itself
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Intent start = getIntent();
        // SDL would hand an "open with" intent's path to the native side as a
        // dropped file; content:// URIs have no usable path, so import it here
        setIntent(new Intent(Intent.ACTION_MAIN));
        unpackData();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) return;
        openIntent(start);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        openIntent(intent);
    }

    private void openIntent(Intent intent) {
        if (intent == null || intent.getData() == null || !Intent.ACTION_VIEW.equals(intent.getAction())) return;
        openPicked(intent.getData());
    }

    // ------------------------------------------------------------ data files
    // Furbtendulator's *.cfg files and the BIOS/ and samples/ folders (with
    // their dir.txt lists) go to the app's files folder, which is the
    // emulator's program folder; again whenever the app is updated.
    private void unpackData() {
        SharedPreferences prefs = getSharedPreferences("furb", MODE_PRIVATE);
        long version;
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = pi.lastUpdateTime;
        } catch (Exception e) {
            version = -1;
        }
        if (prefs.getLong("unpacked", 0) == version) return;
        try {
            copyAssets(getAssets(), "furb", getFilesDir());
            prefs.edit().putLong("unpacked", version).apply();
        } catch (IOException e) {
            Toast.makeText(this, "Could not unpack the data files: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private static void copyAssets(AssetManager am, String path, File to) throws IOException {
        String[] list = am.list(path);
        if (list == null || list.length == 0) {
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(to)) {
                copy(in, out);
            }
            return;
        }
        to.mkdirs();
        for (String name : list) copyAssets(am, path + "/" + name, new File(to, name));
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    // ------------------------------------------------------------ importing
    private String displayName(Uri uri) {
        String name = null;
        if (ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
            try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) name = c.getString(0);
            } catch (Exception ignored) {
            }
            // (older media providers leave the display name empty; their path column has it)
            if (name == null || name.isEmpty())
                try (Cursor c = getContentResolver().query(uri, new String[] {"_data"}, null, null, null)) {
                    if (c != null && c.moveToFirst() && c.getString(0) != null) name = new File(c.getString(0)).getName();
                } catch (Exception ignored) {
                }
        }
        if (name == null) name = uri.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "game.nes";
        return name.replaceAll("[/\\\\:*?\"<>|\\x00-\\x1f]", "_");
    }

    private static boolean isGame(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        for (String e : GAME_EXTENSIONS) if (n.endsWith(e)) return true;
        return false;
    }

    // ------------------------------------------------------------ games and archives
    // A picked game is copied into the app (files/roms): the emulator opens
    // files by path, and its battery saves and save states are named after
    // the file.  ZIP, 7z and gzip files are unpacked: an archive with one game
    // opens it, one with several lists them to choose from.  The copying and
    // unpacking run off the UI thread, with the emulator paused.
    private boolean busy = false;

    private File romsDir() {
        File dir = new File(getFilesDir(), "roms");
        dir.mkdirs();
        return dir;
    }

    private static String baseName(String path) {
        String n = path.substring(path.lastIndexOf('/') + 1);
        n = n.replaceAll("[\\\\:*?\"<>|\\x00-\\x1f]", "_");
        return n.isEmpty() ? "game.nes" : n;
    }

    private void openPicked(Uri uri) {
        android.util.Log.i("furb", "opening " + uri);
        busy = true;
        nativeCommand("pause");
        new Thread(() -> {
            String name = displayName(uri);
            File tmp = null;
            Archive archive = null;
            Runnable then;
            try {
                tmp = File.createTempFile("picked", null, getCacheDir());
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream o = new FileOutputStream(tmp)) {
                    if (in == null) throw new IOException("cannot read it");
                    copy(in, o);
                }
                archive = Archive.open(tmp, name);
                android.util.Log.i("furb", name + ": " + (archive == null ? "not an archive" : archive.getClass().getSimpleName()));
                if (archive == null) {
                    File out = new File(romsDir(), baseName(name));
                    if (!tmp.renameTo(out)) throw new IOException("cannot store it");
                    then = () -> openGame(out);
                } else {
                    List<String> games = archive.games();
                    if (games.isEmpty()) throw new IOException("there is no game in it");
                    if (games.size() == 1) {
                        File out = archive.extract(games.get(0), romsDir());
                        archive.close();
                        tmp.delete();
                        then = () -> openGame(out);
                    } else {
                        Archive a = archive;
                        File t = tmp;
                        then = () -> chooseFromArchive(name, a, games, t);
                    }
                }
            } catch (Exception | LinkageError e) {	// (LinkageError: a library missing a part of Java this Android lacks)
                if (archive != null) archive.close();
                if (tmp != null) tmp.delete();
                String why = e.getMessage() != null ? e.getMessage() : e.toString();
                android.util.Log.w("furb", "could not open " + name, e);
                then = () -> Toast.makeText(this, "Could not open " + name + ": " + why, Toast.LENGTH_LONG).show();
            }
            Runnable r = then;
            runOnUiThread(() -> {
                busy = false;
                r.run();
                closed();
            });
        }).start();
    }

    private void openGame(File f) {
        f.setLastModified(System.currentTimeMillis());
        nativeCommand("open " + f.getAbsolutePath());
    }

    // The games in an archive, with a search box when there are many
    private void chooseFromArchive(String archiveName, Archive archive, List<String> games, File tmp) {
        AlertDialog.Builder b = dialog(archiveName);
        android.content.Context c = b.getContext();
        android.widget.LinearLayout box = new android.widget.LinearLayout(c);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        int pad = (int)(20 * getResources().getDisplayMetrics().density);
        android.widget.ArrayAdapter<String> adapter = new android.widget.ArrayAdapter<>(c, android.R.layout.simple_list_item_1, new ArrayList<>(games));
        if (games.size() > 8) {
            android.widget.EditText search = new android.widget.EditText(c);
            search.setHint("Search " + games.size() + " games");
            search.setSingleLine(true);
            search.addTextChangedListener(new android.text.TextWatcher() {
                public void beforeTextChanged(CharSequence t, int a, int n, int m) {}
                // every word typed, anywhere in the name ("mower" finds Lucky_Lawn_Mower_VT09)
                public void onTextChanged(CharSequence t, int a, int n, int m) {
                    String[] words = t.toString().toLowerCase(Locale.ROOT).trim().split("[\\s_]+");
                    adapter.clear();
                    for (String g : games) {
                        String name = g.toLowerCase(Locale.ROOT);
                        boolean all = true;
                        for (String w : words) if (!name.contains(w)) all = false;
                        if (all) adapter.add(g);
                    }
                }
                public void afterTextChanged(android.text.Editable t) {}
            });
            android.widget.FrameLayout frame = new android.widget.FrameLayout(c);
            frame.setPadding(pad, pad / 2, pad, 0);
            frame.addView(search);
            box.addView(frame);
        }
        android.widget.ListView list = new android.widget.ListView(c);
        list.setAdapter(adapter);
        box.addView(list);
        final boolean[] chosen = {false};
        AlertDialog d = show(b.setView(box).setNegativeButton("Cancel", null), () -> {
            if (chosen[0]) return;
            archive.close();
            tmp.delete();
        });
        list.setOnItemClickListener((parent, v, pos, id) -> {
            String entry = adapter.getItem(pos);
            chosen[0] = true;
            busy = true;
            d.dismiss();
            new Thread(() -> {
                File out = null;
                String why = null;
                try {
                    out = archive.extract(entry, romsDir());
                } catch (Exception | LinkageError e) {
                    why = e.getMessage() != null ? e.getMessage() : e.toString();
                }
                archive.close();
                tmp.delete();
                File f = out;
                String w = why;
                runOnUiThread(() -> {
                    busy = false;
                    if (f != null) openGame(f);
                    else Toast.makeText(this, "Could not unpack " + entry + ": " + w, Toast.LENGTH_LONG).show();
                    closed();
                });
            }).start();
        });
    }

    // ZIP, 7z (Android 7.0 or newer) and gzip, recognised by their contents
    private abstract static class Archive {
        abstract List<String> names() throws IOException;	// the files in it
        abstract InputStream read(String name) throws IOException;
        void close() {}

        static Archive open(File f, String name) throws IOException {
            byte[] h = new byte[8];
            int n;
            try (InputStream in = new FileInputStream(f)) { n = in.read(h); }
            if (n >= 4 && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) return new Zip(f);
            if (n >= 6 && h[0] == '7' && h[1] == 'z' && (h[2] & 0xFF) == 0xBC && (h[3] & 0xFF) == 0xAF) {
                if (android.os.Build.VERSION.SDK_INT < 24) throw new IOException("7z archives need Android 7.0 or newer; use a ZIP");
                return new SevenZ(f);
            }
            if (n >= 2 && (h[0] & 0xFF) == 0x1F && (h[1] & 0xFF) == 0x8B) return new Gzip(f, name);
            if (n >= 4 && h[0] == 'R' && h[1] == 'a' && h[2] == 'r' && h[3] == '!') throw new IOException("RAR archives are not supported; use ZIP or 7z");
            return null;
        }

        // the games, or every file when none has a game's extension
        List<String> games() throws IOException {
            List<String> all = names(), games = new ArrayList<>();
            for (String e : all) if (isGame(e)) games.add(e);
            List<String> out = games.isEmpty() ? all : games;
            java.util.Collections.sort(out, String.CASE_INSENSITIVE_ORDER);
            return out;
        }

        File extract(String name, File dir) throws IOException {
            File out = new File(dir, baseName(name));
            try (InputStream in = read(name); OutputStream o = new FileOutputStream(out)) {
                copy(in, o);
            }
            return out;
        }
    }

    private static class Zip extends Archive {
        final java.util.zip.ZipFile zip;
        Zip(File f) throws IOException {
            java.util.zip.ZipFile z;
            try {
                z = new java.util.zip.ZipFile(f);
                for (java.util.Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements(); ) e.nextElement();
            } catch (IllegalArgumentException e) {	// names that are not UTF-8: old DOS tools wrote code page 437
                if (android.os.Build.VERSION.SDK_INT < 24) throw new IOException("its file names are not UTF-8");
                z = new java.util.zip.ZipFile(f, java.nio.charset.Charset.forName("IBM437"));
            }
            zip = z;
        }
        List<String> names() {
            List<String> l = new ArrayList<>();
            for (java.util.Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                if (!z.isDirectory()) l.add(z.getName());
            }
            return l;
        }
        InputStream read(String name) throws IOException {
            ZipEntry z = zip.getEntry(name);
            if (z == null) throw new IOException("not in the archive");
            return zip.getInputStream(z);
        }
        void close() {
            try { zip.close(); } catch (IOException ignored) {}
        }
    }

    private static class SevenZ extends Archive {
        final org.apache.commons.compress.archivers.sevenz.SevenZFile sz;
        @SuppressWarnings("deprecation")
        SevenZ(File f) throws IOException {
            sz = new org.apache.commons.compress.archivers.sevenz.SevenZFile(new FileInputStream(f).getChannel());
        }
        List<String> names() {
            List<String> l = new ArrayList<>();
            for (org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry e : sz.getEntries())
                if (!e.isDirectory() && e.hasStream()) l.add(e.getName());
            return l;
        }
        InputStream read(String name) throws IOException {
            for (org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry e : sz.getEntries())
                if (name.equals(e.getName())) return sz.getInputStream(e);
            throw new IOException("not in the archive");
        }
        void close() {
            try { sz.close(); } catch (IOException ignored) {}
        }
    }

    private static class Gzip extends Archive {
        final File file;
        final String inner;
        Gzip(File f, String name) {
            file = f;
            String n = name.toLowerCase(Locale.ROOT).endsWith(".gz") ? name.substring(0, name.length() - 3) : name;
            inner = n.isEmpty() ? "game.nes" : n;
        }
        List<String> names() { return new ArrayList<>(java.util.Collections.singletonList(inner)); }
        InputStream read(String name) throws IOException { return new java.util.zip.GZIPInputStream(new FileInputStream(file)); }
    }

    // BIOS files go to files/BIOS under the name Furbtendulator looks for
    // (BIOS/dir.txt lists them with their CRC32), whatever the picked file is
    // called; a file not in the list keeps its own name.
    private void importBios(Uri uri) {
        String name = displayName(uri);
        File dir = new File(getFilesDir(), "BIOS");
        dir.mkdirs();
        try {
            byte[] data;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("cannot read it");
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                copy(in, b);
                data = b.toByteArray();
            }
            CRC32 crc = new CRC32();
            crc.update(data);
            String want = String.format(Locale.ROOT, "%08X", crc.getValue());
            String known = null;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(new File(dir, "dir.txt")), "ISO-8859-1"))) {
                for (String line; (line = r.readLine()) != null; ) {
                    String[] f = line.trim().split("\\s+");
                    if (f.length >= 2 && f[1].equalsIgnoreCase(want)) { known = f[0]; break; }
                }
            } catch (IOException ignored) {
            }
            String target = known != null ? known : name;
            try (OutputStream o = new FileOutputStream(new File(dir, target))) {
                o.write(data);
            }
            Toast.makeText(this, known != null ? "Installed " + target : "Copied " + target + " to BIOS (not a known BIOS file)", Toast.LENGTH_LONG).show();
        } catch (Exception e) {
            Toast.makeText(this, "Could not copy " + name + ": " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        // list the device's storage too, not only Recent and Downloads (older pickers hide it)
        i.putExtra("android.content.extra.SHOW_ADVANCED", true);
        picking = true;
        try {
            startActivityForResult(i, what);
        } catch (Exception e) {
            picking = false;
            Toast.makeText(this, "No file picker on this device", Toast.LENGTH_LONG).show();
            closed();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        picking = false;
        Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri != null && request == PICK_GAME) {
            openPicked(uri);
        } else if (uri != null && request == PICK_BIOS) {
            importBios(uri);
        }
        closed();
    }

    // ------------------------------------------------------------ called from native code
    public void showMenu(final String s) {
        runOnUiThread(() -> {
            state.clear();
            for (String kv : s.split(";")) {
                int eq = kv.indexOf('=');
                if (eq > 0) state.put(kv.substring(0, eq), kv.substring(eq + 1));
            }
            mainMenu();
        });
    }

    // A screen mode whose refresh rate is a whole multiple of the game's
    // (millihertz): on a phone running at 90 Hz, 60 Hz (or 120) shows every
    // frame for the same time.  The current mode if it fits, else the slowest
    // that does (less battery); none fits: the system's choice.
    public void preferRefresh(final String fps) {
        if (Build.VERSION.SDK_INT < 23) return;
        runOnUiThread(() -> {
            double game;
            try {
                game = Integer.parseInt(fps) / 1000.0;
            } catch (NumberFormatException e) {
                return;
            }
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode cur = display.getMode(), best = null;
            for (Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                double n = m.getRefreshRate() / game;
                if (Math.round(n) < 1 || Math.abs(n / Math.round(n) - 1) > 0.02) continue;
                if (m.getModeId() == cur.getModeId()) { best = m; break; }
                if (best == null || m.getRefreshRate() < best.getRefreshRate()) best = m;
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            int want = best != null ? best.getModeId() : 0;
            if (lp.preferredDisplayModeId != want) {
                lp.preferredDisplayModeId = want;
                getWindow().setAttributes(lp);
                android.util.Log.i("furb", "screen mode " + (best != null ? best.getRefreshRate() + " Hz" : "(the system's)") + " for " + game + " fps");
            }
        });
    }

    public void buzz(String unused) {
        runOnUiThread(() -> {
            if (mSurface != null) mSurface.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        });
    }

    // ------------------------------------------------------------ menus
    private int num(String key) {
        try {
            return Integer.parseInt(state.get(key));
        } catch (Exception e) {
            return 0;
        }
    }

    // The emulator stays paused while any menu or the file picker is open.
    private void closed() {
        if (openDialogs == 0 && !picking && !busy) nativeCommand("resume");
    }

    private AlertDialog.Builder dialog(String title) {
        return new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert).setTitle(title);
    }

    private AlertDialog show(AlertDialog.Builder b) {
        return show(b, null);
    }

    private AlertDialog show(AlertDialog.Builder b, Runnable dismissed) {
        AlertDialog d = b.create();
        if (openDialogs++ == 0) nativeCommand("pause");	// (already paused when the native side asked for the menu)
        d.setOnDismissListener(x -> {
            openDialogs--;
            if (dismissed != null) dismissed.run();
            // (posted: a click that opens the next menu or the picker does so first)
            ui.post(this::closed);
        });
        d.show();
        return d;
    }

    private final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());

    private interface Action { void run(); }

    private void menu(String title, List<String> labels, List<Action> actions) {
        show(dialog(title).setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run()));
    }

    private void mainMenu() {
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        boolean loaded = num("loaded") != 0;
        l.add("Open game…"); a.add(() -> pick(PICK_GAME));
        if (recentGames().length > 0) { l.add("Recent games…"); a.add(this::recentMenu); }
        if (loaded) {
            l.add("Save state…"); a.add(() -> slotMenu(true));
            l.add("Load state…"); a.add(() -> slotMenu(false));
            l.add("Reset"); a.add(() -> nativeCommand("reset"));
            l.add("Power cycle"); a.add(() -> nativeCommand("hardreset"));
            if (num("fds") > 0) { l.add("Disk…"); a.add(this::diskMenu); }
            if (num("vs") > 0) { l.add("Insert coin"); a.add(() -> nativeCommand("coin1")); }
            if (num("vs") > 1) { l.add("Insert coin (player 2)"); a.add(() -> nativeCommand("coin2")); }
            if (num("nsf") > 0) { l.add("Song…"); a.add(this::songMenu); }
            l.add("Region…"); a.add(this::regionMenu);
        }
        l.add("Settings…"); a.add(this::settingsMenu);
        l.add("Install a BIOS file…"); a.add(() -> pick(PICK_BIOS));
        if (loaded) { l.add("Close game"); a.add(() -> nativeCommand("close")); }
        l.add("Exit"); a.add(() -> nativeCommand("quit"));
        String name = state.get("name");
        menu(loaded && name != null ? name : "Furbtendulator", l, a);
    }

    private File[] recentGames() {
        File[] f = new File(getFilesDir(), "roms").listFiles();
        if (f == null) return new File[0];
        Arrays.sort(f, (x, y) -> Long.compare(y.lastModified(), x.lastModified()));
        return f;
    }

    private void recentMenu() {
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        for (File f : recentGames()) {
            l.add(f.getName());
            a.add(() -> {
                f.setLastModified(System.currentTimeMillis());
                nativeCommand("open " + f.getAbsolutePath());
            });
        }
        l.add("Remove games from this list…"); a.add(this::removeMenu);
        menu("Recent games", l, a);
    }

    // (the copies only: battery saves and save states stay)
    private void removeMenu() {
        File[] games = recentGames();
        String[] names = new String[games.length];
        boolean[] checked = new boolean[games.length];
        for (int i = 0; i < games.length; i++) names[i] = games[i].getName();
        show(dialog("Remove from the list")
            .setMultiChoiceItems(names, checked, (d, i, on) -> checked[i] = on)
            .setPositiveButton("Remove", (d, w) -> {
                for (int i = 0; i < games.length; i++) if (checked[i]) games[i].delete();
            })
            .setNegativeButton("Cancel", null));
    }

    private void slotMenu(boolean save) {
        Map<Integer, Long> used = new HashMap<>();
        String s = state.get("states");
        if (s != null && !s.isEmpty())
            for (String e : s.split(",")) {
                String[] p = e.split(":");
                if (p.length == 2) used.put(Integer.parseInt(p[0]), Long.parseLong(p[1]));
            }
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        for (int i = 0; i < 10; i++) {
            Long t = used.get(i);
            if (!save && t == null) continue;
            final int slot = i;
            l.add("Slot " + i + (t != null ? "  —  " + fmt.format(new Date(t * 1000)) : "  —  empty"));
            a.add(() -> nativeCommand((save ? "save " : "load ") + slot));
        }
        if (l.isEmpty()) {
            Toast.makeText(this, "No save states for this game yet", Toast.LENGTH_SHORT).show();
            return;
        }
        menu(save ? "Save state" : "Load state", l, a);
    }

    private void diskMenu() {
        menu("Disk (" + num("fds") + " sides)",
            Arrays.asList("Next side", "Previous side", "Eject", "Insert"),
            Arrays.asList(() -> nativeCommand("fds-next"), () -> nativeCommand("fds-prev"),
                          () -> nativeCommand("fds-eject"), () -> nativeCommand("fds-insert")));
    }

    private void songMenu() {
        int n = num("nsf");
        String[] songs = new String[n];
        for (int i = 0; i < n; i++) songs[i] = "Song " + (i + 1);
        show(dialog("Song").setSingleChoiceItems(songs, num("song") - 1, (d, i) -> {
            nativeCommand("nsf-song " + (i + 1));
            d.dismiss();
        }));
    }

    private void regionMenu() {
        show(dialog("Region").setSingleChoiceItems(new String[] {"NTSC (60 Hz)", "PAL (50 Hz)", "Dendy (50 Hz)"}, num("region"), (d, i) -> {
            nativeCommand("region " + i);
            d.dismiss();
        }));
    }

    private void settingsMenu() {
        String[] names = {"Square pixels (instead of a TV's 8:7)", "Smooth the picture", "On-screen controls",
                          "Vibrate on button presses", "Sound", "Swap the 25% and 50% duty cycles (as some clone consoles do)",
                          "Show frames per second"};
        String[] keys = {"aspect", "smooth", "controls", "haptics", "sound", "swapduty", "fps"};
        boolean[] on = new boolean[keys.length];
        for (int i = 0; i < keys.length; i++) on[i] = num(keys[i]) != 0;
        show(dialog("Settings")
            .setMultiChoiceItems(names, on, (d, i, v) -> {
                nativeCommand("pref " + keys[i] + " " + (v ? 1 : 0));
                state.put(keys[i], v ? "1" : "0");
            })
            .setNeutralButton("Controls opacity…", (d, w) -> opacityMenu())
            .setPositiveButton("Done", null));
    }

    private void opacityMenu() {
        final int[] levels = {15, 30, 45, 60, 80};
        String[] names = new String[levels.length];
        int cur = 0;
        for (int i = 0; i < levels.length; i++) {
            names[i] = levels[i] + "%";
            if (Math.abs(levels[i] - num("opacity")) < Math.abs(levels[cur] - num("opacity"))) cur = i;
        }
        show(dialog("On-screen controls opacity").setSingleChoiceItems(names, cur, (d, i) -> {
            nativeCommand("pref opacity " + levels[i]);
            d.dismiss();
        }));
    }
}
