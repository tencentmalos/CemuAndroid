#include "Cafe/GuestPatch/GuestPatchModule.h"

namespace GuestPatch
{
	uint32 LoadedModule::SectionVaById(const std::string& id) const
	{
		for (size_t i = 0; i < manifest.sections.size(); i++)
		{
			if (manifest.sections[i].id == id && i < sectionVa.size())
				return sectionVa[i];
		}
		return 0;
	}
}
