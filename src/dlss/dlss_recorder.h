// The MarkerRecorder that has DLSS evaluate at the upscaler's marker dispatch.
//
// The Upscaler records the marker (the reference pipeline) through RenderingDevice with a job id in
// its push constants (upscale::ReferenceParams::job) and, at the same time, the job's evaluation
// inputs here. When Godot records the marker into its command buffer, record() looks the job up and
// has the provider evaluate into that command buffer; the marker dispatch is then dropped and Godot's
// state re-bound by the layer. Without a job, or when the provider fails, the marker dispatch runs:
// the reference upscaler still fills the output, so a failure shows as lower quality, not garbage.
//
// Pure C++ plus Vulkan headers: unit-tested with a fake provider.
#pragma once

#include "dlss/upscaler_provider.h"
#include "vk/marker_interceptor.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace lu::dlss {

struct DlssRecorderStats {
	uint64_t evaluated = 0;
	uint64_t missing_job = 0;
	uint64_t failed = 0; // the provider did not record
	std::string last_error;
};

class DlssRecorder : public vk::MarkerRecorder {
public:
	static constexpr uint32_t JOB_SLOTS = 16;

	void set_provider(UpscalerProvider *provider);
	void set_job(uint32_t job, const EvaluateInputs &inputs); // job != 0
	bool record(const vk::MarkerDispatch &dispatch) override;
	DlssRecorderStats stats() const;

private:
	struct Slot {
		uint32_t job = 0;
		EvaluateInputs inputs;
	};
	mutable std::mutex mutex_;
	UpscalerProvider *provider_ = nullptr;
	Slot slots_[JOB_SLOTS];
	DlssRecorderStats stats_;
};

} // namespace lu::dlss
