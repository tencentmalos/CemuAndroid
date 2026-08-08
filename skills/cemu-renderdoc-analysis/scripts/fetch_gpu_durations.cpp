#include "api/replay/renderdoc_replay.h"
#include "api/replay/pipestate.inl"

#include <cstdio>
#include <map>
#include <set>
#include <string>

// pipestate.inl references RenderDoc's basic integer stringiser, which is not
// exported by the replay dylib. The standalone helper provides the one
// specialization needed by the API-neutral pipeline accessors.
template <>
rdcstr DoStringise(const uint32_t& value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%u", value);
	return rdcstr(buffer);
}

namespace
{
struct ActionInfo
{
	std::string name;
	uint32_t flags{};
	uint32_t depth{};
	uint32_t numIndices{};
	uint32_t numInstances{};
	uint64_t copySource{};
	uint64_t copyDestination{};
};

struct ResourceInfo
{
	std::string description;
};

struct PassInfo
{
	uint32_t beginEvent{};
	uint32_t endEvent{};
	uint32_t firstDrawEvent{};
	uint32_t drawCount{};
	std::string reason{"unknown"};
};

uint64_t GetResourceId(ResourceId resource)
{
	return resource.GetId();
}

void FlattenActions(const rdcarray<ActionDescription>& actions, const SDFile& structuredFile,
	uint32_t depth, std::map<uint32_t, ActionInfo>& output)
{
	for (const ActionDescription& action : actions)
	{
		output.emplace(action.eventId, ActionInfo{
			action.GetName(structuredFile).c_str(), uint32_t(action.flags), depth,
			action.numIndices, action.numInstances,
			GetResourceId(action.copySource), GetResourceId(action.copyDestination)});
		FlattenActions(action.children, structuredFile, depth + 1, output);
	}
}

std::map<uint64_t, ResourceInfo> DescribeResources(IReplayController* controller)
{
	std::map<uint64_t, ResourceInfo> result;
	for (const TextureDescription& texture : controller->GetTextures())
	{
		char description[256];
		std::snprintf(description, sizeof(description),
			"texture:%ux%ux%u,array=%u,mips=%u,samples=%u,bytes=%llu",
			texture.width, texture.height, texture.depth, texture.arraysize, texture.mips,
			texture.msSamp, static_cast<unsigned long long>(texture.byteSize));
		result.emplace(GetResourceId(texture.resourceId), ResourceInfo{description});
	}
	for (const BufferDescription& buffer : controller->GetBuffers())
	{
		char description[128];
		std::snprintf(description, sizeof(description), "buffer:bytes=%llu",
			static_cast<unsigned long long>(buffer.length));
		result.emplace(GetResourceId(buffer.resourceId), ResourceInfo{description});
	}
	return result;
}

const char* GetResourceDescription(const std::map<uint64_t, ResourceInfo>& resources,
	uint64_t resourceId)
{
	const auto iterator = resources.find(resourceId);
	return iterator == resources.end() ? "" : iterator->second.description.c_str();
}

std::string EscapeTsv(const std::string& input)
{
	std::string result;
	result.reserve(input.size());
	for (char character : input)
		result.push_back(character == '\t' || character == '\r' || character == '\n' ? ' ' : character);
	return result;
}

std::string DescribeBoundResource(const BoundResource& resource,
	const std::map<uint64_t, ResourceInfo>& resources)
{
	const uint64_t resourceId = GetResourceId(resource.resourceId);
	if (resourceId == 0)
		return {};
	char suffix[96];
	std::snprintf(suffix, sizeof(suffix), "#%llu[mip=%d,slice=%d]",
		static_cast<unsigned long long>(resourceId), resource.firstMip, resource.firstSlice);
	std::string result = GetResourceDescription(resources, resourceId);
	result += suffix;
	return result;
}

std::vector<PassInfo> BuildPasses(const std::map<uint32_t, ActionInfo>& actions)
{
	std::vector<PassInfo> result;
	PassInfo current;
	bool insidePass = false;
	for (const auto& [eventId, info] : actions)
	{
		if (info.name.find("vkCmdBeginRenderPass") != std::string::npos)
		{
			current = PassInfo{};
			current.beginEvent = eventId;
			insidePass = true;
		}
		if (!insidePass)
			continue;
		if (info.name.find("vkCmdDraw") != std::string::npos)
		{
			if (current.firstDrawEvent == 0)
				current.firstDrawEvent = eventId;
			current.drawCount++;
		}
		static constexpr const char* kReasonPrefix = "cemu.render_pass.end.";
		if (info.name.rfind(kReasonPrefix, 0) == 0)
			current.reason = info.name.substr(std::char_traits<char>::length(kReasonPrefix));
		if (info.name.find("vkCmdEndRenderPass") != std::string::npos)
		{
			current.endEvent = eventId;
			result.emplace_back(std::move(current));
			insidePass = false;
		}
	}
	return result;
}
}

