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

/**
 * 一个"只接收输入、不保存文本"的 EditText。
 *
 * 所有来自 IME 的输入都通过 [onChar] 以 Unicode 码点送出:
 *   - 普通字符 / 粘贴 -> 发送字符本身
 *   - 删除            -> 发送 8 (退格)
 *   - 回车            -> 发送 10 (换行)
 *
 * EditText 内部文本保持不变,所以:
 *   - 光标永远停在原位,方向键不会误触发输入
 *   - 不会出现文本 diff 推断错误
 *   - 复制 / 粘贴正常工作
 */
class SatDumpEditText : EditText {
    // 输出通道:把收到的 Unicode 码点或退格(8)送出去
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

            // 打字 / 粘贴 都在这里进来
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                emit(text)
                return true // 不写入 EditText
            }

            // 拼音 / 预测输入的组合状态:忽略,等 commitText 再发
            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
                return true
            }

            override fun finishComposingText(): Boolean {
                return true
            }

            // 删除键
            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                var i = 0
                while (i < beforeLength) {
                    onChar?.invoke(8)
                    i++
                }
                return true
            }

            // 硬件键盘 / 某些 IME 的按键
            override fun sendKeyEvent(event: KeyEvent?): Boolean {
                if (event == null) return false
                if (event.action == KeyEvent.ACTION_DOWN) {
                    when (event.keyCode) {
                        KeyEvent.KEYCODE_DEL -> {
                            onChar?.invoke(8)
                            return true
                        }
                        KeyEvent.KEYCODE_ENTER -> {
                            onChar?.invoke(10)
                            return true
                        }
                    }
                }
                return false
            }
        }
    }

    // 硬件键盘走这条路径
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            when (event.keyCode) {
                KeyEvent.KEYCODE_DEL -> {
                    onChar?.invoke(8)
                    return true
                }
                KeyEvent.KEYCODE_FORWARD_DEL -> {
                    return true
                }
                KeyEvent.KEYCODE_ENTER -> {
                    onChar?.invoke(10)
                    return true
                }
            }
        }
        return super.dispatchKeyEvent(event)
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

    // 供 native 代码通过 JNI 拉取
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
        // 放一个空格进去,某些 IME 对完全空的输入框会不弹键盘
        et.setText(" ")
        et.setSelection(1)
        et.requestFocus()

        editText = et
        setContentView(mLayout)
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath()
        val aman = getAssets()

        // 解压 APK assets 里的目录 (包含 pipelines)
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

    // ------------------------------------------------------------------
    //  资源解压
    // ------------------------------------------------------------------

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

    // ------------------------------------------------------------------
    //  文件 / 目录选择器
    // ------------------------------------------------------------------

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
