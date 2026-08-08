package info.cemu.cemu.common.io

import java.io.FileOutputStream
import java.io.InputStream
import java.nio.file.Path
import java.nio.file.Paths
import java.io.IOException
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

fun unzip(stream: InputStream, targetDir: String) {
    val normalizedTargetDir = Paths.get(targetDir).toAbsolutePath().normalize()
    ZipInputStream(stream).use { zipInputStream ->
        val buffer = ByteArray(8192)

        var zipEntry: ZipEntry? = zipInputStream.nextEntry

        while (zipEntry != null) {
            extractZipEntry(zipInputStream, zipEntry, buffer, normalizedTargetDir)
            zipEntry = zipInputStream.nextEntry
        }
    }
}

fun unzip(stream: InputStream, targetDir: Path) = unzip(stream, targetDir.toString())

private fun extractZipEntry(
    zipInputStream: ZipInputStream,
    zipEntry: ZipEntry,
    buffer: ByteArray,
    targetDir: Path,
) {
    val entryPath = targetDir.resolve(zipEntry.name).normalize()
    if (!entryPath.startsWith(targetDir))
        throw IOException("ZIP entry escapes the target directory")

    val file = entryPath.toFile()
    if (zipEntry.isDirectory) {
        file.apply { if (!isDirectory) mkdirs() }
        return
    }
    file.parentFile?.mkdirs()
    FileOutputStream(file).use { fileOutputStream ->
        var bytesRead: Int
        while ((zipInputStream.read(buffer).also { bytesRead = it }) > 0) {
            fileOutputStream.write(buffer, 0, bytesRead)
        }
    }
}
