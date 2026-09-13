#pragma once

// GuestPatchSdk -- the Custom SDK v1 Host dispatcher (spec 8).
//
// Builds a single real guest-callable gateway via RPLLoader_MakePPCCallable and
// binds it to sdk_export imports. The gateway reads the r3..r8 register protocol
// (spec 8.1), validates the module context handle and export id against the
// per-module capability whitelist, copies the request into a Host-owned snapshot
// (never retaining a raw guest pointer), dispatches to the service, and returns a
// Status in r3.
//
// All wire layouts are defined once in guest/custom/v1/include/cemu/sdk_v1.h and
// shared with the guest SDK.

#include <cstdint>
#include <string>

namespace GuestPatch
{
	class Sdk
	{
	public:
		static Sdk& Instance();

		// Ensure the dispatcher gateway exists; returns its guest-callable VA.
		// Called by the loader when binding an sdk_export import.
		uint32 GatewayVa();

		// Register a module context handle for an active module generation.
		// Returns the handle the guest passes in r4.
		uint32 RegisterContext(uint32 titleEpoch, uint32 moduleGeneration,
							   uint32 capabilities);
		void UnregisterContext(uint32 handle);

		// Counters for guest_patch_status.
		std::string Status() const;

		void Reset();
	};
}
