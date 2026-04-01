package kr.co.iefriends.pcsx2.activities;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.GameManager;
import android.app.GameState;
import android.content.ClipData;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.FrameLayout;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.DrawableRes;
import androidx.annotation.IdRes;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.StringRes;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.PopupMenu;
import androidx.core.content.ContextCompat;
import androidx.core.util.Pair;
import androidx.core.view.GravityCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.core.view.ViewCompat;
import androidx.drawerlayout.widget.DrawerLayout;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.floatingactionbutton.FloatingActionButton;
import com.google.android.material.navigation.NavigationView;
import com.google.android.material.slider.Slider;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.Inflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import kr.co.iefriends.pcsx2.BuildConfig;
import kr.co.iefriends.pcsx2.NativeApp;
import kr.co.iefriends.pcsx2.R;
import kr.co.iefriends.pcsx2.hid.HIDDeviceManager;
import kr.co.iefriends.pcsx2.input.ControllerMappingDialog;
import kr.co.iefriends.pcsx2.input.ControllerMappingManager;
import kr.co.iefriends.pcsx2.input.view.DPadView;
import kr.co.iefriends.pcsx2.input.view.JoystickView;
import kr.co.iefriends.pcsx2.input.view.PSButtonView;
import kr.co.iefriends.pcsx2.input.view.PSShoulderButtonView;
import kr.co.iefriends.pcsx2.utils.DataDirectoryManager;
import kr.co.iefriends.pcsx2.utils.DebugLog;
import kr.co.iefriends.pcsx2.utils.DeviceProfiles;
import kr.co.iefriends.pcsx2.utils.DiscordBridge;
import kr.co.iefriends.pcsx2.utils.GameSpecificSettingsManager;
import kr.co.iefriends.pcsx2.utils.LogcatRecorder;
import kr.co.iefriends.pcsx2.utils.RetroAchievementsBridge;
import kr.co.iefriends.pcsx2.utils.SDLControllerManager;
import kr.co.iefriends.pcsx2.utils.SDLSurface;
import kr.co.iefriends.pcsx2.utils.NetworkAdapterCollector;

public class MainActivity extends AppCompatActivity {
    private String m_szGamefile = "";

    private HIDDeviceManager mHIDDeviceManager;
    private Thread mEmulationThread = null;

    // UI groups for on-screen controls
    View llPadSelectStart;
    View llPadRight;
    private DrawerLayout inGameDrawer;
    private FloatingActionButton drawerToggle;
    private FloatingActionButton drawerPauseButton;
    private FloatingActionButton drawerFastForwardButton;
    BiosManager mBiosManager;
    RetroAchievementsManager mRetroAchievementsManager;
    boolean isVmPaused = false;
    private final Runnable hideDrawerToggleRunnable = () -> hideDrawerToggle();
    private boolean isFastForwardEnabled = false;


    // Home UI
    private DrawerLayout drawerLayout;
    private View homeContainer;
    private View emptyContainer;
    private android.widget.EditText etSearch;
    private android.widget.ImageView bgImage;
    private RecyclerView rvGames;
    private GridLayoutManager gamesGridLayoutManager;
    private SpacingDecoration gameSpacingDecoration;
    private TextView tvEmpty;
    private GamesAdapter gamesAdapter;
    private boolean listMode = false;
    Uri gamesFolderUri;
    CoverManager mCoverManager;
    static final String PREFS = "armsx2";
    static final String PREF_GAMES_URI = "games_folder_uri";
    // Preflight
    private Uri pendingGameUri = null;
    private int pendingLaunchRetries = 0;
    float faceButtonsBaseScale = 1.0f;

    PerGameSettingsManager mPerGameSettingsManager;
    ContentImportHelper mContentImportHelper;
    ControllerManager mControllerManager;
    ChdConversionManager mChdConversionManager;
    DataDirectorySetupManager mDataDirectorySetupManager;
    DialogHelper mDialogHelper;
    DrawerSettingsManager mDrawerSettingsManager;
    OnScreenUiStyleManager mOnScreenUiStyleManager;

    // Auto-hide state
    private enum InputSource { TOUCH, CONTROLLER }
    private InputSource lastInput = InputSource.TOUCH;
    private long lastTouchTimeMs = 0L;
    private long lastControllerTimeMs = 0L;
    // 0 = never hide; seconds otherwise
    private long hideDelayMs = 2500L;
    private static final String PREF_HIDE_CONTROLS_SECONDS = "onscreen_timeout_seconds";

    private boolean disableTouchControls;

    public static final String EXTRA_SETTINGS_LAYOUT_CHANGED = "SET_LAYOUT_CHANGED";
    public static final String EXTRA_SETTINGS_GPU_PROFILE_OVERRIDE = "SET_GPU_PROFILE_OVERRIDE";
    public static final String EXTRA_SETTINGS_GPU_PROFILE_PERSISTED = "SET_GPU_PROFILE_PERSISTED";
    
    int currentControllerMode = 0; // 0=2 Sticks, 1=1 Stick+Face, 2=D-Pad Only

    private final OnBackPressedCallback onBackPressCallback =
        new OnBackPressedCallback(false) {
            @Override
            public void handleOnBackPressed() {
                shutdownVmToHome();
            }
        };
    private final OnBackPressedCallback onSearchBackPressCallback =
        new OnBackPressedCallback(true) {
            @Override
            public void handleOnBackPressed() {
                toggleSearchBar();
                remove();
            }
        };

    private final RetroAchievementsBridge.Listener retroAchievementsListener = new RetroAchievementsBridge.Listener() {
        @Override
        public void onStateUpdated(RetroAchievementsBridge.State state) {
            if (mRetroAchievementsManager != null) mRetroAchievementsManager.handleStateChanged(state);
        }

        @Override
        public void onLoginRequested(int reason) {
            // No in-drawer prompt; handled in settings flow.
        }

        @Override
        public void onLoginSuccess(String username, int points, int softPoints, int unreadMessages) {
            // State refresh will surface the appropriate toast.
        }

        @Override
        public void onHardcoreModeChanged(boolean enabled) {
            RetroAchievementsBridge.refreshState();
        }
    };

    private boolean isThread() {
        if (mEmulationThread != null) {
            Thread.State _thread_state = mEmulationThread.getState();
            return _thread_state == Thread.State.BLOCKED
                    || _thread_state == Thread.State.RUNNABLE
                    || _thread_state == Thread.State.TIMED_WAITING
                    || _thread_state == Thread.State.WAITING;
        }
        return false;
    }


