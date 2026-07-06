#include "Cafe/CafeSystem.h"
#include "JNIUtils.h"
#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/debugbus/DumpsysBridge.h"

#include <sstream>
#include <vector>

#define CEMU_STRINGIFY_IMPL(value) #value
#define CEMU_STRINGIFY(value) CEMU_STRINGIFY_IMPL(value)

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
				return out.str();
			});
			registry.Register("pause", "Pause emulation", [](const std::vector<std::string>&) {
				CafeSystem::PauseTitle();
				return std::string{"pause requested\n"};
			});
			registry.Register("resume", "Resume emulation", [](const std::vector<std::string>&) {
				CafeSystem::ResumeTitle();
				return std::string{"resume requested\n"};
			});
			registry.Register("screenshot", "Capture a screenshot",
							  [](const std::vector<std::string>&) {
								  return std::string{
									  "screenshot unavailable: no Android capture callback registered\n"};
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
	return JNIUtils::ToJString(env, GetDebugDumpRegistry().Handle(requestString, requestArgs));
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeDebugDump_initialize(JNIEnv*, [[maybe_unused]] jclass clazz)
{
	(void)GetDebugDumpRegistry();
}
