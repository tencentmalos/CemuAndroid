#include "WuaTool.h"

#include "Cafe/Filesystem/fsc.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Common/FileStream.h"
#include "util/crypto/aes128.h"

#include <zarchive/zarchivereader.h>
#include <zarchive/zarchivewriter.h>

namespace WuaTool
{
	namespace
	{
		struct TitleVersion
		{
			TitleId titleId;
			uint16 version;
		};

		const char* GetTitleTypeName(TitleId titleId)
		{
			switch (TitleIdParser(titleId).GetType())
			{
			case TitleIdParser::TITLE_TYPE::BASE_TITLE:
				return "base";
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO:
				return "demo";
			case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE:
				return "update";
			case TitleIdParser::TITLE_TYPE::AOC:
				return "dlc";
			case TitleIdParser::TITLE_TYPE::HOMEBREW:
				return "homebrew";
			case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
				return "system";
			case TitleIdParser::TITLE_TYPE::SYSTEM_DATA:
				return "system-data";
			case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE:
				return "system-overlay";
			default:
				return "unknown";
			}
		}

		void InitializeFilesystem()
		{
			AES128_init();
			fsc_init();
		}

		std::unique_ptr<TitleInfo> LoadTitle(const char* label, const fs::path& inputPath)
		{
			std::error_code ec;
			fs::path absolutePath = fs::absolute(inputPath, ec);
			if (ec)
			{
				std::cerr << "Unable to resolve " << label << " path: " << _pathToUtf8(inputPath) << '\n';
				return nullptr;
			}

			auto titleInfo = std::make_unique<TitleInfo>(absolutePath.lexically_normal());
			if (!titleInfo->IsValid())
			{
				std::cerr << "Unable to open " << label << " title: " << _pathToUtf8(absolutePath) << '\n';
				return nullptr;
			}

			std::cout << label << ": " << fmt::format("{:016x} v{}", titleInfo->GetAppTitleId(), titleInfo->GetAppTitleVersion())
				<< " (" << _pathToUtf8(absolutePath) << ")\n";
			return titleInfo;
		}

		bool ValidateTitleSet(const TitleInfo& baseTitle, const TitleInfo* updateTitle, const TitleInfo* aocTitle)
		{
			const TitleId baseTitleId = baseTitle.GetAppTitleId();
			const auto baseType = TitleIdParser(baseTitleId).GetType();
			if (baseType != TitleIdParser::TITLE_TYPE::BASE_TITLE && baseType != TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO)
			{
				std::cerr << "The --wua-base title is not a base title\n";
				return false;
			}

			if (updateTitle)
			{
				const TitleId updateTitleId = updateTitle->GetAppTitleId();
				if (TitleIdParser(updateTitleId).GetType() != TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE ||
					TitleIdParser::MakeBaseTitleId(updateTitleId) != baseTitleId)
				{
					std::cerr << "The update title does not belong to the selected base title\n";
					return false;
				}
			}

			if (aocTitle)
			{
				const TitleId aocTitleId = aocTitle->GetAppTitleId();
				const TitleId aocBaseTitleId = aocTitleId & ~(0xFFull << 32);
				if (TitleIdParser(aocTitleId).GetType() != TitleIdParser::TITLE_TYPE::AOC || aocBaseTitleId != baseTitleId)
				{
					std::cerr << "The DLC title does not belong to the selected base title\n";
					return false;
				}
			}

			return true;
		}

		bool InspectArchive(const fs::path& inputPath, std::vector<TitleVersion>* titleVersionsOut)
		{
			std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(inputPath));
			if (!reader)
			{
				std::cerr << "Unable to open WUA archive: " << _pathToUtf8(inputPath) << '\n';
				return false;
			}

			const ZArchiveNodeHandle rootDirectory = reader->LookUp("", false, true);
			if (rootDirectory == ZARCHIVE_INVALID_NODE)
			{
				std::cerr << "WUA archive has no root directory\n";
				return false;
			}

