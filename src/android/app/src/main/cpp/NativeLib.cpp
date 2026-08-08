#include "Cafe/Diagnostics/CemuDiagnostics.h"
#include "JNIUtils.h"
#include "util/Fiber/Fiber.h"

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, [[maybe_unused]] void* reserved)
{
	JNIUtils::SetJavaVM(vm);
	Fiber::SetThreadInitCallback([]() { JNIUtils::GetEnv(); });
	CemuDiagnostics::Initialize();
	return JNI_VERSION_1_6;
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL JNI_OnUnload([[maybe_unused]] JavaVM* vm, [[maybe_unused]] void* reserved)
{
	CemuDiagnostics::Shutdown();
}
