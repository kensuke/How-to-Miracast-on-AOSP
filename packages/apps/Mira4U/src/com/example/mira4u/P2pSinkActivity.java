package com.example.mira4u;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Timer;
import java.util.TimerTask;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pDeviceList;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.net.wifi.p2p.WifiP2pManager.Channel;
import android.net.wifi.p2p.WifiP2pManager.ChannelListener;
import android.net.wifi.p2p.WifiP2pManager.ConnectionInfoListener;
import android.net.wifi.p2p.WifiP2pManager.GroupInfoListener;
import android.net.wifi.p2p.WifiP2pManager.PeerListListener;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.provider.Settings;
import android.text.Editable;
import android.text.Html;
import android.text.TextWatcher;
import android.util.Log;
import android.view.View;
import android.widget.AdapterView;
import android.widget.AdapterView.OnItemSelectedListener;
import android.widget.ArrayAdapter;
import android.widget.RadioGroup;
import android.widget.RadioGroup.OnCheckedChangeListener;
import android.widget.CompoundButton;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.ToggleButton;

/**
 * P2p Sink daemon<br />
 * base src: https://github.com/kensuke/WiFiDirectTestApp
 */
public class P2pSinkActivity extends Activity {

    /** ログ出力用TAG */
    private final String TAG = "P2pSinkActivity";

    /** ログ */
    private TextView mTextView_Log;
    /** 改行 */
    private final String LINE_SEPARATOR = System.getProperty("line.separator");
    private final String LINE_SEPARATOR_HTML = "<br />";
    /** ログをHTML（色付き文字列）で出力
     * TODO 前回実行時の値をPrefへ保存/読込
     */
    private boolean HTML_OUT = true;

    /** BroadcastReceiver */
    private BroadcastReceiver mReceiver;

    /** Wi-Fi Direct 有効/無効状態 */
    private boolean mIsWiFiDirectEnabled;

    /** WifiP2pManager */
    private WifiP2pManager mWifiP2pManager;
    /** Channel */
    private Channel mChannel;
    /** peers */
    private List<WifiP2pDevice> mPeers = new ArrayList<WifiP2pDevice>();
    /** リスナアダプタ */
    private ActionListenerAdapter mActionListenerAdapter;

    /** 子リスト保持 */
    private Spinner mPeersSpinner;
    /** 選択中の子 */
    private String mSelectedDevice;

    /** for delayed execute */
    private Handler mHandler;

