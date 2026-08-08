#include "Cafe/HW/Latte/Core/LatteFrameGraphShadow.h"

#include "Cafe/Diagnostics/GuestProfiler.h"
#include "Cafe/Diagnostics/ReflectionDebugDump.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteTextureView.h"

#include "spatial/profiler/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr std::uint32_t kInvalidNode = std::numeric_limits<std::uint32_t>::max();
	constexpr std::size_t kTextureBindingCount = 96;
	constexpr std::size_t kBufferBindingCount = 80;
	constexpr std::size_t kMaxNodes = 8192;
	constexpr std::size_t kMaxAccesses = 196608;
	constexpr std::size_t kMaxEdges = 262144;
	constexpr std::uint64_t kSamplePeriodFrames = 60;

	enum class NodeType : std::uint8_t
	{
		Render,
		Transfer,
		Query,
		Readback,
		Present,
		HardBarrier,
		Count,
	};

	enum class ResourceKind : std::uint8_t
	{
		Surface,
		Buffer,
		GuestMemory,
	};

	enum class AccessMode : std::uint8_t
	{
		Read,
		Write,
		ReadWrite,
	};

	enum class EdgeType : std::uint8_t
	{
		ReadAfterWrite,
		WriteAfterRead,
		WriteAfterWrite,
		ConservativeReadOrder,
		HardBarrier,
		Count,
	};

	enum class FallbackReason : std::uint8_t
	{
		UnresolvedSurfaceAlias,
		MissingSurfaceIdentity,
		RawAddressOnly,
		AttachmentlessRender,
		UnclosedNode,
		AccessOverflow,
		NodeOverflow,
		EdgeOverflow,
		Count,
	};

	struct ResourceKey
	{
		ResourceKind kind{ResourceKind::GuestMemory};
		std::uint64_t identity{};
		std::uint64_t address{};
		std::uint64_t size{};

		bool operator==(const ResourceKey& other) const
		{
			return kind == other.kind && identity == other.identity;
		}
	};

	struct ResourceKeyHash
	{
		std::size_t operator()(const ResourceKey& key) const
		{
			std::size_t hash = std::hash<std::uint64_t>{}(key.identity);
			hash ^= static_cast<std::size_t>(key.kind) << 1;
			return hash;
		}
	};

	struct BoundResource
	{
		ResourceKey key{};
		std::uint64_t attachmentIdentity{};
		bool valid{};
		bool unresolvedAlias{};
		bool missingIdentity{};
	};

	struct ResourceAccess
	{
		ResourceKey key{};
		AccessMode mode{AccessMode::Read};
	};

	struct Node
	{
		NodeType type{NodeType::HardBarrier};
		std::uint32_t guestTag{kInvalidNode};
		std::uint32_t accessStart{};
		std::uint32_t accessCount{};
		std::uint32_t drawCount{};
		std::uint32_t fastDrawCount{};
		std::uint32_t incomingEdges{};
		std::uint32_t attachmentCount{};
		std::uint64_t attachmentSignature{1469598103934665603ull};
		std::uint64_t fallbackMask{};
		bool requiresHardBoundary{};
		bool attachmentFeedback{};
	};

	struct Edge
	{
		std::uint32_t source{};
		std::uint32_t destination{};
		EdgeType type{EdgeType::ReadAfterWrite};
		std::uint64_t resourceIdentity{};
	};

	struct ResourceState
	{
		std::uint32_t lastWriter{kInvalidNode};
		std::uint32_t lastReader{kInvalidNode};
		std::uint64_t generation{};
	};

	struct FrameState
	{
		std::vector<Node> nodes;
		std::vector<ResourceAccess> accesses;
		std::vector<Edge> edges;
		std::unordered_map<ResourceKey, ResourceState, ResourceKeyHash> resources;
		std::unordered_map<std::uint64_t, std::pair<std::uint64_t, bool>> surfaceFamilies;
		std::unordered_set<std::uint32_t> frontier;
		std::array<std::uint64_t, static_cast<std::size_t>(FallbackReason::Count)> fallbackCounts{};
		std::array<std::uint64_t, static_cast<std::size_t>(EdgeType::Count)> edgeCounts{};
		std::array<std::uint64_t, static_cast<std::size_t>(LatteFrameGraphShadow::HardBarrierReason::Count)> hardBarrierCounts{};
		std::uint32_t activeNode{kInvalidNode};
		std::uint32_t lastHardBarrier{kInvalidNode};
		std::uint64_t actualRenderPasses{};
		std::uint64_t actualSubmits{};
		bool frameOpen{};
		bool captureActive{};
	};

	struct Snapshot
	{
		std::uint64_t generation{};
		std::uint64_t frame{};
		std::uint64_t observedFrame{};
		std::array<std::uint64_t, static_cast<std::size_t>(NodeType::Count)> nodeCounts{};
		std::array<std::uint64_t, static_cast<std::size_t>(EdgeType::Count)> edgeCounts{};
		std::array<std::uint64_t, static_cast<std::size_t>(FallbackReason::Count)> fallbackCounts{};
		std::array<std::uint64_t, static_cast<std::size_t>(LatteFrameGraphShadow::HardBarrierReason::Count)> hardBarrierCounts{};
		std::uint64_t nodes{};
		std::uint64_t accesses{};
		std::uint64_t edges{};
		std::uint64_t resources{};
		std::uint64_t resourceVersions{};
		std::uint64_t taggedRenderNodes{};
		std::uint64_t taggedDraws{};
		std::uint64_t attachmentFeedbackNodes{};
		std::uint64_t hardBoundaryNodes{};
		std::uint64_t renderPassCandidates{};
		std::uint64_t renderNodesMergedIntoCandidates{};
		std::uint64_t actualRenderPasses{};
		std::uint64_t actualSubmits{};
		std::uint64_t buildUs{};
	};

	struct FrameGraphShadowStatusPayload
	{
		bool enabled{};
		std::string mode{"shadow_only"};
		std::uint64_t generation{};
		std::uint64_t frames_compiled{};
		std::uint64_t frames_observed{};
		std::uint64_t sample_period_frames{kSamplePeriodFrames};
		std::uint64_t last_frame{};
		std::uint64_t nodes{};
		std::uint64_t render_nodes{};
		std::uint64_t transfer_nodes{};
		std::uint64_t query_nodes{};
		std::uint64_t readback_nodes{};
		std::uint64_t present_nodes{};
		std::uint64_t hard_barrier_nodes{};
		std::uint64_t hard_boundary_nodes{};
		std::uint64_t accesses{};
		std::uint64_t resources{};
		std::uint64_t resource_versions{};
		std::uint64_t edges{};
		std::uint64_t raw_edges{};
		std::uint64_t war_edges{};
		std::uint64_t waw_edges{};
		std::uint64_t read_order_edges{};
		std::uint64_t hard_barrier_edges{};
		std::uint64_t wait_guest_memory_barriers{};
		std::uint64_t guest_memory_write_barriers{};
		std::uint64_t semaphore_barriers{};
		std::uint64_t bottom_of_pipe_barriers{};
		std::uint64_t guest_visibility_barriers{};
		std::uint64_t display_ordinal_barriers{};
		std::uint64_t wait_for_flip_barriers{};
		std::uint64_t surface_sync_barriers{};
		std::uint64_t unknown_command_barriers{};
		std::uint64_t tagged_render_nodes{};
		std::uint64_t tagged_draws{};
		std::uint64_t attachment_feedback_nodes{};
		std::uint64_t render_pass_candidates{};
		std::uint64_t render_nodes_merged_into_candidates{};
		std::uint64_t actual_vulkan_render_passes{};
		std::uint64_t actual_vulkan_submits{};
		std::uint64_t unresolved_alias_fallbacks{};
		std::uint64_t missing_surface_identity_fallbacks{};
		std::uint64_t raw_address_fallbacks{};
		std::uint64_t attachmentless_render_fallbacks{};
		std::uint64_t unclosed_node_fallbacks{};
		std::uint64_t node_overflows{};
		std::uint64_t access_overflows{};
		std::uint64_t edge_overflows{};
		std::uint64_t build_us{};
	};
	REFLECT_CLASS_PROPERTIES(FrameGraphShadowStatusPayload, enabled, mode, generation, frames_compiled,
		frames_observed, sample_period_frames, last_frame,
		nodes, render_nodes, transfer_nodes, query_nodes, readback_nodes, present_nodes,
		hard_barrier_nodes, hard_boundary_nodes, accesses, resources, resource_versions, edges,
		raw_edges, war_edges,
		waw_edges, read_order_edges, hard_barrier_edges, wait_guest_memory_barriers,
		guest_memory_write_barriers, semaphore_barriers, bottom_of_pipe_barriers,
		guest_visibility_barriers, display_ordinal_barriers, wait_for_flip_barriers,
		surface_sync_barriers, unknown_command_barriers, tagged_render_nodes, tagged_draws,
		attachment_feedback_nodes, render_pass_candidates, render_nodes_merged_into_candidates,
		actual_vulkan_render_passes, actual_vulkan_submits, unresolved_alias_fallbacks,
		missing_surface_identity_fallbacks, raw_address_fallbacks,
		attachmentless_render_fallbacks, unclosed_node_fallbacks, node_overflows,
		access_overflows, edge_overflows,
		build_us)

	std::atomic_bool s_enabled{};
	std::atomic_bool s_reinitializeRequested{};
	std::atomic<std::uint64_t> s_generation{1};
	FrameState s_frame;
	std::array<BoundResource, kTextureBindingCount> s_textureBindings{};
	std::array<BoundResource, kBufferBindingCount> s_bufferBindings{};
	std::array<bool, kTextureBindingCount> s_textureBindingActive{};
	std::array<bool, kTextureBindingCount> s_textureBindingListed{};
	std::array<bool, kBufferBindingCount> s_bufferBindingActive{};
	std::array<bool, kBufferBindingCount> s_bufferBindingListed{};
	std::vector<std::uint16_t> s_activeTextureBindings;
	std::vector<std::uint16_t> s_activeBufferBindings;
	std::mutex s_snapshotMutex;
	Snapshot s_snapshot;
	std::uint64_t s_framesCompiled{};
	std::uint64_t s_frameSequence{};
	std::atomic<std::uint64_t> s_framesObserved{};

	void ReserveStorage()
	{
		if (s_frame.nodes.capacity() < kMaxNodes)
			s_frame.nodes.reserve(kMaxNodes);
		if (s_frame.accesses.capacity() < kMaxAccesses)
			s_frame.accesses.reserve(kMaxAccesses);
		if (s_frame.edges.capacity() < kMaxEdges)
			s_frame.edges.reserve(kMaxEdges);
		if (s_frame.resources.bucket_count() < 4096)
			s_frame.resources.reserve(4096);
		if (s_frame.surfaceFamilies.bucket_count() < 512)
			s_frame.surfaceFamilies.reserve(512);
		if (s_frame.frontier.bucket_count() < kMaxNodes)
			s_frame.frontier.reserve(kMaxNodes);
		if (s_activeTextureBindings.capacity() < kTextureBindingCount)
			s_activeTextureBindings.reserve(kTextureBindingCount);
		if (s_activeBufferBindings.capacity() < kBufferBindingCount)
			s_activeBufferBindings.reserve(kBufferBindingCount);
	}

	void ClearBindings()
	{
		s_textureBindings.fill({});
		s_bufferBindings.fill({});
		s_textureBindingActive.fill(false);
		s_textureBindingListed.fill(false);
		s_bufferBindingActive.fill(false);
		s_bufferBindingListed.fill(false);
		s_activeTextureBindings.clear();
		s_activeBufferBindings.clear();
	}

	void ClearFrameGraph()
	{
		ReserveStorage();
		s_frame.nodes.clear();
		s_frame.accesses.clear();
		s_frame.edges.clear();
		s_frame.resources.clear();
		s_frame.surfaceFamilies.clear();
		s_frame.frontier.clear();
		s_frame.fallbackCounts.fill(0);
		s_frame.edgeCounts.fill(0);
		s_frame.hardBarrierCounts.fill(0);
		s_frame.activeNode = kInvalidNode;
		s_frame.lastHardBarrier = kInvalidNode;
		s_frame.actualRenderPasses = 0;
		s_frame.actualSubmits = 0;
		s_frame.frameOpen = true;
		s_frame.captureActive = true;
	}

	bool IsCapturing()
	{
		return s_enabled.load(std::memory_order_relaxed) && s_frame.frameOpen &&
			s_frame.captureActive;
	}

	bool PrepareMutation()
	{
		if (!IsCapturing())
			return false;
		return true;
	}

	void MarkFallback(Node& node, FallbackReason reason)
	{
		const std::size_t index = static_cast<std::size_t>(reason);
		const std::uint64_t bit = 1ull << index;
		if ((node.fallbackMask & bit) != 0)
			return;
		node.fallbackMask |= bit;
		s_frame.fallbackCounts[index]++;
	}

	std::pair<std::uint64_t, bool> ResolveSurfaceFamilyIdentity(const LatteTexture& texture)
	{
		if (texture.diagnosticSurfaceId != 0)
		{
			if (const auto it = s_frame.surfaceFamilies.find(texture.diagnosticSurfaceId);
				it != s_frame.surfaceFamilies.end())
			{
				return it->second;
			}
		}
		constexpr std::size_t kMaxAliasFamilySurfaces = 64;
		std::array<const LatteTexture*, kMaxAliasFamilySurfaces> pending{};
		std::size_t pendingCount = 1;
		std::size_t current = 0;
		bool overflow = false;
		pending[0] = &texture;
		std::uint64_t identity = texture.diagnosticSurfaceId;
		while (current < pendingCount)
		{
			const LatteTexture* member = pending[current++];
			if (member->diagnosticSurfaceId != 0)
				identity = identity == 0 ? member->diagnosticSurfaceId :
					std::min(identity, member->diagnosticSurfaceId);
			for (const LatteTextureRelation* relation : member->list_compatibleRelations)
			{
				if (!relation)
					continue;
				for (const LatteTexture* candidate : {relation->baseTexture, relation->subTexture})
				{
					if (!candidate || std::find(pending.begin(), pending.begin() + pendingCount,
						candidate) != pending.begin() + pendingCount)
					{
						continue;
					}
					if (pendingCount == pending.size())
					{
						overflow = true;
						continue;
					}
					pending[pendingCount++] = candidate;
				}
			}
		}
		const std::pair result{identity, overflow};
		if (!overflow)
		{
			for (std::size_t index = 0; index < pendingCount; ++index)
			{
				if (pending[index]->diagnosticSurfaceId != 0)
					s_frame.surfaceFamilies[pending[index]->diagnosticSurfaceId] = result;
			}
		}
		return result;
	}

	BoundResource MakeSurfaceResource(const LatteTextureView* view)
	{
		if (!view || !view->baseTexture)
			return {};
		const LatteTexture& texture = *view->baseTexture;
		const std::uint64_t address = texture.texDataPtrLow != 0 ? texture.texDataPtrLow : texture.physAddress;
		const std::uint64_t end = texture.texDataPtrHigh >= address ? texture.texDataPtrHigh : address;
		const auto [familyIdentity, aliasOverflow] = ResolveSurfaceFamilyIdentity(texture);
		const std::uint64_t identity = familyIdentity != 0 ? familyIdentity : address;
		std::uint64_t attachmentIdentity = texture.diagnosticSurfaceId != 0 ?
			texture.diagnosticSurfaceId : address;
		attachmentIdentity ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(view->firstMip)) << 1;
		attachmentIdentity ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(view->firstSlice)) << 17;
		attachmentIdentity ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(view->numMip)) << 33;
		attachmentIdentity ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(view->numSlice)) << 41;
		attachmentIdentity ^= static_cast<std::uint64_t>(view->format) << 49;
		return {
			.key = {
				.kind = ResourceKind::Surface,
				.identity = identity,
				.address = address,
				.size = end - address + 1,
			},
			.attachmentIdentity = attachmentIdentity,
			.valid = identity != 0,
			.unresolvedAlias = aliasOverflow,
			.missingIdentity = texture.diagnosticSurfaceId == 0,
		};
	}

	BoundResource MakeMemoryResource(ResourceKind kind, std::uint32_t address, std::uint64_t size)
	{
		if (address == 0 || size == 0)
			return {};
		return {
			.key = {
				.kind = kind,
				.identity = address,
				.address = address,
				.size = size,
			},
			.valid = true,
		};
	}

	void AddEdge(std::uint32_t source, std::uint32_t destination, EdgeType type,
		std::uint64_t resourceIdentity = 0)
	{
		if (source == kInvalidNode || destination == kInvalidNode || source == destination)
			return;
		s_frame.frontier.erase(source);
		s_frame.frontier.insert(destination);
		const std::size_t typeIndex = static_cast<std::size_t>(type);
		s_frame.edgeCounts[typeIndex]++;
		if (destination < s_frame.nodes.size())
			s_frame.nodes[destination].incomingEdges++;
		if (s_frame.edges.size() >= kMaxEdges)
		{
			if (destination < s_frame.nodes.size())
				MarkFallback(s_frame.nodes[destination], FallbackReason::EdgeOverflow);
			return;
		}
		s_frame.edges.push_back({source, destination, type, resourceIdentity});
	}

	void ResolveResourceDependencies(std::uint32_t nodeId);

	std::uint32_t BeginNode(NodeType type, std::uint32_t guestTag = kInvalidNode)
	{
		if (!PrepareMutation())
			return kInvalidNode;
		if (s_frame.activeNode != kInvalidNode)
		{
			Node& previous = s_frame.nodes[s_frame.activeNode];
			previous.requiresHardBoundary = true;
			MarkFallback(previous, FallbackReason::UnclosedNode);
			ResolveResourceDependencies(s_frame.activeNode);
			s_frame.activeNode = kInvalidNode;
		}
		if (s_frame.nodes.size() >= kMaxNodes)
		{
			s_frame.fallbackCounts[static_cast<std::size_t>(FallbackReason::NodeOverflow)]++;
			return kInvalidNode;
		}
		const std::uint32_t nodeId = static_cast<std::uint32_t>(s_frame.nodes.size());
		s_frame.nodes.push_back({
			.type = type,
			.guestTag = guestTag,
			.accessStart = static_cast<std::uint32_t>(s_frame.accesses.size()),
		});
		s_frame.activeNode = nodeId;
		if (s_frame.lastHardBarrier != kInvalidNode)
			AddEdge(s_frame.lastHardBarrier, nodeId, EdgeType::HardBarrier);
		return nodeId;
	}

	void AddAccess(const BoundResource& resource, AccessMode mode)
	{
		if (!resource.valid || s_frame.activeNode == kInvalidNode)
			return;
		Node& node = s_frame.nodes[s_frame.activeNode];
		if (resource.unresolvedAlias)
		{
			node.requiresHardBoundary = true;
			MarkFallback(node, FallbackReason::UnresolvedSurfaceAlias);
		}
		if (resource.missingIdentity)
			MarkFallback(node, FallbackReason::MissingSurfaceIdentity);

		const std::size_t accessEnd = static_cast<std::size_t>(node.accessStart) + node.accessCount;
		for (std::size_t index = node.accessStart; index < accessEnd; ++index)
		{
			ResourceAccess& access = s_frame.accesses[index];
			if (access.key != resource.key)
				continue;
			if (access.mode != mode)
			{
				access.mode = AccessMode::ReadWrite;
				node.attachmentFeedback = resource.key.kind == ResourceKind::Surface;
				node.requiresHardBoundary = true;
			}
			return;
		}

		if (s_frame.accesses.size() >= kMaxAccesses)
		{
			MarkFallback(node, FallbackReason::AccessOverflow);
			node.requiresHardBoundary = true;
			return;
		}
		s_frame.accesses.push_back({resource.key, mode});
		node.accessCount++;
	}

	void AddAttachmentWrite(const BoundResource& resource, std::uint32_t slot)
	{
		if (s_frame.activeNode == kInvalidNode || !resource.valid)
			return;
		Node& node = s_frame.nodes[s_frame.activeNode];
		node.attachmentCount++;
		node.attachmentSignature ^= resource.attachmentIdentity + 0x9E3779B97F4A7C15ull +
			(static_cast<std::uint64_t>(slot) << 32);
		node.attachmentSignature *= 1099511628211ull;
		AddAccess(resource, AccessMode::Write);
	}

	void ResolveResourceDependencies(std::uint32_t nodeId)
	{
		Node& node = s_frame.nodes[nodeId];
		const std::size_t accessEnd = static_cast<std::size_t>(node.accessStart) + node.accessCount;
		for (std::size_t index = node.accessStart; index < accessEnd; ++index)
		{
			const ResourceAccess& access = s_frame.accesses[index];
			ResourceState& state = s_frame.resources[access.key];
			const bool reads = access.mode == AccessMode::Read || access.mode == AccessMode::ReadWrite;
			const bool writes = access.mode == AccessMode::Write || access.mode == AccessMode::ReadWrite;
			if (reads)
			{
				AddEdge(state.lastWriter, nodeId, EdgeType::ReadAfterWrite, access.key.identity);
				AddEdge(state.lastReader, nodeId, EdgeType::ConservativeReadOrder, access.key.identity);
				state.lastReader = nodeId;
			}
			if (writes)
			{
				AddEdge(state.lastWriter, nodeId, EdgeType::WriteAfterWrite, access.key.identity);
				AddEdge(state.lastReader, nodeId, EdgeType::WriteAfterRead, access.key.identity);
				state.lastWriter = nodeId;
				state.lastReader = kInvalidNode;
				state.generation++;
			}
		}
	}

	void CloseActiveNode()
	{
		if (s_frame.activeNode == kInvalidNode)
			return;
		const std::uint32_t nodeId = s_frame.activeNode;
		s_frame.activeNode = kInvalidNode;
		ResolveResourceDependencies(nodeId);
		Node& node = s_frame.nodes[nodeId];
		if (node.requiresHardBoundary || node.type == NodeType::HardBarrier)
		{
			const std::vector<std::uint32_t> frontierBefore(s_frame.frontier.begin(),
				s_frame.frontier.end());
			for (const std::uint32_t source : frontierBefore)
				AddEdge(source, nodeId, EdgeType::HardBarrier);
			s_frame.lastHardBarrier = nodeId;
		}
		s_frame.frontier.insert(nodeId);
	}

	void PublishSnapshot(const Snapshot& snapshot)
	{
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.nodes_per_frame", snapshot.nodes,
			"Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.render_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Render)], "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.transfer_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Transfer)], "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.query_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Query)], "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.readback_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Readback)], "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.present_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Present)], "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.hard_barrier_nodes_per_frame",
			snapshot.nodeCounts[static_cast<std::size_t>(NodeType::HardBarrier)],
			"Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.hard_boundary_nodes_per_frame",
			snapshot.hardBoundaryNodes, "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.accesses_per_frame", snapshot.accesses,
			"Cemu FrameGraph Shadow", "accesses");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.resources_per_frame", snapshot.resources,
			"Cemu FrameGraph Shadow", "resources");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.resource_versions_per_frame",
			snapshot.resourceVersions, "Cemu FrameGraph Shadow", "versions");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.edges_per_frame", snapshot.edges,
			"Cemu FrameGraph Shadow", "edges");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.render_pass_candidates_per_frame",
			snapshot.renderPassCandidates, "Cemu FrameGraph Shadow", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.render_nodes_merged_per_frame",
			snapshot.renderNodesMergedIntoCandidates, "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.actual_vulkan_render_passes_per_frame",
			snapshot.actualRenderPasses, "Cemu FrameGraph Shadow", "passes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.actual_vulkan_submits_per_frame",
			snapshot.actualSubmits, "Cemu FrameGraph Shadow", "submits");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.attachment_feedback_nodes_per_frame",
			snapshot.attachmentFeedbackNodes, "Cemu FrameGraph Shadow", "nodes");
		SPATIAL_PROFILER_COUNTER_SET("cemu.framegraph.shadow.build_us_per_frame", snapshot.buildUs,
			"Cemu FrameGraph Shadow", "us");
	}

	Snapshot CompileSnapshot()
	{
		SPATIAL_PROFILER_AUTO_SCOPE_NAME("latte.framegraph.shadow.compile");
		const auto start = std::chrono::steady_clock::now();
		Snapshot snapshot;
		snapshot.generation = s_generation.load(std::memory_order_relaxed);
		snapshot.frame = ++s_frameSequence;
		snapshot.observedFrame = s_framesObserved.load(std::memory_order_relaxed);
		snapshot.nodes = s_frame.nodes.size();
		snapshot.accesses = s_frame.accesses.size();
		snapshot.edges = s_frame.edges.size();
		snapshot.resources = s_frame.resources.size();
		snapshot.edgeCounts = s_frame.edgeCounts;
		snapshot.fallbackCounts = s_frame.fallbackCounts;
		snapshot.hardBarrierCounts = s_frame.hardBarrierCounts;
		snapshot.actualRenderPasses = s_frame.actualRenderPasses;
		snapshot.actualSubmits = s_frame.actualSubmits;

		bool candidateOpen = false;
		std::uint64_t candidateSignature{};
		for (const Node& node : s_frame.nodes)
		{
			snapshot.nodeCounts[static_cast<std::size_t>(node.type)]++;
			if (node.guestTag != kInvalidNode && node.type == NodeType::Render)
			{
				snapshot.taggedRenderNodes++;
				snapshot.taggedDraws += node.drawCount;
			}
			if (node.attachmentFeedback)
				snapshot.attachmentFeedbackNodes++;
			if (node.requiresHardBoundary || node.type == NodeType::HardBarrier)
				snapshot.hardBoundaryNodes++;
			if (node.type != NodeType::Render)
			{
				candidateOpen = false;
				continue;
			}
			if (node.attachmentCount == 0)
			{
				snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::AttachmentlessRender)]++;
				candidateOpen = false;
				snapshot.renderPassCandidates++;
				continue;
			}
			if (!candidateOpen || node.requiresHardBoundary || node.attachmentSignature != candidateSignature)
			{
				snapshot.renderPassCandidates++;
				candidateSignature = node.attachmentSignature;
				candidateOpen = !node.requiresHardBoundary;
			}
			else
			{
				snapshot.renderNodesMergedIntoCandidates++;
			}
		}
		for (const auto& [resource, state] : s_frame.resources)
			snapshot.resourceVersions += state.generation;
		snapshot.buildUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - start).count());
		return snapshot;
	}
}