			std::vector<TitleVersion> titleVersions;
			for (uint32 index = 0; index < reader->GetDirEntryCount(rootDirectory); ++index)
			{
				ZArchiveReader::DirEntry entry;
				if (!reader->GetDirEntry(rootDirectory, index, entry) || !entry.isDirectory)
				{
					std::cerr << "WUA archive contains an invalid root entry\n";
					return false;
				}

				TitleId folderTitleId;
				uint16 folderVersion;
				if (!TitleInfo::ParseWuaTitleFolderName(entry.name, folderTitleId, folderVersion))
				{
					std::cerr << "Invalid WUA title folder: " << entry.name << '\n';
					return false;
				}

				TitleInfo titleInfo(inputPath, entry.name);
				if (!titleInfo.IsValid())
				{
					std::cerr << "Unable to read title metadata from WUA folder: " << entry.name << '\n';
					return false;
				}

				const TitleId metadataTitleId = titleInfo.GetAppTitleId();
				const uint16 metadataVersion = titleInfo.GetAppTitleVersion();
				if (folderTitleId != metadataTitleId || folderVersion != metadataVersion)
				{
					std::cerr << "WUA folder name and internal app.xml metadata do not match: " << entry.name << '\n';
					return false;
				}

				titleVersions.emplace_back(metadataTitleId, metadataVersion);
			}

			if (titleVersions.empty())
			{
				std::cerr << "WUA archive contains no titles\n";
				return false;
			}

			std::ranges::sort(titleVersions, {}, &TitleVersion::titleId);
			for (const auto& titleVersion : titleVersions)
			{
				std::cout << GetTitleTypeName(titleVersion.titleId) << ": "
					<< fmt::format("{:016x} v{}", titleVersion.titleId, titleVersion.version) << '\n';
			}

			if (titleVersionsOut)
				*titleVersionsOut = std::move(titleVersions);
			return true;
		}

		bool VerifyArchiveOverlay(const fs::path& archivePath, const TitleInfo& titleInfo, const fs::path& overlayPath)
		{
			std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(archivePath));
			if (!reader)
			{
				std::cerr << "Unable to reopen WUA while verifying baked overlay\n";
				return false;
			}

			const std::string archivePrefix = fmt::format("{:016x}_v{}/", titleInfo.GetAppTitleId(), titleInfo.GetAppTitleVersion());
			std::vector<uint8> sourceBuffer(256 * 1024);
			std::vector<uint8> archiveBuffer(sourceBuffer.size());
			uint32 verifiedFiles = 0;
			for (const auto& entry : fs::recursive_directory_iterator(overlayPath))
			{
				if (!entry.is_regular_file())
					continue;
				const fs::path relativePath = fs::relative(entry.path(), overlayPath);
				const std::string archiveFilePath = archivePrefix + relativePath.generic_string();
				const ZArchiveNodeHandle fileHandle = reader->LookUp(archiveFilePath, true, false);
				if (fileHandle == ZARCHIVE_INVALID_NODE || reader->GetFileSize(fileHandle) != entry.file_size())
				{
					std::cerr << "Baked overlay file is missing or has the wrong size: " << archiveFilePath << '\n';
					return false;
				}

				std::ifstream sourceFile(entry.path(), std::ios::binary);
				uint64 readOffset = 0;
				while (sourceFile)
				{
					sourceFile.read(reinterpret_cast<char*>(sourceBuffer.data()), sourceBuffer.size());
					const size_t sourceBytes = sourceFile.gcount();
					if (sourceBytes == 0)
						break;
					const uint64 archiveBytes = reader->ReadFromFile(fileHandle, readOffset, sourceBytes, archiveBuffer.data());
					if (archiveBytes != sourceBytes || !std::ranges::equal(std::span(sourceBuffer.data(), sourceBytes),
						std::span(archiveBuffer.data(), sourceBytes)))
					{
						std::cerr << "Baked overlay content mismatch: " << archiveFilePath << '\n';
						return false;
					}
					readOffset += sourceBytes;
				}
				if (readOffset != entry.file_size())
				{
					std::cerr << "Unable to read overlay source file: " << _pathToUtf8(entry.path()) << '\n';
					return false;
				}
				++verifiedFiles;
			}