    @Nullable
    private static String stripFileExtension(@Nullable String name) { return ChdConversionManager.stripFileExtension(name); }
    @Nullable static Pair<String, String> getPersistedChdMetadata(@Nullable Context ctx, @Nullable Uri uri) { return ChdConversionManager.getPersistedChdMetadata(ctx, uri); }
    static boolean isChdEntry(@Nullable Uri uri, @Nullable String title) { return ChdConversionManager.isChdEntry(uri, title); }
    private void persistChdMetadata(@Nullable Uri uri, @Nullable String serial, @Nullable String title) { mChdConversionManager.persistChdMetadata(uri, serial, title); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mCoverManager = new CoverManager(this);
        mBiosManager = new BiosManager(this);
        mRetroAchievementsManager = new RetroAchievementsManager(this);
        mPerGameSettingsManager = new PerGameSettingsManager(this);
        mContentImportHelper = new ContentImportHelper(this);
        mControllerManager = new ControllerManager(this);
        mChdConversionManager = new ChdConversionManager(this);
        mDataDirectorySetupManager = new DataDirectorySetupManager(this);
        mDialogHelper = new DialogHelper(this);
        mDrawerSettingsManager = new DrawerSettingsManager(this);
        mOnScreenUiStyleManager = new OnScreenUiStyleManager(this);
        NetworkAdapterCollector.collectAdapters();
        DiscordBridge.updateEngineActivity(this);
        ControllerManager.setInstance(this);
        setContentView(R.layout.activity_main);
        disableTouchControls = DeviceProfiles.isTvOrDesktop(this);
	// Keep screen awake during gameplay
	getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        if (Build.VERSION.SDK_INT >= 33) {
            try {
                GameManager gm = (GameManager) getSystemService(Context.GAME_SERVICE);
                if (gm != null) {
                    gm.setGameState(new GameState(false, GameState.MODE_GAMEPLAY_INTERRUPTIBLE));
                }
            } catch (Throwable ignored) {}
        }

        try {
            if (NativeApp.isFullscreenUIEnabled()) {
                setOnScreenControlsVisible(false);
            }
        } catch (Throwable ignored) {}

        // Hide title/action bar explicitly
        if (getSupportActionBar() != null) getSupportActionBar().hide();

        // Force immersive fullscreen
        applyFullscreen();
        DataDirectoryManager.copyAssetAll(getApplicationContext(), "resources");

    Initialize();

    ControllerMappingManager.init(this);
    refreshVibrationPreference();

    // Load on-screen controls hide timeout
    loadHideTimeoutFromPrefs();

    mOnScreenUiStyleManager.loadScalePreference();
        if (!disableTouchControls) {
            makeButtonTouch();
        }

    setSurfaceView(new SDLSurface(this));

        maybeStartOnboardingFlow();

    // Cache on-screen pad containers
    llPadSelectStart = findViewById(R.id.ll_pad_select_start);
    llPadRight = findViewById(R.id.ll_pad_right);
    JoystickView joystickLeft = findViewById(R.id.joystick_left);
    DPadView dpadView = findViewById(R.id.dpad_view);
    setupInGameDrawer();
    setupTouchRevealOverlay();
    // Home UI
    drawerLayout = findViewById(R.id.drawer_root);
    homeContainer = findViewById(R.id.home_container);
    rvGames = findViewById(R.id.rv_games);
    emptyContainer = findViewById(R.id.empty_container);
    tvEmpty = findViewById(R.id.tv_empty);
    etSearch = findViewById(R.id.et_search);
    bgImage = findViewById(R.id.bg_image);
    getOnBackPressedDispatcher().addCallback(onBackPressCallback);
    if (rvGames != null) {
        gamesGridLayoutManager = new GridLayoutManager(this, getGameGridSpanCount());
        rvGames.setLayoutManager(gamesGridLayoutManager);
        gamesAdapter = new GamesAdapter(new ArrayList<>(), entry -> onGameSelected(entry));
        rvGames.setAdapter(gamesAdapter);
        // Controller navigation
    rvGames.setFocusable(true);
    rvGames.setFocusableInTouchMode(true);
        rvGames.setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
        gameSpacingDecoration = new SpacingDecoration(getResources().getDimensionPixelSize(R.dimen.game_selector_tile_spacing));
        rvGames.addItemDecoration(gameSpacingDecoration);
        rvGames.setOnFocusChangeListener((v, hasFocus) -> {
            if (hasFocus && gamesAdapter.getItemCount() > 0) {
                rvGames.post(() -> {
                    RecyclerView.ViewHolder vh = rvGames.findViewHolderForAdapterPosition(0);
                    if (vh != null) vh.itemView.requestFocus();
                });
            }
        });
        applyGameGridConfig();
    }
        enforceTouchControlsPolicy();
        // Search text change -> filter
        if (etSearch != null) {
            etSearch.addTextChangedListener(new android.text.TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
                @Override public void afterTextChanged(android.text.Editable s) {
                    if (gamesAdapter != null) gamesAdapter.setFilter(s != null ? s.toString() : "");
                }
            });
        }
        // FAB actions: convert ISO to CHD 
        com.google.android.material.floatingactionbutton.FloatingActionButton fab = findViewById(R.id.fab_actions);
        if (fab != null) {
            fab.setOnClickListener(v -> {
                androidx.appcompat.widget.PopupMenu pm = new androidx.appcompat.widget.PopupMenu(this, v);
                pm.getMenuInflater().inflate(R.menu.menu_fab_actions, pm.getMenu());
                pm.setOnMenuItemClickListener(item -> {
                    if (item.getItemId() == R.id.menu_convert_iso_chd) {
                        startPickIsoForChd();
                        return true;
                    }
                    return false;
                });
                pm.show();
            });
        }
        MaterialButton btnChooseFolder = findViewById(R.id.btn_choose_folder);
        if (btnChooseFolder != null) btnChooseFolder.setOnClickListener(v -> pickGamesFolder());
        androidx.appcompat.widget.Toolbar toolbar = findViewById(R.id.toolbar);
        if (toolbar != null) {
            String displayName = DeviceProfiles.getProductDisplayName(this, getString(R.string.app_name));
            toolbar.setTitle(getString(R.string.home_game_selector_title_format, displayName));
            try {
                androidx.appcompat.graphics.drawable.DrawerArrowDrawable dd = new androidx.appcompat.graphics.drawable.DrawerArrowDrawable(this);
                dd.setProgress(0f); 
                toolbar.setNavigationIcon(dd);
            } catch (Throwable ignored) {}
            toolbar.setNavigationOnClickListener(v -> {
                if (drawerLayout != null) drawerLayout.openDrawer(GravityCompat.START);
            });
            try {
                toolbar.inflateMenu(R.menu.menu_toolbar_home);
                Menu menu = toolbar.getMenu();
                if (menu != null) {
                    MenuItem rnItem = menu.findItem(R.id.action_open_rn);
                    if (rnItem != null) {
                        rnItem.setVisible(BuildConfig.ENABLE_RN);
                        rnItem.setEnabled(BuildConfig.ENABLE_RN);
                    }
                }
                toolbar.setOnMenuItemClickListener(item -> {
                    int itemId = item.getItemId();
                    if (itemId == R.id.action_toggle_search) {
                        toggleSearchBar();
                        return true;
                    } else if (itemId == R.id.action_toggle_view) {
                        listMode = !listMode;
                        if (rvGames != null) {
                            if (listMode) {
                                rvGames.setLayoutManager(new androidx.recyclerview.widget.LinearLayoutManager(this));
                                item.setIcon(R.drawable.ic_view_grid_24);
                            } else {
                                if (gamesGridLayoutManager == null) {
                                    gamesGridLayoutManager = new GridLayoutManager(this, getGameGridSpanCount());
                                }
                                gamesGridLayoutManager.setSpanCount(getGameGridSpanCount());
                                rvGames.setLayoutManager(gamesGridLayoutManager);
                                item.setIcon(R.drawable.ic_view_list_24);
                            }
                            if (gamesAdapter != null) gamesAdapter.setListMode(listMode);
                        }
                        return true;
                    } else if (itemId == R.id.action_open_rn) {
                        if (!BuildConfig.ENABLE_RN) {
                            return true;
                        }
                        try {
                            Class<?> rnClass = Class.forName("kr.co.iefriends.pcsx2.RNActivity");
                            startActivity(new Intent(this, rnClass));
                        } catch (Throwable t) {
                            try { Toast.makeText(this, R.string.home_react_native_unavailable, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                        }
                        return true;
                    }
                    return false;
                });
            } catch (Throwable ignored) {}
        }
    // Navigation drawer menus
        NavigationView navStart = findViewById(R.id.nav_view_start);
        NavigationView.OnNavigationItemSelectedListener listener = item -> {
            int id = item.getItemId();
            if (id == R.id.menu_boot_bios) {
                bootBios();
            } else if (id == R.id.menu_manage_bios) {
                showBiosManagerDialog();
            } else if (id == R.id.menu_open_settings) {
                Intent si = new Intent(this, SettingsActivity.class);
                startActivityForResult(si, 7722);
            } else if (id == R.id.menu_choose_folder) {
                pickGamesFolder();
        } else if (id == R.id.menu_refresh) {
            if (gamesFolderUri != null) scanGamesFolder(gamesFolderUri);
            else try { Toast.makeText(this, R.string.home_choose_folder_first, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
        } else if (id == R.id.menu_covers) {
            promptForCoversUrl();
        } else if (id == R.id.menu_clear_cover_url) {
            setCoversUrlTemplate("");
            try { Toast.makeText(this, R.string.home_cover_url_cleared, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            if (gamesFolderUri != null) scanGamesFolder(gamesFolderUri);
        } else if (id == R.id.menu_bg_landscape) {
            pickBackgroundImage(false);
        } else if (id == R.id.menu_bg_portrait) {
            pickBackgroundImage(true);
        } else if (id == R.id.menu_bg_clear) {
            clearBackgroundImages();
        }
        if (drawerLayout != null) drawerLayout.closeDrawers();
        return true;
    };
    if (navStart != null) navStart.setNavigationItemSelectedListener(listener);
    try { if (drawerLayout != null) drawerLayout.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED, GravityCompat.END); } catch (Throwable ignored) {}

    try {
        if (navStart != null && navStart.getHeaderCount() > 0) {
            View header = navStart.getHeaderView(0);
            View img = header.findViewById(R.id.header_image);
            View imgBlur = header.findViewById(R.id.header_image_blur);
            android.graphics.Bitmap bmp = loadHeaderBitmapFromAssets();
            android.graphics.Bitmap blurBmp = loadHeaderBlurBitmapFromAssets();
            if (img instanceof android.widget.ImageView && bmp != null) {
                ((android.widget.ImageView) img).setImageBitmap(bmp);
            }
            android.graphics.Bitmap useForBlur = blurBmp != null ? blurBmp : bmp;
            if (imgBlur instanceof android.widget.ImageView && useForBlur != null) {
                ((android.widget.ImageView) imgBlur).setImageBitmap(useForBlur);
                if (android.os.Build.VERSION.SDK_INT >= 31) {
                    try {
                        imgBlur.setRenderEffect(android.graphics.RenderEffect.createBlurEffect(18f, 18f, android.graphics.Shader.TileMode.CLAMP));
                    } catch (Throwable ignored) {}
                }
            }
        }
    } catch (Throwable ignored) {}

    showHome(true);
    if (tvEmpty != null) tvEmpty.setVisibility(View.VISIBLE);

    try {
        android.content.SharedPreferences sp = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String saved = sp.getString(PREF_GAMES_URI, null);
        if (saved != null) {
            gamesFolderUri = Uri.parse(saved);
            scanGamesFolder(gamesFolderUri);
        }
        applySavedBackground();
    } catch (Throwable ignored) {}

    boolean handledLaunch = false;
    try {
        handledLaunch = handleLaunchIntent(getIntent());
    } catch (Throwable ignored) {}
    if (!handledLaunch) {
        try {
            if (getIntent() != null && getIntent().getBooleanExtra("BOOT_BIOS", false)) {
                bootBios();
            }
        } catch (Throwable ignored) {}
    }
    }
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        try {
            handleLaunchIntent(intent);
        } catch (Throwable ignored) {}
    }

    private boolean handleLaunchIntent(@Nullable Intent intent) {
        if (intent == null) {
            return false;
        }
        Uri dataUri = null;
        try {
            dataUri = intent.getData();
        } catch (Throwable ignored) {}
        if (dataUri == null) {
            try {
                Object stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
                if (stream instanceof Uri) {
                    dataUri = (Uri) stream;
                } else if (stream instanceof String) {
                    dataUri = Uri.parse((String) stream);
                }
            } catch (Throwable ignored) {}
            if (dataUri == null) {
                String streamText = intent.getStringExtra(Intent.EXTRA_STREAM);
                if (!TextUtils.isEmpty(streamText)) {
                    try {
                        dataUri = Uri.parse(streamText);
                    } catch (Throwable ignored) {}
                }
            }
        }
        if (dataUri == null) {
            ClipData clipData = intent.getClipData();
            if (clipData != null && clipData.getItemCount() > 0) {
                ClipData.Item item = clipData.getItemAt(0);
                if (item != null) {
                    dataUri = item.getUri();
                    if (dataUri == null && item.getIntent() != null) {
                        dataUri = item.getIntent().getData();
                    }
                }
            }
        }
        if (dataUri == null) {
            try {
                java.util.ArrayList<Uri> streams = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
                if (streams != null && !streams.isEmpty()) {
                    dataUri = streams.get(0);
                }
            } catch (Throwable ignored) {}
        }
        if (dataUri == null) {
            String extraText = intent.getStringExtra(Intent.EXTRA_TEXT);
            if (!TextUtils.isEmpty(extraText)) {
                try {
                    dataUri = Uri.parse(extraText);
                } catch (Throwable ignored) {}
            }
        }
        if (dataUri == null) {
            return false;
        }
        String action = intent.getAction();
        if (!TextUtils.isEmpty(action)) {
            if (!(Intent.ACTION_VIEW.equals(action)
                    || Intent.ACTION_SEND.equals(action)
                    || Intent.ACTION_SEND_MULTIPLE.equals(action)
                    || Intent.ACTION_MAIN.equals(action))) {
                return false;
            }
        }
        if (TextUtils.isEmpty(dataUri.getScheme())) {
            String path = dataUri.getPath();
            if (!TextUtils.isEmpty(path)) {
                try {
                    dataUri = Uri.fromFile(new File(path));
                } catch (Throwable ignored) {}
            }
        }
        launchGameWithPreflight(dataUri);
        return true;
    }
    private void toggleSearchBar() {
        if (etSearch == null) return;
        boolean nowVisible = etSearch.getVisibility() != View.VISIBLE;
        etSearch.setVisibility(nowVisible ? View.VISIBLE : View.GONE);
        if (nowVisible) {
            getOnBackPressedDispatcher().addCallback(onSearchBackPressCallback);
            etSearch.requestFocus();
            try {
                android.view.inputmethod.InputMethodManager imm = (android.view.inputmethod.InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) imm.showSoftInput(etSearch, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
            } catch (Throwable ignored) {}
        } else {
            onSearchBackPressCallback.remove();
            try {
                android.view.inputmethod.InputMethodManager imm = (android.view.inputmethod.InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) imm.hideSoftInputFromWindow(etSearch.getWindowToken(), 0);
            } catch (Throwable ignored) {}
            etSearch.clearFocus();
        }
    }

    String getCoversUrlTemplate() { return mCoverManager.getCoversUrlTemplate(); }
    void setCoversUrlTemplate(String s) { mCoverManager.setCoversUrlTemplate(s); }
    String getManualCoverUri(String gameKey) { return mCoverManager.getManualCoverUri(gameKey); }
    void setManualCoverUri(String gameKey, String uri) { mCoverManager.setManualCoverUri(gameKey, uri); }
    void removeManualCoverUri(String gameKey) { mCoverManager.removeManualCoverUri(gameKey); }
    void promptForCoversUrl() { mCoverManager.promptForCoversUrl(); }

    static String gameKeyFromEntry(GameEntry e) {
        if (e == null) return "";
        String key = (e.uri != null ? e.uri.toString() : ("file://" + e.title));
        return key;
    }

    private String pendingManualCoverGameKey;

    private final ActivityResultLauncher<Intent> startActivityResultPickImage = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Intent data = result.getData();
                    Uri img = data.getData();
                    if (img != null) {
                        try {
                            final int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                            getContentResolver().takePersistableUriPermission(img, takeFlags);
                        } catch (SecurityException ignored) {}
                        String pendingKey = pendingManualCoverGameKey;
                        pendingManualCoverGameKey = null;
                        if (pendingKey != null) {
                            setManualCoverUri(pendingKey, img.toString());
                            if (gamesFolderUri != null) scanGamesFolder(gamesFolderUri);
                        }
                    }
                }
            });

    private final ActivityResultLauncher<Intent> startActivityResultSaveChd = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result ->
                    mChdConversionManager.handleSaveChdResult(result.getResultCode(), result.getData()));