void LatteFrameGraphShadow::SetEnabled(bool enabled)
{
	const bool changed = s_enabled.exchange(enabled, std::memory_order_acq_rel) != enabled;
	if (changed)
	{
		s_generation.fetch_add(1, std::memory_order_relaxed);
		s_reinitializeRequested.store(true, std::memory_order_release);
	}
}

bool LatteFrameGraphShadow::IsEnabled()
{
	return s_enabled.load(std::memory_order_relaxed);
}

void LatteFrameGraphShadow::Reset()
{
	s_generation.fetch_add(1, std::memory_order_relaxed);
	s_reinitializeRequested.store(true, std::memory_order_release);
	std::scoped_lock lock{s_snapshotMutex};
	s_snapshot = {};
	s_framesCompiled = 0;
	s_framesObserved.store(0, std::memory_order_relaxed);
}

std::string LatteFrameGraphShadow::GetStatus()
{
	Snapshot snapshot;
	std::uint64_t framesCompiled{};
	{
		std::scoped_lock lock{s_snapshotMutex};
		snapshot = s_snapshot;
		framesCompiled = s_framesCompiled;
	}
	FrameGraphShadowStatusPayload payload;
	payload.enabled = IsEnabled();
	payload.generation = s_generation.load(std::memory_order_relaxed);
	payload.frames_compiled = framesCompiled;
	payload.frames_observed = s_framesObserved.load(std::memory_order_relaxed);
	payload.last_frame = snapshot.frame;
	payload.nodes = snapshot.nodes;
	payload.render_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Render)];
	payload.transfer_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Transfer)];
	payload.query_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Query)];
	payload.readback_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Readback)];
	payload.present_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::Present)];
	payload.hard_barrier_nodes = snapshot.nodeCounts[static_cast<std::size_t>(NodeType::HardBarrier)];
	payload.hard_boundary_nodes = snapshot.hardBoundaryNodes;
	payload.accesses = snapshot.accesses;
	payload.resources = snapshot.resources;
	payload.resource_versions = snapshot.resourceVersions;
	payload.edges = snapshot.edges;
	payload.raw_edges = snapshot.edgeCounts[static_cast<std::size_t>(EdgeType::ReadAfterWrite)];
	payload.war_edges = snapshot.edgeCounts[static_cast<std::size_t>(EdgeType::WriteAfterRead)];
	payload.waw_edges = snapshot.edgeCounts[static_cast<std::size_t>(EdgeType::WriteAfterWrite)];
	payload.read_order_edges = snapshot.edgeCounts[static_cast<std::size_t>(EdgeType::ConservativeReadOrder)];
	payload.hard_barrier_edges = snapshot.edgeCounts[static_cast<std::size_t>(EdgeType::HardBarrier)];
	payload.wait_guest_memory_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::WaitGuestMemory)];
	payload.guest_memory_write_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::GuestMemoryWrite)];
	payload.semaphore_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::Semaphore)];
	payload.bottom_of_pipe_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::BottomOfPipe)];
	payload.guest_visibility_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::GuestVisibility)];
	payload.display_ordinal_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::DisplayOrdinal)];
	payload.wait_for_flip_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::WaitForFlip)];
	payload.surface_sync_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::SurfaceSync)];
	payload.unknown_command_barriers = snapshot.hardBarrierCounts[static_cast<std::size_t>(HardBarrierReason::UnknownCommand)];
	payload.tagged_render_nodes = snapshot.taggedRenderNodes;
	payload.tagged_draws = snapshot.taggedDraws;
	payload.attachment_feedback_nodes = snapshot.attachmentFeedbackNodes;
	payload.render_pass_candidates = snapshot.renderPassCandidates;
	payload.render_nodes_merged_into_candidates = snapshot.renderNodesMergedIntoCandidates;
	payload.actual_vulkan_render_passes = snapshot.actualRenderPasses;
	payload.actual_vulkan_submits = snapshot.actualSubmits;
	payload.unresolved_alias_fallbacks = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::UnresolvedSurfaceAlias)];
	payload.missing_surface_identity_fallbacks = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::MissingSurfaceIdentity)];
	payload.raw_address_fallbacks = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::RawAddressOnly)];
	payload.attachmentless_render_fallbacks = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::AttachmentlessRender)];
	payload.unclosed_node_fallbacks = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::UnclosedNode)];
	payload.access_overflows = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::AccessOverflow)];
	payload.node_overflows = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::NodeOverflow)];
	payload.edge_overflows = snapshot.fallbackCounts[static_cast<std::size_t>(FallbackReason::EdgeOverflow)];
	payload.build_us = snapshot.buildUs;
	return "framegraph_shadow_status:\n" + ReflectionDebugDump::PackScalarObject(payload);
}