			if (verifiedFiles == 0)
			{
				std::cerr << "Update overlay contains no files\n";
				return false;
			}
			std::cout << "Verified baked update overlay: " << verifiedFiles << " files\n";
			return true;
		}

		struct ArchiveWriterContext
		{
			static std::string NormalizeOverlayPath(std::string path)
			{
				std::ranges::transform(path, path.begin(), [](unsigned char character) {
					return static_cast<char>(std::tolower(character));
				});
				return path;
			}

			static void NewOutputFile(int32_t partIndex, void* context)
			{
				auto* writerContext = static_cast<ArchiveWriterContext*>(context);
				if (partIndex != -1 || writerContext->outputFile)
				{
					writerContext->hasError = true;
					return;
				}
				writerContext->outputFile.reset(FileStream::createFile2(writerContext->outputPath));
				if (!writerContext->outputFile)
					writerContext->hasError = true;
			}

			static void WriteOutputData(const void* data, size_t length, void* context)
			{
				auto* writerContext = static_cast<ArchiveWriterContext*>(context);
				if (!writerContext->outputFile || length > std::numeric_limits<sint32>::max() ||
					writerContext->outputFile->writeData(data, static_cast<sint32>(length)) != static_cast<sint32>(length))
				{
					writerContext->hasError = true;
				}
			}

			bool AddHostFile(ZArchiveWriter& writer, const std::string& archivePath, const fs::path& inputPath)
			{
				if (!writer.StartNewFile(archivePath.c_str()))
				{
					std::cerr << "Unable to create overlaid WUA file: " << archivePath << '\n';
					return false;
				}
				std::ifstream inputFile(inputPath, std::ios::binary);
				if (!inputFile)
				{
					std::cerr << "Unable to open update overlay file: " << _pathToUtf8(inputPath) << '\n';
					return false;
				}
				while (inputFile)
				{
					inputFile.read(reinterpret_cast<char*>(transferBuffer.data()), transferBuffer.size());
					const size_t bytesRead = inputFile.gcount();
					if (bytesRead == 0)
						break;
					writer.AppendData(transferBuffer.data(), bytesRead);
					transferredBytes += bytesRead;
					if (hasError)
						return false;
				}
				if (inputFile.bad())
				{
					std::cerr << "Unable to finish reading update overlay file: " << _pathToUtf8(inputPath) << '\n';
					return false;
				}
				return true;
			}

			bool AddDirectory(ZArchiveWriter& writer, const std::string& archivePath, const std::string& fscPath,
				const fs::path* overlayPath, const std::string& relativePath)
			{
				sint32 fscStatus = FSC_STATUS_UNDEFINED;
				std::unique_ptr<FSCVirtualFile> directory(fsc_openDirIterator(fscPath.c_str(), &fscStatus));
				if (!directory)
				{
					std::cerr << "Unable to enumerate mounted title directory: " << fscPath << " (FSC " << fscStatus << ")\n";
					return false;
				}
				if (!writer.MakeDir(archivePath.c_str(), false))
				{
					std::cerr << "Unable to create WUA directory: " << archivePath << '\n';
					return false;
				}

				FSCDirEntry entry;
				while (fsc_nextDir(directory.get(), &entry))
				{
					if (entry.isFile)
					{
						const std::string relativeFilePath = relativePath + entry.path;
						const auto overlayFile = overlayFiles.find(NormalizeOverlayPath(relativeFilePath));
						if (overlayPath && overlayFile != overlayFiles.end())
						{
							if (!AddHostFile(writer, archivePath + entry.path, overlayFile->second))
								return false;
							usedOverlayFiles.emplace(overlayFile->first);
							++transferredFiles;
							continue;
						}
						if (!writer.StartNewFile((archivePath + entry.path).c_str()))
						{
							std::cerr << "Unable to create WUA file: " << archivePath + entry.path << '\n';
							return false;
						}
						std::unique_ptr<FSCVirtualFile> inputFile(fsc_open((fscPath + entry.path).c_str(),
							FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &fscStatus));
						if (!inputFile)
						{
							std::cerr << "Unable to open mounted title file: " << fscPath + entry.path
								<< " (FSC " << fscStatus << ")\n";
							return false;
						}

						uint64 transferredFileBytes = 0;
						while (true)
						{
							const uint32 bytesRead = inputFile->fscReadData(transferBuffer.data(), transferBuffer.size());
							if (bytesRead == 0)
								break;
							writer.AppendData(transferBuffer.data(), bytesRead);
							transferredFileBytes += bytesRead;
							transferredBytes += bytesRead;
							if (hasError)
								return false;
						}
						if (transferredFileBytes != entry.fileSize)
						{
							std::cerr << "Mounted title file ended early: " << fscPath + entry.path << " ("
								<< transferredFileBytes << " of " << entry.fileSize << " bytes)\n";
							return false;
						}
						++transferredFiles;
					}
					else if (entry.isDirectory)
					{
						if (!AddDirectory(writer, fmt::format("{}{}/", archivePath, entry.path), fmt::format("{}{}/", fscPath, entry.path),
							overlayPath, fmt::format("{}{}/", relativePath, entry.path)))
							return false;
					}
					else
					{
						std::cerr << "Unsupported mounted title entry: " << fscPath + entry.path << '\n';
						return false;
					}
				}
				return true;
			}

			bool AddTitle(ZArchiveWriter& writer, TitleInfo& titleInfo, const fs::path* overlayPath = nullptr)
			{
				usedOverlayFiles.clear();
				overlayFiles.clear();
				if (overlayPath)
				{
					for (const auto& entry : fs::recursive_directory_iterator(*overlayPath))
					{
						if (!entry.is_regular_file())
							continue;
						const std::string relativePath = fs::relative(entry.path(), *overlayPath).lexically_normal().generic_string();
						if (!overlayFiles.emplace(NormalizeOverlayPath(relativePath), entry.path()).second)
						{
							std::cerr << "Update overlay contains duplicate case-insensitive path: " << relativePath << '\n';
							return false;
						}
					}
					if (overlayFiles.empty())
					{
						std::cerr << "Update overlay contains no files\n";
						return false;
					}
				}
				const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
				if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE))
				{
					std::cerr << "Unable to mount title for WUA conversion: "
						<< fmt::format("{:016x}", titleInfo.GetAppTitleId()) << '\n';
					return false;
				}
				const std::string archivePath = fmt::format("{:016x}_v{}/", titleInfo.GetAppTitleId(), titleInfo.GetAppTitleVersion());
				bool result = AddDirectory(writer, archivePath, mountPath, overlayPath, "");
				titleInfo.Unmount(mountPath);
				if (!result || !overlayPath)
					return result;

				for (const auto& entry : fs::recursive_directory_iterator(*overlayPath))
				{
					if (!entry.is_regular_file())
						continue;
					const std::string relativePath = fs::relative(entry.path(), *overlayPath).lexically_normal().generic_string();
					const std::string normalizedRelativePath = NormalizeOverlayPath(relativePath);
					if (!usedOverlayFiles.contains(normalizedRelativePath))
					{
						const fs::path relativeFilePath(relativePath);
						const std::string archiveParentPath = archivePath + relativeFilePath.parent_path().generic_string();
						if (!writer.MakeDir(archiveParentPath.c_str(), true) ||
							!AddHostFile(writer, archivePath + relativePath, entry.path()))
						{
							std::cerr << "Unable to add new update overlay file: " << relativePath << '\n';
							return false;
						}
						usedOverlayFiles.emplace(normalizedRelativePath);
						++transferredFiles;
					}
				}
				return result;
			}

			fs::path outputPath;
			std::unique_ptr<FileStream> outputFile;
			std::vector<uint8> transferBuffer = std::vector<uint8>(256 * 1024);
			bool hasError{false};
			uint64 transferredBytes{};
			uint32 transferredFiles{};
			std::set<std::string> usedOverlayFiles;
			std::map<std::string, fs::path> overlayFiles;
		};
	}

	bool Create(const fs::path& outputPath,
		const fs::path& baseTitlePath,
		const std::optional<fs::path>& updateTitlePath,
		const std::optional<fs::path>& aocTitlePath,
		const std::optional<fs::path>& updateOverlayPath,
		bool overwrite)
	{
		InitializeFilesystem();

		auto baseTitle = LoadTitle("base", baseTitlePath);
		auto updateTitle = updateTitlePath ? LoadTitle("update", *updateTitlePath) : nullptr;
		auto aocTitle = aocTitlePath ? LoadTitle("dlc", *aocTitlePath) : nullptr;
		if (!baseTitle || (updateTitlePath && !updateTitle) || (aocTitlePath && !aocTitle))
			return false;
		if (!ValidateTitleSet(*baseTitle, updateTitle.get(), aocTitle.get()))
			return false;
		std::error_code ec;
		std::optional<fs::path> absoluteUpdateOverlayPath;
		if (updateOverlayPath)
		{
			if (!updateTitle)
			{
				std::cerr << "--wua-update-overlay requires --wua-update\n";
				return false;
			}
			absoluteUpdateOverlayPath = fs::absolute(*updateOverlayPath, ec).lexically_normal();
			if (ec || !fs::is_directory(*absoluteUpdateOverlayPath, ec))
			{
				std::cerr << "Update overlay path is not a readable directory\n";
				return false;
			}
			std::cout << "update overlay: " << _pathToUtf8(*absoluteUpdateOverlayPath) << '\n';
		}

		ec.clear();
		fs::path absoluteOutputPath = fs::absolute(outputPath, ec).lexically_normal();
		if (ec)
		{
			std::cerr << "Unable to resolve output path: " << _pathToUtf8(outputPath) << '\n';
			return false;
		}
		if (!boost::iequals(_pathToUtf8(absoluteOutputPath.extension()), ".wua"))
		{
			std::cerr << "Output path must use the .wua extension\n";
			return false;
		}

		fs::path temporaryOutputPath = absoluteOutputPath;
		temporaryOutputPath += ".__tmp";
		if ((!overwrite && fs::exists(absoluteOutputPath, ec)) || (!overwrite && fs::exists(temporaryOutputPath, ec)))
		{
			std::cerr << "Output or temporary output already exists; use --wua-overwrite to replace it\n";
			return false;
		}
		if (overwrite)
		{
			fs::remove(temporaryOutputPath, ec);
			if (ec)
			{
				std::cerr << "Unable to remove stale temporary output: " << ec.message() << '\n';
				return false;
			}
		}
		fs::create_directories(absoluteOutputPath.parent_path(), ec);
		if (ec)
		{
			std::cerr << "Unable to create output directory: " << ec.message() << '\n';
			return false;
		}

		ArchiveWriterContext writerContext;
		writerContext.outputPath = temporaryOutputPath;
		{
			ZArchiveWriter writer(&ArchiveWriterContext::NewOutputFile, &ArchiveWriterContext::WriteOutputData, &writerContext);
			if (writerContext.hasError)
			{
				std::cerr << "Unable to create temporary WUA output\n";
				return false;
			}

			std::cout << "Writing titles to " << _pathToUtf8(temporaryOutputPath) << '\n';
			if (!writerContext.AddTitle(writer, *baseTitle) ||
				(updateTitle && !writerContext.AddTitle(writer, *updateTitle,
					absoluteUpdateOverlayPath ? &*absoluteUpdateOverlayPath : nullptr)) ||
				(aocTitle && !writerContext.AddTitle(writer, *aocTitle)) || writerContext.hasError)
			{
				std::cerr << "Failed while writing WUA title data\n";
				writerContext.outputFile.reset();
				fs::remove(temporaryOutputPath, ec);
				return false;
			}
			writer.Finalize();
			if (writerContext.hasError)
			{
				std::cerr << "Failed while finalizing WUA archive\n";
				writerContext.outputFile.reset();
				fs::remove(temporaryOutputPath, ec);
				return false;
			}
			writerContext.outputFile.reset();
		}

		std::cout << "Verifying WUA internal metadata\n";
		std::vector<TitleVersion> actualTitles;
		if (!InspectArchive(temporaryOutputPath, &actualTitles))
		{
			fs::remove(temporaryOutputPath, ec);
			return false;
		}

		std::vector<TitleVersion> expectedTitles{{baseTitle->GetAppTitleId(), baseTitle->GetAppTitleVersion()}};
		if (updateTitle)
			expectedTitles.emplace_back(updateTitle->GetAppTitleId(), updateTitle->GetAppTitleVersion());
		if (aocTitle)
			expectedTitles.emplace_back(aocTitle->GetAppTitleId(), aocTitle->GetAppTitleVersion());
		std::ranges::sort(expectedTitles, {}, &TitleVersion::titleId);
		if (actualTitles.size() != expectedTitles.size() || !std::ranges::equal(actualTitles, expectedTitles, {},
			&TitleVersion::titleId, &TitleVersion::titleId) ||
			!std::ranges::equal(actualTitles, expectedTitles, {}, &TitleVersion::version, &TitleVersion::version))
		{
			std::cerr << "WUA verification did not find the expected title IDs and versions\n";
			fs::remove(temporaryOutputPath, ec);
			return false;
		}
		if (absoluteUpdateOverlayPath && !VerifyArchiveOverlay(temporaryOutputPath, *updateTitle, *absoluteUpdateOverlayPath))
		{
			fs::remove(temporaryOutputPath, ec);
			return false;
		}

		if (overwrite && fs::exists(absoluteOutputPath, ec))
		{
			fs::remove(absoluteOutputPath, ec);
			if (ec)
			{
				std::cerr << "Unable to replace existing output: " << ec.message() << '\n';
				return false;
			}
		}
		fs::rename(temporaryOutputPath, absoluteOutputPath, ec);
		if (ec)
		{
			std::cerr << "Unable to move verified WUA into place: " << ec.message() << '\n';
			return false;
		}

		std::cout << "Created WUA: " << _pathToUtf8(absoluteOutputPath) << '\n';
		std::cout << "Input: " << writerContext.transferredFiles << " files, " << writerContext.transferredBytes << " bytes\n";
		return true;
	}

	bool Inspect(const fs::path& inputPath)
	{
		InitializeFilesystem();
		std::error_code ec;
		const fs::path absoluteInputPath = fs::absolute(inputPath, ec).lexically_normal();
		if (ec || !fs::is_regular_file(absoluteInputPath, ec))
		{
			std::cerr << "WUA path does not point to a readable file: " << _pathToUtf8(inputPath) << '\n';
			return false;
		}
		std::cout << "WUA: " << _pathToUtf8(absoluteInputPath) << '\n';
		return InspectArchive(absoluteInputPath, nullptr);
	}
}
