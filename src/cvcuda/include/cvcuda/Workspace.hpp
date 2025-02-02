/*
 * SPDX-FileCopyrightText: Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CVCUDAERATORS_WORKSPACE_HPP
#define CVCUDAERATORS_WORKSPACE_HPP

#include "Workspace.h"

#include <nvcv/alloc/Allocator.hpp>
#include <nvcv/detail/Align.hpp>

#include <cassert>
#include <functional>
#include <utility>

namespace cvcuda {

using Workspace                = NVCVWorkspace;
using WorkspaceMem             = NVCVWorkspaceMem;
using WorkspaceRequirements    = NVCVWorkspaceRequirements;
using WorkspaceMemRequirements = NVCVWorkspaceMemRequirements;

/** Computes memory requirements that can cover both input requirements.
 *
 * The resulting memory requriements will have alignment and size that is not smaller than that of either
 * of the arguments.
 *
 * alignment = max(a.alignment, b.alignment)
 * size = align_up(max(a.size, b.size), alignment)
 */
inline WorkspaceMemRequirements MaxWorkspaceReq(WorkspaceMemRequirements a, WorkspaceMemRequirements b)
{
    WorkspaceMemRequirements ret;
    assert(!a.size || a.alignment > 0);
    assert(!b.size || b.alignment > 0);
    ret.alignment = b.alignment > a.alignment ? b.alignment : a.alignment;
    ret.size      = b.size > a.size ? b.size : a.size;
    assert((ret.alignment & (ret.alignment - 1)) == 0 && "Alignment must be a power of 2");
    ret.size = nvcv::detail::AlignUp(ret.size, ret.alignment);
    return ret;
}

/** Computes workspace requirements that can cover both input requirments. */
inline NVCVWorkspaceRequirements MaxWorkspaceReq(const WorkspaceRequirements &a, const WorkspaceRequirements &b)
{
    WorkspaceRequirements ret;
    ret.hostMem   = MaxWorkspaceReq(a.hostMem, b.hostMem);
    ret.pinnedMem = MaxWorkspaceReq(a.pinnedMem, b.pinnedMem);
    ret.cudaMem   = MaxWorkspaceReq(a.cudaMem, b.cudaMem);
    return ret;
}

/** A helper class that manages the lifetime of resources stored in a Workspace structure.
 *
 * This class works in a way similar to unique_ptr with a custom deleter.
 */
class UniqueWorkspace
{
public:
    using DeleterFunc = void(NVCVWorkspace &);
    using Deleter     = std::function<DeleterFunc>;

    UniqueWorkspace() = default;

    UniqueWorkspace(const UniqueWorkspace &) = delete;

    UniqueWorkspace(UniqueWorkspace &&ws)
    {
        swap(ws);
    }

    UniqueWorkspace &operator=(const UniqueWorkspace &) = delete;

    UniqueWorkspace &operator=(UniqueWorkspace &&ws) noexcept
    {
        swap(ws);
        ws.reset();
        return *this;
    }

    UniqueWorkspace(Workspace workspace, Deleter del = {})
        : m_impl(workspace)
        , m_del(std::move(del))
    {
    }

    UniqueWorkspace(WorkspaceMem host, WorkspaceMem pinned, WorkspaceMem cuda, Deleter del = {})
        : m_impl{host, pinned, cuda}
        , m_del(std::move(del))
    {
    }

    ~UniqueWorkspace()
    {
        reset();
    }

    void reset() noexcept
    {
        if (m_del)
        {
            m_del(m_impl);
            m_del  = {};
            m_impl = {};
        }
    }

    const Workspace &get() const
    {
        return m_impl;
    }

private:
    void swap(UniqueWorkspace &ws)
    {
        std::swap(m_impl, ws.m_impl);
        std::swap(m_del, ws.m_del);
    }

    Workspace m_impl{};
    Deleter   m_del{};
};

/** Allocates a workspace with an allocator specified in `alloc` (or a default one).
 *
 * This function is meant as a simple helper to simplify the usage operators requiring a workspace, but its intense use
 * may degrade performance due to excessive allocations and deallocations.
 * For code used in tight loops, some workspace reuse scheme and/or resource pools are recommended.
 */
inline UniqueWorkspace AllocateWorkspace(WorkspaceRequirements req, nvcv::Allocator alloc = {})
{
    if (!alloc)
    {
        nvcv::CustomAllocator<> cust{};
        alloc = std::move(cust);
    }
    auto del = [alloc](NVCVWorkspace &ws)
    {
        // TODO(michalz): Add proper CUDA error handling in public API
        if (ws.hostMem.data)
        {
            if (ws.hostMem.ready)
                if (cudaEventSynchronize(ws.hostMem.ready) != cudaSuccess)
                    throw std::runtime_error("cudaEventSynchronize failed");
            alloc.hostMem().free(ws.hostMem.data, ws.hostMem.req.size, ws.hostMem.req.alignment);
            ws.hostMem.data = nullptr;
        }
        if (ws.pinnedMem.data)
        {
            if (ws.pinnedMem.ready)
                if (cudaEventSynchronize(ws.pinnedMem.ready) != cudaSuccess)
                    throw std::runtime_error("cudaEventSynchronize failed");
            alloc.hostPinnedMem().free(ws.pinnedMem.data, ws.pinnedMem.req.size, ws.pinnedMem.req.alignment);
            ws.pinnedMem.data = nullptr;
        }
        if (ws.cudaMem.data)
        {
            if (ws.cudaMem.ready)
                if (cudaEventSynchronize(ws.cudaMem.ready) != cudaSuccess)
                    throw std::runtime_error("cudaEventSynchronize failed");
            alloc.cudaMem().free(ws.cudaMem.data, ws.cudaMem.req.size, ws.cudaMem.req.alignment);
            ws.cudaMem.data = nullptr;
        }
    };
    NVCVWorkspace ws = {};
    try
    {
        ws.hostMem.req   = req.hostMem;
        ws.pinnedMem.req = req.pinnedMem;
        ws.cudaMem.req   = req.cudaMem;

        if (req.hostMem.size)
            ws.hostMem.data = alloc.hostMem().alloc(req.hostMem.size, req.hostMem.alignment);
        if (req.pinnedMem.size)
            ws.pinnedMem.data = alloc.hostPinnedMem().alloc(req.pinnedMem.size, req.pinnedMem.alignment);
        if (req.cudaMem.size)
            ws.cudaMem.data = alloc.cudaMem().alloc(req.cudaMem.size, req.cudaMem.alignment);
        return UniqueWorkspace(ws, std::move(del));
    }
    catch (...)
    {
        del(ws);
        throw;
    }
}

} // namespace cvcuda

#endif // CVCUDAERATORS_WORKSPACE_HPP