    /* ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * Activity API
     */

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_p2p);

        String title = "ANDROID_ID[" + getAndroid_ID() + "]";
        title += "   MAC[" + getMACAddress() + "]";
        //setTitle(title);

        initializeLog();
        initBroadcastToggle();

        addLog(title);
        addLog("onCreate()");

        if (!hasP2P()) {
            toastAndLog("onCreate()", "This Device Has Not P2P Feature!!");
        }
    }

    private boolean mIsAppBoot;

    @Override
    protected void onResume() {
        super.onResume();
        addLog("onResume()");

        // ブロードキャストレシーバで、WIFI_P2P_STATE_CHANGED_ACTIONのコールバックを持ってWi-Fi Direct ON/OFFを判定する
        mIsWiFiDirectEnabled = false;

        mIsAppBoot = true;

        // たぶんこのタイミングでブロードキャストレシーバを登録するのがbetter
        registerBroadcastReceiver();

        mHandler = new Handler();
    }

    @Override
    protected void onPause() {
        super.onPause();
        addLog("onPause()");

        // ブロードキャストレシーバ解除
        unRegisterBroadcastReceiver();
    }

    /**
     * ブロードキャストレシーバ登録
     */
    private void registerBroadcastReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION);
        mReceiver = new WiFiDirectBroadcastReceiver();
        registerReceiver(mReceiver, filter);
        addLog("registerBroadcastReceiver() BroadcastReceiver");
    }

    /**
     * ブロードキャストレシーバ解除
     */
    private void unRegisterBroadcastReceiver() {
        if (mReceiver != null) {
            unregisterReceiver(mReceiver);
            mReceiver = null;
            addLog("unRegisterBroadcastReceiver() BroadcastReceiver");
        }
    }

    /**
     * ブロードキャストレシーバ トグルボタン初期化 入り口
     */
    private void initBroadcastToggle() {
        initBroadcastToggleInner(R.id.toggle_bc_all);
    }

    /**
     * ブロードキャストレシーバ トグルボタン初期化
     */
    private void initBroadcastToggleInner(final int rId_Toggle) {
        // トグルボタンのON/OFF変更を検知
        ToggleButton tb = (ToggleButton)findViewById(rId_Toggle);
        tb.setOnCheckedChangeListener( new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (isChecked) {
                    registerBroadcastReceiver();
                } else {
                    unRegisterBroadcastReceiver();
                }
            }
        });
    }

    /* ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * ログ出力
     * TODO ログ関連をごそっとクラス化したほうがおしゃれだと思う
     */

    /**
     * ログ関係初期化
     */
    private void initializeLog() {
        if (mTextView_Log != null) {
            return;
        }

        mTextView_Log = (TextView)findViewById(R.id.textView_log);

        // テキスト変更(=ログ出力追加)を検知
        mTextView_Log.addTextChangedListener( new TextWatcher() {
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                // オートスクロール可否チェック
                ToggleButton tb = (ToggleButton)findViewById(R.id.toggle_autoscroll);
                if (!tb.isChecked()) {
                    return;
                }

                // テキスト変更時にログウィンドウを末尾へ自動スクロール
                final ScrollView sv = (ScrollView)findViewById(R.id.scrollview_log);
                sv.post( new Runnable() {
                    public void run() {
                        sv.fullScroll(View.FOCUS_DOWN);
                    }
                });
            }

            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            public void afterTextChanged(Editable s) {}
        });

        // ログ種別(色付き(HTML)、モノクロ)変更検知
        RadioGroup rg = (RadioGroup)findViewById(R.id.radiogroup_logkind);
        rg.setOnCheckedChangeListener( new OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                switch (checkedId) {
                case R.id.radiobutton_html: // 色付き
                    HTML_OUT = true;
                    break;
                case R.id.radiobutton_mono: // モノクロ
                    HTML_OUT = false;
                    break;
                default:
                    addLog("initializeLog() Unknown Log Kind["+checkedId+"]");
                    HTML_OUT = false;
                    break;
                }
            }
        });
    }

    /**
     * ログ出力
     */
    private void addLog(String log) {
        Log.d(TAG, log);

        log = log + nl();
        if (mTextView_Log == null) {
            initializeLog();
        }
        mTextView_Log.append( HTML_OUT ? convHtmlStr2CS(log) : log );
    }

    /**
     * HTMl文字列変換
     */
    private CharSequence convHtmlStr2CS(String htmlStr) {
        return Html.fromHtml(htmlStr);
    }

    /**
     * ログ出力書式に応じた改行コード取得
     */
    private String nl() {
        return HTML_OUT ? LINE_SEPARATOR_HTML : LINE_SEPARATOR;
    }

    /**
     * ”P2Pメソッドのログ出力”用メソッド
     */
    private void addMethodLog(String method) {
        if (HTML_OUT) method = "<font color=lime>"+method+"</font>";
        addLog(nl() + method);
    }

    /**
     * トーストあんどログ
     */
    private void toastAndLog(String msg1, String msg2) {
        String log = msg1 + LINE_SEPARATOR + msg2;
        Toast.makeText(this, log, Toast.LENGTH_SHORT).show();

        if (HTML_OUT) log = "<font color=red>" + msg1 + nl() + msg2 + "</font>";
        addLog(log);
    }

    /**
     * ログリセット
     */
    public void onClickResetLog(View view) {
        mTextView_Log.setText("");
    }

    /**
     * ログ保存
     * TODO ログをSDカードへ保存
     */
    public void onClickSaveLog(View view) {
       String log = mTextView_Log.getText().toString();
       Log.d(TAG, "onClickSaveLog() LOG["+log+"]");
    }

    /**
     * デバイス文字列(ログ出力用)
     */
    private String toStringDevice(WifiP2pDevice device) {
        String log = separateCSV(device.toString()) + nl() + "　" + getDeviceStatus(device.status);
        return HTML_OUT ? "<font color=yellow>"+log+"</font>" : log;
    }

    // ":"区切り文字列へ改行付与
    // " Device: Galaxy_Nexus"+
    // " primary type: 12345-xyz"
    // ↓
    // " Device: Galaxy_Nexus<br />"+
    // " primary type: 12345-xyz<br />"
    private String separateCSV(String csvStr) {
        //return csvStr;
        return csvStr.replaceAll("[^:yWFD] ", nl()+"　"); // ": "、"y " でない半角スペース（＝文頭の半角スペース）にマッチする
        // 以下の”意図しない場所での改行"を除外する
        //"deviceAddress: AB:CD"を"deviceAddress:<br />AB:CD"としない
        //"primary type:"を"primary<br />type:"としない
        //"WFD CtrlPort: 554"を”WFD<br />CtrlPort: 554”としない <= Android 4.2以降のMiracastに対応
    }

    // TODO FIXME 上記正規表現ではWFD情報がうまいこと出力されないバグあり
    //sbuf.append("Device: ").append(deviceName);
    //sbuf.append("\n deviceAddress: ").append(deviceAddress);
    //sbuf.append("\n primary type: ").append(primaryDeviceType);
    //sbuf.append("\n secondary type: ").append(secondaryDeviceType);
    //sbuf.append("\n wps: ").append(wpsConfigMethodsSupported);
    //sbuf.append("\n grpcapab: ").append(groupCapability);
    //sbuf.append("\n devcapab: ").append(deviceCapability);
    //sbuf.append("\n status: ").append(status);
    //sbuf.append("\n wfdInfo: ").append(wfdInfo);
    // wfdInfo: WFD enabled: trueWFD DeviceInfo: 349
    // WFD CtrlPort: 554
    // WFD MaxThroughput: 50

    // デバイス状態文字列変換
    private String getDeviceStatus(int deviceStatus) {
        String status = "";
        switch (deviceStatus) {
            case WifiP2pDevice.AVAILABLE:
                status = "Available";
                break;
            case WifiP2pDevice.INVITED:
                status = "Invited";
                break;
            case WifiP2pDevice.CONNECTED:
                status = "Connected";
                break;
            case WifiP2pDevice.FAILED:
                status = "Failed";
                break;
            case WifiP2pDevice.UNAVAILABLE:
                status = "Unavailable";
                break;
            default:
                status = "Unknown";
                break;
        }
        return HTML_OUT ? "[<b><i><u>"+status+"</u></i></b>]" : "["+status+"]";
    }

    /* ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * P2P API
     */

    /**
     * リスナアダプタ
     * WifiP2pManagerクラスの各メソッドは、WifiP2pManager.ActionListenerによって、メソッドの実行結果を知ることができる
     * ただし、successと出たのに失敗したり、failureと出たのに成功したりする
     */
    class ActionListenerAdapter implements WifiP2pManager.ActionListener {

        // 成功
        public void onSuccess() {
            String log = " onSuccess()";
            if (HTML_OUT) log = "<font color=aqua>　"+log+"</font>";
            addLog(log);
        }

        // 失敗
        public void onFailure(int reason) {
            String log = " onFailure("+getReason(reason)+")";
            if (HTML_OUT) log = "<font color=red>　"+log+"</font>";
            addLog(log);
        }

        // 失敗理由intコード -> 文字列変換
        private String getReason(int reason) {
            String[] strs = {"ERROR", "P2P_UNSUPPORTED", "BUSY"};
            try {
                return strs[reason] + "("+reason+")";
            } catch (ArrayIndexOutOfBoundsException e) {
                return "UNKNOWN REASON CODE("+reason+")";
            }
        }
    }

    /**
     * P2Pメソッド実行前のNULLチェック
     */
    private boolean isNull(boolean both) {
        if (mActionListenerAdapter == null) {
            mActionListenerAdapter = new ActionListenerAdapter();
        }

        if (!mIsWiFiDirectEnabled) {
            toastAndLog(" Wi-Fi Direct is OFF!", "try Setting Menu");
            return true;
        }

        if (mWifiP2pManager == null) {
            toastAndLog(" mWifiP2pManager is NULL!", " try getSystemService");
            return true;
        }
        if (both && (mChannel == null) ) {
            toastAndLog(" mChannel is NULL!", " try initialize");
            return true;
        }

        return false;
    }

    /**
     * インスタンス取得
     */
    public void onClickGetSystemService(View view) {
        addMethodLog("getSystemService(Context.WIFI_P2P_SERVICE)");

        mWifiP2pManager = (WifiP2pManager) getSystemService(Context.WIFI_P2P_SERVICE);

        addLog("　Result["+(mWifiP2pManager != null)+"]");
    }

    /**
     * 初期化
     */
    public void onClickInitialize(View view) {
        addMethodLog("mWifiP2pManager.initialize()");
        if (isNull(false)) { return; }

        mChannel = mWifiP2pManager.initialize(this, getMainLooper(), new ChannelListener() {
            public void onChannelDisconnected() {
                addLog("mWifiP2pManager.initialize() -> onChannelDisconnected()");
            }
        });

        addLog("　Result["+(mChannel != null)+"]");
    }

    /**
     * デバイス発見
     */
    public void onClickDiscoverPeers(View view) {
        addMethodLog("mWifiP2pManager.discoverPeers()");
        if (isNull(true)) { return; }

        mWifiP2pManager.discoverPeers(mChannel, mActionListenerAdapter);
    }

    /**
     * 接続キャンセル
     */
    public void onClickCancelConnect(View view) {
        addMethodLog("mWifiP2pManager.cancelConnect()");
        if (isNull(true)) { return; }

        mWifiP2pManager.cancelConnect(mChannel, mActionListenerAdapter);
    }

    /**
     * グループ作成
     */
    public void onClickCreateGroup(View view) {
        addMethodLog("mWifiP2pManager.createGroup()");
        if (isNull(true)) { return; }

        mWifiP2pManager.createGroup(mChannel, mActionListenerAdapter);
    }

    /**
     * グループ削除
     */
    public void onClickRemoveGroup(View view) {
        addMethodLog("mWifiP2pManager.removeGroup()");
        if (isNull(true)) { return; }

        mWifiP2pManager.removeGroup(mChannel, mActionListenerAdapter);
    }

    /**
     * 接続情報要求
     */
    public void onClickRequestConnectionInfo(View view) {
        addMethodLog("mWifiP2pManager.requestConnectionInfo()");
        if (isNull(true)) { return; }

        mWifiP2pManager.requestConnectionInfo(mChannel, new ConnectionInfoListener() {
            // requestConnectionInfo()実行後、非同期応答あり
            public void onConnectionInfoAvailable(WifiP2pInfo info) {
                addLog("　onConnectionInfoAvailable():");
                if (info == null) {
                    addLog("  info is NULL!");
                    return;
                }
                addLog("  groupFormed:" + info.groupFormed);
                addLog("  isGroupOwner:" + info.isGroupOwner);
                addLog("  groupOwnerAddress:" + info.groupOwnerAddress);
            }
        });
    }

    /**
     * グループ情報要求
     */
    public void onClickRequestGroupInfo(View view) {
        addMethodLog("mWifiP2pManager.requestGroupInfo()");
        if (isNull(true)) { return; }

        mWifiP2pManager.requestGroupInfo(mChannel, new GroupInfoListener() {
            // requestGroupInfo()実行後、非同期応答あり
            public void onGroupInfoAvailable(WifiP2pGroup group) {
                addLog("　onGroupInfoAvailable():");
                if (group == null) {
                    addLog("  group is NULL!");
                    return;
                }

                String log = separateCSV(group.toString());

                // パスワードは、G.O.のみ取得可能
                String pass = nl() + "　password: ";
                if (group.isGroupOwner()) {
                    pass += group.getPassphrase();
                } else {
                    pass += "Client Couldn't Get Password";
                }
                if (HTML_OUT) pass = "<font color=red><b>"+pass+"</b></font>"; // たぶんfont colorのネストはできない(パスワードが赤にならない)
                log += pass;
                if (HTML_OUT) log = "<font color=#fffacd>"+log+"</font>"; // color=lemonchiffon
                addLog(log);
            }
        });
    }

    /**
     * ピアリスト要求
     */
    public void onClickRequestPeers(View view) {
        addMethodLog("mWifiP2pManager.requestPeers()");
        if (isNull(true)) { return; }

        mWifiP2pManager.requestPeers(mChannel, new PeerListListener() {
            // requestPeers()実行後、非同期応答あり
            public void onPeersAvailable(WifiP2pDeviceList peers) {
                mPeers.clear();
                mPeers.addAll(peers.getDeviceList());
                int cnt = mPeers.size();
                addLog("　onPeersAvailable() : num of peers["+cnt+"]");
                for (int i = 0; i < cnt; i++) {
                    addLog(nl() + " ***********["+i+"]***********");
                    addLog("  " + toStringDevice(mPeers.get(i)));
                }

                updatePeersSpinner();
            }
        });
    }

    /**
     * 子リストUI処理
     */
    private void updatePeersSpinner() {
        // スピナーインスタンス取得、アイテム選択イベントハンドラ設定
        if (mPeersSpinner == null) {
            mPeersSpinner = (Spinner)findViewById(R.id.spinner_peers);
            mPeersSpinner.setOnItemSelectedListener( new OnItemSelectedListener() {
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    mSelectedDevice = mPeers.get(position).deviceName;
                    addLog(nl() + "Selected Peer["+mSelectedDevice+"]");
                }

                public void onNothingSelected(AdapterView<?> arg0) {}
            });
        }

        // 子リストからデバイス名の配列を生成し、スピナーのアイテムとして設定
        int cnt = mPeers.size();
        String[] peers = new String[cnt];
        for (int i = 0; i < cnt; i++) {
            peers[i] = mPeers.get(i).deviceName;
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, peers);
        mPeersSpinner.setAdapter(adapter);
    }

    /** ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * ブロードキャストレシーバ
     */
    public class WiFiDirectBroadcastReceiver extends BroadcastReceiver {

        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            String log = "onReceive() ["+action+"]";
            if (HTML_OUT) log = "<font color=fuchsia>"+log+"</font>";
            addLog(nl() + log);

            if (WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION.equals(action)) {
                mIsWiFiDirectEnabled = false;
                int state = intent.getIntExtra(WifiP2pManager.EXTRA_WIFI_STATE, -1);
                String sttStr;
                switch (state) {
                case WifiP2pManager.WIFI_P2P_STATE_ENABLED:
                    mIsWiFiDirectEnabled = true;
                    sttStr = "ENABLED";
                    break;
                case WifiP2pManager.WIFI_P2P_STATE_DISABLED:
                    sttStr = "DISABLED";
                    break;
                default:
                    sttStr = "UNKNOWN";
                    break;
                }
                addLog("state["+sttStr+"]("+state+")");
                changeBackgroundColor();

                // 初期化
                if (mIsWiFiDirectEnabled) {
                    onClickGetSystemService(null);
                    onClickInitialize(null);
                }
            } else if (WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION.equals(action)) {
                // このタイミングでrequestPeers()を呼び出すと、peerの変化(ステータス変更とか)がわかる
                // 本テストアプリは、メソッド単位での実行をテストしたいので、ここではrequestPeers()を実行しない
                addLog("try requestPeers()");
            } else if (WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION.equals(action)) {
                NetworkInfo networkInfo = (NetworkInfo) intent.getParcelableExtra(WifiP2pManager.EXTRA_NETWORK_INFO);
                // networkInfo.toString()はCSV文字列(1行)を返す。そのままでは読みにくいので、カンマを改行へ変換する。
                String nlog = networkInfo.toString().replaceAll(",", nl()+"　");
                if (HTML_OUT) nlog = "<font color=#f0e68c>"+nlog+"</font>"; // khaki
                addLog(nlog);

                // invoke Sink
                if (networkInfo.isConnected()) {
                    mIsAppBoot = false;
                    invokeSink();
                } else if (!mIsAppBoot) {
                    //finish();
                    System.exit(0); // force finish Sink Screen. TODO FIXME^^;;
                }
            } else if (WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION.equals(action)) {
                WifiP2pDevice device = (WifiP2pDevice) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_DEVICE);
                addLog(toStringDevice(device));

                // Search
                if (mIsWiFiDirectEnabled) {
                    onClickDiscoverPeers(null);
                }
            }
        }
    }

    /**
     * APIボタンエリアの背景色をWi-Fi Direct有効/無効フラグで色分けする
     * 有効 青
     * 無効 赤
     */
    private void changeBackgroundColor() {
        ScrollView sc = (ScrollView)findViewById(R.id.layout_apibuttons);
        sc.setBackgroundColor( mIsWiFiDirectEnabled ? Color.BLUE : Color.RED );
    }

    /* ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * 割りとどうでもいいメソッド
     */

    /**
     * ANDROID_ID取得
     */
    private String getAndroid_ID() {
        return Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
    }

    /**
     * Wi-Fi MACアドレス取得
     */
    private String getMACAddress() {
        WifiManager manager = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        WifiInfo wifiInfo = manager.getConnectionInfo();
        String mac = wifiInfo.getMacAddress();
        return mac;
    }

    /**
     * 端末はp2p featureを持っているか？
     */
    private boolean hasP2P() {
        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT);
    }

    /**
     * Wi-Fi Direct 設定画面表示
     */
    public void onClickGotoWiFiSetting(View view) {
        String pac = "com.android.settings";
        Intent i = new Intent();

        // まずはこの画面を出してみる(Galaxy Nexus 4.0はこれだったと思う)
        i.setClassName(pac, pac + ".wifi.p2p.WifiP2pSettings");
        try {
            startActivity(i);
        } catch (ActivityNotFoundException e) {
            Log.e(TAG, "onClickGotoWiFiSetting() " + e.getMessage());
            // 17 4.2 JELLY_BEAN_MR1 
            // 16 4.1, 4.1.1 JELLY_BEAN
            // 15 4.0.3, 4.0.4 ICE_CREAM_SANDWICH_MR1
            // 14 4.0, 4.0.1, 4.0.2 ICE_CREAM_SANDWICH
            if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.ICE_CREAM_SANDWICH+1) { // 14, 15 = ICS
                startActivity(new Intent(Settings.ACTION_WIRELESS_SETTINGS)); // ICSの場合は、たぶん、"Wi-Fi→その他"にWi-Fi DirectのON/OFFがあると思う
            } else {
                i.setClassName(pac, pac + ".wifi.WifiSettings"); // その他(JB)の場合はとりあえずWi-Fi設定画面を出しておく^^;
                // TODO 4.1以降は、startActivity()ではなく、startPreferencePanel()なのかも
                //if (getActivity() instanceof PreferenceActivity) {
                //    ((PreferenceActivity) getActivity()).startPreferencePanel(
                //         WifiP2pSettings.class.getCanonicalName(), null, R.string.wifi_p2p_settings_title, null, this, 0);
                try {
                    startActivity(i);
                    Toast.makeText(this, "TRY menu -> Wi-Fi Direct", Toast.LENGTH_LONG).show();
                } catch (ActivityNotFoundException e2) {
                    Log.e(TAG, "onClickGotoWiFiSetting() " + e2.getMessage());
                }
            }
        }
    }

    /* ★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★★
     * Miracast Sink API
     */
 
   /**
     * invoke Sink to Source IP Address
     */
    private void invokeSink() {
        addMethodLog("invokeSink() call requestGroupInfo()");
        if (isNull(true)) { return; }

        mWifiP2pManager.requestGroupInfo(mChannel, new GroupInfoListener() {
            // requestGroupInfo()実行後、非同期応答あり
            public void onGroupInfoAvailable(WifiP2pGroup group) {
                addLog("　onGroupInfoAvailable():");
                if (group == null) {
                    addLog("  group is NULL!");
                    return;
                }

                String log = separateCSV(group.toString());

                // パスワードは、G.O.のみ取得可能
                String pass = nl() + "　password: ";
                if (group.isGroupOwner()) {
                    pass += group.getPassphrase();
                } else {
                    pass += "Client Couldn't Get Password";
                }
                if (HTML_OUT) pass = "<font color=red><b>"+pass+"</b></font>"; // たぶんfont colorのネストはできない(パスワードが赤にならない)
                log += pass;
                if (HTML_OUT) log = "<font color=#fffacd>"+log+"</font>"; // color=lemonchiffon
                addLog(log);

                mP2pControlPort = -1;
                // Miracast device filtering
                Collection<WifiP2pDevice> p2pdevs = group.getClientList();
                //AssertEqual(p2pdevs.size(), 1); // one device?
                for (WifiP2pDevice dev : p2pdevs) {
                    boolean b = isWifiDisplaySource(dev);
                    addLog("invokeSink() isWifiDisplaySource("+dev.deviceName+")=["+b+"]");
                    if (!b) {
                        continue;
                        // return; // because not Miracast Source device
                    }
                }
                if (mP2pControlPort == -1) {
                    //final class WifiDisplayController implements DumpUtils.Dump {
                    //    private static final int DEFAULT_CONTROL_PORT = 7236;
                    mP2pControlPort = 7236;
                    addLog("invokeSink() port=-1?? p2pdevs.size()=["+p2pdevs.size()+"] port assigned=7236");
                }

                // connect
                if (group.isGroupOwner()) { // G.O. don't know client IP, so check /proc/net/arp
                    mP2pInterfaceName = group.getInterface();

                    mArpTableObservationTimer = new Timer();
                    ArpTableObservationTask task = new ArpTableObservationTask();
                    mArpTableObservationTimer.scheduleAtFixedRate(task, 10, 1*1000); // 10ms後から1秒間隔でarpファイルをチェック
                } else { // this device is not G.O. get G.O. address
                    invokeSink2nd();
                }
            }
        });
    }

    /**
     * Miracast device filtering
     */
    private boolean isWifiDisplaySource(WifiP2pDevice dev) {
        if (dev == null || dev.wfdInfo == null) {
            return false;
        }
        WifiP2pWfdInfo wfd = dev.wfdInfo;
        if (!wfd.isWfdEnabled()) {
            return false;
        }

        int type = wfd.getDeviceType();
        mP2pControlPort = wfd.getControlPort();

        boolean source = (type == WifiP2pWfdInfo.WFD_SOURCE) || (type == WifiP2pWfdInfo.SOURCE_OR_PRIMARY_SINK);
        addLog("isWifiDisplaySource() type["+type+"] is-source["+source+"] port["+mP2pControlPort+"]");
        return source;
    }

    /**
     * invoke Sink
     */
    private void invokeSink2nd() {
        addMethodLog("invokeSink2nd() requestConnectionInfo()");
        if (isNull(true)) { return; }

        mWifiP2pManager.requestConnectionInfo(mChannel, new ConnectionInfoListener() {
            // requestConnectionInfo()実行後、非同期応答あり
            public void onConnectionInfoAvailable(WifiP2pInfo info) {
                addLog("　onConnectionInfoAvailable():");
                if (info == null) {
                    addLog("  info is NULL!");
                    return;
                }

                addLog("  groupFormed:" + info.groupFormed);
                addLog("  isGroupOwner:" + info.isGroupOwner);
                addLog("  groupOwnerAddress:" + info.groupOwnerAddress);

                if (!info.groupFormed) {
                    addLog("  not yet groupFormed!");
                    return;
                }

                if (info.isGroupOwner) {
                    addLog("  I'm G.O.? Illegal State!!");
                    return;
                } else {
                    String source_ip = info.groupOwnerAddress.getHostAddress();
                    delayedInvokeSink(source_ip, mP2pControlPort, 3);
                }
            }
        });
    }

    /** Wi-Fi Direct接続後、接続確立待ちタイマ */
    private Timer mArpTableObservationTimer;
    /** arpファイル読み込みリトライ回数 */
    private int mArpRetryCount = 0;
    /** arpファイル読み込み上限回数 */
    private final int MAX_ARP_RETRY_COUNT = 60;

    /** p2p Control Port */
    private int mP2pControlPort = -1;
    /** p2p interface name */
    private String mP2pInterfaceName;

    /**
     * /proc/net/arp テーブル監視タスク
     */
    class ArpTableObservationTask extends TimerTask {
        @Override
        public void run() {
            // arpテーブル読み込み
            RarpImpl rarp = new RarpImpl();
            String source_ip = rarp.execRarp(mP2pInterfaceName);

            // リトライ
            if (source_ip == null) {
                Log.d(TAG, "retry:" + mArpRetryCount);
                if (++mArpRetryCount > MAX_ARP_RETRY_COUNT) {
                    mArpTableObservationTimer.cancel();
                    return;
                }
                return;
            }

            mArpTableObservationTimer.cancel();
            delayedInvokeSink(source_ip, mP2pControlPort, 3);
        }
    }

    private void delayedInvokeSink(final String ip, final int port, int delaySec) {
        mHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                MainActivity.invokeSink(ip, port);
            }
        }, delaySec*1000);
    }

}
