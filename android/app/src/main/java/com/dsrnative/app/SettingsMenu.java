package com.dsrnative.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import org.json.JSONObject;

/**
 * The in-game settings menu, opened by holding - and + for a second (controller or on-screen buttons). Tabs:
 * Driver (the GPU driver used from the next start), Settings, Touch (touch controls and their layout) and Language
 * (the game's language, and the menu's at once). Choices are saved to settings.txt at once; those
 * that can change while the game runs are applied at once (native code), the others at the next start (a message
 * says so). The game is held while the menu shows. Works with touch and with a controller (D-pad / stick to move,
 * A to choose, B to close, L / R to switch tabs).
 */
public final class SettingsMenu {
    /** Applies a setting now if it can be; false = at the next start. */
    static native boolean nativeApply(String key, String value);
    /** The menu opened (true) or closed: the game is held while it shows. */
    static native void nativeMenuOpen(boolean open);
    /** Frames shown on screen so far (the FPS counter). */
    static native long nativeFrames();

    /** 1: the GPU samples BC textures itself, 0: it does not (converted on the CPU), -1: not known yet. */
    static native int nativeGpuHasBc();

    // Cheats (native runtime/cheats.h); text as UTF-8 bytes.
    /** "build\tID\tfile" then per cheat "on\tmaster\tname\terror"; empty before the game has started. */
    static native byte[] nativeCheats();
    static native boolean nativeCheatSet(int index, boolean on);
    /** "+added\tunusable", or why nothing was added. */
    static native byte[] nativeCheatAdd(byte[] text);
    /** The first problem with the cheats in `text`, or empty. */
    static native byte[] nativeCheatCheck(byte[] text);
    static native boolean nativeCheatDelete(int index);
    static native byte[] nativeCheatText(int index);

    static final int REQ_DRIVER = 4101, REQ_FOLDER = 4102, REQ_CHEATS = 4103;

    private static String sSettingsPath = "", sDriversDir = "", sRunningDriver = "system", sGameDir = "",
            sDefaultGameDir = "";
    private static SettingsMenu sOpen;

    /** Called by native code at game start. */
    public static void configure(Activity activity, String settingsPath, String driversDir, String runningDriver,
            String gameDir, String defaultGameDir) {
        sSettingsPath = settingsPath;
        sDriversDir = driversDir;
        sRunningDriver = runningDriver == null || runningDriver.isEmpty() ? "system" : runningDriver;
        sGameDir = gameDir;
        sDefaultGameDir = defaultGameDir;
        Map<String, String> v = readSettings();
        Strings.setLanguage(v.containsKey("language") ? v.get("language") : "en-US");
        TouchControls.sRecenter = !"0".equals(v.get("touch_camera_recenter"));
        TouchControls.setLayout(v.get("touch_layout"));
        if ("1".equals(v.get("fps_counter"))) FpsCounter.apply(activity, true);
        showFirstLaunchTip(activity);
    }

