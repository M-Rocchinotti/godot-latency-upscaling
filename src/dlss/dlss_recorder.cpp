#include "dlss/dlss_recorder.h"

#include "upscale/reference_params.h"

#include <cstring>

namespace lu::dlss {

void DlssRecorder::set_provider(UpscalerProvider *provider) {
	std::lock_guard<std::mutex> lock(mutex_);
	provider_ = provider;
}

void DlssRecorder::set_job(uint32_t job, const EvaluateInputs &inputs) {
	if (job == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	Slot &slot = slots_[job % JOB_SLOTS];
	slot.job = job;
	slot.inputs = inputs;
}

bool DlssRecorder::record(const vk::MarkerDispatch &dispatch) {
	std::lock_guard<std::mutex> lock(mutex_);
	uint32_t job = 0;
	if (dispatch.push_constants_size >= upscale::REFERENCE_PARAMS_JOB_OFFSET + sizeof(job)) {
		std::memcpy(&job, dispatch.push_constants + upscale::REFERENCE_PARAMS_JOB_OFFSET, sizeof(job));
	}
	const Slot &slot = slots_[job % JOB_SLOTS];
	if (job == 0 || slot.job != job || provider_ == nullptr || !slot.inputs.color.valid() || !slot.inputs.depth.valid() ||
			!slot.inputs.velocity.valid() || !slot.inputs.output.valid()) {
		stats_.missing_job++;
		return false;
	}
	std::string error;
	if (!provider_->evaluate(dispatch.command_buffer, slot.inputs, error)) {
		stats_.failed++;
		stats_.last_error = error;
		return false;
	}
	stats_.evaluated++;
	return true;
}

DlssRecorderStats DlssRecorder::stats() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return stats_;
}

} // namespace lu::dlss
