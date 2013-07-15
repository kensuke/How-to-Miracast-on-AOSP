package com.example.mira4u;

import android.os.Bundle;
import android.os.SystemProperties;
import android.preference.CheckBoxPreference;
import android.preference.Preference;
import android.preference.PreferenceActivity;
import android.preference.PreferenceScreen;
import android.widget.Toast;
import android.util.Log;

public class SettingsActivity extends PreferenceActivity {

    @Override
    @Deprecated
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        addPreferencesFromResource(R.xml.pref);
    }

    @Override
    @Deprecated
    public boolean onPreferenceTreeClick(PreferenceScreen preferenceScreen, Preference preference) {
        String key = preference.getKey();

        CheckBoxPreference cbp = (CheckBoxPreference)preference;
        boolean chk = cbp.isChecked();

        SystemProperties.set(key, chk ? "1" : "0");
        Log.d("SettingsActivity", "onPreferenceTreeClick() key["+key+"]["+SystemProperties.get(key, "0")+"]");

        return super.onPreferenceTreeClick(preferenceScreen, preference);
    }

}
