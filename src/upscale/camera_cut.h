// Camera-cut detection: when the upscaler's history must be thrown away.
//
// NVIDIA's DLSS programming guide asks for a history reset on camera cuts (the "reset" flag of the
// evaluation); without one the frame after a cut shows streaks of the previous view. A cut is:
//   - a switch to another Camera3D (the main thread sees the viewport's current camera change),
//   - the same camera turning further than `max_angle_degrees` in one frame (yaw, pitch and roll
//     alike: the angle of the relative rotation), or
//   - the same camera moving further than `max_distance` in one frame (a teleport or respawn).
// A threshold of 0 disables that test. The logic follows the post-effects addon's cut detection
// (E:/godot-post-effects, post_fx_hdr_effect.gd: _is_camera_cut / rotation_angle).
//
// Pure C++: no Godot types, so the trigger logic is unit-tested (tests/unit/test_camera_cut.cpp).
#pragma once

#include <cstdint>

namespace lu::upscale {

// A camera's world transform: basis columns (x, y, z axes; may carry scale) and origin.
struct CameraPose {
	double basis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }; // basis[column][row]
	double origin[3] = { 0, 0, 0 };
};

struct CutThresholds {
	double max_angle_degrees = 60.0; // 0: rotation never counts as a cut
	double max_distance = 10.0; // world units (metres); 0: translation never counts as a cut
};

enum class CutReason {
	None,
	CameraSwitch, // another camera became current
	Rotation, // turned further than max_angle_degrees in one frame
	Translation, // moved further than max_distance in one frame
	InvalidPose, // NaN / infinite / degenerate transform: nothing to reproject from
};

const char *cut_reason_name(CutReason reason);

// The angle (degrees, 0..180) of the rotation from `a` to `b`. Scale is removed first. Returns 180
// when either basis is degenerate or their handedness differs (a mirror is not a rotation).
double rotation_angle_degrees(const CameraPose &a, const CameraPose &b);

// Whether the frame showing `current` is a cut from the frame that showed `previous`.
// `camera_switched`: the main thread saw the current camera change since the previous frame.
CutReason detect_cut(const CameraPose &previous, const CameraPose &current, bool camera_switched,
		const CutThresholds &thresholds);

} // namespace lu::upscale
