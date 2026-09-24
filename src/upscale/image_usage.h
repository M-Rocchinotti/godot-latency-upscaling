// Godot RenderingDevice texture usage bits -> the VkImageUsageFlags its Vulkan driver creates the
// image with (rendering_device_driver_vulkan.cpp texture_create, Godot 4.7.2). Streamline needs the
// usage of every tagged image, and Godot does not hand it out.
//
// Pure, unit-tested.
#pragma once

#include "vk/vk_common.h"

#include <cstdint>

namespace lu::upscale {

// RenderingDevice::TextureUsageBits (rendering_device_commons.h).
enum RdTextureUsage : uint32_t {
	RD_USAGE_SAMPLING = 1u << 0,
	RD_USAGE_COLOR_ATTACHMENT = 1u << 1,
	RD_USAGE_DEPTH_STENCIL_ATTACHMENT = 1u << 2,
	RD_USAGE_STORAGE = 1u << 3,
	RD_USAGE_STORAGE_ATOMIC = 1u << 4,
	RD_USAGE_CPU_READ = 1u << 5,
	RD_USAGE_CAN_UPDATE = 1u << 6,
	RD_USAGE_CAN_COPY_FROM = 1u << 7,
	RD_USAGE_CAN_COPY_TO = 1u << 8,
	RD_USAGE_INPUT_ATTACHMENT = 1u << 9,
	RD_USAGE_VRS_ATTACHMENT = 1u << 10,
	RD_USAGE_TRANSIENT = 1u << 11,
	RD_USAGE_DEPTH_RESOLVE_ATTACHMENT = 1u << 12,
};

inline VkImageUsageFlags vk_image_usage(uint32_t rd_usage) {
	VkImageUsageFlags usage = 0;
	if (rd_usage & RD_USAGE_SAMPLING) {
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	if (rd_usage & RD_USAGE_STORAGE) {
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}
	if (rd_usage & RD_USAGE_COLOR_ATTACHMENT) {
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}
	if (rd_usage & (RD_USAGE_DEPTH_STENCIL_ATTACHMENT | RD_USAGE_DEPTH_RESOLVE_ATTACHMENT)) {
		usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	if (rd_usage & RD_USAGE_INPUT_ATTACHMENT) {
		usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	}
	if (rd_usage & (RD_USAGE_CAN_UPDATE | RD_USAGE_CAN_COPY_TO)) {
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	if (rd_usage & RD_USAGE_CAN_COPY_FROM) {
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	return usage;
}

} // namespace lu::upscale
