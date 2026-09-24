// DlssRecorder: the marker's job id picks the evaluation inputs; the provider records into the
// marker's command buffer; without a job, with incomplete images or a failing provider the marker
// dispatch runs (the recorder declines).
#include "dlss/dlss_recorder.h"
#include "upscale/reference_params.h"
#include "test_framework.h"

#include <cstring>
#include <string>

using namespace lu;
using namespace lu::dlss;

namespace {

class FakeProvider : public UpscalerProvider {
public:
	bool succeed = true;
	int calls = 0;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	EvaluateInputs last;
	bool request_instance_extensions(std::vector<std::string> &, std::string &) override { return true; }
	bool request_device(const vk::PhysicalDeviceCaps &, vk::DeviceRequest &, std::string &) override { return true; }
	bool initialize(const vk::DeviceContext &, std::string &) override { return true; }
	void shutdown() override {}
	bool optimal_render_size(ImageSize, upscale::Quality, ImageSize &) const override { return false; }
	bool evaluate(VkCommandBuffer cb, const EvaluateInputs &inputs, std::string &error) override {
		calls++;
		command_buffer = cb;
		last = inputs;
		if (!succeed) {
			error = "slEvaluateFeature failed: Result::eErrorMissingInputParameter";
		}
		return succeed;
	}
};

template <typename Handle>
Handle handle(uintptr_t value) {
	return reinterpret_cast<Handle>(value);
}

EvaluateImage image(uintptr_t base, uint32_t w, uint32_t h) {
	EvaluateImage i;
	i.image = handle<VkImage>(base);
	i.view = handle<VkImageView>(base + 1);
	i.width = w;
	i.height = h;
	i.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	return i;
}

EvaluateInputs inputs(float jitter_x) {
	EvaluateInputs in;
	in.viewport = 3;
	in.color = image(0x100, 960, 540);
	in.depth = image(0x200, 960, 540);
	in.velocity = image(0x300, 960, 540);
	in.output = image(0x400, 1920, 1080);
	in.mode = upscale::Quality::Performance;
	in.jitter_x = jitter_x;
	return in;
}

vk::MarkerDispatch dispatch_with_job(uint32_t job, uint32_t push_size = sizeof(upscale::ReferenceParams)) {
	upscale::ReferenceParams params;
	params.job = job;
	vk::MarkerDispatch d;
	d.command_buffer = handle<VkCommandBuffer>(0xC0);
	std::memcpy(d.push_constants, &params, sizeof(params));
	d.push_constants_size = push_size;
	return d;
}

} // namespace

TEST(dlss_recorder_evaluates_the_job_named_by_the_marker) {
	FakeProvider provider;
	DlssRecorder recorder;
	recorder.set_provider(&provider);
	recorder.set_job(41, inputs(0.25f));
	recorder.set_job(42, inputs(-0.25f));
	CHECK(recorder.record(dispatch_with_job(42)));
	CHECK_EQ(provider.calls, 1);
	CHECK(provider.command_buffer == handle<VkCommandBuffer>(0xC0)); // Godot's own command buffer
	CHECK_NEAR(provider.last.jitter_x, -0.25f, 0.0);
	CHECK_EQ(provider.last.output.width, 1920u);
	CHECK(recorder.record(dispatch_with_job(41)));
	CHECK_NEAR(provider.last.jitter_x, 0.25f, 0.0);
	CHECK_EQ(recorder.stats().evaluated, 2u);
}

TEST(dlss_recorder_lets_the_marker_run_when_it_cannot_evaluate) {
	FakeProvider provider;
	DlssRecorder recorder;
	CHECK(!recorder.record(dispatch_with_job(1))); // no provider, no job
	recorder.set_provider(&provider);
	CHECK(!recorder.record(dispatch_with_job(0))); // job 0: none
	CHECK(!recorder.record(dispatch_with_job(7))); // never registered
	recorder.set_job(7, inputs(0.0f));
	CHECK(!recorder.record(dispatch_with_job(7, upscale::REFERENCE_PARAMS_JOB_OFFSET))); // the job id is cut off
	recorder.set_job(7 + DlssRecorder::JOB_SLOTS, inputs(0.0f)); // overwrites job 7's slot
	CHECK(!recorder.record(dispatch_with_job(7)));
	EvaluateInputs incomplete = inputs(0.0f);
	incomplete.velocity.view = VK_NULL_HANDLE;
	recorder.set_job(9, incomplete);
	CHECK(!recorder.record(dispatch_with_job(9)));
	CHECK_EQ(provider.calls, 0);
	CHECK_EQ(recorder.stats().missing_job, 6u);

	provider.succeed = false;
	CHECK(!recorder.record(dispatch_with_job(7 + DlssRecorder::JOB_SLOTS)));
	CHECK_EQ(provider.calls, 1);
	CHECK_EQ(recorder.stats().failed, 1u);
	CHECK(recorder.stats().last_error.find("eErrorMissingInputParameter") != std::string::npos);
}