    void carryCoverAssociationAfterChdSave(@Nullable Uri sourceUri,
                                                   @Nullable Uri destinationUri,
                                                   @Nullable String sourceDisplayName,
                                                   @Nullable String destinationDisplayName,
                                                   @Nullable String sourceSerial,
                                                   @Nullable String sourceTitle) {
        if (sourceUri == null || destinationUri == null) {
            return;
        }

        String srcName = !TextUtils.isEmpty(sourceDisplayName) ? sourceDisplayName : sourceUri.getLastPathSegment();
        String dstName = !TextUtils.isEmpty(destinationDisplayName) ? destinationDisplayName : destinationUri.getLastPathSegment();
        if (TextUtils.isEmpty(srcName)) srcName = "source.iso";
        if (TextUtils.isEmpty(dstName)) dstName = "destination.chd";

        GameEntry sourceEntry = new GameEntry(srcName, sourceUri);
        sourceEntry.serial = sourceSerial;
        sourceEntry.gameTitle = sourceTitle;

        GameEntry destinationEntry = new GameEntry(dstName, destinationUri);
        destinationEntry.serial = sourceSerial;
        destinationEntry.gameTitle = sourceTitle;

        String sourceGameKey = gameKeyFromEntry(sourceEntry);
        String destinationGameKey = gameKeyFromEntry(destinationEntry);
        if (!TextUtils.isEmpty(sourceGameKey) && !TextUtils.isEmpty(destinationGameKey)
                && !sourceGameKey.equals(destinationGameKey)) {
            String manualCoverUri = getManualCoverUri(sourceGameKey);
            if (!TextUtils.isEmpty(manualCoverUri)) {
                setManualCoverUri(destinationGameKey, manualCoverUri);
            }
        }

        copyCachedCoverBetweenEntries(sourceEntry, destinationEntry);
    }

    private void copyCachedCoverBetweenEntries(@NonNull GameEntry sourceEntry, @NonNull GameEntry destinationEntry) {
        File coversDir = CoverManager.getCoversCacheDir(this);
        if (coversDir == null) {
            return;
        }
        String sourceBase = CoverManager.computeCoverBaseName(sourceEntry);
        String destinationBase = CoverManager.computeCoverBaseName(destinationEntry);
        if (TextUtils.isEmpty(sourceBase) || TextUtils.isEmpty(destinationBase) || sourceBase.equals(destinationBase)) {
            return;
        }

        File sourceCover = CoverManager.findExistingCoverFile(coversDir, sourceBase);
        if (sourceCover == null || !sourceCover.isFile() || sourceCover.length() <= 0L) {
            return;
        }
        if (CoverManager.findExistingCoverFile(coversDir, destinationBase) != null) {
            return;
        }

        String sourceName = sourceCover.getName();
        int extIndex = sourceName.lastIndexOf('.');
        String ext = extIndex >= 0 ? sourceName.substring(extIndex) : ".jpg";
        File destinationCover = new File(coversDir, destinationBase + ext);
        try (FileInputStream in = new FileInputStream(sourceCover);
             FileOutputStream out = new FileOutputStream(destinationCover)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            out.flush();
            GamesAdapter.registerCachedCover(destinationEntry, destinationCover);
        } catch (IOException ignored) {}
    }

