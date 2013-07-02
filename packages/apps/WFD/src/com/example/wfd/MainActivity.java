package com.example.wfd;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.Enumeration;

import android.os.Bundle;
import android.app.Activity;
import android.content.Context;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.View.OnClickListener;
import android.view.View.OnKeyListener;
import android.view.View.OnTouchListener;
import android.view.inputmethod.InputMethodManager;
import android.widget.AutoCompleteTextView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.RatingBar;
import android.widget.RatingBar.OnRatingBarChangeListener;
import android.widget.Toast;

public class MainActivity extends Activity {

    /** log tag */
    private static final String TAG = "WFD_App";

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
        initRadio();
        setIPAddressToRatingBar();
        initEditTexts();
        initCmdButton();
        initFinishButton();
    }

    /**
     * Source or Sink
     */
    private enum MiraKind {
        Source, Sink
    }

    /**
     * Source or Sink
     */
    private MiraKind mMiraKind = MiraKind.Source;

    /**
     * Select Source or Sink by radio button
     */
    private void initRadio() {
        RadioGroup radioGroup = (RadioGroup) findViewById(R.id.radioGroup);
        radioGroup.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                switch (checkedId) {
                case R.id.radioButton1:
                    mMiraKind = MiraKind.Source;
                    break;
                case R.id.radioButton2:
                    mMiraKind = MiraKind.Sink;
                    break;
                }

                changeCmdButton();
            }
        });
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
        findViewById(R.id.radioButton1).setEnabled(false);
        findViewById(R.id.radioButton2).setEnabled(false);
        findViewById(R.id.ratingBar1).setEnabled(false);
        findViewById(R.id.autoCompleteTextView1).setEnabled(false);
        findViewById(R.id.editText1).setEnabled(false);
        findViewById(R.id.button1).setEnabled(false);
        findViewById(R.id.button2).setEnabled(false);
    }

    /**
     * notify on change IP addres textbox
     */
    private void initEditTexts() {
        AutoCompleteTextView actv = (AutoCompleteTextView) findViewById(R.id.autoCompleteTextView1);
        actv.setOnKeyListener(new OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                // pushed enter key
                if (event.getAction() == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_ENTER) {
                    // hide keyboard
                    InputMethodManager inputMethodManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                    inputMethodManager.hideSoftInputFromWindow(v.getWindowToken(), 0);

                    changeCmdButton();
                    return true;
                }
                return false;
            }
        });

        EditText et = (EditText)findViewById(R.id.editText1);
        et.setOnKeyListener(new OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                // pushed enter key
                if (event.getAction() == KeyEvent.ACTION_DOWN && keyCode == KeyEvent.KEYCODE_ENTER) {
                    // hide keyboard
                    InputMethodManager inputMethodManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                    inputMethodManager.hideSoftInputFromWindow(v.getWindowToken(), 0);

                    changeCmdButton();
                    return true;
                }
                return false;
            }
        });
    }

    /**
     * command button
     */
    private void initCmdButton() {
        Button b = (Button) findViewById(R.id.button1);
        b.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View v) {
                execWfd();
            }
        });
    }

    /**
     * execute wfd command
     */
    private void execWfd() {
        exec(getCmd(), true);
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
    @SuppressWarnings("unused")
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
        changeCmdButton();
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
     * change command button label<br />
     */
    private void changeCmdButton() {
        String cmd = "wfd";

        // Source : listen
        // Sink : connect
        String para = mMiraKind == MiraKind.Source ? "-l" : "-c";

        String ip = getIP();
        String port = getPort();

        cmd = cmd + " " + para + " " + ip + ":" + port;

        Button b = (Button) findViewById(R.id.button1);
        b.setText(cmd);
    }

    /**
     * get command String from TextEdit
     */
    private String getCmd() {
        Button b = (Button) findViewById(R.id.button1);
        return b.getText().toString();
    }

}