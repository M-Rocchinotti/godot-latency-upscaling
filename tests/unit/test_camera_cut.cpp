#include "test_framework.h"
#include "upscale/camera_cut.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace lu::upscale;

namespace {

constexpr double PI = 3.14159265358979323846;

// Rotation about a unit axis by `degrees` (Rodrigues), as basis columns, then scaled.
CameraPose rotated(double ax, double ay, double az, double degrees, double scale = 1.0) {
	const double t = degrees * PI / 180.0;
	const double c = std::cos(t), s = std::sin(t), k = 1.0 - c;
	const double m[3][3] = { // m[row][col]
		{ c + ax * ax * k, ax * ay * k - az * s, ax * az * k + ay * s },
		{ ay * ax * k + az * s, c + ay * ay * k, ay * az * k - ax * s },
		{ az * ax * k - ay * s, az * ay * k + ax * s, c + az * az * k },
	};
	CameraPose pose;
	for (int col = 0; col < 3; ++col) {
		for (int row = 0; row < 3; ++row) {
			pose.basis[col][row] = m[row][col] * scale;
		}
	}
	return pose;
}

CameraPose yaw(double degrees) {
	return rotated(0, 1, 0, degrees);
}

CameraPose at(CameraPose pose, double x, double y, double z) {
	pose.origin[0] = x;
	pose.origin[1] = y;
	pose.origin[2] = z;
	return pose;
}

} // namespace

TEST(camera_cut_rotation_angle_covers_yaw_pitch_and_roll) {
	const CameraPose identity;
	CHECK_NEAR(rotation_angle_degrees(identity, identity), 0.0, 1e-9);
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(30)), 30.0, 1e-6);
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(-30)), 30.0, 1e-6); // an angle, not a signed yaw
	CHECK_NEAR(rotation_angle_degrees(identity, rotated(1, 0, 0, 45)), 45.0, 1e-6); // pitch
	CHECK_NEAR(rotation_angle_degrees(identity, rotated(0, 0, 1, 90)), 90.0, 1e-6); // roll
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(150)), 150.0, 1e-6);
	// Folded into 0..180: a 210 degree yaw is a 150 degree rotation.
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(210)), 150.0, 1e-6);
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(180)), 180.0, 1e-6);
	// Relative: from yaw 100 to yaw 130 is 30, whatever the start.
	CHECK_NEAR(rotation_angle_degrees(yaw(100), yaw(130)), 30.0, 1e-6);
	// Precise for the small per-frame turns of normal play (acos alone is not).
	CHECK_NEAR(rotation_angle_degrees(identity, yaw(0.01)), 0.01, 1e-9);
	const double inv = 1.0 / std::sqrt(3.0);
	CHECK_NEAR(rotation_angle_degrees(rotated(inv, inv, inv, 20), rotated(inv, inv, inv, 95)), 75.0, 1e-6);
}

TEST(camera_cut_rotation_angle_ignores_scale_and_rejects_degenerate_or_mirrored_bases) {
	CHECK_NEAR(rotation_angle_degrees(yaw(10), rotated(0, 1, 0, 40, 2.5)), 30.0, 1e-6); // a scaled camera node
	CameraPose flat; // z axis collapsed onto x: no orientation
	flat.basis[2][0] = 1.0;
	flat.basis[2][2] = 0.0;
	CHECK_NEAR(rotation_angle_degrees(CameraPose(), flat), 180.0, 1e-9);
	CameraPose zero;
	std::memset(zero.basis, 0, sizeof(zero.basis));
	CHECK_NEAR(rotation_angle_degrees(zero, CameraPose()), 180.0, 1e-9);
	CameraPose mirrored; // negative scale on x: the image flips, no history applies
	mirrored.basis[0][0] = -1.0;
	CHECK_NEAR(rotation_angle_degrees(CameraPose(), mirrored), 180.0, 1e-9);
	CHECK_NEAR(rotation_angle_degrees(mirrored, mirrored), 0.0, 1e-9); // two mirrored poses compare normally
}

