package com.altillimity.satdump

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
        private const val ROOT_LABEL = "SatDump" // 截图左侧显示的应用名称
        
        // 定义查询的列
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

    // 1. 告诉系统你暴露的根目录信息 (截图左侧的列表项)
    override fun queryRoots(projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)
        val row = result.newRow()
        
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID)
        row.add(Root.COLUMN_SUMMARY, context?.packageName) // 显示包名，类似截图左侧的副标题
        row.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_CREATE or Root.FLAG_SUPPORTS_SEARCH)
        row.add(Root.COLUMN_TITLE, ROOT_LABEL)
        row.add(Root.COLUMN_MIME_TYPES, "*/*")
        row.add(Root.COLUMN_ICON, R.mipmap.ic_launcher) // 使用你项目的应用图标
        row.add(Root.COLUMN_DOCUMENT_ID, ROOT_ID)
        
        return result
    }

    // 2. 当你点击左侧的 SatDump 后，右侧显示的目录列表
    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val parent = getFileForDocId(parentDocumentId)
        
        parent.listFiles()?.forEach { file ->
            includeFile(result, file)
        }
        return result
    }

    // 3. 查询单个文档的元数据
    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        includeFile(result, getFileForDocId(documentId))
        return result
    }

    // 4. 让其他应用能够读取/写入文件
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
        if (documentId.contains("/") || documentId.contains("..")) {
            throw FileNotFoundException("Invalid document ID")
        }
        return File(context!!.filesDir, documentId)
    }

    // 将 File 对象的数据填入 Cursor 行中
    private fun includeFile(cursor: MatrixCursor, file: File) {
        // 为了简洁，直接用文件名作为 DocumentId (因为只暴露 filesDir 下的文件)
        val documentId = file.name
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
        }
        row.add(Document.COLUMN_FLAGS, flags)
    }
}