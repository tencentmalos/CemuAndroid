package info.cemu.cemu.common.io

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.io.path.exists
import kotlin.io.path.readText

class ZipUtilsTest {
    @Test
    fun unzipExtractsNestedFiles() {
        val targetDir = Files.createTempDirectory("cemu-zip-test")
        try {
            unzip(createZip("nested/file.txt", "driver".encodeToByteArray()).inputStream(), targetDir)

            assertEquals("driver", targetDir.resolve("nested/file.txt").readText())
        } finally {
            targetDir.toFile().deleteRecursively()
        }
    }

    @Test
    fun unzipRejectsEntriesOutsideTargetDirectory() {
        val parentDir = Files.createTempDirectory("cemu-zip-test")
        val targetDir = parentDir.resolve("target")
        Files.createDirectories(targetDir)
        val escapedFile = parentDir.resolve("escaped.txt")
        try {
            assertThrows(IOException::class.java) {
                unzip(createZip("../escaped.txt", "bad".encodeToByteArray()).inputStream(), targetDir)
            }

            assertFalse(escapedFile.exists())
        } finally {
            parentDir.toFile().deleteRecursively()
        }
    }

    private fun createZip(entryName: String, contents: ByteArray): ByteArray {
        val output = ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            zip.putNextEntry(ZipEntry(entryName))
            zip.write(contents)
            zip.closeEntry()
        }
        return output.toByteArray()
    }
}
