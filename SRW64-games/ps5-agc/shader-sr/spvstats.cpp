// spvstats <shader.spv> [push constant bytes] - builds a compute pipeline from SPIR-V and prints the driver's
// statistics (VK_KHR_pipeline_executable_properties: registers, spills / local memory, binary size, shared memory)
// build: g++ -O2 -o spvstats spvstats.cpp -lvulkan
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

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: spvstats <shader.spv> [push constant bytes]\n");
		return 2;
	}
	std::ifstream file(argv[1], std::ios::binary);
	std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (code.empty() || code.size() % 4 != 0) {
		std::fprintf(stderr, "bad SPIR-V file\n");
		return 1;
	}
	const uint32_t push_bytes = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 128u;

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
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	for (auto d: devices) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d, &p);
		if (p.vendorID == 0x10de) physical = d;
	}
	if (physical == VK_NULL_HANDLE) physical = devices.at(0);

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = 0;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &priority;
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec {
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
	exec.pipelineExecutableInfo = VK_TRUE;
	VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR reconv {
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
	reconv.shaderMaximalReconvergence = VK_TRUE;
	reconv.pNext                      = &exec;
	VkPhysicalDeviceVulkan12Features v12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	v12.bufferDeviceAddress               = VK_TRUE;
	v12.shaderInt8                        = VK_TRUE;
	v12.storageBuffer8BitAccess           = VK_TRUE;
	v12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
	v12.pNext                             = &reconv;
	VkPhysicalDeviceVulkan11Features v11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	v11.storageBuffer16BitAccess           = VK_TRUE;
	v11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
	v11.pNext                              = &v12;
	VkPhysicalDeviceVulkan13Features v13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	v13.subgroupSizeControl  = VK_TRUE;
	v13.computeFullSubgroups = VK_TRUE;
	v13.pNext                = &v11;
	VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.features.shaderInt64 = VK_TRUE;
	features.features.shaderInt16 = VK_TRUE;
	features.pNext                = &v13;
	const char* extensions[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
	                            VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME};
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext                   = &features;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = 2;
	dci.ppEnabledExtensionNames = extensions;
	VkDevice device;
	CHECK(vkCreateDevice(physical, &dci, nullptr, &device));

	VkShaderModuleCreateInfo smci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smci.codeSize = code.size();
	smci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule module;
	CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
	VkPushConstantRange push {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
	VkPipelineLayoutCreateInfo plci {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges    = &push;
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
	VkComputePipelineCreateInfo cpci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpci.flags        = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
	cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = module;
	cpci.stage.pName  = "main";
	cpci.stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
	cpci.layout       = layout;
	VkPipeline pipeline;
	const auto started = std::chrono::steady_clock::now();
	CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
	const double seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

	auto get_props = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
	    vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
	auto get_stats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
	    vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
	VkPipelineInfoKHR info {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
	info.pipeline = pipeline;
	uint32_t count = 0;
	get_props(device, &info, &count, nullptr);
	std::vector<VkPipelineExecutablePropertiesKHR> props(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
	get_props(device, &info, &count, props.data());
	std::printf("%s: compile %.2f s\n", argv[1], seconds);
	for (uint32_t i = 0; i < count; i++) {
		VkPipelineExecutableInfoKHR ei {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
		ei.pipeline        = pipeline;
		ei.executableIndex = i;
		uint32_t sc        = 0;
		get_stats(device, &ei, &sc, nullptr);
		std::vector<VkPipelineExecutableStatisticKHR> stats(sc, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
		get_stats(device, &ei, &sc, stats.data());
		std::printf("  %s (subgroup %u):", props[i].name, props[i].subgroupSize);
		for (const auto& s: stats) {
			std::printf(" %s=", s.name);
			switch (s.format) {
				case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("%s", s.value.b32 ? "true" : "false"); break;
				case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::printf("%lld", static_cast<long long>(s.value.i64)); break;
				case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::printf("%llu", static_cast<unsigned long long>(s.value.u64)); break;
				case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: std::printf("%.3f", s.value.f64); break;
				default: break;
			}
		}
		std::printf("\n");
	}
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyShaderModule(device, module, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}
