package org.satdump.mc

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import java.io.File
import java.io.FileNotFoundException

class SatDumpDocumentsProvider : DocumentsProvider() {

    companion object {
        private const val TAG = "SatDumpProvider"
        private const val ROOT_ID = "satdump_root"
        private const val ROOT_LABEL = "SatDump" 
        
        private val DEFAULT_ROOT_PROJECTION = arrayOf(
            Root.COLUMN_ROOT_ID, Root.COLUMN_MIME_TYPES, Root.COLUMN_FLAGS,
            Root.COLUMN_ICON, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY,
            Root.COLUMN_DOCUMENT_ID
        )
        private val DEFAULT_DOCUMENT_PROJECTION = arrayOf(
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_DISPLAY_NAME, Document.COLUMN_LAST_MODIFIED,
            Document.COLUMN_FLAGS, Document.COLUMN_SIZE
        )
    }

    override fun onCreate(): Boolean {
        return true
    }

    // 告诉系统你暴露的根目录信息 (截图左侧的列表项)
    override fun queryRoots(projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)
        val row = result.newRow()
        
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID)
        row.add(Root.COLUMN_SUMMARY, context?.packageName) // 显示包名，类似截图左侧的副标题
        row.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_CREATE or Root.FLAG_SUPPORTS_SEARCH)
        row.add(Root.COLUMN_TITLE, ROOT_LABEL)
        row.add(Root.COLUMN_MIME_TYPES, "*/*")
        // 使用系统内置的文件夹图标，避免因为默认包找不到 R 类而报错
        row.add(Root.COLUMN_ICON, android.R.drawable.ic_menu_agenda) 
        row.add(Root.COLUMN_DOCUMENT_ID, ROOT_ID)
        
        return result
    }

    // 当用户点开某个文件夹时，返回里面的文件列表
    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val parent = getFileForDocId(parentDocumentId)
        
        parent.listFiles()?.forEach { file ->
            // 如果文件处于隐藏状态，可以选择不显示，这里默认全部显示
            includeFile(result, file, parentDocumentId)
        }
        return result
    }

    // 查询单个文档的元数据
    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val file = getFileForDocId(documentId)
        val parentId = if (file.parentFile == context!!.filesDir) ROOT_ID else file.parentFile?.name ?: ROOT_ID
        includeFile(result, file, parentId)
        return result
    }

    // 让其他应用能够读取/写入文件
    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor {
        val file = getFileForDocId(documentId)
        val accessMode = ParcelFileDescriptor.parseMode(mode)
        return ParcelFileDescriptor.open(file, accessMode)
    }

    // 获取文件的 MIME 类型 (供系统识别)
    override fun getDocumentType(documentId: String): String {
        val file = getFileForDocId(documentId)
        return if (file.isDirectory) Document.MIME_TYPE_DIR else "application/octet-stream"
    }

    // 将 DocumentId 映射到实际的文件路径
    private fun getFileForDocId(documentId: String): File {
        if (documentId == ROOT_ID) {
            // 对应 MainActivity 中 getAppDir() 返回的路径
            return context!!.filesDir
        }
        // 防止路径穿越攻击 (../)
        if (documentId.contains("..")) {
            throw FileNotFoundException("Invalid document ID")
        }
        
        // 因为可能存在子文件夹 (resources/pipelines)，这里直接构建绝对路径
        val file = File(context!!.filesDir, documentId)
        if (!file.exists()) {
            throw FileNotFoundException("File not found: $documentId")
        }
        return file
    }

    // 将 File 对象的数据填入 Cursor 行中
    private fun includeFile(cursor: MatrixCursor, file: File, parentDocumentId: String) {
        // 用相对路径作为 DocumentId，以支持嵌套文件夹
        val documentId = if (parentDocumentId == ROOT_ID) {
            file.name
        } else {
            "$parentDocumentId/${file.name}"
        }
        
        val row = cursor.newRow()
        
        row.add(Document.COLUMN_DOCUMENT_ID, documentId)
        row.add(Document.COLUMN_DISPLAY_NAME, file.name)
        row.add(Document.COLUMN_SIZE, file.length())
        row.add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
        
        var flags = 0
        if (file.isDirectory) {
            row.add(Document.COLUMN_MIME_TYPE, Document.MIME_TYPE_DIR)
            if (file.canWrite()) flags = flags or Document.FLAG_DIR_SUPPORTS_CREATE
        } else {
            row.add(Document.COLUMN_MIME_TYPE, "application/octet-stream")
            if (file.canWrite()) flags = flags or Document.FLAG_SUPPORTS_WRITE
            // 允许删除
            if (file.canWrite()) flags = flags or Document.FLAG_SUPPORTS_DELETE 
        }
        row.add(Document.COLUMN_FLAGS, flags)
    }
}