int main(int argc, char** argv)
{
	if (argc != 4 && argc != 5)
	{
		std::fprintf(stderr,
			"usage: %s DEVICE CAPTURE_PATH OUTPUT_TSV\n"
			"       %s DEVICE CAPTURE_PATH --actions-only OUTPUT_TSV\n"
			"       %s DEVICE CAPTURE_PATH --pass-state OUTPUT_TSV\n"
			"       %s DEVICE CAPTURE_PATH --feedback-shaders OUTPUT_TEXT\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}

	GlobalEnvironment environment;
	environment.enumerateGPUs = false;
	RENDERDOC_InitialiseReplay(environment, {});

	IReplayController* controller = nullptr;
	const ResultCode attach = RENDERDOC_AttachRenderdoc(argv[1]);
	if (attach != ResultCode::Succeeded)
	{
		std::fprintf(stderr, "attach failed: %u\n", uint32_t(attach));
		RENDERDOC_ShutdownReplay();
		return 3;
	}

	const ResultCode open = RENDERDOC_OpenAndroidCapture(argv[1], argv[2], &controller);
	if (open != ResultCode::Succeeded || controller == nullptr)
	{
		std::fprintf(stderr, "open failed: %u\n", uint32_t(open));
		RENDERDOC_DetachRenderdoc(argv[1]);
		RENDERDOC_ShutdownReplay();
		return 4;
	}

	std::map<uint32_t, ActionInfo> actions;
	FlattenActions(controller->GetRootActions(), controller->GetStructuredFile(), 0, actions);
	const std::map<uint64_t, ResourceInfo> resources = DescribeResources(controller);
	const char* outputPath = argc == 5 ? argv[4] : argv[3];
	FILE* output = std::fopen(outputPath, "w");
	if (output == nullptr)
	{
		std::perror("fopen");
		RENDERDOC_CloseAndroidCapture(argv[1], controller);
		RENDERDOC_DetachRenderdoc(argv[1]);
		RENDERDOC_ShutdownReplay();
		return 5;
	}

	const bool actionsOnly = argc == 5 && std::string(argv[3]) == "--actions-only";
	const bool passState = argc == 5 && std::string(argv[3]) == "--pass-state";
	const bool feedbackShaders = argc == 5 && std::string(argv[3]) == "--feedback-shaders";
	if (argc == 5 && !actionsOnly && !passState && !feedbackShaders)
	{
		std::fprintf(stderr, "unknown mode: %s\n", argv[3]);
		std::fclose(output);
		RENDERDOC_CloseAndroidCapture(argv[1], controller);
		RENDERDOC_DetachRenderdoc(argv[1]);
		RENDERDOC_ShutdownReplay();
		return 6;
	}

	if (actionsOnly)
	{
		std::fprintf(output,
			"event_id\tflags\tdepth\tcopy_source\tcopy_destination\t"
			"source_description\tdestination_description\tname\n");
		for (const auto& [eventId, info] : actions)
		{
			const std::string name = EscapeTsv(info.name);
			std::fprintf(output, "%u\t%u\t%u\t%llu\t%llu\t%s\t%s\t%s\n",
				eventId, info.flags, info.depth,
				static_cast<unsigned long long>(info.copySource),
				static_cast<unsigned long long>(info.copyDestination),
				GetResourceDescription(resources, info.copySource),
				GetResourceDescription(resources, info.copyDestination), name.c_str());
		}
		std::fprintf(stderr, "wrote %zu actions\n", actions.size());
	}
	else if (feedbackShaders)
	{
		const std::vector<PassInfo> passes = BuildPasses(actions);
		std::set<uint64_t> exportedShaders;
		for (size_t passIndex = 1; passIndex < passes.size(); passIndex++)
		{
			if (passes[passIndex - 1].reason != "self_dependency" ||
				passes[passIndex].firstDrawEvent == 0)
				continue;
			controller->SetFrameEvent(passes[passIndex].firstDrawEvent, true);
			const PipeState& pipeline = controller->GetPipelineState();
			const ResourceId shader = pipeline.GetShader(ShaderStage::Pixel);
			const uint64_t shaderId = GetResourceId(shader);
			if (shaderId == 0 || !exportedShaders.emplace(shaderId).second)
				continue;
			const ShaderReflection* reflection = pipeline.GetShaderReflection(ShaderStage::Pixel);
			if (!reflection)
				continue;
			const rdcstr disassembly = controller->DisassembleShader(
				pipeline.GetGraphicsPipelineObject(), reflection, "");
			std::fprintf(output,
				"===== pixel_shader=%llu event=%u pipeline=%llu =====\n%s\n\n",
				static_cast<unsigned long long>(shaderId), passes[passIndex].firstDrawEvent,
				static_cast<unsigned long long>(GetResourceId(pipeline.GetGraphicsPipelineObject())),
				disassembly.c_str());
		}
		std::fprintf(stderr, "wrote %zu feedback shader disassemblies\n", exportedShaders.size());
	}
	else if (passState)
	{
		std::fprintf(output,
			"pass_index\tbegin_event\tend_event\treason\tdraw_count\tfirst_draw_event\t"
			"num_indices\tnum_instances\tpipeline\tvertex_shader\tgeometry_shader\t"
			"pixel_shader\tviewport_x\tviewport_y\tviewport_width\tviewport_height\t"
			"color_targets\tcolor_write_masks\tdepth_target\tpixel_read_only\tfeedback_targets\tname\n");
		const std::vector<PassInfo> passes = BuildPasses(actions);
		uint32_t passIndex = 0;
		for (const PassInfo& pass : passes)
		{
			passIndex++;
			if (pass.firstDrawEvent == 0)
				continue;
			const ActionInfo& info = actions.at(pass.firstDrawEvent);
			controller->SetFrameEvent(pass.firstDrawEvent, true);
			const PipeState& pipeline = controller->GetPipelineState();
			const Viewport viewport = pipeline.GetViewport(0);
			std::string colorTargets;
			std::set<uint64_t> attachmentIds;
			const rdcarray<BoundResource> outputTargets = pipeline.GetOutputTargets();
			for (uint32_t slot = 0; slot < outputTargets.size(); slot++)
			{
				const BoundResource& target = outputTargets[slot];
				const std::string description = DescribeBoundResource(target, resources);
				if (description.empty())
					continue;
				attachmentIds.emplace(GetResourceId(target.resourceId));
				if (!colorTargets.empty())
					colorTargets += ";";
				colorTargets += "slot=" + std::to_string(slot) + ":" + description;
			}
			std::string colorWriteMasks;
			const rdcarray<ColorBlend> colorBlends = pipeline.GetColorBlends();
			for (uint32_t slot = 0; slot < colorBlends.size(); slot++)
			{
				if (!colorWriteMasks.empty())
					colorWriteMasks += ";";
				char writeMask[32];
				std::snprintf(writeMask, sizeof(writeMask), "slot=%u:0x%02x", slot,
					static_cast<unsigned int>(colorBlends[slot].writeMask));
				colorWriteMasks += writeMask;
			}
			const BoundResource depthResource = pipeline.GetDepthTarget();
			const std::string depthTarget = DescribeBoundResource(depthResource, resources);
			if (GetResourceId(depthResource.resourceId) != 0)
				attachmentIds.emplace(GetResourceId(depthResource.resourceId));
			std::string pixelReadOnly;
			std::string feedbackTargets;
			for (const BoundResourceArray& binding : pipeline.GetReadOnlyResources(ShaderStage::Pixel, true))
			{
				for (uint32_t arrayIndex = 0; arrayIndex < binding.resources.size(); arrayIndex++)
				{
					const BoundResource& resource = binding.resources[arrayIndex];
					const uint64_t resourceId = GetResourceId(resource.resourceId);
					if (resourceId == 0)
						continue;
					char prefix[64];
					std::snprintf(prefix, sizeof(prefix), "set=%d,binding=%d,index=%u:",
						binding.bindPoint.bindset, binding.bindPoint.bind, arrayIndex);
					const std::string description = std::string(prefix) + DescribeBoundResource(resource, resources);
					if (!pixelReadOnly.empty())
						pixelReadOnly += ";";
					pixelReadOnly += description;
					if (attachmentIds.find(resourceId) == attachmentIds.end())
						continue;
					if (!feedbackTargets.empty())
						feedbackTargets += ";";
					feedbackTargets += description;
				}
			}
			std::fprintf(output,
				"%u\t%u\t%u\t%s\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%s\t%s\t%s\t%s\t%s\t%s\n",
				passIndex, pass.beginEvent, pass.endEvent, EscapeTsv(pass.reason).c_str(),
				pass.drawCount, pass.firstDrawEvent, info.numIndices, info.numInstances,
				static_cast<unsigned long long>(GetResourceId(pipeline.GetGraphicsPipelineObject())),
				static_cast<unsigned long long>(GetResourceId(pipeline.GetShader(ShaderStage::Vertex))),
				static_cast<unsigned long long>(GetResourceId(pipeline.GetShader(ShaderStage::Geometry))),
				static_cast<unsigned long long>(GetResourceId(pipeline.GetShader(ShaderStage::Pixel))),
				viewport.x, viewport.y, viewport.width, viewport.height,
				EscapeTsv(colorTargets).c_str(), EscapeTsv(colorWriteMasks).c_str(),
				EscapeTsv(depthTarget).c_str(),
				EscapeTsv(pixelReadOnly).c_str(), EscapeTsv(feedbackTargets).c_str(),
				EscapeTsv(info.name).c_str());
		}
		std::fprintf(stderr, "wrote %zu pass-state rows\n", passes.size());
	}
	else
	{
		rdcarray<GPUCounter> counters;
		counters.push_back(GPUCounter::EventGPUDuration);
		const rdcarray<CounterResult> results = controller->FetchCounters(counters);
		std::fprintf(output, "event_id\tduration_seconds\tflags\tdepth\tname\n");
		for (const CounterResult& result : results)
		{
			if (result.counter != GPUCounter::EventGPUDuration)
				continue;
			const auto action = actions.find(result.eventId);
			const ActionInfo empty{};
			const ActionInfo& info = action == actions.end() ? empty : action->second;
			const std::string name = EscapeTsv(info.name);
			std::fprintf(output, "%u\t%.12f\t%u\t%u\t%s\n", result.eventId,
				result.value.d, info.flags, info.depth, name.c_str());
		}
		std::fprintf(stderr, "wrote %zu GPU duration rows for %zu actions\n",
			results.size(), actions.size());
	}

	std::fclose(output);
	RENDERDOC_CloseAndroidCapture(argv[1], controller);
	RENDERDOC_DetachRenderdoc(argv[1]);
	RENDERDOC_ShutdownReplay();
	return 0;
}