    void showGameOptionsPopup(View anchorView, GameEntry e) {
        if (e == null) return;
        String key = gameKeyFromEntry(e);
        String existing = getManualCoverUri(key);
        PopupMenu menu = new PopupMenu(this, anchorView);
        menu.inflate(R.menu.game_options_menu);
        MenuItem removeChosenCoverMenuItem = menu.getMenu().findItem(R.id.remove_chosen_cover);
        removeChosenCoverMenuItem.setVisible(existing != null);
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == R.id.choose_cover) {
                launchCoverImagePicker(key);
                return true;
            }
            if (item.getItemId() == R.id.remove_chosen_cover) {
                removeManualCoverUri(key);
                if (gamesFolderUri != null) scanGamesFolder(gamesFolderUri);
                return true;
            }
            if (item.getItemId() == R.id.per_game_settings) {
                showPerGameSettingsDialog(e);
                return true;
            }
            return false;
        });
        menu.show();
    }

    private void launchCoverImagePicker(String key) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        pendingManualCoverGameKey = key;
        startActivityResultPickImage.launch(intent);
    }

    private void showPerGameSettingsDialog(GameEntry entry) { mPerGameSettingsManager.showDialog(entry); }
    private void applyPerGameSettingsForEntry(@Nullable GameEntry entry) { mPerGameSettingsManager.applyForEntry(entry); }
    private void applyPerGameSettingsForUri(@Nullable Uri uri) { mPerGameSettingsManager.applyForUri(uri); }
    private void applyPerGameSettingsForKey(@Nullable String gameKey) { mPerGameSettingsManager.applyForKey(gameKey); }
    private void restorePerGameOverrides() { mPerGameSettingsManager.restoreOverrides(); }


    

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyFullscreen();
        if (Build.VERSION.SDK_INT >= 33 && hasFocus) {
            try {
                GameManager gm = (GameManager) getSystemService(Context.GAME_SERVICE);
                if (gm != null) gm.setGameState(new GameState(false, GameState.MODE_GAMEPLAY_INTERRUPTIBLE));
            } catch (Throwable ignored) {}
        }
    }

    private void applyFullscreen() {
        // 1️⃣ Determine if emulation UI is visible
        boolean emulationVisible = !isHomeVisible();
        boolean fullscreen = emulationVisible || isFullscreenUiModeEnabled();

        // 2️⃣ Edge-to-edge: disable system padding
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);

        // 3️⃣ Handle display cutout (notch/punch-hole)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attrs = getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode = emulationVisible
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
            getWindow().setAttributes(attrs);
        }

        // 4️⃣ Get decor view
        View decorView = getWindow().getDecorView();
        applyLegacyImmersiveFlags(decorView, fullscreen);

        // 5️⃣ Disable contrast enforcement on Android Q+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            getWindow().setNavigationBarContrastEnforced(false);
            getWindow().setStatusBarContrastEnforced(false);
        }

        // 6️⃣ Hide system bars
        WindowInsetsControllerCompat controller = new WindowInsetsControllerCompat(getWindow(), decorView);
        if (fullscreen) {
            controller.hide(WindowInsetsCompat.Type.systemBars()); // status + nav bars
            controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars());
            controller.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_DEFAULT);
        }

        // 7️⃣ Consume all insets on root layout so no padding is added
        View root = findViewById(R.id.in_game_root);
        if (root != null) {
            ViewCompat.setOnApplyWindowInsetsListener(root, (v, insets) -> {
                v.setPadding(0, 0, 0, 0); // remove any padding for status/nav/cutout
                return WindowInsetsCompat.CONSUMED;
            });
        }

        // 8️⃣ Optional: touch listener for on-screen controls
        if (fullscreen) {
            decorView.setOnTouchListener((v, e) -> {
                if (disableTouchControls) return false;
                if (e.getAction() == MotionEvent.ACTION_DOWN || e.getAction() == MotionEvent.ACTION_MOVE) {
                    lastInput = InputSource.TOUCH;
                    lastTouchTimeMs = System.currentTimeMillis();
                    if (mEmulationThread != null) {
                        setOnScreenControlsVisible(true);
                        maybeAutoHideControls();
                    }
                    v.performClick();
                }
                return false;
            });
        } else {
            decorView.setOnTouchListener(null);
        }
    }

    private boolean isFullscreenUiModeEnabled() {
        try {
            String value = NativeApp.getSetting("UI", "EnableFullscreenUI", "bool");
            if (!TextUtils.isEmpty(value)) {
                return "true".equalsIgnoreCase(value);
            }
        } catch (Exception ignored) {}
        try {
            return NativeApp.isFullscreenUIEnabled();
        } catch (Throwable ignored) {
            return false;
        }
    }

    private void applyDisplayCutoutMode(boolean emulationVisible) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            return;
        }

        final WindowManager.LayoutParams attrs = getWindow().getAttributes();
        final int targetMode;
        if (!emulationVisible) {
            targetMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
        } else if (isDisplayCutoutExpansionEnabled()) {
            targetMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        } else {
            targetMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER;
        }

        if (attrs.layoutInDisplayCutoutMode != targetMode) {
            attrs.layoutInDisplayCutoutMode = targetMode;
            getWindow().setAttributes(attrs);
        }
    }

    private boolean isDisplayCutoutExpansionEnabled() {
        try {
            String value = NativeApp.getSetting("UI", "ExpandIntoDisplayCutout", "bool");
            return "true".equalsIgnoreCase(value);
        } catch (Exception ignored) {
            return true;
        }
    }

    @SuppressWarnings("deprecation")
    private static void applyLegacyImmersiveFlags(View decorView, boolean fullscreen) {
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        if (fullscreen) {
            flags |= View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        }
        decorView.setSystemUiVisibility(flags);
    }

    public void onSurfaceReady() {
    }

    void ensureBiosPresent() { mBiosManager.ensureBiosPresent(); }
    void openBiosPicker() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.setType("application/octet-stream");
        String[] mimeTypes = new String[]{"application/octet-stream", "application/x-binary"};
        intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
        startActivityResultPickBios.launch(intent);
    }
    private boolean hasBios() { return mBiosManager.hasBios(); }
    private void saveBiosFromUri(Uri uri) { mBiosManager.saveBiosFromUri(uri); }
    private void importBiosFromUri(Uri uri) { mBiosManager.importBiosFromUri(uri); }
    private void showBiosManagerDialog() { mBiosManager.showBiosManagerDialog(); }
    void openBiosImportForManager() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        intent.setType("application/octet-stream");
        String[] mimeTypes = new String[]{"application/octet-stream", "application/x-binary"};
        intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
        startActivityResultImportBios.launch(intent);
    }

    // Buttons
    void configureOnClickListener(@IdRes int id, View.OnClickListener onClickListener) {
        View view = findViewById(id);
        if (view != null) {
            view.setOnClickListener(onClickListener);
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    void configureOnTouchListener(@IdRes int id, int... keyCodes) {
        View view = findViewById(id);
        if (view != null) {
            view.setOnTouchListener((v, event) -> {
                for (int keyCode : keyCodes) {
                    sendKeyAction(v, event.getAction(), keyCode);
                }
                return true;
            });
        }
    }

    private void setupInGameDrawer() {
        inGameDrawer = findViewById(R.id.drawer_in_game);
        drawerToggle = findViewById(R.id.btn_drawer_toggle);
        if (drawerToggle != null) {
            drawerToggle.setVisibility(View.GONE);
            drawerToggle.setOnClickListener(v -> {
                hideDrawerToggle();
                toggleInGameDrawer();
            });
        }
        if (inGameDrawer != null) {
            try {
                inGameDrawer.setDrawerElevation(0f);
            } catch (Throwable ignored) {}
            inGameDrawer.setDrawerLockMode(disableTouchControls ? DrawerLayout.LOCK_MODE_LOCKED_CLOSED : DrawerLayout.LOCK_MODE_UNLOCKED);
            inGameDrawer.addDrawerListener(new DrawerLayout.SimpleDrawerListener() {
                @Override
                public void onDrawerOpened(@NonNull View drawerView) {
                    lastInput = InputSource.TOUCH;
                    lastTouchTimeMs = System.currentTimeMillis();
                    setOnScreenControlsVisible(true);
                    hideDrawerToggle();
                    try {
                        getWindow().getDecorView().removeCallbacks(hideRunnable);
                    } catch (Throwable ignored) {}
                    updateWidescreenToggleVisibility();
                }

                @Override
                public void onDrawerClosed(@NonNull View drawerView) {
                    lastInput = InputSource.TOUCH;
                    lastTouchTimeMs = System.currentTimeMillis();
                    maybeAutoHideControls();
                }
            });
        }

        drawerPauseButton = findViewById(R.id.drawer_btn_pause);
        if (drawerPauseButton != null) {
            drawerPauseButton.setOnClickListener(v -> toggleVmPause());
        }

        drawerFastForwardButton = findViewById(R.id.drawer_btn_fast_forward);
        if (drawerFastForwardButton != null) {
            drawerFastForwardButton.setOnClickListener(v -> toggleFastForward());
            updateFastForwardButtonState();
        }

        FloatingActionButton btnReboot = findViewById(R.id.drawer_btn_reboot);
        if (btnReboot != null) {
            btnReboot.setOnClickListener(v -> {
                restartEmuThread();
                isVmPaused = false;
                updatePauseButtonIcon();
                closeInGameDrawer();
            });
        }

        FloatingActionButton btnPower = findViewById(R.id.drawer_btn_power);
        if (btnPower != null) {
            btnPower.setOnClickListener(v -> {
                shutdownVmToHome();
                closeInGameDrawer();
            });
        }

        MaterialButton btnGames = findViewById(R.id.drawer_btn_games);
        if (btnGames != null) {
            btnGames.setOnClickListener(v -> {
                NativeApp.pause();
                isVmPaused = true;
                updatePauseButtonIcon();
                showHome(true);
                closeInGameDrawer();
            });
        }

        MaterialButton btnGameState = findViewById(R.id.drawer_btn_game_state);
        if (btnGameState != null) {
            btnGameState.setOnClickListener(v -> {
                closeInGameDrawer();
                showGameStateDialog();
            });
        }

        MaterialButton btnTestController = findViewById(R.id.drawer_btn_test_controller);
        if (btnTestController != null) {
            btnTestController.setOnClickListener(v -> {
                closeInGameDrawer();
                new ControllerMappingDialog().show(getSupportFragmentManager(), "controller_mapping");
            });
        }

        MaterialButton btnSettingsDrawer = findViewById(R.id.drawer_btn_settings);
        if (btnSettingsDrawer != null) {
            btnSettingsDrawer.setOnClickListener(v -> {
                closeInGameDrawer();
                startActivityForResult(new Intent(this, SettingsActivity.class), 7722);
            });
        }

        MaterialButton btnAbout = findViewById(R.id.drawer_btn_about);
        if (btnAbout != null) {
            btnAbout.setOnClickListener(v -> {
                closeInGameDrawer();
                showAboutDialog();
            });
        }

        MaterialButton btnImportCheats = findViewById(R.id.drawer_btn_import_cheats);
        if (btnImportCheats != null) {
            btnImportCheats.setOnClickListener(v -> {
                closeInGameDrawer();
                launchCheatImportPicker();
            });
        }

        MaterialButton btnImportTextures = findViewById(R.id.drawer_btn_import_textures);
        if (btnImportTextures != null) {
            btnImportTextures.setOnClickListener(v -> {
                closeInGameDrawer();
                launchTextureImportPicker();
            });
        }

        setupRetroAchievementsDrawerSection();
        mDrawerSettingsManager.setupRendererToggleGroup();
        mDrawerSettingsManager.setupDrawerSpinners();
        mDrawerSettingsManager.setupControllerModeSpinner();
        mDrawerSettingsManager.setupDrawerSwitches();
        Slider uiScaleSlider = findViewById(R.id.drawer_slider_ui_scale);
        TextView uiScaleValue = findViewById(R.id.drawer_ui_scale_value);
        if (uiScaleSlider != null) {
            uiScaleSlider.setValue(mOnScreenUiStyleManager.scaleMultiplier);
            mOnScreenUiStyleManager.updateScaleLabel(uiScaleValue);
            uiScaleSlider.addOnChangeListener((slider, value, fromUser) -> {
                float clamped = Math.max(OnScreenUiStyleManager.ONSCREEN_UI_SCALE_MIN, Math.min(OnScreenUiStyleManager.ONSCREEN_UI_SCALE_MAX, value));
                if (Math.abs(clamped - value) > 0.001f) {
                    slider.setValue(clamped);
                }
                if (Math.abs(mOnScreenUiStyleManager.scaleMultiplier - clamped) > 0.001f) {
                    mOnScreenUiStyleManager.scaleMultiplier = clamped;
                    mOnScreenUiStyleManager.saveScalePreference(clamped);
                    mOnScreenUiStyleManager.updateScaleLabel(uiScaleValue);
                    applyUserUiScale();
                }
            });
        } else {
            mOnScreenUiStyleManager.updateScaleLabel(uiScaleValue);
        }
        updatePauseButtonIcon();
    }

    private void setupTouchRevealOverlay() {
        View root = findViewById(R.id.in_game_root);
        if (root == null) {
            return;
        }
        root.setOnTouchListener((v, event) -> {
            if (disableTouchControls) {
                return false;
            }
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                lastInput = InputSource.TOUCH;
                lastTouchTimeMs = System.currentTimeMillis();
                setOnScreenControlsVisible(true);
                maybeAutoHideControls();
                showDrawerToggleTemporarily();
            }
            return false;
        });
    }

    private void toggleVmPause() {
        if (isVmPaused) {
            NativeApp.resume();
            isVmPaused = false;
        } else {
            NativeApp.pause();
            isVmPaused = true;
        }
        updatePauseButtonIcon();
    }

    private void toggleFastForward() {
        setFastForwardEnabled(!isFastForwardEnabled);
    }

    private void setFastForwardEnabled(boolean enabled) {
        if (isFastForwardEnabled == enabled) {
            updateFastForwardButtonState();
            return;
        }
        isFastForwardEnabled = enabled;
        try {
            NativeApp.speedhackLimitermode(enabled ? 3 : 0);
        } catch (Throwable ignored) {}
        updateFastForwardButtonState();
    }

    private void toggleInGameDrawer() {
        if (inGameDrawer == null) {
            return;
        }
        if (inGameDrawer.isDrawerOpen(GravityCompat.START)) {
            inGameDrawer.closeDrawer(GravityCompat.START);
        } else {
            lastInput = InputSource.TOUCH;
            lastTouchTimeMs = System.currentTimeMillis();
            setOnScreenControlsVisible(true);
            inGameDrawer.openDrawer(GravityCompat.START);
        }
    }

    private void closeInGameDrawer() {
        if (inGameDrawer != null && inGameDrawer.isDrawerOpen(GravityCompat.START)) {
            inGameDrawer.closeDrawer(GravityCompat.START);
        }
    }

    void updatePauseButtonIcon() {
        if (drawerPauseButton == null) {
            return;
        }
        if (isVmPaused) {
            drawerPauseButton.setImageResource(R.drawable.ic_play_circle);
            drawerPauseButton.setContentDescription(getString(R.string.drawer_resume_content_description));
        } else {
            drawerPauseButton.setImageResource(R.drawable.ic_pause_circle);
            drawerPauseButton.setContentDescription(getString(R.string.drawer_pause_content_description));
        }
    }

    private void updateFastForwardButtonState() {
        if (drawerFastForwardButton == null) {
            return;
        }
        int surfaceVariant = resolveThemeColor(android.R.attr.colorBackground);
        int onSurface = resolveThemeColor(android.R.attr.textColorPrimary);
        int primary = resolveThemeColor(android.R.attr.colorPrimary);
        int onPrimary = resolveThemeColor(android.R.attr.textColorPrimary);

        if (isFastForwardEnabled) {
            drawerFastForwardButton.setBackgroundTintList(ColorStateList.valueOf(primary));
            drawerFastForwardButton.setImageTintList(ColorStateList.valueOf(onPrimary));
            drawerFastForwardButton.setContentDescription(getString(R.string.drawer_fast_forward_on_content_description));
        } else {
            drawerFastForwardButton.setBackgroundTintList(ColorStateList.valueOf(surfaceVariant));
            drawerFastForwardButton.setImageTintList(ColorStateList.valueOf(onSurface));
            drawerFastForwardButton.setContentDescription(getString(R.string.drawer_fast_forward_content_description));
        }
    }

    private int resolveThemeColor(int attrRes) {
        TypedValue value = new TypedValue();
        if (getTheme().resolveAttribute(attrRes, value, true)) {
            if (value.type >= TypedValue.TYPE_FIRST_COLOR_INT && value.type <= TypedValue.TYPE_LAST_COLOR_INT) {
                return value.data;
            }
            if (value.resourceId != 0) {
                return ContextCompat.getColor(this, value.resourceId);
            }
        }
        return Color.WHITE;
    }

    private void setupRetroAchievementsDrawerSection() { mRetroAchievementsManager.setupDrawerSection(); }
    private void handleRetroAchievementsStateChanged(RetroAchievementsBridge.State state) { mRetroAchievementsManager.handleStateChanged(state); }


    private void updateWidescreenToggleVisibility() { mDrawerSettingsManager.updateWidescreenToggleVisibility(); }
    private void applyControllerMode(int mode) { mDrawerSettingsManager.applyControllerMode(mode); }
    private void applyRendererSelection(int rendererValue) { mDrawerSettingsManager.applyRendererSelection(rendererValue); }

    boolean readBoolSetting(String section, String key, boolean defaultValue) {
        try {
            String value = NativeApp.getSetting(section, key, "bool");
            if (value == null || value.isEmpty()) {
                return defaultValue;
            }
            return "1".equals(value) || "true".equalsIgnoreCase(value);
        } catch (Exception ignored) {
            return defaultValue;
        }
    }

    private void showGameStateDialog() { mDialogHelper.showGameStateDialog(); }
    private void showAboutDialog() { mDialogHelper.showAboutDialog(); }

    private void refreshOnScreenUiStyleIfNeeded() { mOnScreenUiStyleManager.refreshStyleIfNeeded(); }
    private void refreshOnScreenUiScaleIfNeeded() { mOnScreenUiStyleManager.refreshScaleIfNeeded(); }
    void applyJoystickStyle(JoystickView joystick) { mOnScreenUiStyleManager.applyJoystickStyle(joystick); }
    void applyDpadStyle(DPadView dpadView) { mOnScreenUiStyleManager.applyDpadStyle(dpadView); }
    void applyUserUiScale() { mOnScreenUiStyleManager.applyUserUiScale(); }

    void makeButtonTouch() {
        boolean isNether = OnScreenUiStyleManager.STYLE_NETHER.equals(mOnScreenUiStyleManager.currentStyle);
        PSButtonView btn_pad_select = findViewById(R.id.btn_pad_select);
        if (btn_pad_select != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_select, R.drawable.playstation3_button_select, "ic_controller_select_button.png");
            btn_pad_select.setRectangular(true);
            float selectScale = isNether ? 0.75f : 1.0f;
            btn_pad_select.setScaleX(selectScale);
            btn_pad_select.setScaleY(selectScale);
            btn_pad_select.setOnPSButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_SELECT, 0, pressed));
        }

        PSButtonView btn_pad_start = findViewById(R.id.btn_pad_start);
        if (btn_pad_start != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_start, R.drawable.playstation3_button_start, "ic_controller_start_button.png");
            float selectScale = isNether ? 0.75f : 1.0f;
            btn_pad_start.setScaleX(selectScale);
            btn_pad_start.setScaleY(selectScale);
            btn_pad_start.setOnPSButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_START, 0, pressed));
        }

        float faceScale = isNether ? 0.9f : 1.0f;

        PSButtonView btn_pad_a = findViewById(R.id.btn_pad_a);
        if (btn_pad_a != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_a, R.drawable.playstation_button_color_cross_outline, "ic_controller_cross_button.png");
            btn_pad_a.setScaleX(faceScale);
            btn_pad_a.setScaleY(faceScale);
            btn_pad_a.setOnPSButtonListener(pressed -> {
                int action = pressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
                sendKeyAction(btn_pad_a, action, KeyEvent.KEYCODE_BUTTON_A);
            });
        }

        PSButtonView btn_pad_b = findViewById(R.id.btn_pad_b);
        if (btn_pad_b != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_b, R.drawable.playstation_button_color_circle_outline, "ic_controller_circle_button.png");
            btn_pad_b.setScaleX(faceScale);
            btn_pad_b.setScaleY(faceScale);
            btn_pad_b.setOnPSButtonListener(pressed -> {
                int action = pressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
                sendKeyAction(btn_pad_b, action, KeyEvent.KEYCODE_BUTTON_B);
            });
        }

        PSButtonView btn_pad_x = findViewById(R.id.btn_pad_x);
        if (btn_pad_x != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_x, R.drawable.playstation_button_color_square_outline, "ic_controller_square_button.png");
            btn_pad_x.setScaleX(faceScale);
            btn_pad_x.setScaleY(faceScale);
            btn_pad_x.setOnPSButtonListener(pressed -> {
                int action = pressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
                sendKeyAction(btn_pad_x, action, KeyEvent.KEYCODE_BUTTON_X);
            });
        }

        PSButtonView btn_pad_y = findViewById(R.id.btn_pad_y);
        if (btn_pad_y != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_y, R.drawable.playstation_button_color_triangle_outline, "ic_controller_triangle_button.png");
            btn_pad_y.setScaleX(faceScale);
            btn_pad_y.setScaleY(faceScale);
            btn_pad_y.setOnPSButtonListener(pressed -> {
                int action = pressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
                sendKeyAction(btn_pad_y, action, KeyEvent.KEYCODE_BUTTON_Y);
            });
        }

        PSShoulderButtonView btn_pad_l1 = findViewById(R.id.btn_pad_l1);
        if (btn_pad_l1 != null) {
            mOnScreenUiStyleManager.applyShoulderIcon(btn_pad_l1, R.drawable.playstation_trigger_l1_alternative_outline, "ic_controller_l1_button.png");
            btn_pad_l1.setScaleX(1.0f);
            btn_pad_l1.setScaleY(isNether ? 0.6f : 1.0f);
            btn_pad_l1.setOnPSShoulderButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_L1, 0, pressed));
        }

        PSShoulderButtonView btn_pad_r1 = findViewById(R.id.btn_pad_r1);
        if (btn_pad_r1 != null) {
            mOnScreenUiStyleManager.applyShoulderIcon(btn_pad_r1, R.drawable.playstation_trigger_r1_alternative_outline, "ic_controller_r1_button.png");
            btn_pad_r1.setScaleX(1.0f);
            btn_pad_r1.setScaleY(isNether ? 0.6f : 1.0f);
            btn_pad_r1.setOnPSShoulderButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_R1, 0, pressed));
        }

        PSShoulderButtonView btn_pad_l2 = findViewById(R.id.btn_pad_l2);
        if (btn_pad_l2 != null) {
            mOnScreenUiStyleManager.applyShoulderIcon(btn_pad_l2, R.drawable.playstation_trigger_l2_alternative_outline, "ic_controller_l2_button.png");
            btn_pad_l2.setScaleX(1.0f);
            btn_pad_l2.setScaleY(isNether ? 0.6f : 1.0f);
            btn_pad_l2.setOnPSShoulderButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_L2, 0, pressed));
        }

        PSShoulderButtonView btn_pad_r2 = findViewById(R.id.btn_pad_r2);
        if (btn_pad_r2 != null) {
            mOnScreenUiStyleManager.applyShoulderIcon(btn_pad_r2, R.drawable.playstation_trigger_r2_alternative_outline, "ic_controller_r2_button.png");
            btn_pad_r2.setScaleX(1.0f);
            btn_pad_r2.setScaleY(isNether ? 0.6f : 1.0f);
            btn_pad_r2.setOnPSShoulderButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_R2, 0, pressed));
        }

        PSButtonView btn_pad_l3 = findViewById(R.id.btn_pad_l3);
        if (btn_pad_l3 != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_l3, R.drawable.playstation_button_l3_outline, "ic_controller_l3_button.png");
            btn_pad_l3.setOnPSButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_THUMBL, 0, pressed));
        }

        PSButtonView btn_pad_r3 = findViewById(R.id.btn_pad_r3);
        if (btn_pad_r3 != null) {
            mOnScreenUiStyleManager.applyButtonIcon(btn_pad_r3, R.drawable.playstation_button_r3_outline, "ic_controller_r3_button.png");
            btn_pad_r3.setOnPSButtonListener(pressed -> NativeApp.setPadButton(KeyEvent.KEYCODE_BUTTON_THUMBR, 0, pressed));
        }

        applyUserUiScale();

        JoystickView joystickLeft = findViewById(R.id.joystick_left);
        if (joystickLeft != null) {
            applyJoystickStyle(joystickLeft);
            joystickLeft.setOnJoystickMoveListener((x, y) -> {
                float clampedX = Math.max(-1f, Math.min(1f, x));
                float clampedY = Math.max(-1f, Math.min(1f, y));
                sendAnalog(111, Math.max(0f, clampedX));
                sendAnalog(113, Math.max(0f, -clampedX));
                sendAnalog(112, Math.max(0f, clampedY));
                sendAnalog(110, Math.max(0f, -clampedY));
                lastInput = InputSource.TOUCH;
                lastTouchTimeMs = System.currentTimeMillis();
                maybeAutoHideControls();
            });
        }

        JoystickView joystickRight = findViewById(R.id.joystick_right);
        if (joystickRight != null) {
            applyJoystickStyle(joystickRight);
            joystickRight.setOnJoystickMoveListener((x, y) -> {
                float clampedX = Math.max(-1f, Math.min(1f, x));
                float clampedY = Math.max(-1f, Math.min(1f, y));
                sendAnalog(121, Math.max(0f, clampedX));
                sendAnalog(123, Math.max(0f, -clampedX));
                sendAnalog(122, Math.max(0f, clampedY));
                sendAnalog(120, Math.max(0f, -clampedY));
                lastInput = InputSource.TOUCH;
                lastTouchTimeMs = System.currentTimeMillis();
                maybeAutoHideControls();
            });
        }

        DPadView dpadView = findViewById(R.id.dpad_view);
        if (dpadView != null) {
            applyDpadStyle(dpadView);
            dpadView.setOnDPadListener((direction, pressed) -> {
                int keycode = -1;
                switch (direction) {
                    case UP:
                        keycode = KeyEvent.KEYCODE_DPAD_UP;
                        break;
                    case DOWN:
                        keycode = KeyEvent.KEYCODE_DPAD_DOWN;
                        break;
                    case LEFT:
                        keycode = KeyEvent.KEYCODE_DPAD_LEFT;
                        break;
                    case RIGHT:
                        keycode = KeyEvent.KEYCODE_DPAD_RIGHT;
                        break;
                }

                if (keycode != -1) {
                    int action = pressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
                    sendKeyAction(dpadView, action, keycode);
                }
            });
        }
        applyUserUiScale();
    }

    private boolean importMemcardToSlot1(Uri uri) { return mContentImportHelper.importMemcardToSlot1(uri); }
    private void importCheatFile(Uri uri) { mContentImportHelper.importCheatFile(uri); }
    private void importTextureArchive(Uri uri) { mContentImportHelper.importTextureArchive(uri); }
    private String getDisplayNameForUri(Uri uri) { return mContentImportHelper.getDisplayNameForUri(uri); }
    private File createUniqueFile(File directory, String name) { return mContentImportHelper.createUniqueFile(directory, name); }
    private boolean isFileInsideBase(File base, File target) { return mContentImportHelper.isFileInsideBase(base, target); }
    private void persistUriPermission(Uri uri) { mContentImportHelper.persistUriPermission(uri); }
    private void showDrawerImportFailureDialog(@StringRes int titleRes, String details) { mContentImportHelper.showDrawerImportFailureDialog(titleRes, details); }

    private void showSettingsDialog() { mDialogHelper.showSettingsDialog(); }

    public final ActivityResultLauncher<Intent> startActivityResultLocalFilePlay = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(),
            result -> {
                if (result.getResultCode() == Activity.RESULT_OK) {
                    try {
                        Intent _intent = result.getData();
                        if(_intent != null) {
                            Uri picked = _intent.getData();
                            if (picked != null) {
                                applyPerGameSettingsForUri(picked);
                                m_szGamefile = picked.toString();
                                if(!TextUtils.isEmpty(m_szGamefile)) {
                                    handleSelectedGameUri(picked);
                                }
                            }
                        }
                    } catch (Exception ignored) {}
                }
            });

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 7722 && resultCode == Activity.RESULT_OK && data != null) {
            if (data.hasExtra("SET_RENDERER")) {
                int r = data.getIntExtra("SET_RENDERER", -1000);
                if (r != -1000) {
                    applyRendererSelection(r);
                }
            }
            if (data.getBooleanExtra(EXTRA_SETTINGS_LAYOUT_CHANGED, false)) {
                applyFullscreen();
            }
            if (data.hasExtra(EXTRA_SETTINGS_GPU_PROFILE_OVERRIDE)) {
                String selected = data.getStringExtra(EXTRA_SETTINGS_GPU_PROFILE_OVERRIDE);
                boolean persisted = data.getBooleanExtra(EXTRA_SETTINGS_GPU_PROFILE_PERSISTED, true);
                if (!TextUtils.isEmpty(selected) && !persisted) {
                    boolean recovered = false;
                    try {
                        NativeApp.setSetting("EmuCore/GS", "AndroidGpuProfileOverride", "string", selected);
                        String verify = NativeApp.getSetting("EmuCore/GS", "AndroidGpuProfileOverride", "string");
                        recovered = selected.equalsIgnoreCase(verify);
                    } catch (Throwable ignored) {}
                    int msg = recovered
                            ? R.string.settings_gpu_profile_persist_recovered
                            : R.string.settings_gpu_profile_persist_failed;
                    try { Toast.makeText(this, msg, Toast.LENGTH_LONG).show(); } catch (Throwable ignored) {}
                } else if (!TextUtils.isEmpty(selected)) {
                    try { Toast.makeText(this, R.string.settings_gpu_profile_saved_hint, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
                }
            }
        }
        if (requestCode == 9911 && resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
            if (importMemcardToSlot1(uri)) {
                NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "false");
                NativeApp.setSetting("MemoryCards", "Slot1_Filename", "string", "Mcd001.ps2");
                NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "true");
                Toast.makeText(this, R.string.settings_memory_card_inserted_slot1, Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, R.string.settings_memory_card_import_failed, Toast.LENGTH_LONG).show();
            }
        }
    }

    private final ActivityResultLauncher<Intent> startActivityResultPickBios = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null && result.getData().getData() != null) {
                    saveBiosFromUri(result.getData().getData());
                }
            });

    private final ActivityResultLauncher<Intent> startActivityResultImportBios = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null && result.getData().getData() != null) {
                    Uri uri = result.getData().getData();
                    try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) {}
                    importBiosFromUri(uri);
                    showBiosManagerDialog();
                }
            });
    private final ActivityResultLauncher<Intent> startActivityResultOnboarding =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result ->
                    mDataDirectorySetupManager.handleOnboardingResult(result.getResultCode()));

    @Override
    public void onConfigurationChanged(@NonNull Configuration p_newConfig) {
        super.onConfigurationChanged(p_newConfig);
        applyGameGridConfig();
    }

    @Override
    protected void onPause() {
        RetroAchievementsBridge.setListener(null);
        NativeApp.pause();
        isVmPaused = true;
        updatePauseButtonIcon();
        super.onPause();
        ////
        if (mHIDDeviceManager != null) {
            mHIDDeviceManager.setFrozen(true);
        }
    }

    @Override
    protected void onResume() {
        NativeApp.resume();
        isVmPaused = false;
        updatePauseButtonIcon();
        super.onResume();
        RetroAchievementsBridge.setListener(retroAchievementsListener);
        RetroAchievementsBridge.refreshState();
        DiscordBridge.updateEngineActivity(this);
        ////
        if (mHIDDeviceManager != null) {
            mHIDDeviceManager.setFrozen(false);
        }
        // Re-apply immersive fullscreen when resuming
        applyFullscreen();
        loadHideTimeoutFromPrefs();
        refreshOnScreenUiStyleIfNeeded();
        refreshOnScreenUiScaleIfNeeded();
    }

	@Override
	protected void onDestroy() {
		stopEmuThread();
		LogcatRecorder.shutdown();
		super.onDestroy();
		////
		if (mHIDDeviceManager != null) {
			HIDDeviceManager.release(mHIDDeviceManager);
			mHIDDeviceManager = null;
        }
        ////
        mEmulationThread = null;
        ControllerManager.clearInstance();
    }

    /// ///////////////////////////////////////////////////////////////////////////////////////////

    public void Initialize() {
        File dataDir = DataDirectoryManager.getDataRoot(getApplicationContext());
        if (dataDir != null) {
            NativeApp.setDataRootOverride(dataDir.getAbsolutePath());
        }
        NativeApp.initializeOnce(getApplicationContext());
        
        // Restore custom GPU driver if one was previously selected
        restoreGpuDriver();
        
        LogcatRecorder.initialize(getApplicationContext());
        boolean recordLogs = false;
        try {
            String current = NativeApp.getSetting("Logging", "RecordAndroidLog", "bool");
            recordLogs = "true".equalsIgnoreCase(current);
        } catch (Exception ignored) {}
        LogcatRecorder.setEnabled(recordLogs);

		// Set up JNI
		SDLControllerManager.nativeSetupJNI();

		// Initialize state
        SDLControllerManager.initialize();

        mHIDDeviceManager = HIDDeviceManager.acquire(this);
    }
    
    private void restoreGpuDriver() {
        try {
            // Set native library directory for libadrenotools
            String nativeLibDir = getApplicationInfo().nativeLibraryDir;
            if (!TextUtils.isEmpty(nativeLibDir)) {
                NativeApp.setNativeLibraryDir(nativeLibDir);
            }
        } catch (Exception e) {
            // If there's any error, just continue with defaults
        }
    }

    private boolean isOnboardingComplete() { return mDataDirectorySetupManager.isOnboardingComplete(); }
    private void setOnboardingComplete() { mDataDirectorySetupManager.setOnboardingComplete(); }
    private void maybeStartOnboardingFlow() { mDataDirectorySetupManager.maybeStartOnboardingFlow(); }
    private void runPostOnboardingPrompts() { mDataDirectorySetupManager.runPostOnboardingPrompts(); }
    private void maybeShowDataDirectoryPrompt() { mDataDirectorySetupManager.maybeShowDataDirectoryPrompt(); }
    void launchOnboardingIntent(Intent i) { startActivityResultOnboarding.launch(i); }
    void launchDataDirPickerIntent(Intent i) { startActivityResultPickDataDir.launch(i); }

    private void setSurfaceView(Object p_value) {
        FrameLayout fl_board = findViewById(R.id.fl_board);
        if (fl_board != null) {
            if (fl_board.getChildCount() > 0) {
                fl_board.removeAllViews();
            }
            ////
            if (p_value instanceof SDLSurface) {
                fl_board.addView((SDLSurface) p_value);
            }
        }
    }

    public synchronized void startEmuThread() {
        if (!hasBios()) {
            ensureBiosPresent();
            return;
        }
        stopEmuThread(false);
        for (int attempts = 0; attempts < 40 && NativeApp.hasValidVm(); attempts++) {
            SystemClock.sleep(50);
        }
        if (NativeApp.hasValidVm()) {
            NativeApp.shutdown();
            SystemClock.sleep(100);
            if (NativeApp.hasValidVm()) {
                DebugLog.w("VM", "VM still reporting active after shutdown; proceeding with clean boot");
            }
        }
        try { NativeApp.resetKeyStatus(); } catch (Throwable ignored) {}
        if (isThread()) {
            return;
        }
        isVmPaused = false;
        updatePauseButtonIcon();
        mEmulationThread = new Thread(() -> {
            runOnUiThread(() -> {
                try { if (NativeApp.isFullscreenUIEnabled()) setOnScreenControlsVisible(true); } catch (Throwable ignored) {}
                try {
                    String p = m_szGamefile;
                    if (p != null && !p.isEmpty()) {
                        Toast.makeText(this, getString(R.string.home_launching_game, p), Toast.LENGTH_SHORT).show();
                    }
                } catch (Throwable ignored) {}
            });
            NativeApp.runVMThread(m_szGamefile);
        });
        mEmulationThread.start();
    }

    private void stopEmuThread() {
        stopEmuThread(true);
    }

    private synchronized void stopEmuThread(boolean forceShutdown) {
        if (mEmulationThread != null) {
            NativeApp.shutdown();
            try {
                mEmulationThread.join();
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            mEmulationThread = null;
        } else if (forceShutdown) {
            NativeApp.shutdown();
        }
        try { NativeApp.resetKeyStatus(); } catch (Throwable ignored) {}
        setFastForwardEnabled(false);
        isVmPaused = false;
        updatePauseButtonIcon();
    }

    private void restartEmuThread() {
        startEmuThread();
    }

    //////////////////////////////////////////////////////////////////////////////////////////////

    private void handleSelectedGameUri(@NonNull Uri uri) {
        String scheme = uri.getScheme();
        String lastSeg = uri.getLastPathSegment();
        String mime = null;
        try { mime = getContentResolver().getType(uri); } catch (Exception ignored) {}
        boolean hasChdSuffix = (lastSeg != null && lastSeg.toLowerCase().endsWith(".chd")) ||
                (m_szGamefile.toLowerCase().endsWith(".chd"));

        boolean headerSaysChd = false;
        byte[] header = readFirstBytes(uri, 16);
        if (header != null && header.length >= 8) {
            String hv = new String(header, 0, 8);
            headerSaysChd = "MComprHD".equals(hv);
        }

        if ("content".equals(scheme)) {
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Throwable ignored) {}
            m_szGamefile = uri.toString();
            try { lastInput = InputSource.TOUCH; lastTouchTimeMs = System.currentTimeMillis(); setOnScreenControlsVisible(true); } catch (Throwable ignored) {}
            showHome(false);
            restartEmuThread();
            return;
        }

        m_szGamefile = uri.toString();
        try { lastInput = InputSource.TOUCH; lastTouchTimeMs = System.currentTimeMillis(); setOnScreenControlsVisible(true); } catch (Throwable ignored) {}
        showHome(false);
        restartEmuThread();
    }

    private String copyToCache(@NonNull Uri uri, @NonNull String fileName) {
        java.io.InputStream in = null;
        java.io.FileOutputStream out = null;
        try {
            java.io.File dir = new java.io.File(getCacheDir(), "games");
            if (!dir.exists()) 
                dir.mkdirs();
            String ext = "";
            int dot = fileName.lastIndexOf('.');
            if (dot > 0) ext = fileName.substring(dot);
            String base = java.util.Objects.toString(Integer.toHexString(uri.toString().hashCode()));
            java.io.File dst = new java.io.File(dir, base + ext);
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            out = new java.io.FileOutputStream(dst, false);
            byte[] buf = new byte[1024 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            return dst.getAbsolutePath();
        } catch (Exception ignored) {
            return null;
        } finally {
            try { if (in != null) in.close(); } catch (Exception ignored) {}
            try { if (out != null) out.close(); } catch (Exception ignored) {}
        }
    }

    private byte[] readFirstBytes(Uri uri, int count) {
        try {
            InputStream in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            byte[] buf = new byte[count];
            int read = in.read(buf);
            in.close();
            if (read <= 0) return null;
            if (read < count) {
                byte[] cut = new byte[read];
                System.arraycopy(buf, 0, cut, 0, read);
                return cut;
            }
            return buf;
        } catch (Exception ignored) { return null; }
    }


    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        ControllerManager.updateLastControllerDeviceId(event.getDeviceId());
        if (SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())) {
            SDLControllerManager.handleJoystickMotionEvent(event);
            handleGamepadMotion(event);
            lastInput = InputSource.CONTROLLER;
            lastControllerTimeMs = System.currentTimeMillis();
            maybeAutoHideControls();
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int p_keyCode, KeyEvent p_event) {
        if ((p_event.getSource() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) {
            if (p_event.getRepeatCount() == 0) {
                ControllerManager.updateLastControllerDeviceId(p_event.getDeviceId());
                SDLControllerManager.onNativePadDown(p_event.getDeviceId(), p_keyCode);
                forwardKeyToPad(true, p_keyCode);
                lastInput = InputSource.CONTROLLER;
                lastControllerTimeMs = System.currentTimeMillis();
                maybeAutoHideControls();
                return true;
            }
        }
        return super.onKeyDown(p_keyCode, p_event);
    }

    @Override
    public boolean onKeyUp(int p_keyCode, KeyEvent p_event) {
        if ((p_event.getSource() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) {
            if (p_event.getRepeatCount() == 0) {
                ControllerManager.updateLastControllerDeviceId(p_event.getDeviceId());
                SDLControllerManager.onNativePadUp(p_event.getDeviceId(), p_keyCode);
                forwardKeyToPad(false, p_keyCode);
                lastInput = InputSource.CONTROLLER;
                lastControllerTimeMs = System.currentTimeMillis();
                maybeAutoHideControls();
                return true;
            }
        }
        return super.onKeyUp(p_keyCode, p_event);
    }

    private void sendKeyAction(View p_view, int p_action, int p_keycode) {
        if(p_action == MotionEvent.ACTION_DOWN) {
            p_view.setPressed(true);
            int pad_force = 0;
            if (p_keycode >= 110) {
                float _abs = 90; 
                _abs = Math.min(_abs, 100);
                pad_force = (int) (_abs * 32766.0f / 100);
            }
            NativeApp.setPadButton(p_keycode, pad_force, true);
            lastInput = InputSource.TOUCH;
            lastTouchTimeMs = System.currentTimeMillis();
            maybeAutoHideControls();
        } else if(p_action == MotionEvent.ACTION_UP || p_action == MotionEvent.ACTION_CANCEL) {
            p_view.setPressed(false);
            NativeApp.setPadButton(p_keycode, 0, false);
        }
    }

    private void maybeAutoHideControls() {
        if (disableTouchControls) {
            setOnScreenControlsVisible(false);
            return;
        }
        if (lastInput == InputSource.CONTROLLER) {
            setOnScreenControlsVisible(false);
        } else {
            setOnScreenControlsVisible(true);
            getWindow().getDecorView().removeCallbacks(hideRunnable);
            if (hideDelayMs != 0L)
                getWindow().getDecorView().postDelayed(hideRunnable, hideDelayMs);
        }
    }

    private final Runnable hideRunnable = new Runnable() {
        @Override public void run() {
            if (hideDelayMs != 0L && lastInput == InputSource.TOUCH) {
                long dt = System.currentTimeMillis() - lastTouchTimeMs;
                if (dt >= hideDelayMs) setOnScreenControlsVisible(false);
            }
        }
    };

    private void setOnScreenControlsVisible(boolean visible) {
        if (disableTouchControls) {
            visible = false;
        }
        int vis = visible ? View.VISIBLE : View.GONE;
        if (llPadSelectStart != null) llPadSelectStart.setVisibility(vis);
        if (llPadRight != null) llPadRight.setVisibility(vis);
        View leftShoulders = findViewById(R.id.ll_pad_shoulders_left);
        if (leftShoulders != null) leftShoulders.setVisibility(vis);
        View rightShoulders = findViewById(R.id.ll_pad_shoulders_right);
        if (rightShoulders != null) rightShoulders.setVisibility(vis);
        
        JoystickView joystickLeft = findViewById(R.id.joystick_left);
        JoystickView joystickRight = findViewById(R.id.joystick_right);
        DPadView dpadView = findViewById(R.id.dpad_view);
        
        if (joystickLeft != null) {
            if (currentControllerMode == 2) {
                joystickLeft.setVisibility(View.GONE);
            } else {
                joystickLeft.setVisibility(vis);
            }
        }
        
        if (joystickRight != null) {
            if (currentControllerMode == 1 || currentControllerMode == 2) {
                joystickRight.setVisibility(View.GONE);
            } else {
                joystickRight.setVisibility(vis);
            }
        }
        
        if (dpadView != null) {
            if (currentControllerMode == 1) {
                dpadView.setVisibility(View.GONE);
            } else {
                dpadView.setVisibility(vis);
            }
        }
        
        if (!visible) {
            hideDrawerToggle();
        }
        if (!disableTouchControls && visible) {
            try {
                getWindow().getDecorView().removeCallbacks(hideRunnable);
                if (hideDelayMs != 0L && lastInput == InputSource.TOUCH) {
                    getWindow().getDecorView().postDelayed(hideRunnable, hideDelayMs);
                }
            } catch (Throwable ignored) {}
        } else if (disableTouchControls) {
            try {
                getWindow().getDecorView().removeCallbacks(hideRunnable);
            } catch (Throwable ignored) {}
        }
    }

    private void showDrawerToggleTemporarily() {
        if (disableTouchControls) {
            return;
        }
        if (drawerToggle == null) {
            return;
        }
        drawerToggle.removeCallbacks(hideDrawerToggleRunnable);
        drawerToggle.setVisibility(View.VISIBLE);
        long delay = hideDelayMs != 0L ? hideDelayMs : 2000L;
        drawerToggle.postDelayed(hideDrawerToggleRunnable, delay);
    }

    private void hideDrawerToggle() {
        if (drawerToggle == null) {
            return;
        }
        drawerToggle.removeCallbacks(hideDrawerToggleRunnable);
        drawerToggle.setVisibility(View.GONE);
    }

    private void applyGameGridConfig() {
        if (rvGames == null) return;
        final int span = getGameGridSpanCount();
        if (!listMode) {
            if (gamesGridLayoutManager == null) {
                gamesGridLayoutManager = new GridLayoutManager(this, span);
                rvGames.setLayoutManager(gamesGridLayoutManager);
            } else {
                gamesGridLayoutManager.setSpanCount(span);
                if (rvGames.getLayoutManager() != gamesGridLayoutManager) {
                    rvGames.setLayoutManager(gamesGridLayoutManager);
                }
            }
        }
        if (gameSpacingDecoration != null) {
            gameSpacingDecoration.updateSpacing(getResources().getDimensionPixelSize(R.dimen.game_selector_tile_spacing));
            rvGames.invalidateItemDecorations();
        }
        final int padding = getResources().getDimensionPixelSize(R.dimen.game_selector_grid_padding);
        rvGames.setPadding(padding, padding, padding, padding);
    }

    private int getGameGridSpanCount() {
        return getResources().getInteger(R.integer.game_selector_span_count);
    }

    int dpToPx(int dp) { return Math.round(dp * getResources().getDisplayMetrics().density); }

    void showStorageAccessError(File targetDir) {
        boolean canGrant = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !DataDirectoryManager.hasAllFilesAccess();
        String message = "Android denied direct file access for:\n" + targetDir.getAbsolutePath() +
                "\n\nGrant 'Allow access to all files' in system settings or choose a folder inside ARMSX2's storage.";
        MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(this)
            .setTitle("Permission required")
            .setMessage(message)
            .setNegativeButton("OK", (d, w) -> d.dismiss());
        if (canGrant) {
            builder.setPositiveButton("Open settings", (d, w) -> {
                d.dismiss();
                openAllFilesAccessSettings();
            });
        }
        builder.show();
    }

    private void openAllFilesAccessSettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                intent.setData(Uri.parse("package:" + getPackageName()));
                startActivity(intent);
            } catch (Exception ignored) {
                try {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                    startActivity(intent);
                } catch (Exception ignored2) {}
            }
        }
    }

    private final ActivityResultLauncher<Intent> startActivityResultPickIso = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result ->
                    mChdConversionManager.handlePickIsoResult(result.getResultCode(), result.getData()));

    private void startPickIsoForChd() { mChdConversionManager.startPickIsoForChd(); }
    void launchIsoPickerIntent(Intent i) { startActivityResultPickIso.launch(i); }
    void launchSaveChdIntent(Intent i) { startActivityResultSaveChd.launch(i); }
    private String queryOpenableDisplayName(Uri uri) { return mChdConversionManager.queryOpenableDisplayName(uri); }

    private void performIsoToChd(Uri isoUri, String isoDisplayName) {
        // Note: performIsoToChd stays in ChdConversionManager; this stub kept for 
        // the inline lambda in startActivityResultPickIso which calls it via handlePickIsoResult.
        // This method is not called directly anymore.
    }
    private void promptForChdSave(String chdCachePath, String displayName, @Nullable Uri sourceUri, @Nullable String sourceSerial, @Nullable String sourceTitle) {
        mChdConversionManager.promptForChdSave(chdCachePath, displayName, sourceUri, sourceSerial, sourceTitle);
    }
    private boolean saveChdToUri(File chdFile, Uri destinationUri) { return mChdConversionManager.saveChdToUri(chdFile, destinationUri); }
    private void showConversionResult(boolean success, String message) { }

    private void loadHideTimeoutFromPrefs() {
        try {
            android.content.SharedPreferences sp = getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            int sec = sp.getInt(PREF_HIDE_CONTROLS_SECONDS, 10); 
            if (sec < 0) sec = 0;
            if (sec > 60) sec = 60;
            hideDelayMs = (sec == 0) ? 0L : sec * 1000L;
        } catch (Throwable ignored) { hideDelayMs = 2500L; }
    }

    private void forwardKeyToPad(boolean down, int keycode) { mControllerManager.forwardKeyToPad(down, keycode); }
    private void handleGamepadMotion(MotionEvent e) { mControllerManager.handleGamepadMotion(e); }
    private void sendAnalog(int keyCode, float normalized) { mControllerManager.sendAnalog(keyCode, normalized); }
    private void refreshVibrationPreference() { mControllerManager.refreshVibrationPreference(); }
    public static void requestControllerRumble(float large, float small) { ControllerManager.requestControllerRumble(large, small); }
    public static void setVibrationPreference(boolean enabled) { ControllerManager.setVibrationPreference(enabled); }

    private final ActivityResultLauncher<Intent> startActivityResultPickDataDir = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Intent data = result.getData();
                    Uri tree = data.getData();
                    if (tree != null) {
                        final int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                        try {
                            getContentResolver().takePersistableUriPermission(tree, takeFlags);
                        } catch (SecurityException ignored) {}
                        mDataDirectorySetupManager.handleDataDirectorySelection(tree);
                        return;
                    }
                }
                mDataDirectorySetupManager.maybeShowDataDirectoryPrompt();
            });

    //////////////////////////////////////////////////////////////////////////////////////////////

    // HOME FLOW
    private final ActivityResultLauncher<Intent> startActivityResultPickFolder = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Intent data = result.getData();
                    Uri tree = data.getData();
                    if (tree != null) {
                        try {
                            final int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                            getContentResolver().takePersistableUriPermission(tree, takeFlags);
                        } catch (SecurityException ignored) {}
                        gamesFolderUri = tree;
                        try {
                            getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                                .edit().putString(PREF_GAMES_URI, tree.toString()).apply();
                        } catch (Throwable ignored) {}
                        scanGamesFolder(tree);
                    }
                }
            });

    private final ActivityResultLauncher<Intent> startActivityResultImportCheats = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Uri uri = result.getData().getData();
                    if (uri != null) {
                        persistUriPermission(uri);
                        importCheatFile(uri);
                    }
                }
            });

    private final ActivityResultLauncher<Intent> startActivityResultImportTextures = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Uri uri = result.getData().getData();
                    if (uri != null) {
                        persistUriPermission(uri);
                        importTextureArchive(uri);
                    }
                }
            });

    private void pickGamesFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityResultPickFolder.launch(intent);
    }

    private void launchCheatImportPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_TITLE, getString(R.string.drawer_import_cheats_picker_title));
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/x-pnach", "application/octet-stream", "text/plain"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityResultImportCheats.launch(intent);
    }

    private void launchTextureImportPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE, getString(R.string.drawer_import_textures_picker_title));
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityResultImportTextures.launch(intent);
    }

    void scanGamesFolder(Uri folder) {
    List<GameEntry> entries = GameScanner.scanFolder(this, folder);
        try {
            java.util.Collections.sort(entries, (a, b) -> {
                String ta = a != null ? (a.title != null ? a.title : "") : "";
                String tb = b != null ? (b.title != null ? b.title : "") : "";
                int ga = sortGroup(ta);
                int gb = sortGroup(tb);
                if (ga != gb) return Integer.compare(ga, gb);
                return ta.compareToIgnoreCase(tb);
            });
        } catch (Throwable ignored) {}
    gamesAdapter.update(entries);
        final List<GameEntry> toResolve = new ArrayList<>();
        for (GameEntry ge : entries) {
            try {
                if (ge != null && (ge.serial == null || ge.serial.isEmpty())) {
                    String name = ge.title != null ? ge.title.toLowerCase() : "";
                    if (name.endsWith(".iso") || name.endsWith(".img") || name.endsWith(".bin"))
                        toResolve.add(ge);
                }
            } catch (Throwable ignored) {}
        }
        if (!toResolve.isEmpty()) {
            new Thread(() -> {
                android.content.ContentResolver cr = getContentResolver();
                int n = 0;
                for (GameEntry ge : toResolve) {
                    try {
                        RedumpDB.Result rd = RedumpDB.lookupByFile(cr, ge.uri);
                        if (rd != null && rd.serial != null && !rd.serial.isEmpty()) {
                            ge.serial = rd.serial;
                            ge.gameTitle = rd.name;
                            n++;
                            if (n % 2 == 1) {
                                runOnUiThread(() -> gamesAdapter.notifyDataSetChanged());
                            }
                        }
                    } catch (Throwable ignored) {}
                }
                if (n > 0) runOnUiThread(() -> gamesAdapter.notifyDataSetChanged());
            }, "RedumpResolve").start();
        }
        if (etSearch != null && etSearch.getText() != null && etSearch.length() > 0) {
            gamesAdapter.setFilter(etSearch.getText().toString());
        }
        if (rvGames != null && gamesAdapter.getItemCount() > 0) {
            rvGames.post(() -> {
                rvGames.requestFocus(); 
                rvGames.postDelayed(() -> {
                    RecyclerView.ViewHolder vh = rvGames.findViewHolderForAdapterPosition(0);
                    if (vh != null && vh.itemView != null) {
                        vh.itemView.requestFocus();
                    }
                }, 100); 
            });
        }
        boolean empty = entries.isEmpty();
    try { Toast.makeText(this, getString(R.string.home_games_found_count, entries.size()), Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
        if (tvEmpty != null) {
            tvEmpty.setText(empty ? getString(R.string.home_no_games_detected) : "");
            tvEmpty.setVisibility(empty ? View.VISIBLE : View.GONE);
        }
        if (emptyContainer != null) emptyContainer.setVisibility(empty ? View.VISIBLE : View.GONE);
        if (rvGames != null) rvGames.setVisibility(empty ? View.GONE : View.VISIBLE);
        if (!empty) showHome(true);

    }

    private static int sortGroup(String title) {
        if (title == null) return 2;
        String t = title.trim();
        if (t.isEmpty()) return 2;
        char c = t.charAt(0);
        if (c >= '0' && c <= '9') return 0; 
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return 1; 
        return 2;
    }

    private void showHome(boolean show) {
        if (show) {
            restorePerGameOverrides();
        }

        if (homeContainer != null) {
            homeContainer.setVisibility(show ? View.VISIBLE : View.GONE);
            onBackPressCallback.setEnabled(!show);
        }
        if (drawerLayout != null) {
            drawerLayout.setVisibility(show ? View.VISIBLE : View.GONE);
            try {
                drawerLayout.setDrawerLockMode(show ? DrawerLayout.LOCK_MODE_UNLOCKED : DrawerLayout.LOCK_MODE_LOCKED_CLOSED);
                drawerLayout.setScrimColor(android.graphics.Color.TRANSPARENT);
            } catch (Throwable ignored) {}
        }
        if (inGameDrawer != null) {
            if (show) {
                try {
                    inGameDrawer.closeDrawer(GravityCompat.START);
                } catch (Throwable ignored) {}
                inGameDrawer.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED);
            } else {
                inGameDrawer.setDrawerLockMode(disableTouchControls ? DrawerLayout.LOCK_MODE_LOCKED_CLOSED : DrawerLayout.LOCK_MODE_UNLOCKED);
            }
        }
        if (show) {
            setFastForwardEnabled(false);
            if (rvGames != null && rvGames.getVisibility() == View.VISIBLE && gamesAdapter != null && gamesAdapter.getItemCount() > 0) {
                rvGames.post(() -> {
                    rvGames.requestFocus();
                    RecyclerView.ViewHolder vh = rvGames.findViewHolderForAdapterPosition(0);
                    if (vh != null && vh.itemView != null) {
                        vh.itemView.requestFocus();
                    }
                });
            }
        }
        if (show || disableTouchControls) {
            hideDrawerToggle();
        }
        int vis = show ? View.GONE : View.VISIBLE;
        setOnScreenControlsVisible(!show);
        if (llPadSelectStart != null) llPadSelectStart.setVisibility(vis);
        if (llPadRight != null) llPadRight.setVisibility(vis);
        View j = findViewById(R.id.joystick_left);
        if (j != null) j.setVisibility(vis);
        View jr = findViewById(R.id.joystick_right);
        if (jr != null) jr.setVisibility(vis);
        View d = findViewById(R.id.dpad_view);
        if (d != null) d.setVisibility(vis);
        try {
            if (show) {
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
            } else {
                if (TextUtils.isEmpty(m_szGamefile)) {
                    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
                } else {
                    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
                }
            }
        } catch (Throwable ignored) {}
        if (show && tvEmpty != null && gamesFolderUri == null) {
            tvEmpty.setText(R.string.home_nav_choose_games_folder);
            tvEmpty.setVisibility(View.VISIBLE);
            if (emptyContainer != null) emptyContainer.setVisibility(View.VISIBLE);
            if (rvGames != null) rvGames.setVisibility(View.GONE);
        }
        if (bgImage != null) {
            if (show) {
                android.graphics.drawable.Drawable ddraw = bgImage.getDrawable();
                bgImage.setVisibility(ddraw != null ? View.VISIBLE : View.GONE);
            } else {
                bgImage.setVisibility(View.GONE);
            }
        }
        applyFullscreen();
    }

    private boolean isHomeVisible() {
        return homeContainer == null || homeContainer.getVisibility() == View.VISIBLE;
    }

    private void shutdownVmToHome() {
        pendingGameUri = null;
        try {
            getWindow().getDecorView().removeCallbacks(pendingLaunchRunnable);
        } catch (Throwable ignored) {}
        stopEmuThread();
        m_szGamefile = "";
        showHome(true);
        lastInput = InputSource.TOUCH;
        lastTouchTimeMs = System.currentTimeMillis();
        setOnScreenControlsVisible(false);
        applyFullscreen();
        requestControllerRumble(0f, 0f);
        isVmPaused = false;
        updatePauseButtonIcon();
        setFastForwardEnabled(false);
    }

    private void enforceTouchControlsPolicy() {
        if (!disableTouchControls) {
            return;
        }
        setOnScreenControlsVisible(false);
        View joystick = findViewById(R.id.joystick_left);
        if (joystick != null) joystick.setVisibility(View.GONE);
        View joystickRight = findViewById(R.id.joystick_right);
        if (joystickRight != null) joystickRight.setVisibility(View.GONE);
        View dpad = findViewById(R.id.dpad_view);
        if (dpad != null) dpad.setVisibility(View.GONE);
        View padLeft = findViewById(R.id.ll_pad_select_start);
        if (padLeft != null) padLeft.setVisibility(View.GONE);
        View padRight = findViewById(R.id.ll_pad_right);
        if (padRight != null) padRight.setVisibility(View.GONE);
        View leftShoulders = findViewById(R.id.ll_pad_shoulders_left);
        if (leftShoulders != null) leftShoulders.setVisibility(View.GONE);
        View rightShoulders = findViewById(R.id.ll_pad_shoulders_right);
        if (rightShoulders != null) rightShoulders.setVisibility(View.GONE);
        hideDrawerToggle();
        setFastForwardEnabled(false);
        if (inGameDrawer != null) {
            try {
                inGameDrawer.closeDrawer(GravityCompat.START);
            } catch (Throwable ignored) {}
            inGameDrawer.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED);
        }
    }

    private static final String PREF_BG_L = "bg_landscape";
    private static final String PREF_BG_P = "bg_portrait";
    private void pickBackgroundImage(boolean portrait) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        i.setType("image/*");
        i.putExtra("PORTRAIT_BG", portrait);
        startActivityResultPickBg.launch(i);
    }
    private final ActivityResultLauncher<Intent> startActivityResultPickBg = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(), result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Intent data = result.getData();
                    Uri img = data.getData();
                    boolean portrait = data.getBooleanExtra("PORTRAIT_BG", false);
                    if (img != null) {
                        try {
                            final int takeFlags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                            getContentResolver().takePersistableUriPermission(img, takeFlags);
                        } catch (SecurityException ignored) {}
                        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                                .putString(portrait ? PREF_BG_P : PREF_BG_L, img.toString())
                                .apply();
                        applySavedBackground();
                    }
                }
            });
    private void applySavedBackground() {
        if (bgImage == null) return;
        android.content.SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
        String l = sp.getString(PREF_BG_L, null);
        String p = sp.getString(PREF_BG_P, null);
        boolean isPortrait = getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT;
        String use = isPortrait ? (p != null ? p : l) : (l != null ? l : p);
        if (use == null || use.isEmpty()) { bgImage.setImageDrawable(null); bgImage.setVisibility(View.GONE); return; }
        try (InputStream is = getContentResolver().openInputStream(Uri.parse(use))) {
            if (is != null) {
                android.graphics.Bitmap bmp = android.graphics.BitmapFactory.decodeStream(is);
                if (bmp != null) {
                    bgImage.setImageBitmap(bmp);
                    bgImage.setVisibility(View.VISIBLE);
                    if (android.os.Build.VERSION.SDK_INT >= 31) {
                        try {
                            bgImage.setRenderEffect(android.graphics.RenderEffect.createBlurEffect(0f, 8f, android.graphics.Shader.TileMode.CLAMP));
                        } catch (Throwable ignored) {}
                    }
                }
            }
        } catch (Exception ignored) {}
    }
    private void clearBackgroundImages() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(PREF_BG_L).remove(PREF_BG_P).apply();
        if (bgImage != null) { bgImage.setImageDrawable(null); bgImage.setVisibility(View.GONE); }
        try { Toast.makeText(this, R.string.home_background_cleared, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
    }

    private void onGameSelected(GameEntry entry) {
        launchGameWithPreflight(entry.uri);
    }

    // Cheap but effective: if emulator isn't running yet, boot BIOS first, then load the game like the File button flow.
    private void launchGameWithPreflight(@NonNull Uri uri) {
        applyPerGameSettingsForUri(uri);
        if (isThread()) {
            handleSelectedGameUri(uri);
            return;
        }
        // Start BIOS first
        try { Toast.makeText(this, R.string.home_preflight_boot_bios, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
        pendingGameUri = uri;
        pendingLaunchRetries = 0;
        bootBios();
        getWindow().getDecorView().postDelayed(pendingLaunchRunnable, 900);
        schedulePreflightFallback();
    }

    private final Runnable pendingLaunchRunnable = new Runnable() {
        @Override public void run() {
            if (pendingGameUri == null) return;
            try { Toast.makeText(MainActivity.this, R.string.home_preflight_launch_selected_game, Toast.LENGTH_SHORT).show(); } catch (Throwable ignored) {}
            Uri toLaunch = pendingGameUri;
            pendingGameUri = null;
            handleSelectedGameUri(toLaunch);
        }
    };

    private void schedulePreflightFallback() {
        try {
            getWindow().getDecorView().postDelayed(() -> {
                if (pendingGameUri != null) {
                    Uri toLaunch = pendingGameUri;
                    pendingGameUri = null;
                    handleSelectedGameUri(toLaunch);
                }
            }, 2000);
        } catch (Throwable ignored) {}
    }

    private void bootBios() {
        m_szGamefile = "";
        showHome(false);
        try { setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR); } catch (Throwable ignored) {}
        try {
            lastInput = InputSource.TOUCH;
            lastTouchTimeMs = System.currentTimeMillis();
            setOnScreenControlsVisible(true);
        } catch (Throwable ignored) {}
        if (isThread()) {
            restartEmuThread();
        } else {
            startEmuThread();
        }
    }

    private android.graphics.Bitmap loadHeaderBitmapFromAssets() {
        try (java.io.InputStream is = getAssets().open("icon.png")) {
            return android.graphics.BitmapFactory.decodeStream(is);
        } catch (Exception ignored) {
            try (java.io.InputStream is2 = getAssets().open("app_icons/icon.png")) {
                return android.graphics.BitmapFactory.decodeStream(is2);
            } catch (Exception ignored2) { return null; }
        }
    }


    private android.graphics.Bitmap loadHeaderBlurBitmapFromAssets() {
        try (java.io.InputStream is = getAssets().open("app_icons/icon-old.png")) {
            return android.graphics.BitmapFactory.decodeStream(is);
        } catch (Exception ignored) {
            try (java.io.InputStream is2 = getAssets().open("app_icons/icon.png")) {
                return android.graphics.BitmapFactory.decodeStream(is2);
            } catch (Exception ignored2) {
                try (java.io.InputStream is3 = getAssets().open("icon.png")) {
                    return android.graphics.BitmapFactory.decodeStream(is3);
                } catch (Exception ignored3) { return null; }
            }
        }
    }

    // Recycler adapter

}