TEST(camera_cut_triggers_on_a_switch_a_turn_or_a_jump_past_the_thresholds) {
	const CutThresholds t{ 60.0, 10.0 };
	const CameraPose a = at(yaw(-60), 0, 1.6, 6);
	// Normal motion: a fast turn (1.2 degrees per frame) and a strafe are not cuts.
	CHECK(detect_cut(a, at(yaw(-58.8), 0, 1.6, 6), false, t) == CutReason::None);
	CHECK(detect_cut(a, at(yaw(-60), 0.03, 1.6, 6), false, t) == CutReason::None);
	CHECK(detect_cut(a, a, false, t) == CutReason::None);
	// The reviewer's 150 degree cut.
	CHECK(detect_cut(a, at(yaw(90), 0, 1.6, 6), false, t) == CutReason::Rotation);
	// Just below a threshold is not a cut, just past it is (translation: exactly at it is not).
	CHECK(detect_cut(a, at(yaw(-0.01), 0, 1.6, 6), false, t) == CutReason::None);
	CHECK(detect_cut(a, at(yaw(0.01), 0, 1.6, 6), false, t) == CutReason::Rotation);
	CHECK(detect_cut(a, at(yaw(-60), 10.0, 1.6, 6), false, t) == CutReason::None);
	CHECK(detect_cut(a, at(yaw(-60), 10.001, 1.6, 6), false, t) == CutReason::Translation);
	CHECK(detect_cut(a, at(yaw(-60), 6, 1.6 + 6, 6 + 6), false, t) == CutReason::Translation); // |(6,6,6)| = 10.4
	// A switch to another camera is a cut even at the same pose.
	CHECK(detect_cut(a, a, true, t) == CutReason::CameraSwitch);
}

TEST(camera_cut_zero_thresholds_disable_their_test) {
	const CameraPose a;
	const CameraPose turned = yaw(170);
	const CameraPose moved = at(CameraPose(), 1000, 0, 0);
	CHECK(detect_cut(a, turned, false, CutThresholds{ 0.0, 10.0 }) == CutReason::None);
	CHECK(detect_cut(a, moved, false, CutThresholds{ 60.0, 0.0 }) == CutReason::None);
	CHECK(detect_cut(a, turned, false, CutThresholds{ 60.0, 0.0 }) == CutReason::Rotation);
	CHECK(detect_cut(a, moved, false, CutThresholds{ 0.0, 10.0 }) == CutReason::Translation);
	// Detection off entirely: only an explicit camera switch counts.
	CHECK(detect_cut(a, at(turned, 1000, 0, 0), false, CutThresholds{ 0.0, 0.0 }) == CutReason::None);
	CHECK(detect_cut(a, a, true, CutThresholds{ 0.0, 0.0 }) == CutReason::CameraSwitch);
}

TEST(camera_cut_invalid_poses_reset_the_history) {
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	CameraPose bad;
	bad.origin[1] = nan;
	CHECK(detect_cut(CameraPose(), bad, false, CutThresholds{}) == CutReason::InvalidPose);
	CHECK(detect_cut(bad, CameraPose(), false, CutThresholds{}) == CutReason::InvalidPose);
	CameraPose bad_basis;
	bad_basis.basis[1][2] = inf;
	CHECK(detect_cut(CameraPose(), bad_basis, false, CutThresholds{ 0.0, 0.0 }) == CutReason::InvalidPose);
}

TEST(camera_cut_reason_names) {
	CHECK(std::strcmp(cut_reason_name(CutReason::None), "none") == 0);
	CHECK(std::strcmp(cut_reason_name(CutReason::CameraSwitch), "camera_switch") == 0);
	CHECK(std::strcmp(cut_reason_name(CutReason::Rotation), "rotation") == 0);
	CHECK(std::strcmp(cut_reason_name(CutReason::Translation), "translation") == 0);
	CHECK(std::strcmp(cut_reason_name(CutReason::InvalidPose), "invalid_pose") == 0);
}
