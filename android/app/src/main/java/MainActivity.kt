package com.altillimity.satdump

import android.app.NativeActivity
import android.os.Bundle
import android.content.Context
import android.util.AttributeSet
import android.view.inputmethod.InputMethodManager
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import android.view.inputmethod.EditorInfo
import java.util.concurrent.LinkedBlockingQueue
import android.util.Log
import android.content.res.AssetManager
import java.io.*

import android.content.Intent
import android.net.Uri

import RealPathUtil

import android.Manifest
import android.support.v4.content.PermissionChecker
import android.support.v4.app.ActivityCompat

import android.content.BroadcastReceiver
import android.content.IntentFilter

import android.view.View
import android.view.ViewGroup
import android.view.KeyEvent

import android.widget.RelativeLayout
import android.widget.EditText
import android.text.InputType

import android.view.WindowManager

fun Intent?.getFilePath(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, data) ?: "" } ?: ""
}

fun Intent?.getFilePathDir(context: Context): String {
    return this?.data?.let { data ->
        RealPathUtil.getRealPath(
            context,
            android.provider.DocumentsContract.buildDocumentUriUsingTree(
                data,
                android.provider.DocumentsContract.getTreeDocumentId(data)
            )
        ) ?: ""
    } ?: ""
}

class SatDumpEditText : EditText {
    var onChar: ((Int) -> Unit)? = null

    constructor(context: Context) : super(context)
    constructor(context: Context, attrs: AttributeSet?) : super(context, attrs)
    constructor(context: Context, attrs: AttributeSet?, defStyle: Int) : super(context, attrs, defStyle)

    private fun emit(text: CharSequence?) {
        if (text == null) return
        var i = 0
        while (i < text.length) {
            onChar?.invoke(text[i].toInt())
            i++
        }
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        val base = super.onCreateInputConnection(outAttrs) ?: return null
        return object : InputConnectionWrapper(base, false) {

            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                emit(text)
                return true
            }

            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
                return true
            }

            override fun finishComposingText(): Boolean {
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                var i = 0
                while (i < beforeLength) {
                    onChar?.invoke(8)
                    i++
                }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent?): Boolean {
                if (event == null) return false
                if (event.action == KeyEvent.ACTION_DOWN) {
                    if (handleKey(event.keyCode)) return true
                }
                return false
            }
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            if (handleKey(event.keyCode)) return true
        }
        return super.dispatchKeyEvent(event)
    }

    private fun handleKey(keyCode: Int): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_FORWARD_DEL -> {
                onChar?.invoke(8)
                return true
            }
            KeyEvent.KEYCODE_ENTER -> {
                onChar?.invoke(10)
                return true
            }
            KeyEvent.KEYCODE_TAB -> {
                onChar?.invoke(9)
                return true
            }
        }
        return false
    }
}

class MainActivity : NativeActivity() {
    private val TAG: String = "SatDump"

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PermissionChecker.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1)
        }
    }

    private var ACTION_USB_PERMISSION = "libusb.android.USB_PERMISSION"

    private var usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (ACTION_USB_PERMISSION == intent.action) {
                synchronized(this) {
                    val _this = context as MainActivity
                    Log.w(TAG, "Got Intent Reply USB!!!! Reset Activity (libusb bug?)")
                    _this.recreate()
                }
            }
        }
    }

    public var mLayout: ViewGroup? = null
    public var editText: SatDumpEditText? = null

    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE)
        checkAndAsk(Manifest.permission.INTERNET)

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        registerReceiver(usbReceiver, filter)

        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        )
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        mLayout = RelativeLayout(this)
        val et = SatDumpEditText(this)
        et.onChar = { code -> unicodeCharacterQueue.offer(code) }

        mLayout!!.addView(et, RelativeLayout.LayoutParams(10000, 10000))
        et.setVisibility(View.VISIBLE)
        et.setInputType(
            InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                InputType.TYPE_TEXT_FLAG_MULTI_LINE
        )
        et.setText(" ")
        et.setSelection(1)
        et.requestFocus()

        editText = et
        setContentView(mLayout)
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath()
        val aman = getAssets()

        val assetDirs = arrayOf("resources", "pipelines")
        for (dir in assetDirs) {
            try {
                extractDir(aman, fdir + "/" + dir, dir)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to extract '" + dir + "': " + e.message)
            }
        }

        try {
            extractFile(aman, fdir + "/satdump_cfg.json", "satdump_cfg.json")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract satdump_cfg.json: " + e.message)
        }

        return fdir
    }

    public fun get_plugins_directory(): String {
        return getApplicationInfo().nativeLibraryDir
    }

    public fun get_dpi(): Float {
        return getResources().getDisplayMetrics().density
    }

    fun showSoftInput() {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(editText, 0)
    }

    fun hideSoftInput() {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(editText!!.windowToken, 0)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun extractFile(aman: AssetManager, local: String, rsrc: String): Int {
        Log.w(TAG, "Extracting '" + rsrc + "' -> '" + local + "'")
        try {
            val input = aman.open(rsrc)
            try {
                val output = FileOutputStream(local)
                try {
                    input.copyTo(output)
                } finally {
                    output.close()
                }
            } finally {
                input.close()
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to extract file '" + rsrc + "': " + e.message)
            return -1
        }
        return 0
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc) ?: return 0
        if (flist.size == 0) return 0

        createIfDoesntExist(local)

        var count = 0
        for (fp in flist) {
            val lpath = local + "/" + fp
            val rpath = rsrc + "/" + fp

            val sublist = aman.list(rpath)
            if (sublist != null && sublist.size > 0) {
                extractDir(aman, lpath, rpath)
            } else {
                extractFile(aman, lpath, rpath)
            }
            count++
        }
        return count
    }

    public fun createIfDoesntExist(path: String) {
        val folder = File(path)
        var success = true
        if (!folder.exists()) {
            success = folder.mkdirs()
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path)
        }
    }

    var select_file_result: String = ""
    public fun select_file() {
        val file_intent = Intent(Intent.ACTION_GET_CONTENT)
        file_intent.setType("*/*")
        file_intent.addCategory(Intent.CATEGORY_OPENABLE)
        val final_intent = Intent.createChooser(file_intent, "Select File")
        startActivityForResult(final_intent, 1)
    }

    public fun select_file_get(): String {
        val tmp = select_file_result
        select_file_result = ""
        return tmp
    }

    var select_directory_result: String = ""
    public fun select_directory() {
        val file_intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
        file_intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        file_intent.addCategory(Intent.CATEGORY_DEFAULT)
        val final_intent = Intent.createChooser(file_intent, "Select Directory")
        startActivityForResult(final_intent, 2)
    }

    public fun select_directory_get(): String {
        val tmp = select_directory_result
        select_directory_result = ""
        return tmp
    }

    public fun openURL(url: String) {
        val browserIntent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
        startActivity(browserIntent)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode == 1) {
            if (resultCode == RESULT_OK)
                select_file_result = data.getFilePath(getApplicationContext())
            else if (resultCode == RESULT_CANCELED)
                select_file_result = "NO_PATH_SELECTED"
        }

        if (requestCode == 2) {
            if (resultCode == RESULT_OK)
                select_directory_result = data.getFilePathDir(getApplicationContext())
            else if (resultCode == RESULT_CANCELED)
                select_directory_result = "NO_PATH_SELECTED"
        }
    }
}