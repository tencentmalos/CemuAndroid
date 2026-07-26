#include "Cafe/CafeSystem.h"
#include "JNIUtils.h"
#include "spatial/core/FoundationApiVersion.h"
#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/debugbus/DumpsysBridge.h"

#include <sstream>
#include <vector>

#define CEMU_STRINGIFY_IMPL(value) #value
#define CEMU_STRINGIFY(value) CEMU_STRINGIFY_IMPL(value)

static_assert(SPATIAL_FOUNDATION_API_VERSION >= 1, "Unsupported spatial foundation API version");

namespace
{
	spatial::debugbus::DebugCommandRegistry& GetDebugDumpRegistry()
	{
		static spatial::debugbus::DebugCommandRegistry registry;
		static const bool initialized = [] {
			registry.Register("status", "Dump Cemu Android debug status", [](const std::vector<std::string>&) {
				std::ostringstream out;
				out << "cemu_debug_status:\n";
				out << "native_debugbus=true\n";
				out << "emulator_hash=" << CEMU_STRINGIFY(EMULATOR_HASH) << "\n";
				out << "title_running=" << (CafeSystem::IsTitleRunning() ? "true" : "false") << "\n";
				out << "title_paused=" << (CafeSystem::IsTitlePaused() ? "true" : "false") << "\n";
				return out.str();
			});
			registry.Register("pause", "Pause emulation", [](const std::vector<std::string>&) {
				if (!CafeSystem::IsTitleRunning())
				{
					return std::string{"pause unavailable: no title running\n"};
				}
				if (CafeSystem::IsTitlePaused())
				{
					return std::string{"pause unchanged: title already paused\n"};
				}
				CafeSystem::PauseTitle();
				return CafeSystem::IsTitlePaused() ? std::string{"pause succeeded\n"}
												  : std::string{"pause failed: title remains active\n"};
			});
			registry.Register("resume", "Resume emulation", [](const std::vector<std::string>&) {
				if (!CafeSystem::IsTitleRunning())
				{
					return std::string{"resume unavailable: no title running\n"};
				}
				if (!CafeSystem::IsTitlePaused())
				{
					return std::string{"resume unchanged: title already active\n"};
				}
				CafeSystem::ResumeTitle();
				return CafeSystem::IsTitlePaused() ? std::string{"resume failed: title remains paused\n"}
												  : std::string{"resume succeeded\n"};
			});
			spatial::debugbus::SetDumpsysRegistry(&registry);
			return true;
		}();
		(void)initialized;
		return registry;
	}

	std::vector<std::string> ToStringVector(JNIEnv* env, jobjectArray args)
	{
		std::vector<std::string> out;
		if (args == nullptr)
		{
			return out;
		}

		const jsize count = env->GetArrayLength(args);
		out.reserve(static_cast<size_t>(count));
		for (jsize i = 0; i < count; ++i)
		{
			auto arg = static_cast<jstring>(env->GetObjectArrayElement(args, i));
			out.emplace_back(JNIUtils::FromJString(env, arg));
			env->DeleteLocalRef(arg);
		}
		return out;
	}
} // namespace

extern "C" [[maybe_unused]] JNIEXPORT jstring JNICALL
Java_info_cemu_cemu_nativeinterface_NativeDebugDump_getDebugDump(JNIEnv* env, [[maybe_unused]] jclass clazz,
																 jstring request, jobjectArray args)
{
	const std::string requestString = JNIUtils::FromJString(env, request);
	const std::vector<std::string> requestArgs = ToStringVector(env, args);
	(void)GetDebugDumpRegistry();
	return JNIUtils::ToJString(env, spatial::debugbus::HandleDumpsysRequest(requestString, requestArgs));
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeDebugDump_initialize(JNIEnv*, [[maybe_unused]] jclass clazz)
{
	(void)GetDebugDumpRegistry();
}
