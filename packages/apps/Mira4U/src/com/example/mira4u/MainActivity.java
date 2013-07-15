package com.example.mira4u;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.Enumeration;

import android.os.Bundle;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.View.OnClickListener;
import android.view.View.OnKeyListener;
import android.view.View.OnTouchListener;
import android.view.inputmethod.InputMethodManager;
import android.widget.AutoCompleteTextView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RatingBar;
import android.widget.RatingBar.OnRatingBarChangeListener;
import android.widget.Toast;

public class MainActivity extends Activity {

    /** log tag */
    private static final String TAG = "Mira_for_You";

    /** static self for toast */
    private static MainActivity mSelf;

    /**
     * App exit on back key pressed
     */
    @Override
    public void onBackPressed() {
        super.onBackPressed();
        System.exit(0);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        mSelf = this;
    }

    @Override
    protected void onResume() {
        super.onResume();
        init();
    }

    /**
     * initialize app screen
     */
    private void init() {
        setIPAddressToRatingBar();
        initIPEditText();
        initFinishButton();
        initFWButtons();
    }

    /**
     * android device has some IP addresses, Wi-Fi, Wi-Fi Direct created group, 3G, Eth,,,
     */
    private ArrayList<String> mIPAddress = null;

    /**
     * Select IP address by rating bar
     */
    private void setIPAddressToRatingBar() {
        final RatingBar rb = (RatingBar) findViewById(R.id.ratingBar1);

        // get ip address
        mIPAddress = getLocalIPAddress();
        if (mIPAddress == null || mIPAddress.size() == 0) {
            Toast.makeText(this, R.string.err_no_ip, Toast.LENGTH_SHORT).show();
            disableAll();
            return;
        }

        // set stars as number of ip address
        final int ips = mIPAddress.size();
        rb.setNumStars(ips);
        rb.setStepSize(1);

        // select ip address
        rb.setOnRatingBarChangeListener(new OnRatingBarChangeListener() {
            @Override
            public void onRatingChanged(RatingBar ratingBar, float rating, boolean fromUser) {
                int r = (int) rating;
                Log.d(TAG, "onRatingChanged() rating[" + r + "]");
                if (r <= 0 || r > ips) {
                    return;
                }

                // set ip to textbox
                try {
                    String ipaddr = mIPAddress.get(r - 1);
                    setIPAddrToEdit(ipaddr);
                } catch (ArrayIndexOutOfBoundsException e) {
                    e.printStackTrace();
                }
            }
        });

        // reload when on touch star
        rb.setOnTouchListener(new OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                // select first ip
                rb.setRating(0);
                rb.setRating(1);
                return false;
            }
        });

        // select first ip
        rb.setRating(1);
    }

    /**
     * get ip address<br />
     * interface up && !loopback && IPv4
     */
    private ArrayList<String> getLocalIPAddress() {
        ArrayList<String> addrs = null;
        try {
            // device has some network interfaces
            for (Enumeration<NetworkInterface> en = NetworkInterface.getNetworkInterfaces(); en.hasMoreElements();) {
                NetworkInterface intf = en.nextElement();
                if (!intf.isUp()) { // skip !up interface
                    Log.d(TAG, "getLocalIPAddress() [" + intf.getDisplayName() + "] is not up");
                    continue;
                }

                // a network interface has some addresses
                for (Enumeration<InetAddress> enumIpAddr = intf.getInetAddresses(); enumIpAddr.hasMoreElements();) {
                    InetAddress inetAddress = enumIpAddr.nextElement();
                    if (inetAddress.isLoopbackAddress()) { // skip loopback address
                        continue;
                    }
                    if (!(inetAddress instanceof Inet4Address)) { // skip !IPv4 address
                        continue;
                    }

                    // interface up && !loopback && IPv4
                    Log.d(TAG, "getLocalIPaddress() IPAddress Found[" + inetAddress.getHostAddress() + "] Interface[" + intf.getName() + "]");
                    if (addrs == null) {
                        addrs = new ArrayList<String>();
                    }
                    addrs.add(inetAddress.getHostAddress());
                }
            }
        } catch (SocketException e) {
            e.printStackTrace();
        }

        return addrs;
    }

    /**
     * screen gui disabled
     */
    private void disableAll() {
        findViewById(R.id.ratingBar1).setEnabled(false);
        findViewById(R.id.autoCompleteTextView1).setEnabled(false);
        findViewById(R.id.editText1).setEnabled(false);
        findViewById(R.id.button3).setEnabled(false);
        findViewById(R.id.button31).setEnabled(false);
        findViewById(R.id.button4).setEnabled(false);
        //findViewById(R.id.button5).setEnabled(false); // skip p2p JNI Sink disabled
    }

    /**
     * notify on change IP addres textbox
     */
    private void initIPEditText() {
        AutoCompleteTextView actv = (AutoCompleteTextView) findViewById(R.id.autoCompleteTextView1);
        actv.setOnKeyListener(new OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                // pushed enter key
                if (event.getAction() == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_ENTER) {
                    // hide keyboard
                    InputMethodManager inputMethodManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                    inputMethodManager.hideSoftInputFromWindow(v.getWindowToken(), 0);
                    return true;
                }
                return false;
            }
        });
    }

    /**
     * execute any command
     */
    private BufferedReader exec(String cmd, boolean toast) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(cmd);
        } catch (IOException e) {
            e.printStackTrace();
            Toast.makeText(this, e.getLocalizedMessage(), Toast.LENGTH_SHORT).show();
            return null;
        }

        if (toast) {
            Toast.makeText(this, cmd + " / " + p.toString(), Toast.LENGTH_SHORT).show();
            return null;
        }

        int w = -1;
        try {
            w = p.waitFor();
        } catch (InterruptedException e) {
            e.printStackTrace();
            Toast.makeText(this, e.getLocalizedMessage(), Toast.LENGTH_SHORT).show();
            return null;
        }

        Log.d(TAG, "exec(" + cmd + ") " + p.toString() + " exit[" + w + "]");
        InputStreamReader isr = new InputStreamReader(p.getInputStream());
        BufferedReader br = new BufferedReader(isr);
        return br;
    }

    /**
     * execute command in su<br />
     * Warning! This method not yet implemented!<br />
     * TODO: FIXME
     */
    private BufferedReader exec_su(String cmd, boolean toast) {
        // String[] su = {"su", "-c", cmd};

        Process p = null;
        //try {
        //    p = Runtime.getRuntime().exec("su -c " + cmd);
        //} catch (IOException e) {
        //    e.printStackTrace();
        //    Toast.makeText(this, e.getLocalizedMessage(), Toast.LENGTH_SHORT).show();
        //    return null;
        //}

        DataOutputStream dos = null;
        try {
            p = Runtime.getRuntime().exec("su");
        } catch (IOException e) {
            e.printStackTrace();
            return null;
        }

        dos = new DataOutputStream(p.getOutputStream());
        try {
            dos.writeBytes(cmd);
            dos.writeBytes("\n");
            dos.writeBytes("exit\n");
        } catch (IOException e) {
            e.printStackTrace();
        }

        if (toast) {
            Toast.makeText(this, cmd + " / " + p.toString(), Toast.LENGTH_SHORT).show();
            return null;
        }

        int w = -1;
        try {
            w = p.waitFor();
        } catch (InterruptedException e) {
            e.printStackTrace();
            Toast.makeText(this, e.getLocalizedMessage(), Toast.LENGTH_SHORT).show();
            return null;
        }

        Log.d(TAG, "exec(" + cmd + ") " + p.toString() + " exit[" + w + "]");
        InputStreamReader isr = new InputStreamReader(p.getInputStream());
        BufferedReader br = new BufferedReader(isr);
        return br;
    }

    /**
     * kill wfd command process
     */
    private void initFinishButton() {
        Button b = (Button) findViewById(R.id.button2);
        b.setOnClickListener(new OnClickListener() {
            // @Override
            public void onClick(View v) {
                exec_pswfd_killwfd();
            }

            // 1) ps (find wfd pid)
            // 2) kill wfd pid
            private void exec_pswfd_killwfd() {
                BufferedReader br = exec("ps wfd", false);
                if (br == null) {
                    Toast.makeText(MainActivity.this, "exec_lswfd_killwfd() failed-1.", Toast.LENGTH_SHORT).show();
                    return;
                }

                try {
                    int linec = 0;
                    String line = null;
                    while ((line = br.readLine()) != null) {
                        Log.d(TAG, String.format("[%02d]%s", linec++, line));
                        if (linec == 1) { // skip ps command output first line
                            continue;
                        }

                        String pid = getpid(line);
                        if (pid == null) {
                            continue;
                        }

                        exec("kill " + pid, true);
                    }
                } catch (IOException e) {
                    Toast.makeText(MainActivity.this, "exec_lswfd_killwfd() failed.", Toast.LENGTH_SHORT).show();
                    e.printStackTrace();
                }
            }

            // get wfd command PID in ps command output
            private String getpid(String line) {
                if (line == null) {
                    return null;
                }

                // [00]USER PID PPID VSIZE RSS WCHAN PC NAME
                // [01]root 17013 1 11444 3660 ffffffff 00000000 S wfd
                String words[] = line.split("\\s+");
                if (words == null || words.length < 2) {
                    return null;
                }

                return words[1];
            }
        });

    }

    /**
     * set IP address to TextEdit
     */
    private void setIPAddrToEdit(String ip) {
        AutoCompleteTextView actv = (AutoCompleteTextView) findViewById(R.id.autoCompleteTextView1);
        actv.setText(ip);
    }

    /**
     * get IP address String from TextEdit
     */
    private String getIP() {
        AutoCompleteTextView actv = (AutoCompleteTextView) findViewById(R.id.autoCompleteTextView1);
        return actv.getText().toString();
    }

    /**
     * get Port String from TextEdit
     */
    private String getPort() {
        EditText et = (EditText) findViewById(R.id.editText1);
        return et.getText().toString();
    }

    /**
     * get Port int from TextEdit
     */
    private int getIntPort() {
        String p = getPort();
        int port = -1;

        try {
            port = Integer.parseInt(p);
        } catch (NumberFormatException e) {
            e.printStackTrace();
            port = 19000;
        }

        Log.d(TAG, "getIntPort() return["+port+"]");
        return port;
    }

    /**
     * framework buttons
     */
    private void initFWButtons() {
        // f/w - wi-fi - source
        Button b = (Button) findViewById(R.id.button3);
        b.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                invokeSource();
            }
        });

        // f/w - wi-fi - source
        b = (Button) findViewById(R.id.button31);
        b.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                invokeSource_wfd(getIP(), getIntPort());
            }
        });

        // f/w - wi-fi - sink
        b = (Button) findViewById(R.id.button4);
        b.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                invokeSink(getIP(), getIntPort());
            }
        });

        // f/w - p2p - sink
        b = (Button) findViewById(R.id.button5);
        b.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                gotoP2pSink();
            }
        });
    }

    /** DisplayManager.connectWifiDisplay() */
    private DisplayManager mDisplayManager;

    /**
     * invoke Source for listen Sink
     */
    private void invokeSource() {
        if (mDisplayManager == null) {
            mDisplayManager = (DisplayManager)getSystemService(Context.DISPLAY_SERVICE);
        }

        if (mDisplayManager == null) {
            Log.e(TAG, "invokeSource() mDisplayManager is NULL!!");
            return;
        }

        String iface = getIP() + ":" + getPort();
        Log.d(TAG, "invokeSource() listen["+iface+"]");

        final boolean FWBUILD = true;
        if (FWBUILD) {
            mDisplayManager.connectWifiDisplay(iface); // @hide method
            Toast.makeText(this, "invokeSource() called connectWifiDisplay("+iface+")", Toast.LENGTH_SHORT).show();
        } else {
            setupSourceRef(iface);
        }
    }

    /**
     * invoke Source by Java Reflection
     */
    private boolean setupSourceRef(String iface) {
        // get class
        Class<?> clazz = mDisplayManager.getClass();

        // get method
        Method met;
        try {
            met = clazz.getMethod("connectWifiDisplay", String.class);
        } catch (NoSuchMethodException e) {
            e.printStackTrace();
            return false;
        }

        // invoke method
        Object ret;
        try {
            ret = met.invoke(mDisplayManager, iface);
        } catch (IllegalArgumentException e) {
            e.printStackTrace();
            return false;
        } catch (IllegalAccessException e) {
            e.printStackTrace();
            return false;
        } catch (InvocationTargetException e) {
            e.printStackTrace();
            return false;
        }

        Toast.makeText(this, "setupSourceRef("+iface+") [" + ret + "]", Toast.LENGTH_SHORT).show();

        Log.d(TAG, "setupSourceRef() ["+ret+"]");
        return true;
    }

    /**
     * invoke Source for listen Sink
     */
    private void invokeSource_wfd(String ip, int port) {
        Log.d(TAG, "invokeSource_wfd() Source Addr["+ip+":"+port+"]");
        new AvoidANRThread(true, ip, port).start();
        showStaticToast("invokeSource_wfd() called nativeInvokeSource("+ip+":"+port+")");
    }

    /**
     * invoke Sink wrapper
     */
    public static void invokeSink(String ip, int port) {
        Log.d(TAG, "invokeSink() Source Addr["+ip+":"+port+"]");
        new AvoidANRThread(false, ip, port).start();
        showStaticToast("invokeSink() called nativeInvokeSink("+ip+":"+port+")");
    }

    /**
     * static toast
     */
    private static void showStaticToast(final String msg) {
        mSelf.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(mSelf, msg, Toast.LENGTH_SHORT).show();
            }
        });
    }

    /**
     * Avoid ANR
     */
    private static class AvoidANRThread extends Thread {
        private final boolean source;
        private final String ip;
        private final int port;

        AvoidANRThread(boolean _source, String _ip, int _port) {
            source = _source;
            ip = _ip;
            port = _port;
        }

        public void run() {
            if (source) {
                nativeInvokeSource(ip, port);
            } else {
                nativeInvokeSink(ip, port);
            }
        }
    }

    /**
     * JNI:invoke Source
     */
    private static native void nativeInvokeSource(String ip, int port);

    /**
     * JNI:invoke Sink
     */
    private static native void nativeInvokeSink(String ip, int port);

    /**
     * invoke p2p Sink Activity
     */
    private void gotoP2pSink() {
        String pac = "com.example.mira4u";

        Intent i = new Intent();
        i.setClassName(pac, pac + ".P2pSinkActivity");
        startActivity(i);
    }

    /**
     * invoke Settings Activity
     */
    private void gotoSettings() {
        String pac = "com.example.mira4u";

        Intent i = new Intent();
        i.setClassName(pac, pac + ".SettingsActivity");
        startActivity(i);
    }

    /**
     * onCreateOptionsMenu
     */
    @Override
    public boolean onCreateOptionsMenu(Menu menu){
        menu.add(Menu.NONE, 0, Menu.NONE, getString(R.string.set_name));
        return true;
    }

    /**
     * onOptionsItemSelected
     */
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
        case 0:
            gotoSettings();
            return true;

        default:
            Log.w(TAG, "onOptionsItemSelected() Unknown Menu Id Passed["+item.getItemId()+"]");
            return true;
        }
    }
 
    static {
        System.loadLibrary("Mira4U");
    }

}
