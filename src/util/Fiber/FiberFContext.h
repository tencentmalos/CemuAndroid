#pragma once

#include <async_simple/uthread/internal/thread_impl.h>
#include <functional>

namespace fcontext = async_simple::uthread::internal;

class Fiber
{
  public:
	Fiber(void (*FiberEntryPoint)(void* userParam), void* userParam, void* privateData);
	~Fiber();

	static Fiber* PrepareCurrentThread(void* privateData = nullptr);
	static void SetThreadInitCallback(std::function<void()> callback);
	static void Switch(Fiber& targetFiber);
	static void RunOnThreadFiber(std::function<void()> callback);
	static void* GetFiberPrivateData();

  private:
	static void Start(fcontext::transfer_t transfer);
	Fiber(void* privateData); // fiber from current thread

	fcontext::fcontext_t m_context{};

	void (*m_entryPoint)(void* userParam){};
	void* m_userParam{};

	void* m_privateData;

	void* m_stackPtr{};

	Fiber* m_prevFiber{};
};
