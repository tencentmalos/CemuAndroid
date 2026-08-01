#include "Cafe/Diagnostics/CemuDiagnostics.h"
#include "JNIUtils.h"

#include <vector>

namespace
{
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
	return JNIUtils::ToJString(env, CemuDiagnostics::HandleDebugCommand(requestString, requestArgs));
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeDebugDump_initialize(JNIEnv*, [[maybe_unused]] jclass clazz)
{
	CemuDiagnostics::Initialize();
}
