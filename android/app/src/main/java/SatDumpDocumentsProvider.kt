import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileNotFoundException

class SatDumpDocumentsProvider : DocumentsProvider() {

    companion object {
        private const val ROOT_ID = "satdump_root"
        
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

    private fun getMimeType(file: File): String {
        if (file.isDirectory) {
            return Document.MIME_TYPE_DIR
        }
        val name = file.name
        val lastDot = name.lastIndexOf('.')
        if (lastDot >= 0) {
            val extension = name.substring(lastDot + 1).lowercase()
            val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension)
            if (mime != null) {
                return mime
            }
        }
        return "application/octet-stream"
    }

    private fun getFileForDocId(documentId: String): File {
        if (documentId == ROOT_ID || documentId.isEmpty()) {
            return context!!.filesDir
        }
        if (documentId.contains("..")) {
            throw FileNotFoundException("Invalid document ID")
        }
        return File(context!!.filesDir, documentId)
    }

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)
        val row = result.newRow()
        
        val appLabel = context?.applicationInfo?.loadLabel(context!!.packageManager)?.toString() ?: "SatDump"
        
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID)
        row.add(Root.COLUMN_SUMMARY, context?.packageName)
        row.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_IS_CHILD)
        row.add(Root.COLUMN_TITLE, appLabel)
        row.add(Root.COLUMN_MIME_TYPES, "*/*")
        row.add(Root.COLUMN_ICON, context?.applicationInfo?.icon) 
        row.add(Root.COLUMN_DOCUMENT_ID, ROOT_ID)
        
        return result
    }

    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val parent = getFileForDocId(parentDocumentId)
        
        parent.listFiles()?.forEach { file ->
            val docId = if (parentDocumentId == ROOT_ID) file.name else "$parentDocumentId/${file.name}"
            includeFile(result, docId, file)
        }
        return result
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val file = getFileForDocId(documentId)
        includeFile(result, documentId, file)
        return result
    }

    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor {
        val file = getFileForDocId(documentId)
        val accessMode = ParcelFileDescriptor.parseMode(mode)
        return ParcelFileDescriptor.open(file, accessMode)
    }

    override fun getDocumentType(documentId: String): String {
        return getMimeType(getFileForDocId(documentId))
    }

    private fun includeFile(cursor: MatrixCursor, documentId: String, file: File) {
        val row = cursor.newRow()
        row.add(Document.COLUMN_DOCUMENT_ID, documentId)
        row.add(Document.COLUMN_DISPLAY_NAME, file.name)
        row.add(Document.COLUMN_SIZE, file.length())
        row.add(Document.COLUMN_LAST_MODIFIED, file.lastModified())
        row.add(Document.COLUMN_MIME_TYPE, getMimeType(file))
        
        var flags = 0
        if (file.isDirectory) {
            flags = flags or Document.FLAG_DIR_SUPPORTS_CREATE
        } else {
            flags = flags or Document.FLAG_SUPPORTS_WRITE
        }
        
        if (file.parentFile?.canWrite() == true) {
            flags = flags or Document.FLAG_SUPPORTS_DELETE
            flags = flags or Document.FLAG_SUPPORTS_RENAME
        }
        
        row.add(Document.COLUMN_FLAGS, flags)
    }

    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        val parent = getFileForDocId(parentDocumentId)
        val newFile = File(parent, displayName)
        val success = if (mimeType == Document.MIME_TYPE_DIR) {
            newFile.mkdirs()
        } else {
            try {
                newFile.createNewFile()
            } catch (e: Exception) {
                false
            }
        }
        if (success) {
            return if (parentDocumentId == ROOT_ID) newFile.name else "$parentDocumentId/${newFile.name}"
        }
        throw FileNotFoundException("Failed to create document")
    }

    override fun deleteDocument(documentId: String) {
        val file = getFileForDocId(documentId)
        if (!file.delete()) {
            throw FileNotFoundException("Failed to delete document")
        }
    }

    override fun renameDocument(documentId: String, displayName: String): String {
        val file = getFileForDocId(documentId)
        val newFile = File(file.parentFile, displayName)
        if (file.renameTo(newFile)) {
            val parentId = documentId.substringBeforeLast('/', "")
            return if (parentId.isEmpty() || parentId == ROOT_ID) newFile.name else "$parentId/${newFile.name}"
        }
        throw FileNotFoundException("Failed to rename document")
    }
}