    /** The first time the game starts: how to open this menu. */
    private static void showFirstLaunchTip(final Activity activity) {
        final SharedPreferences prefs = activity.getSharedPreferences("dsrnative", Context.MODE_PRIVATE);
        if (prefs.getBoolean("menu_tip_shown", false)) return;
        activity.runOnUiThread(() -> new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            if (activity.isFinishing() || sOpen != null || TouchControls.sEditing) return;
            try {
                AlertDialog d = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(Strings.t("tip_t"))
                        .setMessage(Strings.t("tip"))
                        .setPositiveButton(Strings.t("ok"), null)
                        .create();
                d.setOnDismissListener(x -> {
                    if (sOpen == null && !TouchControls.sEditing) nativeMenuOpen(false);
                });
                nativeMenuOpen(true);  // the game waits while the tip shows
                GameActivity.showImmersive(d);
                prefs.edit().putBoolean("menu_tip_shown", true).apply();
            } catch (Exception e) {
                android.util.Log.w("DSRNative", "settings menu: could not show the first-launch tip", e);
            }
        }, 1500));
    }

    /** Called by native code when - and + have been held for a second. */
    public static void show(final Activity activity) {
        activity.runOnUiThread(() -> {
            if (sOpen != null || TouchControls.sEditing) return;
            try {
                new SettingsMenu(activity).open(0);
            } catch (Exception e) {
                android.util.Log.w("DSRNative", "settings menu: could not open", e);
                sOpen = null;
                nativeMenuOpen(false);
            }
        });
    }

    /** From GameActivity.onActivityResult: true if the result was for the menu. */
    static boolean onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_DRIVER && requestCode != REQ_FOLDER && requestCode != REQ_CHEATS) return false;
        Uri uri = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (sOpen == null || uri == null) return true;
        if (requestCode == REQ_DRIVER)
            sOpen.importDriver(uri);
        else if (requestCode == REQ_CHEATS)
            sOpen.importCheats(uri);
        else
            sOpen.chooseGameFolder(uri);
        return true;
    }

    // ---- settings.txt -------------------------------------------------------------------------------------------

    /** key -> value of settings.txt (the last line for a key wins, as in native code). */
    static Map<String, String> readSettings() {
        Map<String, String> m = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(sSettingsPath))) {
            String line;
            while ((line = r.readLine()) != null) {
                String t = line.trim();
                if (t.isEmpty() || t.startsWith("#")) continue;
                int eq = t.indexOf('=');
                if (eq <= 0) continue;
                m.put(t.substring(0, eq).trim(), t.substring(eq + 1).trim());
            }
        } catch (IOException e) {
            android.util.Log.w("DSRNative", "settings menu: cannot read " + sSettingsPath, e);
        }
        return m;
    }

    /** Sets key = value in settings.txt: every line of the key is replaced, or a line appended. */
    static boolean writeSetting(String key, String value) {
        List<String> lines = new ArrayList<>();
        File f = new File(sSettingsPath);
        if (f.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = r.readLine()) != null) lines.add(line);
            } catch (IOException e) {
                return false;
            }
        }
        boolean done = false;
        for (int i = 0; i < lines.size(); i++) {
            String t = lines.get(i).trim();
            if (t.startsWith("#")) continue;
            int eq = t.indexOf('=');
            if (eq <= 0 || !t.substring(0, eq).trim().equals(key)) continue;
            lines.set(i, key + " = " + value);
            done = true;
        }
        if (!done) {
            lines.add("");
            lines.add(key + " = " + value);
        }
        try (FileWriter w = new FileWriter(f, false)) {
            for (String l : lines) {
                w.write(l);
                w.write('\n');
            }
            return true;
        } catch (IOException e) {
            android.util.Log.w("DSRNative", "settings menu: cannot write " + sSettingsPath, e);
            return false;
        }
    }

    // ---- folders ------------------------------------------------------------------------------------------------

    /** A folder picked with ACTION_OPEN_DOCUMENT_TREE as a file path (the app has "All files access"). */
    static String treeToPath(Uri uri) {
        try {
            String id = DocumentsContract.getTreeDocumentId(uri);
            if (id == null) return null;
            if (id.startsWith("raw:")) return id.substring(4);
            if ("com.android.providers.downloads.documents".equals(uri.getAuthority()) && "downloads".equals(id))
                return Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS).getPath();
            int c = id.indexOf(':');
            if (c < 0) return null;
            String volume = id.substring(0, c), rel = id.substring(c + 1);
            String root = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory().getPath()
                                                             : "/storage/" + volume;
            while (rel.endsWith("/")) rel = rel.substring(0, rel.length() - 1);
            return rel.isEmpty() ? root : root + "/" + rel;
        } catch (Exception e) {
            return null;
        }
    }

    /** The files a game folder must hold: the program and the data archives the game opens at start-up. */
    static final String[] GAME_FILES = {"exefs/main", "dvdbnd0.bhd5", "dvdbnd0.bdt", "dvdbnd1.bhd5", "dvdbnd1.bdt"};

    /** The required game files missing from a folder, comma separated ("" when complete). */
    static String missingGameFiles(String dir) {
        StringBuilder b = new StringBuilder();
        for (String f : GAME_FILES)
            if (!new File(dir, f).isFile()) b.append(b.length() == 0 ? "" : ", ").append(f);
        return b.toString();
    }

    private static int presentGameFiles(File dir) {
        int n = 0;
        for (String f : GAME_FILES)
            if (new File(dir, f).isFile()) n++;
        return n;
    }

    /** The folder itself, its game/ subfolder or (for an exefs folder) its parent. */
    private static File[] gameRootCandidates(String path) {
        File p = new File(path);
        File parent = p.getName().equals("exefs") ? p.getParentFile() : null;
        return new File[] {p, new File(p, "game"), parent};
    }

    /** The game's root in or around a chosen folder (holding exefs/main and the data archives), or null. */
    static String findGameRoot(String path) {
        if (path == null) return null;
        for (File c : gameRootCandidates(path))
            if (c != null && presentGameFiles(c) == GAME_FILES.length) return c.getPath();
        return null;
    }

    /** A folder in or around the chosen one with part of the game (not all of it), or null. */
    static String findPartialGameRoot(String path) {
        if (path == null) return null;
        for (File c : gameRootCandidates(path))
            if (c != null && presentGameFiles(c) > 0) return c.getPath();
        return null;
    }

    // ---- the menu -----------------------------------------------------------------------------------------------

    private static final int GOLD = 0xFFC9A55C, PANEL = 0xF2121212, CARD = 0xFF242424, TEXT = 0xFFEDEDED,
            DIM = 0xFF9A9A9A;

    private final Activity mA;
    private final float mDp;
    private Dialog mDialog;
    private LinearLayout mBody;
    private ScrollView mScroll;
    private static final String[] TAB_KEYS = {"tab_driver", "tab_settings", "tab_graphics", "tab_touch", "tab_cheats",
            "tab_language"};
    private static final int TAB_GRAPHICS = 2, TAB_TOUCH = 3, TAB_CHEATS = 4, TAB_LANGUAGE = 5;
    private final TextView[] mTabs = new TextView[TAB_KEYS.length];
    private TextView mHint, mClose, mScaleLabel;
    private int mTab = 0;
    private boolean mEditingLayout;
    private Map<String, String> mValues = new HashMap<>();

    private SettingsMenu(Activity a) {
        mA = a;
        mDp = a.getResources().getDisplayMetrics().density;
    }

    private int dp(float v) {
        return Math.round(v * mDp);
    }

    private void open(int tab) {
        sOpen = this;
        nativeMenuOpen(true);
        TouchControls.releaseForMenu();
        mValues = readSettings();

        LinearLayout root = new LinearLayout(mA);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(14), dp(18), dp(12));
        root.setBackground(rounded(PANEL, 14, 0xFF4A3D22));

        LinearLayout header = new LinearLayout(mA);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("DSR Native", 20, GOLD, true);
        header.addView(title);
        // Tabs: scroll sideways when the names do not fit (long words in some languages, small screens).
        HorizontalScrollView tabScroll = new HorizontalScrollView(mA);
        tabScroll.setHorizontalScrollBarEnabled(false);
        tabScroll.setFillViewport(true);
        LinearLayout tabs = new LinearLayout(mA);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        for (int i = 0; i < mTabs.length; i++) {
            final int t = i;
            mTabs[i] = chip("", true);
            mTabs[i].setOnClickListener(v -> showTab(t));
            tabs.addView(mTabs[i]);
        }
        tabScroll.addView(tabs, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        tlp.leftMargin = dp(10);
        header.addView(tabScroll, tlp);
        mClose = chip("", true);
        mClose.setOnClickListener(v -> mDialog.dismiss());
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.leftMargin = dp(18);
        header.addView(mClose, clp);
        root.addView(header);

        mHint = text("", 12, DIM, false);
        mHint.setPadding(0, dp(4), 0, dp(8));
        root.addView(mHint);
        relabel();

        mScroll = new ScrollView(mA);
        mScroll.setFillViewport(true);
        mBody = new LinearLayout(mA);
        mBody.setOrientation(LinearLayout.VERTICAL);
        mScroll.addView(mBody, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(mScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        mDialog = new Dialog(mA);
        mDialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        mDialog.setContentView(root);
        Window w = mDialog.getWindow();
        if (w != null) w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        mDialog.setCanceledOnTouchOutside(true);
        mDialog.setOnDismissListener(d -> {
            sOpen = null;
            if (!mEditingLayout) nativeMenuOpen(false);  // the layout editor keeps the game held
        });
        mDialog.setOnKeyListener((d, keyCode, e) -> {
            boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
            switch (keyCode) {
                case KeyEvent.KEYCODE_BUTTON_B:
                case KeyEvent.KEYCODE_BACK:
                    if (down && e.getRepeatCount() == 0) mDialog.dismiss();
                    return true;
                case KeyEvent.KEYCODE_BUTTON_A: {
                    if (down && e.getRepeatCount() == 0) {
                        View f = mDialog.getCurrentFocus();
                        if (f != null) f.performClick();
                    }
                    return true;
                }
                case KeyEvent.KEYCODE_BUTTON_L1:
                    if (down && e.getRepeatCount() == 0) showTab((mTab + mTabs.length - 1) % mTabs.length);
                    return true;
                case KeyEvent.KEYCODE_BUTTON_R1:
                    if (down && e.getRepeatCount() == 0) showTab((mTab + 1) % mTabs.length);
                    return true;
                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_BUTTON_SELECT:
                    return true;  // the - and + that opened the menu: nothing
                default:
                    return false;
            }
        });
        showTab(tab);
        GameActivity.showImmersive(mDialog);
        if (w != null) {
            DisplayMetrics m = mA.getResources().getDisplayMetrics();
            w.setLayout(Math.round(m.widthPixels * 0.86f), Math.round(m.heightPixels * 0.92f));
        }
        mTabs[tab].requestFocus();
    }

    /** Texts of the tabs, Close and the hint, in the menu's language. */
    private void relabel() {
        for (int i = 0; i < mTabs.length; i++) mTabs[i].setText(Strings.t(TAB_KEYS[i]));
        mClose.setText(Strings.t("close"));
        mHint.setText(Strings.t("hint"));
    }

    private void showTab(int tab) {
        mTab = tab;
        for (int i = 0; i < mTabs.length; i++) mTabs[i].setSelected(i == tab);
        mBody.removeAllViews();
        mScaleLabel = null;
        mValues = readSettings();
        switch (tab) {
            case 0: buildDriverTab(); break;
            case 1: buildSettingsTab(); break;
            case TAB_GRAPHICS: buildGraphicsTab(); break;
            case TAB_TOUCH: buildTouchTab(); break;
            case TAB_CHEATS: buildCheatsTab(); break;
            default: buildLanguageTab(); break;
        }
        mScroll.scrollTo(0, 0);
    }

    // ---- Graphics tab -------------------------------------------------------------------------------------------

    private void buildGraphicsTab() {
        section(Strings.t("sec_display"));
        choice("fps", Strings.t("fps_t"), Strings.t("fps_auto_n") + " " + Strings.t("changes_now"),
                new String[] {"30", "45", "60", "auto"},
                new String[] {"30 fps", "45 fps", "60 fps", Strings.t("fps_auto")}, "60");
        choice("present_mode", Strings.t("present_t"), Strings.t("changes_next"),
                new String[] {"fifo", "mailbox"},
                new String[] {Strings.t("present_vsync") + " " + Strings.t("default_mark"), Strings.t("present_mailbox")},
                "fifo");
        choice("aspect_ratio", Strings.t("aspect_t"), Strings.t("aspect_n"),
                new String[] {"16:9", "auto", "4:3", "21:9"},
                new String[] {"16:9 " + Strings.t("default_mark"), Strings.t("aspect_fill"), "4:3", "21:9"}, "16:9");
        choice("exposure", Strings.t("bright_t"), Strings.t("changes_now"), new String[] {"1.0", "1.37", "1.7"},
                new String[] {Strings.t("bright_orig"), Strings.t("bright_def"), Strings.t("bright_more")}, "1.37");
        toggle("fps_counter", Strings.t("fpscounter_t"), Strings.t("fpscounter_n") + " " + Strings.t("changes_now"),
                "1", "0", "0");

        section(Strings.t("sec_quality"));
        toggle("performance_mode", Strings.t("perf_t"), Strings.t("perf_n") + " " + Strings.t("changes_next"), "1", "0",
                "0");
        renderScale();  // performance mode sets 75%; it can be moved freely afterwards

        section(Strings.t("sec_gpu"));
        note(Strings.t("gpu_n"));
        toggle("bc3_etc2", Strings.t("bc3_t"), Strings.t("bc3_n") + " " + Strings.t("changes_next"), "1", "0", "0");
        toggle("texture_mip_skip", Strings.t("mip_t"), Strings.t("mip_n") + " " + Strings.t("changes_next"), "1", "0",
                "0");
        toggle("mali_barriers", Strings.t("malib_t"), Strings.t("malib_n") + " " + Strings.t("changes_next"), "1", "0",
                "0");
    }

    // ---- Driver tab ---------------------------------------------------------------------------------------------

    private static String driverLabel(String d) {
        return d == null || d.isEmpty() || d.equals("system") ? Strings.t("system_driver") : d;
    }

    private void buildDriverTab() {
        section(Strings.t("gpu_driver"));
        note(Strings.t("driver_note", sDriversDir));
        note(Strings.t("in_use_now", driverLabel(sRunningDriver)));

        String chosen = mValues.containsKey("gpu_driver") ? mValues.get("gpu_driver") : "system";
        if (chosen == null || chosen.isEmpty()) chosen = "system";
        List<String> drivers = new ArrayList<>();
        drivers.add("system");
        File dir = new File(sDriversDir);
        File[] files = dir.listFiles();
        if (files != null) {
            Arrays.sort(files, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.ROOT);
                if (f.isFile() && (n.endsWith(".zip") || n.endsWith(".so"))) drivers.add(f.getName());
            }
        }
        if (!drivers.contains(chosen)) drivers.add(chosen);  // chosen, but the file is gone: still shown

        final List<TextView> rows = new ArrayList<>();
        for (final String d : drivers) {
            String label = d.equals("system") ? Strings.t("system_driver") + " " + Strings.t("default_mark") : d;
            if (d.equals(sRunningDriver)) label += "   • " + Strings.t("mark_in_use");
            if (!d.equals("system") && !new File(dir, d).isFile()) label += "   • " + Strings.t("mark_missing");
            final TextView row = chip(label, false);
            row.setSelected(d.equals(chosen));
            row.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            row.setOnClickListener(v -> {
                if (row.isSelected()) return;
                if (!writeSetting("gpu_driver", d)) {
                    cantSave();
                    return;
                }
                for (TextView r : rows) r.setSelected(r == row);
                mValues.put("gpu_driver", d);
                if (d.equals(sRunningDriver))
                    popup(Strings.t("tab_driver"), Strings.t("driver_same", driverLabel(d)));
                else
                    popup(Strings.t("driver_changed_t"), Strings.t("driver_changed", driverLabel(d)));
            });
            rows.add(row);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(6);
            mBody.addView(row, lp);
        }

        TextView add = chip(Strings.t("add_driver"), false);
        add.setGravity(Gravity.CENTER);
        add.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                mA.startActivityForResult(i, REQ_DRIVER);
            } catch (Exception e) {
                popup(Strings.t("no_picker_t"), Strings.t("no_picker", sDriversDir));
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(14);
        mBody.addView(add, lp);
    }

    private void importDriver(Uri uri) {
        String name = null;
        try (Cursor c = mA.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null,
                null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception ignored) {
        }
        if (name == null) name = uri.getLastPathSegment();
        if (name != null) name = name.substring(name.lastIndexOf('/') + 1);
        String lower = name == null ? "" : name.toLowerCase(Locale.ROOT);
        if (!lower.endsWith(".zip") && !lower.endsWith(".so")) {
            popup(Strings.t("not_driver_t"), Strings.t("not_driver"));
            return;
        }
        File dir = new File(sDriversDir);
        dir.mkdirs();
        File out = new File(dir, name);
        try (InputStream in = mA.getContentResolver().openInputStream(uri);
                OutputStream o = new FileOutputStream(out)) {
            if (in == null) throw new IOException("cannot open the file");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        } catch (IOException e) {
            out.delete();
            popup(Strings.t("driver_failed_t"), Strings.t("driver_failed", name, e.getMessage()));
            return;
        }
        if (mTab == 0) showTab(0);
        popup(Strings.t("driver_added_t"), Strings.t("driver_added", name));
    }

    // ---- Settings tab -------------------------------------------------------------------------------------------

    private void buildSettingsTab() {
        section(Strings.t("sec_game"));
        toggle("skip_intro", Strings.t("skip_t"), Strings.t("skip_n"), "1", "0", "0");

        section(Strings.t("sec_controls"));
        toggle("face_buttons", Strings.t("swap_t"), Strings.t("swap_n") + " " + Strings.t("changes_now"),
                "position", "label", "label");

        section(Strings.t("sec_folder"));
        String custom = mValues.containsKey("game_dir") ? mValues.get("game_dir") : "";
        note(Strings.t("folder_inuse", sGameDir));
        note(Strings.t("folder_note", sDefaultGameDir)
                + (custom == null || custom.isEmpty() ? "" : "\n" + Strings.t("folder_chosen", custom)));
        TextView pick = chip(Strings.t("folder_pick"), false);
        pick.setGravity(Gravity.CENTER);
        pick.setOnClickListener(v -> {
            try {
                mA.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
            } catch (Exception e) {
                popup(Strings.t("no_fm_t"), Strings.t("no_fm"));
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        mBody.addView(pick, lp);
        if (mA instanceof GameActivity) {
            note(Strings.t("install_n"));
            TextView inst = chip(Strings.t("install_pkg"), false);
            inst.setGravity(Gravity.CENTER);
            inst.setOnClickListener(v -> ((GameActivity) mA).installFromMenu(sGameDir));
            LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            ilp.topMargin = dp(8);
            mBody.addView(inst, ilp);
        }
        note(Strings.t("saved_in", sSettingsPath));
    }

    // ---- render scale -------------------------------------------------------------------------------------------

    private static final float SCALE_MIN = 0.5f, SCALE_STEP = 0.05f;
    private static final int SCALE_STEPS = 10;  // 0.5 .. 1.0

    /** The screen's long side / short side (what "Fill screen" uses). */
    private float screenAspect() {
        DisplayMetrics m = new DisplayMetrics();
        mA.getWindowManager().getDefaultDisplay().getRealMetrics(m);
        float a = Math.max(m.widthPixels, m.heightPixels), b = Math.min(m.widthPixels, m.heightPixels);
        return b > 0 ? a / b : 16f / 9f;
    }

    /** The game's render size for a scale, worked out as native code does (hooks.cpp). */
    private int[] renderSize(float scale) {
        String ar = mValues.containsKey("aspect_ratio") ? mValues.get("aspect_ratio") : "16:9";
        float aspect = 16f / 9f;
        if ("auto".equalsIgnoreCase(ar)) {
            aspect = screenAspect();
        } else if (ar != null) {
            int c = ar.indexOf(':');
            try {
                float w = Float.parseFloat(c > 0 ? ar.substring(0, c) : ar);
                float h = c > 0 ? Float.parseFloat(ar.substring(c + 1)) : 1f;
                if (w > 0 && h > 0 && w / h >= 1.0f && w / h <= 4.0f) aspect = w / h;
            } catch (NumberFormatException ignored) {
            }
        }
        float s = Math.max(0.5f, Math.min(1.0f, scale));
        int h = Math.max(360, Math.min(720, Math.round(720f * s / 8f) * 8));
        int w = Math.max(320, Math.min(1920, Math.round(h * aspect / 8f) * 8));
        if (Math.abs(aspect - 16f / 9f) < 0.01f && h == 720) w = 1280;
        return new int[] {w, h};
    }

    private void updateScaleLabel(float scale) {
        if (mScaleLabel == null) return;
        int[] r = renderSize(scale);
        mScaleLabel.setText(String.format(Locale.ROOT, "%d%%   ·   ", Math.round(scale * 100))
                + Strings.t("scale_res", r[0], r[1]));
    }

    private void renderScale() {
        final String title = Strings.t("scale_t");
        TextView t = text(title, 15, TEXT, true);
        t.setPadding(0, dp(10), 0, 0);
        mBody.addView(t);
        mBody.addView(text(Strings.t("scale_n"), 12, DIM, false));
        float cur = parse(mValues.get("render_scale"), 1.0f);
        cur = Math.max(SCALE_MIN, Math.min(1.0f, cur));
        mScaleLabel = text("", 14, GOLD, true);
        mScaleLabel.setPadding(0, dp(6), 0, 0);
        mBody.addView(mScaleLabel);
        updateScaleLabel(cur);

        final SeekBar bar = new SeekBar(mA);
        bar.setMax(SCALE_STEPS);
        bar.setProgress(Math.round((cur - SCALE_MIN) / SCALE_STEP));
        bar.setFocusable(true);
        bar.setPadding(dp(18), dp(12), dp(18), dp(12));
        bar.setProgressTintList(ColorStateList.valueOf(GOLD));
        bar.setThumbTintList(ColorStateList.valueOf(GOLD));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(0xFF555555));
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_focused}, rounded(0xFF3A3A3A, 10, 0xFFFFFFFF));
        bg.addState(new int[] {}, rounded(CARD, 10, 0));
        bar.setBackground(bg);
        final boolean[] touching = {false};
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar b, int progress, boolean fromUser) {
                float s = SCALE_MIN + progress * SCALE_STEP;
                updateScaleLabel(s);
                if (fromUser && !touching[0]) save(s, false);  // D-pad: saved at each step
            }

            @Override
            public void onStartTrackingTouch(SeekBar b) {
                touching[0] = true;
            }

            @Override
            public void onStopTrackingTouch(SeekBar b) {
                touching[0] = false;
                save(SCALE_MIN + b.getProgress() * SCALE_STEP, true);
            }

            private void save(float s, boolean tell) {
                String v = String.format(Locale.ROOT, "%.2f", s);
                if (same(v, mValues.get("render_scale")) || (!mValues.containsKey("render_scale") && same(v, "1.0")))
                    return;
                if (!writeSetting("render_scale", v)) {
                    cantSave();
                    return;
                }
                mValues.put("render_scale", v);
                if (tell)
                    popup(title, Strings.t("next_start", title));
                else
                    Toast.makeText(mA, Strings.t("changes_next"), Toast.LENGTH_SHORT).show();
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        mBody.addView(bar, lp);
    }

    // ---- Touch tab ----------------------------------------------------------------------------------------------

    private void buildTouchTab() {
        section(Strings.t("sec_touch"));
        toggle("touch_controls", Strings.t("touch_t"), Strings.t("changes_now"), "1", "0", "1");
        choice("touch_opacity", Strings.t("opacity_t"), null, new String[] {"0.3", "0.5", "0.7", "1.0"},
                new String[] {"30%", "50%", "70%", "100%"}, "0.7");
        choice("touch_camera_speed", Strings.t("camspeed_t"), null, new String[] {"0.75", "1.0", "1.5", "2.0"},
                new String[] {Strings.t("speed_slow"), Strings.t("speed_normal"), Strings.t("speed_fast"),
                        Strings.t("speed_faster")}, "1.0");
        toggle("touch_camera_invert", Strings.t("invert_t"), null, "1", "0", "0");
        toggle("touch_camera_recenter", Strings.t("drift_t"), Strings.t("drift_n"), "0", "1", "1");

        section(Strings.t("sec_layout"));
        note(Strings.t("layout_n"));
        TextView edit = chip(Strings.t("edit_layout"), false);
        edit.setGravity(Gravity.CENTER);
        edit.setOnClickListener(v -> editLayout());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        mBody.addView(edit, lp);
        TextView reset = chip(Strings.t("reset_layout"), false);
        reset.setGravity(Gravity.CENTER);
        reset.setOnClickListener(v -> {
            if (!writeSetting("touch_layout", "")) {
                cantSave();
                return;
            }
            mValues.put("touch_layout", "");
            TouchControls.setLayout("");
            Toast.makeText(mA, Strings.t("layout_reset_done"), Toast.LENGTH_SHORT).show();
        });
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp2.topMargin = dp(8);
        mBody.addView(reset, lp2);

        section(Strings.t("sec_presets"));
        note(Strings.t("presets_n"));
        TextView savePreset = chip(Strings.t("preset_save"), false);
        savePreset.setGravity(Gravity.CENTER);
        savePreset.setOnClickListener(v -> savePresetDialog());
        LinearLayout.LayoutParams lp3 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp3.topMargin = dp(8);
        mBody.addView(savePreset, lp3);
        JSONObject presets = loadPresets(mA);
        List<String> names = new ArrayList<>();
        for (Iterator<String> it = presets.keys(); it.hasNext();) names.add(it.next());
        java.util.Collections.sort(names, String.CASE_INSENSITIVE_ORDER);
        String current = mValues.containsKey("touch_layout") ? mValues.get("touch_layout") : "";
        for (final String name : names) {
            final String layout = presets.optString(name, "");
            TextView row = chip(name, false);
            row.setGravity(Gravity.CENTER);
            row.setSelected(same(layout, current) || (layout.isEmpty() && (current == null || current.isEmpty())));
            row.setOnClickListener(v -> presetDialog(name, layout));
            LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            rl.topMargin = dp(6);
            mBody.addView(row, rl);
        }
    }

    // ---- touch layout presets (app preferences, "touch_presets": a JSON object of name -> touch_layout) -------------

    static JSONObject loadPresets(Context c) {
        try {
            return new JSONObject(c.getSharedPreferences("dsrnative", Context.MODE_PRIVATE).getString("touch_presets", "{}"));
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    static void storePresets(Context c, JSONObject presets) {
        c.getSharedPreferences("dsrnative", Context.MODE_PRIVATE).edit().putString("touch_presets", presets.toString())
                .apply();
    }

    private void savePresetDialog() {
        final EditText name = new EditText(mA);
        name.setSingleLine(true);
        name.setHint(Strings.t("preset_name"));
        name.setTextColor(TEXT);
        name.setHintTextColor(DIM);
        LinearLayout box = new LinearLayout(mA);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(name, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        AlertDialog d = new AlertDialog.Builder(mA, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(Strings.t("preset_save"))
                .setView(box)
                .setPositiveButton(Strings.t("save"), (dlg, w) -> {
                    String n = name.getText().toString().trim();
                    if (n.isEmpty()) n = Strings.t("sec_presets") + " " + (loadPresets(mA).length() + 1);
                    if (n.length() > 40) n = n.substring(0, 40);
                    String layout = mValues.containsKey("touch_layout") ? mValues.get("touch_layout") : "";
                    JSONObject presets = loadPresets(mA);
                    try {
                        presets.put(n, layout == null ? "" : layout);
                    } catch (Exception e) {
                        return;
                    }
                    storePresets(mA, presets);
                    Toast.makeText(mA, Strings.t("preset_saved", n), Toast.LENGTH_SHORT).show();
                    if (mTab == TAB_TOUCH) showTab(TAB_TOUCH);
                })
                .setNegativeButton(Strings.t("cancel"), null)
                .create();
        GameActivity.showImmersive(d);
    }

    private void presetDialog(final String name, final String layout) {
        AlertDialog d = new AlertDialog.Builder(mA, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(name)
                .setPositiveButton(Strings.t("preset_use"), (dlg, w) -> {
                    if (!writeSetting("touch_layout", layout)) {
                        cantSave();
                        return;
                    }
                    mValues.put("touch_layout", layout);
                    TouchControls.setLayout(layout);
                    Toast.makeText(mA, Strings.t("preset_loaded", name), Toast.LENGTH_SHORT).show();
                    if (mTab == TAB_TOUCH) showTab(TAB_TOUCH);
                })
                .setNeutralButton(Strings.t("preset_delete"), (dlg, w) -> {
                    JSONObject presets = loadPresets(mA);
                    presets.remove(name);
                    storePresets(mA, presets);
                    Toast.makeText(mA, Strings.t("preset_deleted", name), Toast.LENGTH_SHORT).show();
                    if (mTab == TAB_TOUCH) showTab(TAB_TOUCH);
                })
                .setNegativeButton(Strings.t("cancel"), null)
                .create();
        GameActivity.showImmersive(d);
    }

    /** Closes the menu (the game stays held), lets the player move the controls, then opens the menu again. */
    private void editLayout() {
        mEditingLayout = true;
        TouchControls.sEditing = true;  // no menu from - and + meanwhile
        final Activity a = mA;
        mDialog.dismiss();
        TouchControls.editLayout(a, () -> {
            try {
                new SettingsMenu(a).open(TAB_TOUCH);
            } catch (Exception e) {
                android.util.Log.w("DSRNative", "settings menu: could not open again", e);
                sOpen = null;
                nativeMenuOpen(false);
            }
        });
    }


    // ---- Cheats tab ---------------------------------------------------------------------------------------------

    private static final java.nio.charset.Charset UTF8 = java.nio.charset.StandardCharsets.UTF_8;

    private static String str(byte[] b) {
        return b == null ? "" : new String(b, UTF8);
    }

    private void buildCheatsTab() {
        section(Strings.t("sec_cheats"));
        String list;
        try {
            list = str(nativeCheats());
        } catch (Throwable e) {
            list = "";
        }
        if (list.isEmpty()) {
            note(Strings.t("cheats_nogame"));
            return;
        }
        String[] lines = list.split("\n");
        String[] head = lines[0].split("\t", -1);
        final String file = head.length > 2 ? head[2] : "";
        final String folder = file.lastIndexOf('/') > 0 ? file.substring(0, file.lastIndexOf('/')) : file;
        note(Strings.t("cheats_n", head.length > 1 ? head[1] : "?", file));
        if (lines.length == 1) note(Strings.t("cheats_none"));
        for (int i = 1; i < lines.length; i++) cheatRow(i - 1, lines[i].split("\t", -1));

        section(Strings.t("sec_cheats_add"));
        TextView imp = chip(Strings.t("cheat_import"), false);
        imp.setGravity(Gravity.CENTER);
        imp.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            try {
                mA.startActivityForResult(i, REQ_CHEATS);
            } catch (Exception e) {
                popup(Strings.t("no_fm_t"), Strings.t("cheat_no_picker", folder));
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        mBody.addView(imp, lp);
        TextView type = chip(Strings.t("cheat_type"), false);
        type.setGravity(Gravity.CENTER);
        type.setOnClickListener(v -> typeCheatDialog("", ""));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        tlp.topMargin = dp(8);
        mBody.addView(type, tlp);
    }

    /** One cheat: name, ON / OFF (master codes are always on; unusable ones say why). Long press: codes, delete. */
    private void cheatRow(final int index, String[] f) {
        final String name = f.length > 2 ? f[2] : "?";
        final boolean master = f.length > 1 && "1".equals(f[1]);
        final String error = f.length > 3 ? f[3] : "";
        LinearLayout row = new LinearLayout(mA);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(10), dp(14), dp(10));
        row.setFocusable(true);
        row.setClickable(true);
        row.setLongClickable(true);
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_focused}, rounded(0xFF3A3A3A, 10, 0xFFFFFFFF));
        bg.addState(new int[] {android.R.attr.state_pressed}, rounded(0xFF3A3A3A, 10, 0));
        bg.addState(new int[] {}, rounded(CARD, 10, 0));
        row.setBackground(bg);
        LinearLayout texts = new LinearLayout(mA);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(name, 15, TEXT, true));
        if (!error.isEmpty())
            texts.addView(text(Strings.t("cheat_bad", error), 12, 0xFFE07A5F, false));
        else if (master)
            texts.addView(text(Strings.t("cheat_master"), 12, DIM, false));
        row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final TextView state = new TextView(mA);
        state.setTextSize(14);
        state.setTypeface(Typeface.DEFAULT_BOLD);
        state.setGravity(Gravity.CENTER);
        state.setMinWidth(dp(64));
        state.setPadding(dp(12), dp(6), dp(12), dp(6));
        final boolean[] on = {f.length > 0 && "1".equals(f[0])};
        final Runnable paint = () -> {
            state.setText(!error.isEmpty() ? "—" : on[0] ? Strings.t("on") : Strings.t("off"));
            state.setTextColor(on[0] ? 0xFF151515 : TEXT);
            state.setBackground(rounded(on[0] ? GOLD : 0xFF444444, 14, 0));
        };
        paint.run();
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.leftMargin = dp(12);
        row.addView(state, slp);
        row.setOnClickListener(v -> {
            if (master || !error.isEmpty()) {
                cheatDialog(index, name);
                return;
            }
            boolean ok;
            try {
                ok = nativeCheatSet(index, !on[0]);
            } catch (Throwable e) {
                ok = false;
            }
            if (!ok) {
                showTab(TAB_CHEATS);  // the list changed underneath (e.g. a file was edited)
                return;
            }
            on[0] = !on[0];
            paint.run();
        });
        row.setOnLongClickListener(v -> {
            cheatDialog(index, name);
            return true;
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        mBody.addView(row, lp);
    }

    /** A cheat's codes, with Delete. */
    private void cheatDialog(final int index, final String name) {
        String codes;
        try {
            codes = str(nativeCheatText(index)).trim();
        } catch (Throwable e) {
            codes = "";
        }
        TextView t = text(codes, 13, TEXT, false);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        t.setPadding(dp(20), dp(8), dp(20), dp(8));
        ScrollView sv = new ScrollView(mA);
        sv.addView(t);
        AlertDialog d = new AlertDialog.Builder(mA, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(name)
                .setView(sv)
                .setPositiveButton(Strings.t("close"), null)
                .setNeutralButton(Strings.t("preset_delete"), (dlg, w) -> {
                    boolean ok;
                    try {
                        ok = nativeCheatDelete(index);
                    } catch (Throwable e) {
                        ok = false;
                    }
                    if (ok) Toast.makeText(mA, Strings.t("preset_deleted", name), Toast.LENGTH_SHORT).show();
                    else popup(Strings.t("tab_cheats"), Strings.t("cheat_del_failed"));
                    if (mTab == TAB_CHEATS) showTab(TAB_CHEATS);
                })
                .create();
        GameActivity.showImmersive(d);
    }

    /** Name and codes typed in; checked before they are saved (the dialog comes back with the problem). */
    private void typeCheatDialog(String nameText, String codesText) {
        final EditText name = new EditText(mA);
        name.setSingleLine(true);
        name.setHint(Strings.t("cheat_name"));
        name.setText(nameText);
        name.setTextColor(TEXT);
        name.setHintTextColor(DIM);
        final EditText codes = new EditText(mA);
        codes.setHint(Strings.t("cheat_codes"));
        codes.setText(codesText);
        codes.setTypeface(Typeface.MONOSPACE);
        codes.setMinLines(4);
        codes.setGravity(Gravity.TOP | Gravity.START);
        codes.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | android.text.InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
        codes.setTextColor(TEXT);
        codes.setHintTextColor(DIM);
        LinearLayout box = new LinearLayout(mA);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(name, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        box.addView(codes, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        ScrollView sv = new ScrollView(mA);
        sv.addView(box);
        AlertDialog d = new AlertDialog.Builder(mA, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(Strings.t("cheat_type"))
                .setView(sv)
                .setPositiveButton(Strings.t("save"), (dlg, w) -> {
                    String n = name.getText().toString().replace('[', ' ').replace(']', ' ').trim();
                    String c = codes.getText().toString().trim();
                    if (n.isEmpty()) n = Strings.t("cheat_name");
                    String block = "[" + n + "]\n" + c + "\n";
                    String problem;
                    try {
                        problem = str(nativeCheatCheck(block.getBytes(UTF8)));
                    } catch (Throwable e) {
                        problem = e.toString();
                    }
                    if (!problem.isEmpty()) {
                        final String fn = n;
                        AlertDialog err = new AlertDialog.Builder(mA,
                                android.R.style.Theme_DeviceDefault_Dialog_Alert)
                                .setTitle(Strings.t("tab_cheats"))
                                .setMessage(problem)
                                .setPositiveButton(Strings.t("ok"), (x, y) -> typeCheatDialog(fn, c))
                                .create();
                        GameActivity.showImmersive(err);
                        return;
                    }
                    addCheats(block);
                })
                .setNegativeButton(Strings.t("cancel"), null)
                .create();
        GameActivity.showImmersive(d);
    }

    private void importCheats(Uri uri) {
        java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream();
        try (InputStream in = mA.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("cannot open the file");
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0) {
                buf.write(b, 0, n);
                if (buf.size() > (2 << 20)) throw new IOException("the file is larger than 2 MB");
            }
        } catch (IOException e) {
            popup(Strings.t("tab_cheats"), e.getMessage());
            return;
        }
        addCheats(new String(buf.toByteArray(), UTF8));
    }

    private void addCheats(String text) {
        String r;
        try {
            r = str(nativeCheatAdd(text.getBytes(UTF8)));
        } catch (Throwable e) {
            r = e.toString();
        }
        if (mTab == TAB_CHEATS) showTab(TAB_CHEATS);
        if (!r.startsWith("+")) {
            popup(Strings.t("tab_cheats"), r);
            return;
        }
        String[] p = r.substring(1).split("\t");
        int added = 0, bad = 0;
        try {
            added = Integer.parseInt(p[0]);
            bad = p.length > 1 ? Integer.parseInt(p[1]) : 0;
        } catch (NumberFormatException ignored) {
        }
        if (bad > 0)
            popup(Strings.t("tab_cheats"), Strings.t("cheats_added_bad", added, bad));
        else
            Toast.makeText(mA, Strings.t("cheats_added", added), Toast.LENGTH_SHORT).show();
    }

    // ---- Language tab -------------------------------------------------------------------------------------------

    private void buildLanguageTab() {
        section(Strings.t("tab_language"));
        note(Strings.t("lang_n"));
        String cur = mValues.containsKey("language") ? mValues.get("language") : "en-US";
        int curIndex = Strings.indexOf(cur);
        for (int i = 0; i < Strings.CODES.length; i++) {
            final String code = Strings.CODES[i];
            final TextView row = chip(Strings.NAMES[i], false);
            row.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            row.setSelected(i == curIndex);
            row.setOnClickListener(v -> {
                if (row.isSelected()) return;
                if (!writeSetting("language", code)) {
                    cantSave();
                    return;
                }
                mValues.put("language", code);
                Strings.setLanguage(code);
                relabel();
                showTab(TAB_LANGUAGE);
                mBody.post(() -> {  // keep the controller's focus on the chosen language
                    for (int k = 0; k < mBody.getChildCount(); k++) {
                        View c = mBody.getChildAt(k);
                        if (c.isSelected()) {
                            c.requestFocus();
                            break;
                        }
                    }
                });
                popup(Strings.t("tab_language"), Strings.t("lang_changed"));
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(6);
            mBody.addView(row, lp);
        }
    }

    private void chooseGameFolder(Uri uri) {
        String path = treeToPath(uri);
        String root = findGameRoot(path);
        if (root == null) {
            String partial = findPartialGameRoot(path);
            if (partial != null)
                popup(Strings.t("incomplete_t"), Strings.t("incomplete", partial, missingGameFiles(partial)));
            else
                popup(Strings.t("nofiles_t"), Strings.t("nofiles", path == null ? Strings.t("that_folder") : path));
            return;
        }
        if (!writeSetting("game_dir", root)) {
            cantSave();
            return;
        }
        if (mTab == 1) showTab(1);
        popup(Strings.t("sec_folder"), Strings.t("folder_set", root, sDefaultGameDir));
    }

    // ---- building blocks ----------------------------------------------------------------------------------------

    private static boolean same(String a, String b) {
        if (a == null || b == null) return false;
        if (a.equalsIgnoreCase(b)) return true;
        try {
            return Math.abs(Float.parseFloat(a) - Float.parseFloat(b)) < 1e-3f;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /** A row of choices for one settings.txt key. */
    private void choice(final String key, final String title, String noteText, final String[] values,
            String[] labels, String def) {
        String cur = mValues.containsKey(key) ? mValues.get(key) : def;
        TextView t = text(title, 15, TEXT, true);
        t.setPadding(0, dp(10), 0, 0);
        mBody.addView(t);
        if (noteText != null) {
            TextView n = text(noteText, 12, DIM, false);
            mBody.addView(n);
        }
        HorizontalScrollView hs = new HorizontalScrollView(mA);
        hs.setHorizontalScrollBarEnabled(false);
        LinearLayout row = new LinearLayout(mA);
        row.setOrientation(LinearLayout.HORIZONTAL);
        hs.addView(row);
        final List<TextView> chips = new ArrayList<>();
        for (int i = 0; i < values.length; i++) {
            final String value = values[i];
            final TextView c = chip(labels[i], true);
            c.setSelected(same(value, cur));
            c.setOnClickListener(v -> {
                if (c.isSelected()) return;
                if (!writeSetting(key, value)) {
                    cantSave();
                    return;
                }
                for (TextView o : chips) o.setSelected(o == c);
                mValues.put(key, value);
                applyChoice(key, title, value);
            });
            chips.add(c);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = dp(8);
            lp.topMargin = dp(6);
            row.addView(c, lp);
        }
        mBody.addView(hs);
    }

    /** An on / off switch for one settings.txt key. */
    private void toggle(final String key, final String title, String noteText, final String onValue,
            final String offValue, String def) {
        String cur = mValues.containsKey(key) ? mValues.get(key) : def;
        LinearLayout row = new LinearLayout(mA);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(10), dp(14), dp(10));
        row.setFocusable(true);
        row.setClickable(true);
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_focused}, rounded(0xFF3A3A3A, 10, 0xFFFFFFFF));
        bg.addState(new int[] {android.R.attr.state_pressed}, rounded(0xFF3A3A3A, 10, 0));
        bg.addState(new int[] {}, rounded(CARD, 10, 0));
        row.setBackground(bg);
        LinearLayout texts = new LinearLayout(mA);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(title, 15, TEXT, true));
        if (noteText != null) texts.addView(text(noteText, 12, DIM, false));
        row.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final TextView state = new TextView(mA);
        state.setTextSize(14);
        state.setTypeface(Typeface.DEFAULT_BOLD);
        state.setGravity(Gravity.CENTER);
        state.setMinWidth(dp(64));
        state.setPadding(dp(12), dp(6), dp(12), dp(6));
        state.setDuplicateParentStateEnabled(false);
        final boolean[] on = {same(cur, onValue)};
        final Runnable paint = () -> {
            state.setText(on[0] ? Strings.t("on") : Strings.t("off"));
            state.setTextColor(on[0] ? 0xFF151515 : TEXT);
            state.setBackground(rounded(on[0] ? GOLD : 0xFF444444, 14, 0));
        };
        paint.run();
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.leftMargin = dp(12);
        row.addView(state, slp);
        row.setOnClickListener(v -> {
            String value = on[0] ? offValue : onValue;
            if (!writeSetting(key, value)) {
                cantSave();
                return;
            }
            on[0] = !on[0];
            paint.run();
            mValues.put(key, value);
            applyChoice(key, title, value);
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        mBody.addView(row, lp);
    }

    /** Render scale performance mode sets (the player can move it afterwards; turning the mode off restores theirs). */
    private static final String PERF_SCALE = "0.75";

    /** 1: the GPU has no BC support (performance mode then turns texture_mip_skip on too). */
    private static boolean gpuLacksBc() {
        try {
            return nativeGpuHasBc() == 0;
        } catch (Throwable e) {
            return false;
        }
    }

    /**
     * Performance mode on: remembers the render scale and sets 75%, and on GPUs without BC support remembers
     * texture_mip_skip and turns it on; off: puts the remembered values back.
     */
    private void performanceScale(boolean on) {
        if (on && gpuLacksBc()) {
            String mip = mValues.containsKey("texture_mip_skip") ? mValues.get("texture_mip_skip") : "0";
            writeSetting("texture_mip_skip_before_perf", mip);
            mValues.put("texture_mip_skip_before_perf", mip);
            if (writeSetting("texture_mip_skip", "1")) mValues.put("texture_mip_skip", "1");
        } else if (!on) {
            String before = mValues.get("texture_mip_skip_before_perf");
            if (before != null && !before.isEmpty()) {  // performance mode had turned it on: restored
                if (writeSetting("texture_mip_skip", before)) mValues.put("texture_mip_skip", before);
                writeSetting("texture_mip_skip_before_perf", "");
                mValues.put("texture_mip_skip_before_perf", "");
            }
        }
        String cur = mValues.containsKey("render_scale") ? mValues.get("render_scale") : "1.00";
        String scale;
        if (on) {
            writeSetting("render_scale_before_perf", cur);
            mValues.put("render_scale_before_perf", cur);
            scale = PERF_SCALE;
        } else {
            String before = mValues.get("render_scale_before_perf");
            scale = before == null || before.isEmpty() ? "1.00" : before;
            writeSetting("render_scale_before_perf", "");
            mValues.put("render_scale_before_perf", "");
        }
        if (!writeSetting("render_scale", scale)) {
            cantSave();
            return;
        }
        mValues.put("render_scale", scale);
        showTab(TAB_GRAPHICS);  // the slider shows the new scale
    }

    private void applyChoice(String key, String title, String value) {
        if (key.equals("performance_mode")) performanceScale("1".equals(value));
        boolean now = false;
        try {
            now = nativeApply(key, value);
        } catch (Throwable e) {
            android.util.Log.w("DSRNative", "settings menu: nativeApply failed", e);
        }
        if (key.equals("fps_counter")) {
            FpsCounter.apply(mA, "1".equals(value));
            now = true;
        } else if (key.equals("touch_camera_recenter")) {
            TouchControls.sRecenter = !"0".equals(value);
            now = true;
        } else if (key.startsWith("touch_") && !key.equals("touch_camera_invert")) {
            boolean on = !"0".equals(mValues.get("touch_controls"));
            TouchControls.applySettings(mA, on, parse(mValues.get("touch_opacity"), 0.7f),
                    parse(mValues.get("touch_camera_speed"), 1.0f));
            now = true;
        }
        if (key.equals("aspect_ratio")) updateScaleLabel(parse(mValues.get("render_scale"), 1.0f));
        if (now)
            Toast.makeText(mA, Strings.t("applied", title), Toast.LENGTH_SHORT).show();
        else
            popup(title, Strings.t("next_start", title));
    }

    private void cantSave() {
        popup(Strings.t("cant_save_t"), Strings.t("cant_save", sSettingsPath));
    }

    private static float parse(String s, float def) {
        try {
            return s == null ? def : Float.parseFloat(s);
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private void section(String title) {
        TextView t = text(title.toUpperCase(Locale.getDefault()), 13, GOLD, true);
        t.setLetterSpacing(0.08f);
        t.setPadding(0, dp(mBody.getChildCount() == 0 ? 2 : 18), 0, dp(2));
        mBody.addView(t);
    }

    private void note(String s) {
        TextView n = text(s, 12, DIM, false);
        n.setPadding(0, dp(4), 0, 0);
        mBody.addView(n);
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(mA);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable rounded(int fill, float radiusDp, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(radiusDp));
        if (stroke != 0) g.setStroke(dp(1.5f), stroke);
        return g;
    }

    /** A focusable, clickable choice: gold when chosen, outlined when it has the controller's focus. */
    private TextView chip(String label, boolean compact) {
        TextView c = new TextView(mA);
        c.setText(label);
        c.setTextSize(compact ? 14 : 15);
        c.setPadding(dp(compact ? 14 : 16), dp(compact ? 8 : 12), dp(compact ? 14 : 16), dp(compact ? 8 : 12));
        c.setFocusable(true);
        c.setClickable(true);
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] {android.R.attr.state_selected, android.R.attr.state_focused}, rounded(GOLD, 10, 0xFFFFFFFF));
        bg.addState(new int[] {android.R.attr.state_selected}, rounded(GOLD, 10, 0));
        bg.addState(new int[] {android.R.attr.state_focused}, rounded(0xFF3A3A3A, 10, 0xFFFFFFFF));
        bg.addState(new int[] {android.R.attr.state_pressed}, rounded(0xFF3A3A3A, 10, 0));
        bg.addState(new int[] {}, rounded(CARD, 10, 0));
        c.setBackground(bg);
        c.setTextColor(new ColorStateList(new int[][] {{android.R.attr.state_selected}, {}},
                new int[] {0xFF151515, TEXT}));
        if (compact) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = dp(8);
            c.setLayoutParams(lp);
        }
        return c;
    }

    private void popup(String title, String message) {
        AlertDialog d = new AlertDialog.Builder(mA, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton(Strings.t("ok"), null)
                .create();
        GameActivity.showImmersive(d);
    }
}
