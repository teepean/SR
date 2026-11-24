// wavetest <wavetest.spv> [workgroups] [rounds] [runs] [threads] - runs wavetest.comp (512 or 1024-thread workgroups:
// guest waves of 2 warps each) and checks every wave's checksum against a CPU model; reports handshakes that gave up.
// build: g++ -O2 -o wavetest wavetest.cpp -lvulkan
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define CHECK(x)                                                                   \
	do {                                                                           \
		VkResult r_ = (x);                                                         \
		if (r_ != VK_SUCCESS) {                                                    \
			std::fprintf(stderr, "%s failed: %d\n", #x, static_cast<int>(r_));     \
			return 1;                                                              \
		}                                                                          \
	} while (0)

static uint32_t Hash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}

// the CPU model of one guest wave of wavetest.comp
static uint32_t Wave(uint32_t seed, uint32_t wg, uint32_t wave, uint32_t rounds) {
	const uint32_t h         = Hash(seed ^ (wg * 977u) ^ (wave * 131u));
	const uint32_t my_rounds = rounds + (h & 63u);
	uint32_t       value[64], sum = 0;
	for (uint32_t l = 0; l < 64; l++) value[l] = Hash(h ^ l);
	for (uint32_t r = 0;; r++) {
		bool any = false;
		for (uint32_t l = 0; l < 64; l++) any |= r < my_rounds + ((value[l] >> 3) & 1u) * (l & 1u);
		if (!any) break;
		uint32_t pick = value[(h + r) & 63u];
		if (((h >> (r & 15u)) & 1u) != 0u) pick ^= value[(h * 7u + r) & 63u] + r;
		sum = sum * 31u + pick;
		for (uint32_t l = 0; l < 64; l++) value[l] = Hash(value[l] + pick);
	}
	return sum + sum;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: wavetest <wavetest.spv> [workgroups] [rounds] [runs]\n");
		return 2;
	}
	const uint32_t groups = argc > 2 ? std::atoi(argv[2]) : 1024;
	const uint32_t rounds = argc > 3 ? std::atoi(argv[3]) : 100;
	const uint32_t runs   = argc > 4 ? std::atoi(argv[4]) : 4;
	const uint32_t Waves = (argc > 5 ? std::atoi(argv[5]) : 512) / 64;
	std::ifstream file(argv[1], std::ios::binary);
	std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance;
	CHECK(vkCreateInstance(&ici, nullptr, &instance));
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(instance, &n, nullptr);
	std::vector<VkPhysicalDevice> devices(n);
	vkEnumeratePhysicalDevices(instance, &n, devices.data());
	VkPhysicalDevice physical = devices.at(0);
	for (auto d: devices) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d, &p);
		if (p.vendorID == 0x10de) physical = d;
	}
	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = 0;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &priority;
	VkPhysicalDeviceVulkan12Features v12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	v12.bufferDeviceAddress = VK_TRUE;
	v12.vulkanMemoryModel   = VK_TRUE;
	VkPhysicalDeviceVulkan13Features v13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	v13.computeFullSubgroups = VK_TRUE;
	v13.subgroupSizeControl  = VK_TRUE;
	v13.pNext                = &v12;
	VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.features.shaderInt64 = VK_TRUE;
	features.pNext                = &v13;
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext                = &features;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos    = &qci;
	VkDevice device;
	CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);

	const VkDeviceSize size = (uint64_t {groups} * Waves + 16) * 4;
	VkBufferCreateInfo bci {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size  = size;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	VkBuffer buffer;
	CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, buffer, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(physical, &mp);
	uint32_t type = 0;
	for (; type < mp.memoryTypeCount; type++) {
		const auto f = mp.memoryTypes[type].propertyFlags;
		if ((req.memoryTypeBits & (1u << type)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
		    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			break;
	}
	VkMemoryAllocateFlagsInfo flags {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo mai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext           = &flags;
	mai.allocationSize  = req.size;
	mai.memoryTypeIndex = type;
	VkDeviceMemory memory;
	CHECK(vkAllocateMemory(device, &mai, nullptr, &memory));
	CHECK(vkBindBufferMemory(device, buffer, memory, 0));
	void* mapped = nullptr;
	CHECK(vkMapMemory(device, memory, 0, size, 0, &mapped));
	VkBufferDeviceAddressInfo bai {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
	bai.buffer = buffer;
	const uint64_t address = vkGetBufferDeviceAddress(device, &bai);

	VkShaderModuleCreateInfo smci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smci.codeSize = code.size();
	smci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule module;
	CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
	VkPushConstantRange push {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
	VkPipelineLayoutCreateInfo plci {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges    = &push;
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
	VkComputePipelineCreateInfo cpci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = module;
	cpci.stage.pName  = "main";
	cpci.stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
	cpci.layout       = layout;
	VkPipeline pipeline;
	CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

	VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &cpi, nullptr, &pool));
	VkCommandBufferAllocateInfo cbai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool        = pool;
	cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
	VkFenceCreateInfo fci {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CHECK(vkCreateFence(device, &fci, nullptr, &fence));

	int failures = 0;
	for (uint32_t run = 0; run < runs; run++) {
		std::memset(mapped, 0, size);
		struct {
			uint64_t out;
			uint32_t rounds, seed;
		} pc {address, rounds, 0x1234567u + run * 7919u};
		VkCommandBufferBeginInfo cbbi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		CHECK(vkBeginCommandBuffer(cb, &cbbi));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cb, groups, 1, 1);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers    = &cb;
		const auto started = std::chrono::steady_clock::now();
		CHECK(vkQueueSubmit(queue, 1, &si, fence));
		const VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 20ull * 1000 * 1000 * 1000);
		const double   ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (waited != VK_SUCCESS) {
			std::printf("run %u: fence wait %d after %.0f ms\n", run, static_cast<int>(waited), ms);
			return 1;
		}
		vkResetFences(device, 1, &fence);
		const auto* out = static_cast<const uint32_t*>(mapped);
		uint32_t    wrong = 0, first = UINT32_MAX;
		for (uint32_t wg = 0; wg < groups; wg++)
			for (uint32_t w = 0; w < Waves; w++)
				if (out[wg * Waves + w] != Wave(pc.seed, wg, w, rounds)) {
					wrong++;
					if (first == UINT32_MAX) first = wg * Waves + w;
				}
		std::printf("run %u: %.1f ms, %u of %u waves wrong%s, %u handshakes gave up\n", run, ms, wrong, groups * Waves,
		            wrong ? " (first at " : "", out[groups * Waves]);
		if (wrong) std::printf("  first wrong wave %u\n", first);
		failures += wrong != 0 || out[groups * Waves] != 0;
	}
	std::printf(failures ? "FAILED\n" : "PASSED\n");
	vkDestroyFence(device, fence, nullptr);
	vkDestroyCommandPool(device, pool, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyShaderModule(device, module, nullptr);
	vkDestroyBuffer(device, buffer, nullptr);
	vkFreeMemory(device, memory, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return failures ? 1 : 0;
}
