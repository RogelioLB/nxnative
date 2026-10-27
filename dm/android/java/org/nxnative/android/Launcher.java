// The settings screen before the game starts: Android's counterpart of the iOS launcher (dm/ios/Sources/Launcher.swift; the player's page is docs/settings.md).
// It is the app's entry activity: it shows the game's icon and name (game/icon.jpg and game/control.nacp, copied by scripts/build_android_from_nsp.py) and the settings
// the Android host reads, grouped into Game and Controls. Play writes the choices to <external files>/launcher.txt (KEY=VALUE per line) and starts the NativeActivity,
// which reads that file after env.txt without overriding it (front_android.c env_file): a key env.txt sets is shown greyed out with its value. The choices are kept in
// SharedPreferences. Defaults: the setting's own, or game/defaults.txt (KEY=VALUE, written by the packing script: the game language picked with --language).
// No game in game/ (the self test) or a game already running in this process: straight to the NativeActivity.
package org.nxnative.android;

import android.app.Activity;
import android.app.NativeActivity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class Launcher extends Activity {
    static boolean started;   // the game runs in this process: the NativeActivity ends the process when it closes (front_android.c android_main)

    // nacp.py LANGUAGES: one bit of SupportedLanguageFlag (0x302C) per language, in this order
    static final String[][] LANGUAGES = {{"en-US", "American English"}, {"en-GB", "British English"}, {"ja", "Japanese"}, {"fr", "French"}, {"de", "German"},
        {"es-419", "Latin American Spanish"}, {"es", "Spanish"}, {"it", "Italian"}, {"nl", "Dutch"}, {"fr-CA", "Canadian French"}, {"pt", "Portuguese"}, {"ru", "Russian"},
        {"ko", "Korean"}, {"zh-Hant", "Traditional Chinese"}, {"zh-Hans", "Simplified Chinese"}, {"pt-BR", "Brazilian Portuguese"}};

    // A setting: an environment variable the host reads (dm/games/settings.tsv's columns; only those the Android host honours)
    static final class Setting {
        final String key, kind, section, label, note; String[] values, labels; float lo, hi, step; String def;
        Setting(String key, String kind, String section, String label, String note) { this.key = key; this.kind = kind; this.section = section; this.label = label; this.note = note; }
    }

    File ext, game; SharedPreferences prefs; Map<String, String> envFile, defaults;
    final List<Setting> settings = new ArrayList<>();
    final Map<String, String> chosen = new HashMap<>();
    LinearLayout list; float dp;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        ext = getExternalFilesDir(null); if (ext == null) ext = getFilesDir();
        game = new File(ext, "game");
        if (started || !new File(game, "code/main.fixups.tsv").exists()) { play(false); return; }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        dp = getResources().getDisplayMetrics().density;
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        envFile = readKeys(new File(ext, "env.txt")); defaults = readKeys(new File(game, "defaults.txt"));
        byte[] nacp = readAll(new File(game, "control.nacp"));
        define(nacp);
        for (Setting s : settings) chosen.put(s.key, prefs.getString(s.key, s.def));
        setContentView(build(nacp));
    }

    void define(byte[] nacp) {
        Setting s = new Setting("DM_OPMODE", "choice", "Game", "Console mode", "Docked renders at the TV resolution: sharper, slower on a phone.");
        s.values = new String[] {"0", "1"}; s.labels = new String[] {"Handheld", "Docked"}; s.def = "0"; settings.add(s);
        List<String[]> langs = new ArrayList<>();
        int flags = nacp != null && nacp.length >= 0x3030 ? (nacp[0x302C] & 0xFF) | (nacp[0x302D] & 0xFF) << 8 | (nacp[0x302E] & 0xFF) << 16 | (nacp[0x302F] & 0xFF) << 24 : 0;
        for (int i = 0; i < LANGUAGES.length; i++) if ((flags >> i & 1) != 0) langs.add(LANGUAGES[i]);
        if (langs.size() > 1) {
            s = new Setting("DM_LANG", "choice", "Game", "Game language", null);
            s.values = new String[langs.size()]; s.labels = new String[langs.size()];
            for (int i = 0; i < langs.size(); i++) { s.values[i] = langs.get(i)[0]; s.labels[i] = langs.get(i)[1]; }
            s.def = defaults.containsKey("DM_LANG") ? defaults.get("DM_LANG") : (flags & 1) != 0 ? "en-US" : s.values[0]; settings.add(s);
        }
        byte[] fx = readAll(new File(game, "code/main.fixups.tsv"));   // the rewriter marks a build with a render-scale stub (patch_nso.py RENDER_SCALE)
        if (fx != null && new String(fx, StandardCharsets.UTF_8).contains("# RENDER_SCALE	1")) {
            s = new Setting("DM_RENDER_SCALE", "range", "Game", "Render scale", "Lower draws the 3D scene at fewer pixels: faster, softer picture.");
            s.lo = 0.5f; s.hi = 1.0f; s.step = 0.05f; s.def = "1.0"; settings.add(s);
        }
        s = new Setting("DM_AUDIO_PACE", "choice", "Game", "Game timing",
            "Game time keeps levels timed to the music in step with the picture when the phone runs the game below its frame rate; the music can stutter.");
        s.values = new String[] {"real", "game"}; s.labels = new String[] {"Real time", "Game time"}; s.def = "real"; settings.add(s);
        s = new Setting("DM_SHOW_FPS", "toggle", "Game", "Frame-rate counter", null); s.def = "0"; settings.add(s);
        s = new Setting("DM_RUMBLE", "toggle", "Controls", "Game rumble", null); s.def = "1"; settings.add(s);
        s = new Setting("DM_RUMBLE_GAIN", "range", "Controls", "Rumble strength", null); s.lo = 0; s.hi = 3; s.step = 0.1f; s.def = "1.0"; settings.add(s);
        for (Setting t : settings) if (defaults.containsKey(t.key)) t.def = defaults.get(t.key);
    }

    View build(byte[] nacp) {
        int pad = (int) (16 * dp);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.HORIZONTAL); root.setBackgroundColor(0xFF101418); root.setPadding(pad * 2, pad, pad * 2, pad);
        // left: the game's icon, name and Play
        LinearLayout left = new LinearLayout(this); left.setOrientation(LinearLayout.VERTICAL); left.setGravity(Gravity.CENTER);
        ImageView icon = new ImageView(this);
        Bitmap bm = BitmapFactory.decodeFile(new File(game, "icon.jpg").getPath());
        if (bm != null) icon.setImageBitmap(bm); else icon.setBackgroundColor(0xFF2A3038);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER); icon.setClipToOutline(true);
        GradientDrawable round = new GradientDrawable(); round.setCornerRadius(14 * dp); round.setColor(0xFF2A3038); icon.setBackground(round);
        left.addView(icon, new LinearLayout.LayoutParams((int) (190 * dp), (int) (190 * dp)));
        TextView name = text(title(nacp), 20, Color.WHITE, true); name.setGravity(Gravity.CENTER); name.setPadding(0, pad, 0, pad);
        left.addView(name, new LinearLayout.LayoutParams((int) (230 * dp), LinearLayout.LayoutParams.WRAP_CONTENT));
        Button play = new Button(this); play.setText("Play"); play.setTextSize(18); play.setAllCaps(false);
        play.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { play(true); } });
        left.addView(play, new LinearLayout.LayoutParams((int) (200 * dp), LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(left, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.MATCH_PARENT));
        // right: the settings, one scrollable list
        ScrollView sv = new ScrollView(this); list = new LinearLayout(this); list.setOrientation(LinearLayout.VERTICAL); list.setPadding(pad * 2, 0, pad, pad);
        sv.addView(list); fill();
        root.addView(sv, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1));
        return root;
    }

    void fill() {
        list.removeAllViews();
        String section = null;
        for (Setting s : settings) {
            if (!s.section.equals(section)) { section = s.section; TextView h = text(section, 14, 0xFF8FB8FF, true); h.setPadding(0, (int) (18 * dp), 0, (int) (4 * dp)); list.addView(h); }
            list.addView(row(s));
        }
        Button reset = new Button(this); reset.setText("Reset to tested defaults"); reset.setAllCaps(false);
        reset.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { prefs.edit().clear().apply(); for (Setting s : settings) chosen.put(s.key, s.def); fill(); } });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT); lp.topMargin = (int) (20 * dp);
        list.addView(reset, lp);
    }

    View row(final Setting s) {
        LinearLayout r = new LinearLayout(this); r.setOrientation(LinearLayout.VERTICAL); r.setPadding(0, (int) (8 * dp), 0, (int) (8 * dp));
        LinearLayout line = new LinearLayout(this); line.setOrientation(LinearLayout.HORIZONTAL); line.setGravity(Gravity.CENTER_VERTICAL);
        line.addView(text(s.label, 16, Color.WHITE, false), new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        final boolean fixed = envFile.containsKey(s.key); String cur = fixed ? envFile.get(s.key) : chosen.get(s.key);
        View ctl;
        if (s.kind.equals("choice")) {
            Spinner sp = new Spinner(this, Spinner.MODE_DROPDOWN);
            ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, s.labels); ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            sp.setAdapter(ad); int at = indexOf(s.values, cur); sp.setSelection(Math.max(at, 0));
            sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                public void onItemSelected(AdapterView<?> p, View v, int i, long id) { set(s, s.values[i]); }
                public void onNothingSelected(AdapterView<?> p) {}
            });
            ctl = sp;
        } else if (s.kind.equals("toggle")) {
            Switch sw = new Switch(this); sw.setChecked(!"0".equals(cur)); sw.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() { public void onCheckedChanged(android.widget.CompoundButton v, boolean on) { set(s, on ? "1" : "0"); } }); ctl = sw;
        } else {
            LinearLayout box = new LinearLayout(this); box.setGravity(Gravity.CENTER_VERTICAL);
            final TextView val = text("", 14, 0xFFC0C8D0, false); val.setMinWidth((int) (52 * dp)); val.setGravity(Gravity.END);
            SeekBar sb = new SeekBar(this); final int n = Math.round((s.hi - s.lo) / s.step); sb.setMax(n);
            float f = parse(cur, Float.parseFloat(s.def)); sb.setProgress(Math.round((f - s.lo) / s.step)); val.setText(pct(f));
            sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                public void onProgressChanged(SeekBar b, int p, boolean user) { float v = s.lo + p * s.step; val.setText(pct(v)); if (user) set(s, String.format(Locale.ROOT, "%.2f", v)); }
                public void onStartTrackingTouch(SeekBar b) {}
                public void onStopTrackingTouch(SeekBar b) {}
            });
            box.addView(sb, new LinearLayout.LayoutParams((int) (180 * dp), LinearLayout.LayoutParams.WRAP_CONTENT)); box.addView(val);
            ctl = box;
        }
        ctl.setEnabled(!fixed); if (fixed) { ctl.setAlpha(0.45f); if (ctl instanceof LinearLayout) for (int i = 0; i < ((LinearLayout) ctl).getChildCount(); i++) ((LinearLayout) ctl).getChildAt(i).setEnabled(false); }
        line.addView(ctl);
        r.addView(line);
        String note = fixed ? "Set in env.txt: " + s.key + "=" + cur : s.note;
        if (note != null) { TextView t = text(note, 12, 0xFF8890A0, false); t.setPadding(0, (int) (2 * dp), (int) (8 * dp), 0); r.addView(t); }
        return r;
    }

    void set(Setting s, String v) { chosen.put(s.key, v); prefs.edit().putString(s.key, v).apply(); }

    void play(boolean write) {
        if (write) {
            StringBuilder sb = new StringBuilder("# written by the settings screen (Launcher.java); env.txt wins over these\n");
            for (Setting s : settings) if (!envFile.containsKey(s.key)) sb.append(s.key).append('=').append(chosen.get(s.key)).append('\n');
            try (FileOutputStream o = new FileOutputStream(new File(ext, "launcher.txt"))) { o.write(sb.toString().getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { }
        } else if (!started) new File(ext, "launcher.txt").delete();   // the self test: no choices
        started = true;
        Intent i = new Intent(this, NativeActivity.class); i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        startActivity(i); overridePendingTransition(0, 0); finish();
    }

    // ---- helpers ----
    TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp); t.setTextColor(color); if (bold) t.setTypeface(Typeface.DEFAULT_BOLD); return t;
    }
    static String pct(float v) { return Math.round(v * 100) + "%"; }
    static float parse(String s, float d) { try { return Float.parseFloat(s); } catch (Exception e) { return d; } }
    static int indexOf(String[] a, String v) { for (int i = 0; i < a.length; i++) if (a[i].equals(v)) return i; return -1; }
    static String title(byte[] nacp) {   // the first language entry with a name (0x300 bytes each: name 0x200, publisher 0x100)
        if (nacp != null && nacp.length > 2) try {   // newer SDKs compress the title area: a u16 size + raw deflate, 32 languages x 0x300 (nacp.py name)
            java.util.zip.Inflater z = new java.util.zip.Inflater(true); z.setInput(nacp, 2, Math.min((nacp[0] & 0xFF) | (nacp[1] & 0xFF) << 8, nacp.length - 2));
            byte[] t = new byte[0x6000]; int n = 0; while (n < t.length && !z.finished()) { int k = z.inflate(t, n, t.length - n); if (k == 0 && (z.needsInput() || z.needsDictionary())) break; n += k; }
            z.end(); if (n == 0x6000) nacp = t;
        } catch (java.util.zip.DataFormatException e) { }   // not compressed
        if (nacp != null) for (int i = 0; i < 16 && (i + 1) * 0x300 <= nacp.length; i++) {
            int o = i * 0x300, e = o; while (e < o + 0x200 && nacp[e] != 0) e++;
            if (e > o) return new String(nacp, o, e - o, StandardCharsets.UTF_8);
        }
        return "nxnative";
    }
    static byte[] readAll(File f) {
        try (FileInputStream in = new FileInputStream(f)) { byte[] b = new byte[(int) f.length()]; int n = 0; while (n < b.length) { int k = in.read(b, n, b.length - n); if (k < 0) break; n += k; } return b; }
        catch (IOException e) { return null; }
    }
    static Map<String, String> readKeys(File f) {   // KEY=VALUE per line, # comments (as front_android.c env_file)
        Map<String, String> m = new LinkedHashMap<>(); byte[] b = readAll(f); if (b == null) return m;
        for (String line : new String(b, StandardCharsets.UTF_8).split("\r?\n")) { int eq = line.indexOf('='); if (line.startsWith("#") || eq <= 0) continue; m.put(line.substring(0, eq), line.substring(eq + 1)); }
        return m;
    }
}
