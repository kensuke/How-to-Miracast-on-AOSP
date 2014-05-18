package com.example.mira4u;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.SharedPreferences.Editor;
import android.os.Bundle;
import android.os.SystemProperties;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.CheckBoxPreference;
import android.preference.Preference;
import android.preference.Preference.OnPreferenceChangeListener;
import android.preference.PreferenceActivity;
import android.preference.PreferenceScreen;
import android.widget.Toast;
import android.util.Log;

public class SettingsActivity extends PreferenceActivity {

    private final String TAG = "SettingsActivity";

    /**
     * App exit on back key pressed
     */
    @Override
    public void onBackPressed() {
        super.onBackPressed();
        System.exit(0);
    }

    @Override
    @Deprecated
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.pref);

        // always shows scrollbar
        getListView().setScrollbarFadingEnabled(false);

        // initialize list
        initList("persist.sys.wfd.resolution", R.string.resolution_summary_1st, R.string.resolution_summary);
        initList("persist.sys.wfd.bitrate",    R.string.bitrate_summary_1st,    R.string.bitrate_summary);
        initList("persist.sys.wfd.framerate",  R.string.framerate_summary_1st,  R.string.framerate_summary);
    }

    private void initList(final String listKey, final int initialKey, final int setKey) {
        ListPreference list = (ListPreference)findPreference(listKey); 
        setSummaryOnAppBoot(list, listKey, initialKey, setKey);

        list.setOnPreferenceChangeListener( new OnPreferenceChangeListener() {
            public boolean onPreferenceChange(Preference preference, Object newValue){
                if (newValue == null) {
                    return false; 
                }

                ListPreference listpref = (ListPreference) preference;
                String s = getString(setKey, (String)newValue);
                if (s.startsWith("-1")) {
                    s = getString(R.string.dontcare);
                }
                listpref.setSummary(s);

                SharedPreferences pref = getSharedPreferences("prefs", Context.MODE_WORLD_READABLE);
                Editor edit = pref.edit();
                edit.putString(listKey, (String)newValue);
                edit.commit();

                return true;
            }
        } );
    }

    // non selected list summary
    private void setSummaryOnAppBoot(ListPreference list, String prefkey, int initialKey, int setKey) {
        SharedPreferences pref = getSharedPreferences("prefs", Context.MODE_WORLD_READABLE);
        String f = pref.getString(prefkey, "0");
        String s = f.equals("0") ? getString(initialKey) : getString(setKey, f);
        if (s.startsWith("-1")) {
            s = getString(R.string.dontcare);
        }
        list.setSummary(s);
    }

    @Override
    @Deprecated
    public boolean onPreferenceTreeClick(PreferenceScreen preferenceScreen, Preference preference) {
        String key = preference.getKey();

        // button
        if (key.startsWith("button_")) {
            if (key.equals("button_wfd")) {
                gotoWfd();
            } else if (key.equals("button_sink")) {
                //gotoP2pSink();
                gotoP2pSink2();
            } else if (key.equals("button_wfddev")) {
                gotoWfdDev();
            } else {
                String msg = "onPreferenceTreeClick() Unknown Key["+key+"]";
                Log.e(TAG, msg);
                Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
            }
            return super.onPreferenceTreeClick(preferenceScreen, preference);
        }

        // list
        if (
             (key.equals("persist.sys.wfd.resolution"))
          || (key.equals("persist.sys.wfd.bitrate"))
          || (key.equals("persist.sys.wfd.framerate"))
           ) {
            return super.onPreferenceTreeClick(preferenceScreen, preference);
        }

        // checkbox
        CheckBoxPreference cbp = (CheckBoxPreference)preference;
        boolean chk = cbp.isChecked();

        // for OTA-package
        SystemProperties.set(key, chk ? "1" : "0");
        Log.d("SettingsActivity", "onPreferenceTreeClick() key["+key+"]["+SystemProperties.get(key, "0")+"]");

        // for update.zip
        SharedPreferences pref = getSharedPreferences("prefs", Context.MODE_WORLD_READABLE);
        Editor edit = pref.edit();
        edit.putString(key, chk ? "1" : "0");
        edit.commit();

        return super.onPreferenceTreeClick(preferenceScreen, preference);
    }

    /**
     * invoke Wifi Display Settings Activity
     */
    private void gotoWfd() {
        //String pac = "com.android.settings.wfd";

        //Intent i = new Intent();
        //i.setClassName(pac, pac + ".WifiDisplaySettings");
        //startActivity(i);        startActivity(i);

        Intent i = new Intent();
        i.setAction("android.settings.WIFI_DISPLAY_SETTINGS");
        i.addCategory(Intent.CATEGORY_DEFAULT);
        gotoXXX(i);
    }

    /**
     * invoke p2p Sink Activity
     */
    private void gotoP2pSink() {
        String pac = "com.example.mira4u";

        Intent i = new Intent();
        i.setClassName(pac, pac + ".P2pSinkActivity");
        gotoXXX(i);
    }

    /**
     * invoke p2p Sink Activity for supported return
     */
    private void gotoP2pSink2() {
        String pac = "com.example.mira4u";

        Intent i = new Intent();
        i.setClassName(pac, pac + ".P2pSinkActivity");
        try {
            startActivityForResult(i, P2pSinkActivity.LAUNCH_CODE_SINK);
        } catch (Exception e) {
            String msg = "gotoXXX() " + e.getMessage();
            Log.e(TAG, msg);
            Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
        }
    }

    /**
     * p2p Sink Activity return
     */
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Log.d(TAG, "onActivityResult() requestCode["+requestCode+"] resultCode["+resultCode+"] data["+data+"]");
        // guard
        if (requestCode != P2pSinkActivity.LAUNCH_CODE_SINK) {
            return;
        }
        if (resultCode != P2pSinkActivity.FINISH_CODE_SINK_LOOP) {
            return;
        }
        if (!isSinkAdapter()) {
            return;
        }

        // restart
        gotoP2pSink2();
    }

    /**
     * check Sink Device?
     */
    private boolean isSinkAdapter() {
        SharedPreferences pref = getSharedPreferences("prefs", Context.MODE_WORLD_READABLE);
        String s = pref.getString("persist.sys.wfd.sinkloop", "0");
        return s.equals("1");
    }

    /**
     * invoke Wfd Activity
     */
    private void gotoWfdDev() {
        String pac = "com.example.mira4u";

        Intent i = new Intent();
        i.setClassName(pac, pac + ".WfdActivity");
        gotoXXX(i);
    }

    /**
     * invoke XXX Activity
     */
    private boolean gotoXXX(Intent i) {
        try {
            startActivity(i);
        } catch (Exception e) {
            String msg = "gotoXXX() " + e.getMessage();
            Log.e(TAG, msg);
            Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
            return false;
        }
        return true;
    }

}