void LatteFrameGraphShadow::BeginFrame()
{
	if (!IsEnabled())
		return;
	if (s_reinitializeRequested.exchange(false, std::memory_order_acq_rel))
	{
		ClearBindings();
	}
	const std::uint64_t observedFrame = s_framesObserved.fetch_add(1,
		std::memory_order_relaxed) + 1;
	if (((observedFrame - 1) % kSamplePeriodFrames) != 0)
	{
		s_frame.frameOpen = false;
		s_frame.captureActive = false;
		return;
	}
	ClearFrameGraph();
}

void LatteFrameGraphShadow::EndFrame()
{
	if (!IsCapturing())
		return;
	CloseActiveNode();
	const Snapshot snapshot = CompileSnapshot();
	PublishSnapshot(snapshot);
	{
		std::scoped_lock lock{s_snapshotMutex};
		s_snapshot = snapshot;
		s_framesCompiled++;
	}
	s_frame.frameOpen = false;
}

void LatteFrameGraphShadow::BeginRenderNode(std::uint32_t guestTag, bool fastDraw,
	std::uint32_t drawCount)
{
	if (!IsCapturing())
		return;
	(void)drawCount;
	if (BeginNode(NodeType::Render, guestTag) == kInvalidNode)
		return;
	Node& node = s_frame.nodes[s_frame.activeNode];
	node.drawCount = 1;
	node.fastDrawCount = fastDraw ? 1 : 0;
	if (!fastDraw)
		ClearBindings();
}

