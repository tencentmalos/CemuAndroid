#include "Cafe/Diagnostics/GuestExecutableDump.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "config/ActiveSettings.h"

#include <openssl/sha.h>

#include "spatial/debugbus/DebugCommandRegistry.h"

namespace
{
	std::mutex s_dumpMutex;

	std::string JsonEscape(std::string_view value)
	{
		std::string escaped;
		escaped.reserve(value.size());
		for (const char ch : value)
		{
			switch (ch)
			{
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped += ch; break;
			}
		}
		return escaped;
	}

	std::string Sha256Hex(std::span<const uint8> bytes)
	{
		std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
		SHA256(bytes.data(), bytes.size(), digest.data());

		std::string result;
		result.reserve(digest.size() * 2);
		for (const unsigned char value : digest)
			result += fmt::format("{:02x}", value);
		return result;
	}

	bool WriteFile(const fs::path& path, std::span<const uint8> bytes)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			return false;
		stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return stream.good();
	}

	bool WriteTextFile(const fs::path& path, std::string_view text)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			return false;
		stream.write(text.data(), static_cast<std::streamsize>(text.size()));
		return stream.good();
	}

	std::optional<uint32> GetLinkedSectionAddress(const RPLModule& module, uint32 sectionIndex)
	{
		if (sectionIndex >= module.sectionAddressTable2.size())
			return std::nullopt;
		const auto& section = module.sectionTablePtr[sectionIndex];
		const uint32 flags = section.flags;
		if ((flags & 2) == 0 || module.sectionAddressTable2[sectionIndex].ptr == nullptr)
			return std::nullopt;

		if ((flags & SHF_EXECUTE) != 0)
		{
			return module.regionMappingBase_text.GetMPTR() + module.fileInfo.trampolineAdjustment +
				(static_cast<uint32>(section.virtualAddress) - module.regionOrigAddr_text);
		}
		if ((flags & 1) != 0)
		{
			return module.regionMappingBase_data +
				(static_cast<uint32>(section.virtualAddress) - module.regionOrigAddr_data);
		}
		return std::nullopt;
	}

	void AppendModuleJson(std::ostringstream& out, const RPLModule& module)
	{
		out << "    {\n";
		out << "      \"name\": \"" << JsonEscape(module.moduleName) << "\",\n";
		out << "      \"is_rpx\": " << (module.IsRPX() ? "true" : "false") << ",\n";
		out << "      \"patch_crc\": \"" << fmt::format("0x{:08X}", module.patchCRC) << "\",\n";
		out << "      \"entry_point\": \"" << fmt::format("0x{:08X}", RPLLoader_GetModuleEntrypoint(const_cast<RPLModule*>(&module))) << "\",\n";
		out << "      \"text_mapping\": {\"base\": \"" << fmt::format("0x{:08X}", module.regionMappingBase_text.GetMPTR())
			<< "\", \"size\": " << module.regionSize_text << ", \"original_base\": \""
			<< fmt::format("0x{:08X}", module.regionOrigAddr_text) << "\"},\n";
		out << "      \"data_mapping\": {\"base\": \"" << fmt::format("0x{:08X}", module.regionMappingBase_data)
			<< "\", \"size\": " << module.regionSize_data << ", \"original_base\": \""
			<< fmt::format("0x{:08X}", module.regionOrigAddr_data) << "\"},\n";
		out << "      \"sections\": [\n";

		const uint32 sectionCount = module.rplHeader.sectionTableEntryCount;
		for (uint32 index = 0; index < sectionCount; ++index)
		{
			const auto& section = module.sectionTablePtr[index];
			out << "        {\"index\": " << index
				<< ", \"type\": \"" << fmt::format("0x{:08X}", static_cast<uint32>(section.type))
				<< "\", \"flags\": \"" << fmt::format("0x{:08X}", static_cast<uint32>(section.flags))
				<< "\", \"original_address\": \"" << fmt::format("0x{:08X}", static_cast<uint32>(section.virtualAddress))
				<< "\", \"size\": " << static_cast<uint32>(section.sectionSize);
			if (const auto linkedAddress = GetLinkedSectionAddress(module, index))
				out << ", \"linked_address\": \"" << fmt::format("0x{:08X}", *linkedAddress) << "\"";
			out << "}" << (index + 1 == sectionCount ? "\n" : ",\n");
		}
		out << "      ]\n";
		out << "    }";
	}

	std::string BuildMetadata(const CafeSystem::ForegroundExecutableData& executable, std::string_view sha256)
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"schema\": \"cemu.guest-executable.v1\",\n";
		out << "  \"title_id\": \"" << fmt::format("{:016X}", static_cast<uint64>(CafeSystem::GetForegroundTitleId())) << "\",\n";
		out << "  \"title_version\": " << CafeSystem::GetForegroundTitleVersion() << ",\n";
		out << "  \"executable_kind\": \"" << (executable.isBaseVersion ? "base" : "updated") << "\",\n";
		out << "  \"virtual_path\": \"" << JsonEscape(executable.virtualPath) << "\",\n";
		out << "  \"size_bytes\": " << executable.bytes.size() << ",\n";
		out << "  \"sha256\": \"" << sha256 << "\",\n";
		out << "  \"cemu_rpx_hash_base\": \"" << fmt::format("0x{:08X}", CafeSystem::GetRPXHashBase()) << "\",\n";
		out << "  \"cemu_rpx_hash_updated\": \"" << fmt::format("0x{:08X}", CafeSystem::GetRPXHashUpdated()) << "\",\n";
		out << "  \"address_model\": \"guest-effective-address\",\n";
		out << "  \"modules\": [\n";

		RPLModule** modules = RPLLoader_GetModuleList();
		const sint32 moduleCount = RPLLoader_GetModuleCount();
		for (sint32 index = 0; index < moduleCount; ++index)
		{
			AppendModuleJson(out, *modules[index]);
			out << (index + 1 == moduleCount ? "\n" : ",\n");
		}
		out << "  ]\n";
		out << "}\n";
		return out.str();
	}

	std::string DumpExecutable(const std::vector<std::string>& args)
	{
		if (args.size() > 1 || (!args.empty() && args[0] != "updated" && args[0] != "base"))
			return "usage: guest_dump_executable [updated|base]\n";
		if (!CafeSystem::IsTitleRunning() || applicationRPX == nullptr)
			return "guest_dump_executable unavailable: no RPX title running\n";

		std::scoped_lock lock{s_dumpMutex};
		const bool baseVersion = !args.empty() && args[0] == "base";
		auto executable = CafeSystem::ExtractForegroundExecutable(baseVersion);
		if (!executable)
			return "guest_dump_executable failed: mounted executable could not be read\n";

		const std::string sha256 = Sha256Hex(executable->bytes);
		const std::string titleId = fmt::format("{:016X}", static_cast<uint64>(CafeSystem::GetForegroundTitleId()));
		const std::string stem = fmt::format("{}_v{}_{}_{}", titleId, CafeSystem::GetForegroundTitleVersion(),
			baseVersion ? "base" : "updated", sha256.substr(0, 12));
		const fs::path outputDirectory = ActiveSettings::GetUserDataPath("debug/guest/{}", titleId);
		std::error_code error;
		fs::create_directories(outputDirectory, error);
		if (error)
			return fmt::format("guest_dump_executable failed: cannot create {}: {}\n", _pathToUtf8(outputDirectory), error.message());

		const fs::path executablePath = outputDirectory / (stem + ".rpx");
		const fs::path metadataPath = outputDirectory / (stem + ".json");
		if (!WriteFile(executablePath, executable->bytes))
			return fmt::format("guest_dump_executable failed: cannot write {}\n", _pathToUtf8(executablePath));

		const std::string metadata = BuildMetadata(*executable, sha256);
		if (!WriteTextFile(metadataPath, metadata))
			return fmt::format("guest_dump_executable partial: RPX written but metadata failed at {}\n", _pathToUtf8(metadataPath));

		std::ostringstream out;
		out << "guest_dump_executable succeeded\n";
		out << "evidence=exported\n";
		out << "kind=" << (baseVersion ? "base" : "updated") << "\n";
		out << "title_id=" << titleId << "\n";
		out << "title_version=" << CafeSystem::GetForegroundTitleVersion() << "\n";
		out << "module=" << applicationRPX->moduleName << "\n";
		out << "patch_crc=" << fmt::format("0x{:08X}", applicationRPX->patchCRC) << "\n";
		out << "sha256=" << sha256 << "\n";
		out << "size_bytes=" << executable->bytes.size() << "\n";
		out << "executable_path=" << _pathToUtf8(executablePath) << "\n";
		out << "metadata_path=" << _pathToUtf8(metadataPath) << "\n";
		return out.str();
	}

	std::string ListModules(const std::vector<std::string>& args)
	{
		if (!args.empty())
			return "usage: guest_modules\n";
		if (!CafeSystem::IsTitleRunning() || applicationRPX == nullptr)
			return "guest_modules unavailable: no RPX title running\n";

		std::ostringstream out;
		out << "guest_modules:\n";
		out << "address_model=guest-effective-address\n";
		RPLModule** modules = RPLLoader_GetModuleList();
		const sint32 moduleCount = RPLLoader_GetModuleCount();
		out << "count=" << moduleCount << "\n";
		for (sint32 index = 0; index < moduleCount; ++index)
		{
			const RPLModule& module = *modules[index];
			out << fmt::format("module[{}]={} patch_crc=0x{:08X} entry=0x{:08X} text=0x{:08X}+0x{:X} data=0x{:08X}+0x{:X}\n",
				index, module.moduleName, module.patchCRC, RPLLoader_GetModuleEntrypoint(modules[index]),
				module.regionMappingBase_text.GetMPTR(), module.regionSize_text,
				module.regionMappingBase_data, module.regionSize_data);
		}
		return out.str();
	}
}

void GuestExecutableDump::RegisterDebugCommands(spatial::debugbus::DebugCommandRegistry& registry)
{
	registry.Register("guest_dump_executable", "Export the current decrypted Guest RPX and address metadata", DumpExecutable);
	registry.Register("guest_modules", "List loaded Guest modules and linked address ranges", ListModules);
}
