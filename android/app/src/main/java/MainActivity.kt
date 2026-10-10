package com.altillimity.satdump

import android.app.NativeActivity
import android.os.Bundle
import android.content.Context
import android.view.inputmethod.InputMethodManager
import android.view.KeyEvent
import java.util.concurrent.LinkedBlockingQueue
import android.util.Log
import android.content.res.AssetManager
import java.io.*
import java.util.concurrent.atomic.AtomicBoolean

import android.content.Intent;
import android.app.Activity;
import android.net.Uri;

import RealPathUtil;

import android.Manifest;
import android.support.v4.content.PermissionChecker;
import android.support.v4.app.ActivityCompat;
import android.content.pm.PackageManager;
import android.provider.DocumentsContract;

import android.content.BroadcastReceiver;
import android.app.PendingIntent;
import android.content.IntentFilter;

import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import android.widget.RelativeLayout;
import android.widget.EditText;
import android.text.TextWatcher;
import android.text.Editable;
import android.text.InputType;

import android.view.WindowManager;

// Extension on intent
fun Intent?.getFilePath(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, data) ?: "" } ?: ""
}

// Extension on intent
fun Intent?.getFilePathDir(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, DocumentsContract.buildDocumentUriUsingTree(data, DocumentsContract.getTreeDocumentId(data))) ?: "" } ?: ""
}

class MainActivity : NativeActivity(), TextWatcher {
    private val TAG : String = "SatDump";

    // Sentinel character prefixed on the EditText. Kept identical to the original
    // behavior so native code doesn't need any change.
    private val SENTINEL: Char = ' '
    private val KEY_BACKSPACE: Int = 8

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PermissionChecker.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    private var ACTION_USB_PERMISSION = "libusb.android.USB_PERMISSION";

