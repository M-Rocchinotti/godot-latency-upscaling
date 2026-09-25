#include "upscale/camera_cut.h"

#include <cmath>

namespace lu::upscale {
namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double EPSILON = 1e-9;

struct Vec3 {
	double x, y, z;
};

Vec3 column(const CameraPose &pose, int c) {
	return Vec3{ pose.basis[c][0], pose.basis[c][1], pose.basis[c][2] };
}

double dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

Vec3 scaled(const Vec3 &v, double s) {
	return Vec3{ v.x * s, v.y * s, v.z * s };
}

Vec3 minus(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

bool normalize(Vec3 &v) {
	const double length = std::sqrt(dot(v, v));
	if (!(length > EPSILON) || !std::isfinite(length)) {
		return false;
	}
	v = scaled(v, 1.0 / length);
	return true;
}

// Gram-Schmidt on the basis columns; the third axis keeps the basis' handedness (`mirrored`).
bool orthonormal_frame(const CameraPose &pose, Vec3 frame[3], bool &mirrored) {
	Vec3 x = column(pose, 0);
	Vec3 y = column(pose, 1);
	const Vec3 z = column(pose, 2);
	if (!normalize(x)) {
		return false;
	}
	y = minus(y, scaled(x, dot(y, x)));
	if (!normalize(y)) {
		return false;
	}
	Vec3 n = cross(x, y);
	const double side = dot(n, z);
	if (!(std::fabs(side) > EPSILON)) {
		return false; // z lies in the x-y plane: degenerate
	}
	mirrored = side < 0.0;
	frame[0] = x;
	frame[1] = y;
	frame[2] = mirrored ? scaled(n, -1.0) : n;
	return true;
}

bool finite_pose(const CameraPose &pose) {
	for (int c = 0; c < 3; ++c) {
		for (int r = 0; r < 3; ++r) {
			if (!std::isfinite(pose.basis[c][r])) {
				return false;
			}
		}
		if (!std::isfinite(pose.origin[c])) {
			return false;
		}
	}
	return true;
}

} // namespace

const char *cut_reason_name(CutReason reason) {
	switch (reason) {
		case CutReason::None:
			return "none";
		case CutReason::CameraSwitch:
			return "camera_switch";
		case CutReason::Rotation:
			return "rotation";
		case CutReason::Translation:
			return "translation";
		case CutReason::InvalidPose:
			return "invalid_pose";
	}
	return "unknown";
}

double rotation_angle_degrees(const CameraPose &a, const CameraPose &b) {
	Vec3 fa[3], fb[3];
	bool mirrored_a = false, mirrored_b = false;
	if (!orthonormal_frame(a, fa, mirrored_a) || !orthonormal_frame(b, fb, mirrored_b) || mirrored_a != mirrored_b) {
		return 180.0;
	}
	// R = A^T B: r[i][j] = dot(a_i, b_j). angle = atan2(|skew part| / 2, (trace - 1) / 2), which stays
	// accurate near 0 and 180 degrees (acos alone loses precision near 0).
	double r[3][3];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			r[i][j] = dot(fa[i], fb[j]);
		}
	}
	const double sx = r[2][1] - r[1][2];
	const double sy = r[0][2] - r[2][0];
	const double sz = r[1][0] - r[0][1];
	const double sine = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
	const double cosine = 0.5 * (r[0][0] + r[1][1] + r[2][2] - 1.0);
	return std::atan2(sine, cosine) * 180.0 / PI;
}

CutReason detect_cut(const CameraPose &previous, const CameraPose &current, bool camera_switched,
		const CutThresholds &thresholds) {
	if (camera_switched) {
		return CutReason::CameraSwitch;
	}
	if (!finite_pose(previous) || !finite_pose(current)) {
		return CutReason::InvalidPose;
	}
	if (thresholds.max_distance > 0.0) {
		const double dx = current.origin[0] - previous.origin[0];
		const double dy = current.origin[1] - previous.origin[1];
		const double dz = current.origin[2] - previous.origin[2];
		if (std::sqrt(dx * dx + dy * dy + dz * dz) > thresholds.max_distance) {
			return CutReason::Translation;
		}
	}
	if (thresholds.max_angle_degrees > 0.0 && rotation_angle_degrees(previous, current) > thresholds.max_angle_degrees) {
		return CutReason::Rotation;
	}
	return CutReason::None;
}

} // namespace lu::upscale
