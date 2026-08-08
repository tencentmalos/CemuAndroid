package info.cemu.cemu.settings.customdrivers

import android.util.Log
import io.ktor.client.HttpClient
import io.ktor.client.request.get
import io.ktor.client.request.header
import io.ktor.client.statement.bodyAsBytes
import io.ktor.http.HttpHeaders
import io.ktor.http.isSuccess
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.IOException

private const val MAX_DRIVER_DOWNLOAD_SIZE = 64L * 1024L * 1024L

private data class DriverSource(
    val name: String,
    val repository: String,
    val recommendedForAdreno8xx: Boolean,
    val assetMatches: (String) -> Boolean,
)

private val DRIVER_SOURCES = listOf(
    DriverSource(
        name = "MrPurple Turnip",
        repository = "MrPurple666/purple-turnip",
        recommendedForAdreno8xx = true,
        assetMatches = { it.endsWith(".adpkg.zip", ignoreCase = true) },
    ),
    DriverSource(
        name = "KIMCHI Turnip (experimental A8xx)",
        repository = "K11MCH1/AdrenoToolsDrivers",
        recommendedForAdreno8xx = false,
        assetMatches = { it.equals("turnip_a8xx.zip", ignoreCase = true) },
    ),
)

@Serializable
private data class GitHubRelease(
    @SerialName("tag_name")
    val tagName: String,
    val name: String? = null,
    val body: String = "",
    val prerelease: Boolean = false,
    @SerialName("published_at")
    val publishedAt: String,
    val assets: List<GitHubAsset>,
)

@Serializable
private data class GitHubAsset(
    val name: String,
    val size: Long,
    @SerialName("browser_download_url")
    val browserDownloadUrl: String,
)

data class RemoteDriver(
    val sourceName: String,
    val repository: String,
    val releaseName: String,
    val releaseNotes: String,
    val publishedAt: String,
    val assetName: String,
    val assetSize: Long,
    val downloadUrl: String,
    val recommendedForAdreno8xx: Boolean,
)

class CustomDriverDownloader(private val client: HttpClient = HttpClient()) {
    private val json = Json { ignoreUnknownKeys = true }

    suspend fun fetchDrivers(): List<RemoteDriver> = coroutineScope {
        DRIVER_SOURCES.map { source ->
            async {
                runCatching { fetchLatestDriver(source) }
                    .onFailure { Log.w(TAG, "Unable to fetch ${source.repository}", it) }
                    .getOrNull()
            }
        }.awaitAll().filterNotNull().sortedByDescending { it.recommendedForAdreno8xx }
    }

    private suspend fun fetchLatestDriver(source: DriverSource): RemoteDriver? {
        val response = client.get(
            "https://api.github.com/repos/${source.repository}/releases?per_page=20"
        ) {
            header(HttpHeaders.Accept, "application/vnd.github+json")
            header(HttpHeaders.UserAgent, "Cemu-Android")
        }
        if (!response.status.isSuccess())
            throw IOException("GitHub returned HTTP ${response.status.value}")

        val releases = json.decodeFromString<List<GitHubRelease>>(response.bodyAsBytes().decodeToString())
        releases.forEach { release ->
            if (release.prerelease)
                return@forEach
            val asset = release.assets.firstOrNull { source.assetMatches(it.name) }
                ?: return@forEach
            if (asset.size <= 0L || asset.size > MAX_DRIVER_DOWNLOAD_SIZE)
                return@forEach
            if (!isExpectedGitHubAsset(source.repository, asset.browserDownloadUrl))
                return@forEach

            return RemoteDriver(
                sourceName = source.name,
                repository = source.repository,
                releaseName = release.name?.takeIf { it.isNotBlank() } ?: release.tagName,
                releaseNotes = release.body,
                publishedAt = release.publishedAt,
                assetName = asset.name,
                assetSize = asset.size,
                downloadUrl = asset.browserDownloadUrl,
                recommendedForAdreno8xx = source.recommendedForAdreno8xx,
            )
        }
        return null
    }

    suspend fun download(driver: RemoteDriver): ByteArray {
        if (!DRIVER_SOURCES.any { it.repository == driver.repository })
            throw IOException("Driver source is not allowlisted")
        if (!isExpectedGitHubAsset(driver.repository, driver.downloadUrl))
            throw IOException("Unexpected driver download URL")
        if (driver.assetSize <= 0L || driver.assetSize > MAX_DRIVER_DOWNLOAD_SIZE)
            throw IOException("Invalid driver download size")

        val response = client.get(driver.downloadUrl) {
            header(HttpHeaders.Accept, "application/octet-stream")
            header(HttpHeaders.UserAgent, "Cemu-Android")
        }
        if (!response.status.isSuccess())
            throw IOException("Driver download returned HTTP ${response.status.value}")

        val bytes = response.bodyAsBytes()
        if (bytes.size.toLong() != driver.assetSize)
            throw IOException("Driver download size does not match GitHub metadata")
        if (bytes.size < 4 || bytes[0] != 'P'.code.toByte() || bytes[1] != 'K'.code.toByte())
            throw IOException("Downloaded driver is not a ZIP archive")
        return bytes
    }

    private fun isExpectedGitHubAsset(repository: String, url: String): Boolean =
        url.startsWith("https://github.com/$repository/releases/download/")

    companion object {
        private const val TAG = "CemuCustomDrivers"
    }
}