void LatteFrameGraphShadow::RecordSurfaceBinding(const LatteTextureView* view,
	std::uint32_t bindingSlot)
{
	if (!IsCapturing() || bindingSlot >= s_textureBindings.size())
		return;
	s_textureBindings[bindingSlot] = MakeSurfaceResource(view);
	s_textureBindingActive[bindingSlot] = s_textureBindings[bindingSlot].valid;
	if (s_textureBindingActive[bindingSlot] && !s_textureBindingListed[bindingSlot])
	{
		s_textureBindingListed[bindingSlot] = true;
		s_activeTextureBindings.push_back(static_cast<std::uint16_t>(bindingSlot));
	}
}

void LatteFrameGraphShadow::RecordBufferBinding(std::uint32_t bindingSlot,
	std::uint32_t address, std::uint32_t size)
{
	if (!IsCapturing() || bindingSlot >= s_bufferBindings.size())
		return;
	s_bufferBindings[bindingSlot] = MakeMemoryResource(ResourceKind::Buffer, address, size);
	s_bufferBindingActive[bindingSlot] = s_bufferBindings[bindingSlot].valid;
	if (s_bufferBindingActive[bindingSlot] && !s_bufferBindingListed[bindingSlot])
	{
		s_bufferBindingListed[bindingSlot] = true;
		s_activeBufferBindings.push_back(static_cast<std::uint16_t>(bindingSlot));
	}
}

