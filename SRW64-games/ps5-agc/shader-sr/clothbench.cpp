// clothbench <capture.bin> <reference dir> [<candidate dir>] [runs] - offline test of the cloth solver programs on one
// captured frame (hooks/srhooks.c SRH_CLOTH_CAPTURE).
// - Builds the frame's guest memory: the captured ranges in 16 KB pages, consecutive guest pages consecutive on the
//   device, and a BDA page table as the emulator's (prelude.py).
// - Runs every instance of the frame phase by phase, one batched dispatch per phase and slot as the emulator's batch
//   mode, with the programs <dir>/cloth-slot<N>.spv. A candidate dir overrides the reference's slots it has.
// - Reports the GPU time per phase, pages a program needed that the capture lacks (fault bitmap), the memory the
//   reference changed, and where the candidate's memory after the frame differs from the reference's.
// build: g++ -O2 -o clothbench clothbench.cpp -lvulkan
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define CHECK(x)                                                                   \
	do {                                                                           \
		VkResult r_ = (x);                                                         \
		if (r_ != VK_SUCCESS) {                                                    \
			std::fprintf(stderr, "%s failed: %d\n", #x, static_cast<int>(r_));     \
			std::exit(1);                                                          \
		}                                                                          \
	} while (0)

static const uint64_t ExtendedBase = 0x080000000000ull;
static const uint64_t LowerSize    = 0x010000000000ull;
static const uint32_t NumPages     = 0x6000000u;
static const uint64_t PageSize     = 16384;

struct Item {
	uint64_t table;
	uint32_t phase, slot, verts, pad;
};
struct Range {
	uint64_t             address;
	std::vector<uint8_t> bytes;
};
struct Push {
	uint64_t pages, faults, user_table, counters, wg_map, scalars;
	uint32_t use_words, user_count, tgid_mask, debug;
	uint32_t user[16];
};
static_assert(sizeof(Push) == 128, "push constant block");

static uint64_t GuestPage(uint64_t a) {
	const uint64_t packed = a >= ExtendedBase ? a - (ExtendedBase - LowerSize) : a;
	return packed >> 14;
}

struct Vk {
	VkInstance                       instance;
	VkPhysicalDevice                 physical;
	VkDevice                         device;
	VkQueue                          queue;
	VkPhysicalDeviceMemoryProperties mp;
	float                            ns_per_tick;
};

struct Buffer {
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint64_t       address = 0;
	void*          mapped  = nullptr;
	VkDeviceSize   size    = 0;
};

static Buffer MakeBuffer(Vk& vk, VkDeviceSize size, bool host) {
	Buffer             b;
	VkBufferCreateInfo bci {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size  = size;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
	            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CHECK(vkCreateBuffer(vk.device, &bci, nullptr, &b.buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk.device, b.buffer, &req);
	const VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
	                                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	uint32_t type = 0;
	for (; type < vk.mp.memoryTypeCount; type++)
		if ((req.memoryTypeBits & (1u << type)) && (vk.mp.memoryTypes[type].propertyFlags & want) == want) break;
	if (type == vk.mp.memoryTypeCount && host) {
		for (type = 0; type < vk.mp.memoryTypeCount; type++) {
			const auto f = vk.mp.memoryTypes[type].propertyFlags;
			if ((req.memoryTypeBits & (1u << type)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
			    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
				break;
		}
	}
	VkMemoryAllocateFlagsInfo flags {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo mai {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext           = &flags;
	mai.allocationSize  = req.size;
	mai.memoryTypeIndex = type;
	CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &b.memory));
	CHECK(vkBindBufferMemory(vk.device, b.buffer, b.memory, 0));
	if (host) CHECK(vkMapMemory(vk.device, b.memory, 0, size, 0, &b.mapped));
	VkBufferDeviceAddressInfo bai {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
	bai.buffer = b.buffer;
	b.address  = vkGetBufferDeviceAddress(vk.device, &bai);
	b.size     = size;
	return b;
}

static void Barrier(VkCommandBuffer cb) {
	VkMemoryBarrier mb {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
	                   VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
	                     nullptr);
}

static std::vector<char> ReadFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 3) {
		std::fprintf(stderr, "usage: clothbench <capture.bin> <reference dir> [<candidate dir>] [runs]\n");
		return 2;
	}
	const std::string cand_dir = argc > 3 ? argv[3] : "";
	const int         runs     = argc > 4 ? std::max(1, std::atoi(argv[4])) : 3;
	const bool        serial   = std::getenv("CLOTHBENCH_SERIAL") != nullptr;

	// ------------------------------------------------------------------ capture
	std::vector<char> cap = ReadFile(argv[1]);
	size_t            pos = 0;
	auto              take = [&](void* dst, size_t n) {
        if (pos + n > cap.size()) {
            std::fprintf(stderr, "capture truncated\n");
            std::exit(1);
        }
        std::memcpy(dst, cap.data() + pos, n);
        pos += n;
	};
	uint32_t header[4];
	take(header, 16);
	if (header[0] != 0x50414343u || header[1] != 1) {
		std::fprintf(stderr, "not a cloth capture\n");
		return 1;
	}
	uint64_t code[4];
	uint32_t tg[4];
	take(code, 32);
	take(tg, 16);
	std::vector<Item> items(header[2]);
	for (auto& it: items) take(&it, 24);
	std::vector<Range> ranges(header[3]);
	uint64_t           captured = 0;
	for (auto& r: ranges) {
		uint64_t size;
		take(&r.address, 8);
		take(&size, 8);
		r.bytes.resize(size);
		take(r.bytes.data(), size);
		captured += size;
	}
	uint32_t per_phase_slot[3][5] = {};
	for (const auto& it: items) per_phase_slot[std::min(it.phase, 2u)][std::min(it.slot, 4u)]++;
	std::printf("capture: %zu instances, %zu ranges, %.1f MB; tg %u/%u/%u/%u\n", items.size(), ranges.size(),
	            captured / 1048576.0, tg[0], tg[1], tg[2], tg[3]);
	for (int p = 0; p < 3; p++)
		std::printf("  phase %d: slot0 %u slot1 %u slot2 %u slot3 %u other %u\n", p, per_phase_slot[p][0],
		            per_phase_slot[p][1], per_phase_slot[p][2], per_phase_slot[p][3], per_phase_slot[p][4]);

	// guest pages -> device offsets: runs of consecutive guest pages stay consecutive
	std::map<uint64_t, uint64_t> page_offset;
	for (const auto& r: ranges)
		for (uint64_t p = GuestPage(r.address); p <= GuestPage(r.address + r.bytes.size() - 1); p++) page_offset[p] = 0;
	uint64_t total = 0, prev = ~0ull;
	for (auto& kv: page_offset) {
		if (prev != ~0ull && kv.first != prev + 1) total = (total + PageSize) & ~(PageSize - 1);   // gap page
		kv.second = total;
		total += PageSize;
		prev = kv.first;
	}
	std::vector<uint8_t> image(total, 0);
	for (const auto& r: ranges)
		for (uint64_t k = 0; k < r.bytes.size();) {
			const uint64_t a = r.address + k, in_page = a & (PageSize - 1);
			const uint64_t n = std::min<uint64_t>(PageSize - in_page, r.bytes.size() - k);
			std::memcpy(&image[page_offset[GuestPage(a)] + in_page], &r.bytes[k], n);
			k += n;
		}
	std::printf("guest memory: %zu pages, %.1f MB on the device\n", page_offset.size(), total / 1048576.0);

	// ------------------------------------------------------------------ Vulkan
	Vk                vk {};
	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, nullptr, &vk.instance));
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(vk.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> devices(n);
	vkEnumeratePhysicalDevices(vk.instance, &n, devices.data());
	vk.physical = devices.at(0);
	for (auto d: devices) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(d, &p);
		if (p.vendorID == 0x10de) vk.physical = d;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vk.physical, &props);
	vk.ns_per_tick = props.limits.timestampPeriod;
	vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.mp);

	VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR reconv {
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
	VkPhysicalDeviceVulkan13Features v13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	v13.pNext = &reconv;
	VkPhysicalDeviceVulkan12Features v12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	v12.pNext = &v13;
	VkPhysicalDeviceVulkan11Features v11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	v11.pNext = &v12;
	VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.pNext = &v11;
	vkGetPhysicalDeviceFeatures2(vk.physical, &features);   // everything the device has
	v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
	v12.bufferDeviceAddressMultiDevice   = VK_FALSE;
	float                   priority = 1.0f;
	VkDeviceQueueCreateInfo qci {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = 0;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &priority;
	const char*        extensions[] = {"VK_KHR_shader_maximal_reconvergence"};
	VkDeviceCreateInfo dci {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext                   = &features;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = reconv.shaderMaximalReconvergence ? 1 : 0;
	dci.ppEnabledExtensionNames = extensions;
	if (!reconv.shaderMaximalReconvergence) v13.pNext = nullptr;
	CHECK(vkCreateDevice(vk.physical, &dci, nullptr, &vk.device));
	vkGetDeviceQueue(vk.device, 0, 0, &vk.queue);

	Buffer guest   = MakeBuffer(vk, total + PageSize, false);
	Buffer staging = MakeBuffer(vk, total + PageSize, true);
	Buffer pages   = MakeBuffer(vk, uint64_t {NumPages} * 8, false);
	Buffer faults  = MakeBuffer(vk, uint64_t {NumPages} / 4, false);   // fault bitmap + use bitmap
	Buffer fback   = MakeBuffer(vk, uint64_t {NumPages} / 8, true);
	Buffer user    = MakeBuffer(vk, items.size() * 96 + 64, true);
	Buffer wgmap   = MakeBuffer(vk, items.size() * 4 + 64, true);
	std::memcpy(staging.mapped, image.data(), total);

	VkCommandPoolCreateInfo cpi {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(vk.device, &cpi, nullptr, &pool));
	VkCommandBufferAllocateInfo cbai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool        = pool;
	cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(vk.device, &cbai, &cb));
	VkFenceCreateInfo fci {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence           fence;
	CHECK(vkCreateFence(vk.device, &fci, nullptr, &fence));
	VkQueryPoolCreateInfo qpci {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
	qpci.queryCount = 16;
	VkQueryPool queries;
	CHECK(vkCreateQueryPool(vk.device, &qpci, nullptr, &queries));
	auto submit = [&]() {
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers    = &cb;
		CHECK(vkQueueSubmit(vk.queue, 1, &si, fence));
		const VkResult w = vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000);
		if (w != VK_SUCCESS) {
			std::fprintf(stderr, "GPU did not finish (%d)\n", static_cast<int>(w));
			std::exit(1);
		}
		vkResetFences(vk.device, 1, &fence);
	};
	auto begin = [&]() {
		VkCommandBufferBeginInfo cbbi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		CHECK(vkBeginCommandBuffer(cb, &cbbi));
	};

	// page table: 0 everywhere, the captured pages -> their device pages
	begin();
	vkCmdFillBuffer(cb, pages.buffer, 0, VK_WHOLE_SIZE, 0);
	Barrier(cb);
	for (const auto& kv: page_offset) {
		const uint64_t dev = guest.address + kv.second;
		vkCmdUpdateBuffer(cb, pages.buffer, kv.first * 8, 8, &dev);
	}
	submit();

	// user table (24 dwords per guest dispatch) and workgroup maps per phase and slot
	auto*                              ut = static_cast<uint32_t*>(user.mapped);
	auto*                              wm = static_cast<uint32_t*>(wgmap.mapped);
	std::vector<std::vector<uint32_t>> batches(12);   // phase * 4 + slot -> instances
	for (uint32_t i = 0; i < items.size(); i++)
		if (items[i].slot < 4 && items[i].phase < 3) batches[items[i].phase * 4 + items[i].slot].push_back(i);
	std::vector<uint32_t> batch_first(12);
	uint32_t              wm_n = 0;
	for (uint32_t b = 0; b < 12; b++) {
		batch_first[b] = wm_n;
		for (uint32_t k = 0; k < batches[b].size(); k++) {
			const uint32_t i = batches[b][k];
			uint32_t*      e = ut + 24 * i;
			std::memset(e, 0, 96);
			e[0]          = static_cast<uint32_t>(items[i].table);
			e[1]          = static_cast<uint32_t>(items[i].table >> 32);
			e[16]         = serial ? 0 : k;   // first workgroup of this guest dispatch in its dispatch
			e[17]         = 1;
			e[18]         = 1;
			e[19]         = 1;
			wm[wm_n++] = i;
		}
	}

	// ------------------------------------------------------------------ programs
	VkPushConstantRange        push_range {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
	VkPipelineLayoutCreateInfo plci {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges    = &push_range;
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(vk.device, &plci, nullptr, &layout));
	// pipeline cache on disk: the translated programs take minutes to compile
	const char* cache_env  = std::getenv("CLOTHBENCH_CACHE");
	std::string cache_path = cache_env ? cache_env : std::string(argv[1]) + ".pipelines";
	std::vector<char> cache_data = ReadFile(cache_path);
	VkPipelineCacheCreateInfo pcci {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	pcci.initialDataSize = cache_data.size();
	pcci.pInitialData    = cache_data.empty() ? nullptr : cache_data.data();
	VkPipelineCache pipeline_cache;
	if (vkCreatePipelineCache(vk.device, &pcci, nullptr, &pipeline_cache) != VK_SUCCESS) {
		pcci.initialDataSize = 0;
		pcci.pInitialData    = nullptr;
		CHECK(vkCreatePipelineCache(vk.device, &pcci, nullptr, &pipeline_cache));
	}
	auto save_cache = [&]() {
		size_t n = 0;
		vkGetPipelineCacheData(vk.device, pipeline_cache, &n, nullptr);
		std::vector<char> data(n);
		vkGetPipelineCacheData(vk.device, pipeline_cache, &n, data.data());
		std::ofstream out(cache_path + ".new", std::ios::binary);
		out.write(data.data(), static_cast<std::streamsize>(n));
		out.close();
		std::rename((cache_path + ".new").c_str(), cache_path.c_str());
	};
	auto load = [&](const std::string& path) -> VkPipeline {
		std::vector<char> spv = ReadFile(path);
		if (spv.empty()) return VK_NULL_HANDLE;
		VkShaderModuleCreateInfo smci {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		smci.codeSize = spv.size();
		smci.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
		VkShaderModule module;
		CHECK(vkCreateShaderModule(vk.device, &smci, nullptr, &module));
		VkComputePipelineCreateInfo cpci {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		cpci.stage.module = module;
		cpci.stage.pName  = "main";
		cpci.stage.flags  = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
		cpci.layout       = layout;
		VkPipeline pipeline;
		const double t0 = static_cast<double>(clock()) / CLOCKS_PER_SEC;
		CHECK(vkCreateComputePipelines(vk.device, pipeline_cache, 1, &cpci, nullptr, &pipeline));
		std::printf("  %s: pipeline in %.1f s\n", path.c_str(), static_cast<double>(clock()) / CLOCKS_PER_SEC - t0);
		std::fflush(stdout);
		save_cache();
		vkDestroyShaderModule(vk.device, module, nullptr);
		return pipeline;
	};
	VkPipeline ref[4], cand[4];
	// CLOTHBENCH_CAND_ONLY=<instance list>: the candidate's programs run only these instances (others: the reference's)
	std::vector<bool> cand_only;
	if (const char* only = std::getenv("CLOTHBENCH_CAND_ONLY")) {
		cand_only.assign(items.size(), false);
		for (const char* p = only; *p;) {
			char* end;
			const long k = std::strtol(p, &end, 10);
			if (end == p) break;
			if (k >= 0 && static_cast<size_t>(k) < items.size()) cand_only[k] = true;
			p = *end ? end + 1 : end;
		}
	}
	for (int s = 0; s < 4; s++) {
		ref[s]  = load(std::string(argv[2]) + "/cloth-slot" + std::to_string(s) + ".spv");
		cand[s] = cand_dir.empty() ? VK_NULL_HANDLE : load(cand_dir + "/cloth-slot" + std::to_string(s) + ".spv");
		if (cand[s] == VK_NULL_HANDLE) cand[s] = ref[s];
	}

	if (std::getenv("CLOTHBENCH_COMPILE_ONLY")) return 0;   // pipelines are in the cache now

	// ------------------------------------------------------------------ one frame
	std::vector<uint8_t> pristine(image);
	auto frame = [&](VkPipeline* programs, std::vector<uint8_t>& result, double phase_ms[3], uint32_t& fault_pages) {
		std::memcpy(staging.mapped, pristine.data(), total);
		begin();
		vkCmdResetQueryPool(cb, queries, 0, 16);
		VkBufferCopy copy {0, 0, total};
		vkCmdCopyBuffer(cb, staging.buffer, guest.buffer, 1, &copy);
		vkCmdFillBuffer(cb, faults.buffer, 0, VK_WHOLE_SIZE, 0);
		Barrier(cb);
		for (uint32_t p = 0; p < 3; p++) {
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 2 * p);
			for (uint32_t s = 0; s < 4; s++) {
				const uint32_t b = p * 4 + s;
				if (batches[b].empty() || programs[s] == VK_NULL_HANDLE) continue;
				Push pc {};
				pc.pages      = pages.address;
				pc.faults     = faults.address;
				pc.user_table = user.address;
				pc.wg_map     = wgmap.address + 4ull * batch_first[b];
				pc.use_words  = NumPages / 32;
				pc.user_count = 2;
				pc.tgid_mask  = 1;
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, programs[s]);
				if (serial) {
					for (uint32_t k = 0; k < batches[b].size(); k++) {
						if (!cand_only.empty() && programs == cand) {
							VkPipeline use = cand_only[batches[b][k]] ? cand[s] : ref[s];
							vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, use);
						}
						pc.wg_map = wgmap.address + 4ull * (batch_first[b] + k);
						vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
						vkCmdDispatch(cb, 1, 1, 1);
						Barrier(cb);
					}
				} else {
					vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
					vkCmdDispatch(cb, static_cast<uint32_t>(batches[b].size()), 1, 1);
				}
			}
			Barrier(cb);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 2 * p + 1);
		}
		VkBufferCopy back {0, 0, total};
		vkCmdCopyBuffer(cb, guest.buffer, staging.buffer, 1, &back);
		VkBufferCopy fcopy {0, 0, uint64_t {NumPages} / 8};
		vkCmdCopyBuffer(cb, faults.buffer, fback.buffer, 1, &fcopy);
		submit();
		uint64_t ts[6];
		CHECK(vkGetQueryPoolResults(vk.device, queries, 0, 6, sizeof(ts), ts, 8,
		                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
		for (int p = 0; p < 3; p++) phase_ms[p] = (ts[2 * p + 1] - ts[2 * p]) * vk.ns_per_tick / 1e6;
		result.assign(static_cast<uint8_t*>(staging.mapped), static_cast<uint8_t*>(staging.mapped) + total);
		fault_pages = 0;
		const auto* fw = static_cast<const uint32_t*>(fback.mapped);
		for (uint32_t w = 0; w < NumPages / 32; w++)
			if (fw[w] != 0) {
				for (int bit = 0; bit < 32; bit++)
					if (fw[w] & (1u << bit)) {
						if (fault_pages < 8)
							std::printf("    missing page %#llx (guest %#llx)\n", static_cast<unsigned long long>(w * 32 + bit),
							            static_cast<unsigned long long>(uint64_t {w * 32u + bit} << 14));
						fault_pages++;
					}
			}
	};

	auto report_time = [&](const char* name, VkPipeline* programs, std::vector<uint8_t>& out) {
		double   best[3] = {1e30, 1e30, 1e30};
		uint32_t fault_pages = 0;
		for (int r = 0; r < runs; r++) {
			double ms[3];
			frame(programs, out, ms, fault_pages);
			for (int p = 0; p < 3; p++) best[p] = std::min(best[p], ms[p]);
		}
		std::printf("%s: phase 0 %.3f ms, phase 1 %.3f ms, phase 2 %.3f ms (best of %d), %u missing pages\n", name, best[0],
		            best[1], best[2], runs, fault_pages);
	};

	// differences of b against a over the captured ranges, as f32 dwords
	auto compare = [&](const char* what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool list) {
		uint64_t diff_dwords = 0, big = 0, ranges_with = 0;
		double   worst = 0;
		for (const auto& r: ranges) {
			uint64_t n = 0, nbig = 0, first = ~0ull;
			double   rworst = 0;
			for (uint64_t k = 0; k + 4 <= r.bytes.size(); k += 4) {
				const uint64_t addr = r.address + k;
				const uint64_t off  = page_offset[GuestPage(addr)] + (addr & (PageSize - 1));
				uint32_t       x, y;
				std::memcpy(&x, &a[off], 4);
				std::memcpy(&y, &b[off], 4);
				if (x == y) continue;
				n++;
				if (first == ~0ull) first = k;
				float fx, fy;
				std::memcpy(&fx, &x, 4);
				std::memcpy(&fy, &y, 4);
				const double d = std::fabs(static_cast<double>(fx) - fy) / std::max(1.0, std::fabs(static_cast<double>(fx)));
				if (!(d < 1e-3)) nbig++;
				if (d > rworst || d != d) rworst = d != d ? 1e30 : d;
			}
			if (n == 0) continue;
			ranges_with++;
			diff_dwords += n;
			big += nbig;
			worst = std::max(worst, rworst);
			if (list)
				std::printf("    %#llx +%#llx: %llu dwords differ (%llu beyond 1e-3, worst %.3g), first at +%#llx\n",
				            static_cast<unsigned long long>(r.address), static_cast<unsigned long long>(r.bytes.size()),
				            static_cast<unsigned long long>(n), static_cast<unsigned long long>(nbig), rworst,
				            static_cast<unsigned long long>(first));
		}
		std::printf("%s: %llu dwords in %llu ranges differ, %llu beyond 1e-3 (worst relative %.3g)\n", what,
		            static_cast<unsigned long long>(diff_dwords), static_cast<unsigned long long>(ranges_with),
		            static_cast<unsigned long long>(big), worst);
	};

	std::vector<uint8_t> out_ref, out_ref2, out_cand;
	report_time("reference", ref, out_ref);
	compare("reference vs capture (what the frame writes)", pristine, out_ref, true);
	{
		double   ms[3];
		uint32_t fp;
		frame(ref, out_ref2, ms, fp);
		compare("reference run-to-run", out_ref, out_ref2, true);
	}
	if (!cand_dir.empty()) {
		report_time("candidate", cand, out_cand);
		compare("candidate vs reference", out_ref, out_cand, true);
	}
	// CLOTHBENCH_DUMP=<prefix>: the memory after the frame as captures (<prefix>.ref.bin, <prefix>.cand.bin)
	if (const char* dump = std::getenv("CLOTHBENCH_DUMP")) {
		auto write = [&](const std::string& path, const std::vector<uint8_t>& img) {
			std::ofstream out(path, std::ios::binary);
			out.write(cap.data(), static_cast<std::streamsize>(64 + 24 * items.size()));
			for (const auto& r: ranges) {
				const uint64_t size = r.bytes.size();
				out.write(reinterpret_cast<const char*>(&r.address), 8);
				out.write(reinterpret_cast<const char*>(&size), 8);
				for (uint64_t k = 0; k < size;) {
					const uint64_t a = r.address + k, in_page = a & (PageSize - 1);
					const uint64_t n = std::min<uint64_t>(PageSize - in_page, size - k);
					out.write(reinterpret_cast<const char*>(&img[page_offset[GuestPage(a)] + in_page]), static_cast<std::streamsize>(n));
					k += n;
				}
			}
		};
		write(std::string(dump) + ".ref.bin", out_ref);
		if (!cand_dir.empty()) write(std::string(dump) + ".cand.bin", out_cand);
	}
	vkDeviceWaitIdle(vk.device);
	return 0;
}