    private var usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (ACTION_USB_PERMISSION == intent.action) {
                synchronized(this) {
                    var _this = context as MainActivity;
                    Log.w(TAG, "Got Intent Reply USB!!!! Reset Activity (libusb bug?)");
                    _this.recreate();
                }
            }
        }
    }

    public var mLayout : ViewGroup? = null;
    public var editText : EditText? = null;
    public var lastFiller : String? = null;
    // Guard against re-entrant afterTextChanged when we call setText() ourselves.
    private var isProcessingText : Boolean = false;

    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.INTERNET);

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        registerReceiver(usbReceiver, filter)

        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Text input hack setup
        mLayout = RelativeLayout(this);
        editText = EditText(this.applicationContext!!);
        mLayout!!.addView(editText, RelativeLayout.LayoutParams(10000, 10000));
        editText!!.setVisibility(View.VISIBLE);
        // TYPE_CLASS_TEXT is required in addition to flags, otherwise some IMEs
        // (and the paste action) will misbehave.
        editText!!.setInputType(
            InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
            InputType.TYPE_TEXT_FLAG_MULTI_LINE
        );
        editText!!.requestFocus();
        editText!!.setText(SENTINEL.toString());
        editText!!.setSelection(1);
        lastFiller = SENTINEL.toString();
        editText!!.addTextChangedListener(this);

        setContentView(mLayout);
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        val aman = getAssets();

        // Extract each directory that ships inside the APK. Missing ones are
        // logged but do not crash the app.
        val assetDirs = arrayOf("resources", "pipelines");
        for (dir in assetDirs) {
            try {
                extractDir(aman, fdir + "/" + dir, dir);
            } catch (e: Exception) {
                Log.e(TAG, "Failed to extract '$dir': ${e.message}");
            }
        }

        try {
            extractFile(aman, fdir + "/satdump_cfg.json", "satdump_cfg.json");
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract satdump_cfg.json: ${e.message}");
        }

        return fdir;
    }

    public fun get_plugins_directory() : String {
        return getApplicationInfo().nativeLibraryDir;
    }

    public fun get_dpi() : Float {
        return getResources().getDisplayMetrics().density;
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.showSoftInput(editText, 0)
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(editText!!.windowToken, 0)
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // ---------------------------------------------------------------------------
    //  Text input handling
    //
    //  Strategy:
    //   - The EditText always keeps a leading SENTINEL character (' ').
    //   - On every change we compare the new text with the previous snapshot and
    //     compute the smallest common prefix. From that we derive whether it was
    //     an append, a delete, or a replace (paste, cut, multi-char delete).
    //   - Appends  -> push new characters in order.
    //   - Deletes  -> push exactly one backspace per removed character.
    //   - Replace  -> push backspaces for the removed tail then the new chars.
    //   - If the sentinel got removed (e.g. user selected everything and pasted
    //     text, or pressed backspace at position 1) we clear the native buffer
    //     first and then feed the whole new content.
    // ---------------------------------------------------------------------------

    override fun afterTextChanged(s : Editable) {
        if (isProcessingText) return;

        val newText = editText!!.text.toString();

        // --- Sentinel lost ---
        if (newText.isEmpty() || newText[0] != SENTINEL) {
            isProcessingText = true;
            try {
                val oldText = lastFiller ?: SENTINEL.toString();
                // Number of characters that native currently holds (excluding sentinel)
                val oldContentLen = if (oldText.length > 0) oldText.length - 1 else 0;

                // Clear whatever native currently has
                repeat(oldContentLen) { unicodeCharacterQueue.offer(KEY_BACKSPACE); }
                // Then push the whole new content
                for (c in newText) {
                    unicodeCharacterQueue.offer(c.code);
                }

                // Restore sentinel in the EditText
                val restored = SENTINEL + newText;
                editText!!.setText(restored);
                editText!!.setSelection(restored.length);
                lastFiller = restored;
            } finally {
                isProcessingText = false;
            }
            return;
        }

        val oldText = lastFiller ?: SENTINEL.toString();
        if (newText == oldText) return;

        // Smallest common prefix
        var p = 0;
        val minLen = Math.min(oldText.length, newText.length);
        while (p < minLen && oldText[p] == newText[p]) p++;

        val oldTail = oldText.substring(p);
        val newTail = newText.substring(p);

        when {
            // Pure append (typing, paste at end)
            newTail.length >= oldTail.length && newTail.startsWith(oldTail) -> {
                for (i in oldTail.length until newTail.length) {
                    unicodeCharacterQueue.offer(newTail[i].code);
                }
            }
            // Pure delete (backspace at end)
            oldTail.length > newTail.length && oldTail.startsWith(newTail) -> {
                repeat(oldTail.length - newTail.length) {
                    unicodeCharacterQueue.offer(KEY_BACKSPACE);
                }
            }
            // Replace (paste over selection, mid-string edit, cut)
            else -> {
                repeat(oldTail.length) { unicodeCharacterQueue.offer(KEY_BACKSPACE); }
                for (c in newTail) {
                    unicodeCharacterQueue.offer(c.code);
                }
            }
        }

        lastFiller = newText;
    }

    override fun beforeTextChanged(s : CharSequence, start: Int, count: Int, after: Int) {
        // Not needed
    }

    override fun onTextChanged(s : CharSequence, start: Int, before: Int, count: Int) {
        // All logic is handled in afterTextChanged to avoid double-counting
        // events fired by certain IMEs during a single key press.
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    // ---------------------------------------------------------------------------
    //  Asset extraction
    // ---------------------------------------------------------------------------

    public fun extractFile(aman: AssetManager, local: String, rsrc: String): Int {
        Log.w(TAG, "Extracting '$rsrc' to '$local'");
        try {
            aman.open(rsrc).use { input ->
                FileOutputStream(local).use { output ->
                    input.copyTo(output);
                }
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to extract file '$rsrc': ${e.message}");
            return -1;
        }
        return 0;
    }

    /**
     * Recursively extracts a directory from the assets.
     *
     * AssetManager.list() returns null for a file, and a (possibly empty) array
     * for a directory. In an APK empty directories are stripped, so any entry
     * that returns an array is a real directory.
     */
    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc) ?: return 0;
        if (flist.isEmpty()) return 0;

        createIfDoesntExist(local);

        var count = 0;
        for (fp in flist) {
            val lpath = "$local/$fp";
            val rpath = "$rsrc/$fp";

            val sublist = aman.list(rpath);
            if (sublist != null && sublist.isNotEmpty()) {
                // Directory -> recurse
                extractDir(aman, lpath, rpath);
            } else {
                // File -> extract
                extractFile(aman, lpath, rpath);
            }
            count++;
        }
        return count;
    }

    public fun createIfDoesntExist(path: String) {
        val folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    // ---------------------------------------------------------------------------
    //  File / directory pickers (unchanged)
    // ---------------------------------------------------------------------------

    var select_file_result : String = "";
    public fun select_file() {
        var file_intent = Intent(Intent.ACTION_GET_CONTENT);
        file_intent.setType("*/*");
        file_intent.addCategory(Intent.CATEGORY_OPENABLE);
        val final_intent = Intent.createChooser(file_intent, "Select File");
        startActivityForResult(final_intent, 1);
    }

    public fun select_file_get() : String {
        var tmp = select_file_result;
        select_file_result = "";
        return tmp;
    }

    var select_directory_result : String = "";
    public fun select_directory() {
        var file_intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        file_intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        file_intent.addCategory(Intent.CATEGORY_DEFAULT);
        val final_intent = Intent.createChooser(file_intent, "Select Directory");
        startActivityForResult(final_intent, 2);
    }

    public fun select_directory_get() : String {
        var tmp = select_directory_result;
        select_directory_result = "";
        return tmp;
    }

    public fun openURL(url: String) {
        val browserIntent = Intent(Intent.ACTION_VIEW, Uri.parse(url));
        startActivity(browserIntent);
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == 1) {
            if(resultCode == RESULT_OK)
                select_file_result = data.getFilePath(getApplicationContext());
            else if(resultCode == RESULT_CANCELED)
                select_file_result = "NO_PATH_SELECTED";
        }

        if (requestCode == 2) {
            if(resultCode == RESULT_OK)
                select_directory_result = data.getFilePathDir(getApplicationContext());
            else if(resultCode == RESULT_CANCELED) // Note: kept for reference, use RESULT_CANCELED below
                select_directory_result = "NO_PATH_SELECTED";
        }
    }
}
