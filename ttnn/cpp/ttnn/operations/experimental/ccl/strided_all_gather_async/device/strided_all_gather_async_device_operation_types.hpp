// SPDX-FileCopyrightText: © 2025 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>

#include "ttnn/tensor/tensor.hpp"
#include "ttnn/operations/ccl/ccl_common.hpp"
#include "ttnn/types.hpp"

// Pulls in the full definition of tt::tt_metal::GlobalSemaphoreImpl. This header (or a struct it
// defines) stores a GlobalSemaphore by value (directly, in std::optional, or in std::vector), and
// MSVC's aggregate/attribute reflection machinery (device_operation.hpp, tt_stl/reflection.hpp)
// needs the pimpl type to be complete at this point.
#include <tt_metal/impl/buffers/global_semaphore_impl.hpp>
namespace ttnn::experimental::prim {

// Matmul-signal aggregators (fused all-gather only) need one extra worker core per direction on top of the
// mux/worker cores. Auto falls back to reader-signaled matmul when those cores do not fit at the requested
// core_grid_offset; On requires them, Off never uses them.
enum class MMSignalAggregatorMode : uint8_t { Auto, On, Off };

struct StridedAllGatherAsyncParams {
    const std::vector<tt::tt_metal::IDevice*> devices;
    const uint32_t dim;
    const uint32_t num_links;
    const uint32_t ring_size;
    const tt::tt_metal::MemoryConfig output_mem_config;
    const ttnn::ccl::Topology topology;
    const std::vector<GlobalSemaphore> semaphore;
    const std::optional<uint32_t> cluster_axis;
    const std::optional<uint32_t> num_workers_per_link;
    const std::optional<uint32_t> num_buffers_per_channel;
    const std::optional<uint32_t> mm_cores_y;
    const std::optional<uint32_t> mm_block_ht;
    const std::optional<uint32_t> mm_block_wt;
};

struct StridedAllGatherAsyncInputs {
    const Tensor input_tensor;
    const std::optional<Tensor> persistent_output_buffer;
};

}  // namespace ttnn::experimental::prim
