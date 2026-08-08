#include "FiberFContext.h"

namespace
{
	thread_local Fiber* sCurrentFiber = nullptr;
	thread_local Fiber* sThreadFiber = nullptr;
	thread_local bool sThreadFiberCallbackPending = false;
	std::function<void()> sThreadInitCallback;
	struct ThreadFiberCallback
	{
		std::function<void()>* callback;
		Fiber* caller;
	};

	// A guest Fiber can resume on a different PPC host thread. Keep every access
	// behind a noinline boundary so the compiler cannot retain the previous
	// thread's TLS address across _fl_jump_fcontext().
	TLS_WORKAROUND_NOINLINE Fiber* GetCurrentFiber()
	{
		return sCurrentFiber;
	}

	TLS_WORKAROUND_NOINLINE void SetCurrentFiber(Fiber* fiber)
	{
		sCurrentFiber = fiber;
	}

	TLS_WORKAROUND_NOINLINE Fiber* GetThreadFiber()
	{
		return sThreadFiber;
	}

	TLS_WORKAROUND_NOINLINE void SetThreadFiber(Fiber* fiber)
	{
		sThreadFiber = fiber;
	}

	TLS_WORKAROUND_NOINLINE bool IsThreadFiberCallbackPending()
	{
		return sThreadFiberCallbackPending;
	}

	TLS_WORKAROUND_NOINLINE void SetThreadFiberCallbackPending(bool pending)
	{
		sThreadFiberCallbackPending = pending;
	}
}

using namespace fcontext;

Fiber::Fiber(void (*fiberEntryPoint)(void* userParam), void* userParam, void* privateData)
	: m_entryPoint(fiberEntryPoint),
	  m_userParam(userParam),
	  m_privateData(privateData)
{
	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);
	m_context = _fl_make_fcontext(static_cast<uint8*>(m_stackPtr) + stackSize, stackSize, Fiber::Start);
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
}

void Fiber::Start(transfer_t transfer)
{
	auto fiber = static_cast<Fiber*>(transfer.data);
	fiber->m_prevFiber->m_context = transfer.fctx;
	fiber->m_entryPoint(fiber->m_userParam);
}

Fiber::~Fiber()
{
	if (m_stackPtr)
		free(m_stackPtr);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	if (sThreadInitCallback)
		sThreadInitCallback();

	auto* fiber = new Fiber(privateData);
	SetCurrentFiber(fiber);
	SetThreadFiber(fiber);
	return fiber;
}

void Fiber::SetThreadInitCallback(std::function<void()> callback)
{
	sThreadInitCallback = std::move(callback);
}

void Fiber::Switch(Fiber& targetFiber)
{
	Fiber* thisFiber = GetCurrentFiber();
	if (&targetFiber == thisFiber)
		return;

	SetCurrentFiber(&targetFiber);
	targetFiber.m_prevFiber = thisFiber;
	transfer_t transfer = _fl_jump_fcontext(targetFiber.m_context, &targetFiber);

	while (IsThreadFiberCallbackPending())
	{
		auto* request = static_cast<ThreadFiberCallback*>(transfer.data);
		(*request->callback)();
		SetCurrentFiber(request->caller);
		transfer = _fl_jump_fcontext(transfer.fctx, nullptr);
		SetCurrentFiber(thisFiber);
	}

	thisFiber->m_prevFiber->m_context = transfer.fctx;
}

void Fiber::RunOnThreadFiber(std::function<void()> callback)
{
	Fiber* currentFiber = GetCurrentFiber();
	Fiber* threadFiber = GetThreadFiber();
	if (currentFiber == nullptr || currentFiber == threadFiber)
	{
		callback();
		return;
	}

	ThreadFiberCallback request{&callback, currentFiber};
	SetThreadFiberCallbackPending(true);
	SetCurrentFiber(threadFiber);
	_fl_jump_fcontext(threadFiber->m_context, &request);
	SetCurrentFiber(currentFiber);
	SetThreadFiberCallbackPending(false);
}

void* Fiber::GetFiberPrivateData()
{
	return GetCurrentFiber()->m_privateData;
}