void LatteFrameGraphShadow::RecordBufferRead(std::uint32_t address, std::uint32_t size)
{
	if (!IsCapturing())
		return;
	AddAccess(MakeMemoryResource(ResourceKind::Buffer, address, size), AccessMode::Read);
}

void LatteFrameGraphShadow::EndRenderNode()
{
	if (!IsCapturing() || s_frame.activeNode == kInvalidNode ||
		s_frame.nodes[s_frame.activeNode].type != NodeType::Render)
	{
		return;
	}
	for (const std::uint16_t bindingSlot : s_activeTextureBindings)
	{
		if (s_textureBindingActive[bindingSlot])
			AddAccess(s_textureBindings[bindingSlot], AccessMode::Read);
	}
	for (const std::uint16_t bindingSlot : s_activeBufferBindings)
	{
		if (s_bufferBindingActive[bindingSlot])
			AddAccess(s_bufferBindings[bindingSlot], AccessMode::Read);
	}
	for (std::uint32_t index = 0; index < 8; ++index)
		AddAttachmentWrite(MakeSurfaceResource(LatteMRT::GetColorAttachment(index)), index);
	AddAttachmentWrite(MakeSurfaceResource(LatteMRT::GetDepthAttachment()), 8);
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordTransfer(std::uint32_t sourceAddress, std::uint64_t sourceSize,
	std::uint32_t destinationAddress, std::uint64_t destinationSize)
{
	if (BeginNode(NodeType::Transfer, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	Node& node = s_frame.nodes[s_frame.activeNode];
	MarkFallback(node, FallbackReason::RawAddressOnly);
	AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, sourceAddress, sourceSize), AccessMode::Read);
	AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, destinationAddress, destinationSize), AccessMode::Write);
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordClear(std::uint32_t colorAddress, std::uint64_t colorSize,
	std::uint32_t depthAddress, std::uint64_t depthSize, std::uint32_t clearMask)
{
	if (BeginNode(NodeType::Transfer, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	Node& node = s_frame.nodes[s_frame.activeNode];
	MarkFallback(node, FallbackReason::RawAddressOnly);
	if ((clearMask & 1) != 0)
		AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, colorAddress, colorSize), AccessMode::Write);
	if ((clearMask & 6) != 0)
		AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, depthAddress, depthSize), AccessMode::Write);
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordQuery(std::uint32_t address, bool begin)
{
	if (BeginNode(NodeType::Query, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, address, 16),
		begin ? AccessMode::ReadWrite : AccessMode::Write);
	s_frame.nodes[s_frame.activeNode].requiresHardBoundary = true;
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordReadback(const LatteTextureView* view)
{
	if (BeginNode(NodeType::Readback, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	AddAccess(MakeSurfaceResource(view), AccessMode::Read);
	s_frame.nodes[s_frame.activeNode].requiresHardBoundary = true;
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordPresent(std::uint32_t address, std::uint64_t size)
{
	if (BeginNode(NodeType::Present, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	Node& node = s_frame.nodes[s_frame.activeNode];
	MarkFallback(node, FallbackReason::RawAddressOnly);
	AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, address, size), AccessMode::Read);
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordHardBarrier(HardBarrierReason reason,
	std::uint32_t address, std::uint32_t size)
{
	if (BeginNode(NodeType::HardBarrier, GuestProfiler::GetActiveGpuTagSection()) == kInvalidNode)
		return;
	const std::size_t reasonIndex = static_cast<std::size_t>(reason);
	if (reasonIndex < s_frame.hardBarrierCounts.size())
		s_frame.hardBarrierCounts[reasonIndex]++;
	if (address != 0 && size != 0)
		AddAccess(MakeMemoryResource(ResourceKind::GuestMemory, address, size), AccessMode::ReadWrite);
	Node& node = s_frame.nodes[s_frame.activeNode];
	node.requiresHardBoundary = true;
	if (reason == HardBarrierReason::UnknownCommand)
		MarkFallback(node, FallbackReason::RawAddressOnly);
	CloseActiveNode();
}

void LatteFrameGraphShadow::RecordActualRenderPass()
{
	if (PrepareMutation())
		s_frame.actualRenderPasses++;
}

void LatteFrameGraphShadow::RecordActualSubmit()
{
	if (PrepareMutation())
		s_frame.actualSubmits++;
}
