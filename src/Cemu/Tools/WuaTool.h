#pragma once

#include <optional>

namespace WuaTool
{
	bool Create(const fs::path& outputPath,
		const fs::path& baseTitlePath,
		const std::optional<fs::path>& updateTitlePath,
		const std::optional<fs::path>& aocTitlePath,
		const std::optional<fs::path>& updateOverlayPath,
		bool overwrite);

	bool Inspect(const fs::path& inputPath);
}
