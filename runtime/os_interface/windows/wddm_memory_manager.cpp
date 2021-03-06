/*
 * Copyright (C) 2017-2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "runtime/os_interface/windows/wddm_memory_manager.h"
#include "runtime/command_stream/command_stream_receiver_hw.h"
#include "runtime/device/device.h"
#include "runtime/gmm_helper/gmm.h"
#include "runtime/gmm_helper/gmm_helper.h"
#include "runtime/gmm_helper/resource_info.h"
#include "runtime/helpers/aligned_memory.h"
#include "runtime/helpers/ptr_math.h"
#include "runtime/helpers/surface_formats.h"
#include "runtime/memory_manager/deferrable_deletion.h"
#include "runtime/memory_manager/deferred_deleter.h"
#include "runtime/os_interface/windows/wddm/wddm.h"
#include "runtime/os_interface/windows/wddm_allocation.h"
#include "runtime/os_interface/windows/wddm_residency_controller.h"
#include "runtime/os_interface/windows/os_context_win.h"
#include "runtime/platform/platform.h"
#include <algorithm>

namespace OCLRT {

WddmMemoryManager::~WddmMemoryManager() {
    applyCommonCleanup();

    for (auto osContext : this->registeredOsContexts) {
        if (osContext) {
            auto &residencyController = osContext->get()->getResidencyController();
            residencyController.acquireTrimCallbackLock();
            wddm->unregisterTrimCallback(trimCallback);
            residencyController.releaseTrimCallbackLock();

            // Wait for lock to ensure trimCallback ended
            residencyController.acquireTrimCallbackLock();
            residencyController.releaseTrimCallbackLock();
        }
    }
}

WddmMemoryManager::WddmMemoryManager(bool enable64kbPages, bool enableLocalMemory, Wddm *wddm, ExecutionEnvironment &executionEnvironment) : MemoryManager(enable64kbPages, enableLocalMemory, executionEnvironment) {
    DEBUG_BREAK_IF(wddm == nullptr);
    this->wddm = wddm;
    allocator32Bit = std::unique_ptr<Allocator32bit>(new Allocator32bit(wddm->getHeap32Base(), wddm->getHeap32Size()));
    wddm->registerTrimCallback(trimCallback, this);
    asyncDeleterEnabled = DebugManager.flags.EnableDeferredDeleter.get();
    if (asyncDeleterEnabled)
        deferredDeleter = createDeferredDeleter();
    mallocRestrictions.minAddress = wddm->getWddmMinAddress();
}

void APIENTRY WddmMemoryManager::trimCallback(_Inout_ D3DKMT_TRIMNOTIFICATION *trimNotification) {
    WddmMemoryManager *wddmMemMngr = (WddmMemoryManager *)trimNotification->Context;
    DEBUG_BREAK_IF(wddmMemMngr == nullptr);

    if (wddmMemMngr->getOsContextCount() == 0) {
        return;
    }

    wddmMemMngr->getRegisteredOsContext(0)->get()->getResidencyController().acquireTrimCallbackLock();
    wddmMemMngr->trimResidency(trimNotification->Flags, trimNotification->NumBytesToTrim);
    wddmMemMngr->getRegisteredOsContext(0)->get()->getResidencyController().releaseTrimCallbackLock();
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemoryForImage(ImageInfo &imgInfo, Gmm *gmm) {
    if (!GmmHelper::allowTiling(*imgInfo.imgDesc) && imgInfo.mipCount == 0) {
        delete gmm;
        return allocateGraphicsMemory(imgInfo.size);
    }
    auto allocation = new WddmAllocation(nullptr, imgInfo.size, nullptr, MemoryPool::SystemCpuInaccessible, getOsContextCount());
    allocation->gmm = gmm;

    if (!WddmMemoryManager::createWddmAllocation(allocation, AllocationOrigin::EXTERNAL_ALLOCATION)) {
        delete allocation;
        return nullptr;
    }
    return allocation;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory64kb(size_t size, size_t alignment, bool forcePin, bool preferRenderCompressed) {
    size_t sizeAligned = alignUp(size, MemoryConstants::pageSize64k);
    Gmm *gmm = nullptr;

    auto wddmAllocation = new WddmAllocation(nullptr, sizeAligned, nullptr, sizeAligned, nullptr, MemoryPool::System64KBPages, getOsContextCount());

    gmm = new Gmm(nullptr, sizeAligned, false, preferRenderCompressed, true);
    wddmAllocation->gmm = gmm;

    if (!wddm->createAllocation64k(wddmAllocation)) {
        delete gmm;
        delete wddmAllocation;
        return nullptr;
    }

    auto cpuPtr = lockResource(wddmAllocation);
    wddmAllocation->setLocked(true);

    wddmAllocation->setAlignedCpuPtr(cpuPtr);
    // 64kb map is not needed
    auto status = wddm->mapGpuVirtualAddress(wddmAllocation, cpuPtr, false, false, false);
    DEBUG_BREAK_IF(!status);
    wddmAllocation->setCpuPtrAndGpuAddress(cpuPtr, (uint64_t)wddmAllocation->gpuPtr);

    return wddmAllocation;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory(size_t size, size_t alignment, bool forcePin, bool uncacheable) {
    size_t newAlignment = alignment ? alignUp(alignment, MemoryConstants::pageSize) : MemoryConstants::pageSize;
    size_t sizeAligned = size ? alignUp(size, MemoryConstants::pageSize) : MemoryConstants::pageSize;
    void *pSysMem = allocateSystemMemory(sizeAligned, newAlignment);
    Gmm *gmm = nullptr;

    if (pSysMem == nullptr) {
        return nullptr;
    }

    auto wddmAllocation = new WddmAllocation(pSysMem, sizeAligned, pSysMem, sizeAligned, nullptr, MemoryPool::System4KBPages, getOsContextCount());
    wddmAllocation->cpuPtrAllocated = true;

    gmm = new Gmm(pSysMem, sizeAligned, uncacheable);

    wddmAllocation->gmm = gmm;

    if (!createWddmAllocation(wddmAllocation, AllocationOrigin::EXTERNAL_ALLOCATION)) {
        delete gmm;
        delete wddmAllocation;
        freeSystemMemory(pSysMem);
        return nullptr;
    }
    return wddmAllocation;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemoryForNonSvmHostPtr(size_t size, void *cpuPtr) {
    auto alignedPtr = alignDown(cpuPtr, MemoryConstants::pageSize);
    auto offsetInPage = ptrDiff(cpuPtr, alignedPtr);
    auto alignedSize = alignSizeWholePage(cpuPtr, size);

    auto wddmAllocation = new WddmAllocation(cpuPtr, size, alignedPtr, alignedSize, nullptr, MemoryPool::System4KBPages, getOsContextCount());
    wddmAllocation->allocationOffset = offsetInPage;

    auto gmm = new Gmm(alignedPtr, alignedSize, false);

    wddmAllocation->gmm = gmm;

    if (!createWddmAllocation(wddmAllocation, AllocationOrigin::EXTERNAL_ALLOCATION)) {
        delete gmm;
        delete wddmAllocation;
        return nullptr;
    }
    return wddmAllocation;
}

GraphicsAllocation *WddmMemoryManager::allocateGraphicsMemory(size_t size, const void *ptrArg) {
    void *ptr = const_cast<void *>(ptrArg);

    if (ptr == nullptr) {
        DEBUG_BREAK_IF(true);
        return nullptr;
    }

    if (mallocRestrictions.minAddress > reinterpret_cast<uintptr_t>(ptrArg)) {
        void *reserve = nullptr;
        void *ptrAligned = alignDown(ptr, MemoryConstants::allocationAlignment);
        size_t sizeAligned = alignSizeWholePage(ptr, size);
        size_t offset = ptrDiff(ptr, ptrAligned);

        if (!wddm->reserveValidAddressRange(sizeAligned, reserve)) {
            return nullptr;
        }

        auto allocation = new WddmAllocation(ptr, size, ptrAligned, sizeAligned, reserve, MemoryPool::System4KBPages, getOsContextCount());
        allocation->allocationOffset = offset;

        Gmm *gmm = new Gmm(ptrAligned, sizeAligned, false);
        allocation->gmm = gmm;
        if (createWddmAllocation(allocation, AllocationOrigin::EXTERNAL_ALLOCATION)) {
            return allocation;
        }
        freeGraphicsMemory(allocation);
        return nullptr;
    }

    return MemoryManager::allocateGraphicsMemory(size, ptr);
}

GraphicsAllocation *WddmMemoryManager::allocate32BitGraphicsMemory(size_t size, const void *ptr, AllocationOrigin allocationOrigin) {
    Gmm *gmm = nullptr;
    const void *ptrAligned = nullptr;
    size_t sizeAligned = size;
    void *pSysMem = nullptr;
    size_t offset = 0;
    bool cpuPtrAllocated = false;

    if (ptr) {
        ptrAligned = alignDown(ptr, MemoryConstants::allocationAlignment);
        sizeAligned = alignSizeWholePage(ptr, size);
        offset = ptrDiff(ptr, ptrAligned);
    } else {
        sizeAligned = alignUp(size, MemoryConstants::allocationAlignment);
        pSysMem = allocateSystemMemory(sizeAligned, MemoryConstants::allocationAlignment);
        if (pSysMem == nullptr) {
            return nullptr;
        }
        ptrAligned = pSysMem;
        cpuPtrAllocated = true;
    }

    auto wddmAllocation = new WddmAllocation(const_cast<void *>(ptrAligned), sizeAligned, const_cast<void *>(ptrAligned), sizeAligned, nullptr, MemoryPool::System4KBPagesWith32BitGpuAddressing, getOsContextCount());
    wddmAllocation->cpuPtrAllocated = cpuPtrAllocated;
    wddmAllocation->is32BitAllocation = true;
    wddmAllocation->allocationOffset = offset;

    gmm = new Gmm(ptrAligned, sizeAligned, false);
    wddmAllocation->gmm = gmm;

    if (!createWddmAllocation(wddmAllocation, allocationOrigin)) {
        delete gmm;
        delete wddmAllocation;
        freeSystemMemory(pSysMem);
        return nullptr;
    }

    wddmAllocation->is32BitAllocation = true;
    auto baseAddress = allocationOrigin == AllocationOrigin::EXTERNAL_ALLOCATION ? allocator32Bit->getBase() : this->wddm->getGfxPartition().Heap32[1].Base;
    wddmAllocation->gpuBaseAddress = GmmHelper::canonize(baseAddress);

    return wddmAllocation;
}

GraphicsAllocation *WddmMemoryManager::createAllocationFromHandle(osHandle handle, bool requireSpecificBitness, bool ntHandle) {
    auto allocation = new WddmAllocation(nullptr, 0, handle, MemoryPool::SystemCpuInaccessible, getOsContextCount());
    bool is32BitAllocation = false;

    bool status = ntHandle ? wddm->openNTHandle((HANDLE)((UINT_PTR)handle), allocation)
                           : wddm->openSharedHandle(handle, allocation);

    if (!status) {
        delete allocation;
        return nullptr;
    }

    // Shared objects are passed without size
    size_t size = allocation->gmm->gmmResourceInfo->getSizeAllocation();
    allocation->setSize(size);

    void *ptr = nullptr;
    if (is32bit) {
        if (!wddm->reserveValidAddressRange(size, ptr)) {
            delete allocation;
            return nullptr;
        }
        allocation->setReservedAddress(ptr);
    } else if (requireSpecificBitness && this->force32bitAllocations) {
        is32BitAllocation = true;
        allocation->is32BitAllocation = true;
        allocation->gpuBaseAddress = GmmHelper::canonize(allocator32Bit->getBase());
    }
    status = wddm->mapGpuVirtualAddress(allocation, ptr, is32BitAllocation, false, false);
    DEBUG_BREAK_IF(!status);
    allocation->setGpuAddress(allocation->gpuPtr);
    return allocation;
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocationFromSharedHandle(osHandle handle, bool requireSpecificBitness) {
    return createAllocationFromHandle(handle, requireSpecificBitness, false);
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocationFromNTHandle(void *handle) {
    return createAllocationFromHandle((osHandle)((UINT_PTR)handle), false, true);
}

void WddmMemoryManager::addAllocationToHostPtrManager(GraphicsAllocation *gfxAllocation) {
    WddmAllocation *wddmMemory = static_cast<WddmAllocation *>(gfxAllocation);
    FragmentStorage fragment = {};
    fragment.driverAllocation = true;
    fragment.fragmentCpuPointer = gfxAllocation->getUnderlyingBuffer();
    fragment.fragmentSize = alignUp(gfxAllocation->getUnderlyingBufferSize(), MemoryConstants::pageSize);

    fragment.osInternalStorage = new OsHandle();
    fragment.osInternalStorage->gpuPtr = gfxAllocation->getGpuAddress();
    fragment.osInternalStorage->handle = wddmMemory->handle;
    fragment.osInternalStorage->gmm = gfxAllocation->gmm;
    fragment.residency = &wddmMemory->getResidencyData();
    hostPtrManager.storeFragment(fragment);
}

void WddmMemoryManager::removeAllocationFromHostPtrManager(GraphicsAllocation *gfxAllocation) {
    auto buffer = gfxAllocation->getUnderlyingBuffer();
    auto fragment = hostPtrManager.getFragment(buffer);
    if (fragment && fragment->driverAllocation) {
        OsHandle *osStorageToRelease = fragment->osInternalStorage;
        if (hostPtrManager.releaseHostPtr(buffer)) {
            delete osStorageToRelease;
        }
    }
}

void *WddmMemoryManager::lockResource(GraphicsAllocation *graphicsAllocation) {
    return wddm->lockResource(static_cast<WddmAllocation *>(graphicsAllocation));
};
void WddmMemoryManager::unlockResource(GraphicsAllocation *graphicsAllocation) {
    wddm->unlockResource(static_cast<WddmAllocation *>(graphicsAllocation));
};

void WddmMemoryManager::freeGraphicsMemoryImpl(GraphicsAllocation *gfxAllocation) {
    WddmAllocation *input = static_cast<WddmAllocation *>(gfxAllocation);
    DEBUG_BREAK_IF(!validateAllocation(input));
    if (gfxAllocation == nullptr) {
        return;
    }

    for (auto &osContext : this->registeredOsContexts) {
        if (osContext) {
            auto &residencyController = osContext->get()->getResidencyController();
            residencyController.acquireLock();
            residencyController.removeFromTrimCandidateListIfUsed(input, true);
            residencyController.releaseLock();
        }
    }

    UNRECOVERABLE_IF(DebugManager.flags.CreateMultipleDevices.get() == 0 &&
                     gfxAllocation->taskCount != ObjectNotUsed && this->executionEnvironment.commandStreamReceivers.size() > 0 &&
                     this->getCommandStreamReceiver(0) && this->getCommandStreamReceiver(0)->getTagAddress() &&
                     gfxAllocation->taskCount > *this->getCommandStreamReceiver(0)->getTagAddress());

    if (input->gmm) {
        if (input->gmm->isRenderCompressed && wddm->getPageTableManager()) {
            auto status = wddm->updateAuxTable(input->gpuPtr, input->gmm, false);
            DEBUG_BREAK_IF(!status);
        }
        delete input->gmm;
    }

    if (input->peekSharedHandle() == false &&
        input->cpuPtrAllocated == false &&
        input->fragmentsStorage.fragmentCount > 0) {
        cleanGraphicsMemoryCreatedFromHostPtr(gfxAllocation);
    } else {
        D3DKMT_HANDLE *allocationHandles = nullptr;
        uint32_t allocationCount = 0;
        D3DKMT_HANDLE resourceHandle = 0;
        void *cpuPtr = nullptr;
        if (input->peekSharedHandle()) {
            resourceHandle = input->resourceHandle;
        } else {
            allocationHandles = &input->handle;
            allocationCount = 1;
            if (input->cpuPtrAllocated) {
                cpuPtr = input->getAlignedCpuPtr();
            }
        }
        if (input->isLocked()) {
            unlockResource(input);
            input->setLocked(false);
        }
        auto status = tryDeferDeletions(allocationHandles, allocationCount, resourceHandle);
        DEBUG_BREAK_IF(!status);
        alignedFreeWrapper(cpuPtr);
    }
    wddm->releaseReservedAddress(input->getReservedAddress());
    delete gfxAllocation;
}

bool WddmMemoryManager::tryDeferDeletions(D3DKMT_HANDLE *handles, uint32_t allocationCount, D3DKMT_HANDLE resourceHandle) {
    bool status = true;
    if (deferredDeleter) {
        deferredDeleter->deferDeletion(DeferrableDeletion::create(wddm, handles, allocationCount, resourceHandle));
    } else {
        status = wddm->destroyAllocations(handles, allocationCount, resourceHandle);
    }
    return status;
}

bool WddmMemoryManager::validateAllocation(WddmAllocation *alloc) {
    if (alloc == nullptr)
        return false;
    auto size = alloc->getUnderlyingBufferSize();
    if (alloc->getGpuAddress() == 0u || size == 0 || (alloc->handle == 0 && alloc->fragmentsStorage.fragmentCount == 0))
        return false;
    return true;
}

MemoryManager::AllocationStatus WddmMemoryManager::populateOsHandles(OsHandleStorage &handleStorage) {
    uint32_t allocatedFragmentIndexes[maxFragmentsCount];
    uint32_t allocatedFragmentsCounter = 0;

    for (unsigned int i = 0; i < maxFragmentsCount; i++) {
        // If no fragment is present it means it already exists.
        if (!handleStorage.fragmentStorageData[i].osHandleStorage && handleStorage.fragmentStorageData[i].cpuPtr) {
            handleStorage.fragmentStorageData[i].osHandleStorage = new OsHandle();
            handleStorage.fragmentStorageData[i].residency = new ResidencyData();

            handleStorage.fragmentStorageData[i].osHandleStorage->gmm = new Gmm(handleStorage.fragmentStorageData[i].cpuPtr, handleStorage.fragmentStorageData[i].fragmentSize, false);
            allocatedFragmentIndexes[allocatedFragmentsCounter] = i;
            allocatedFragmentsCounter++;
        }
    }
    NTSTATUS result = wddm->createAllocationsAndMapGpuVa(handleStorage);

    if (result == STATUS_GRAPHICS_NO_VIDEO_MEMORY) {
        return AllocationStatus::InvalidHostPointer;
    }

    for (uint32_t i = 0; i < allocatedFragmentsCounter; i++) {
        hostPtrManager.storeFragment(handleStorage.fragmentStorageData[allocatedFragmentIndexes[i]]);
    }

    return AllocationStatus::Success;
}

void WddmMemoryManager::cleanOsHandles(OsHandleStorage &handleStorage) {

    D3DKMT_HANDLE handles[maxFragmentsCount] = {0};
    auto allocationCount = 0;

    for (unsigned int i = 0; i < maxFragmentsCount; i++) {
        if (handleStorage.fragmentStorageData[i].freeTheFragment) {
            handles[allocationCount] = handleStorage.fragmentStorageData[i].osHandleStorage->handle;
            handleStorage.fragmentStorageData[i].residency->resident = false;
            allocationCount++;
        }
    }

    bool success = tryDeferDeletions(handles, allocationCount, 0);

    for (unsigned int i = 0; i < maxFragmentsCount; i++) {
        if (handleStorage.fragmentStorageData[i].freeTheFragment) {
            if (success) {
                handleStorage.fragmentStorageData[i].osHandleStorage->handle = 0;
            }
            delete handleStorage.fragmentStorageData[i].osHandleStorage->gmm;
            delete handleStorage.fragmentStorageData[i].osHandleStorage;
            delete handleStorage.fragmentStorageData[i].residency;
        }
    }
}

void WddmMemoryManager::obtainGpuAddresFromFragments(WddmAllocation *allocation, OsHandleStorage &handleStorage) {
    if (this->force32bitAllocations && (handleStorage.fragmentCount > 0)) {
        auto hostPtr = allocation->getUnderlyingBuffer();
        auto fragment = hostPtrManager.getFragment(hostPtr);
        if (fragment && fragment->driverAllocation) {
            auto gpuPtr = handleStorage.fragmentStorageData[0].osHandleStorage->gpuPtr;
            for (uint32_t i = 1; i < handleStorage.fragmentCount; i++) {
                if (handleStorage.fragmentStorageData[i].osHandleStorage->gpuPtr < gpuPtr) {
                    gpuPtr = handleStorage.fragmentStorageData[i].osHandleStorage->gpuPtr;
                }
            }
            allocation->allocationOffset = reinterpret_cast<uint64_t>(hostPtr) & MemoryConstants::pageMask;
            allocation->setGpuAddress(gpuPtr);
        }
    }
}

GraphicsAllocation *WddmMemoryManager::createGraphicsAllocation(OsHandleStorage &handleStorage, size_t hostPtrSize, const void *hostPtr) {
    auto allocation = new WddmAllocation(const_cast<void *>(hostPtr), hostPtrSize, const_cast<void *>(hostPtr), hostPtrSize, nullptr, MemoryPool::System4KBPages, getOsContextCount());
    allocation->fragmentsStorage = handleStorage;
    obtainGpuAddresFromFragments(allocation, handleStorage);
    return allocation;
}

uint64_t WddmMemoryManager::getSystemSharedMemory() {
    return wddm->getSystemSharedMemory();
}

uint64_t WddmMemoryManager::getMaxApplicationAddress() {
    return wddm->getMaxApplicationAddress();
}

uint64_t WddmMemoryManager::getInternalHeapBaseAddress() {
    return this->wddm->getGfxPartition().Heap32[1].Base;
}

bool WddmMemoryManager::makeResidentResidencyAllocations(ResidencyContainer &allocationsForResidency, OsContext &osContext) {
    size_t residencyCount = allocationsForResidency.size();
    std::unique_ptr<D3DKMT_HANDLE[]> handlesForResidency(new D3DKMT_HANDLE[residencyCount * maxFragmentsCount]);

    uint32_t totalHandlesCount = 0;

    osContext.get()->getResidencyController().acquireLock();

    DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "currentFenceValue =", osContext.get()->getResidencyController().getMonitoredFence().currentFenceValue);

    for (uint32_t i = 0; i < residencyCount; i++) {
        WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(allocationsForResidency[i]);
        bool mainResidency = false;
        bool fragmentResidency[3] = {false, false, false};

        mainResidency = allocation->getResidencyData().resident;

        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation =", allocation, mainResidency ? "resident" : "not resident");

        if (allocation->getTrimCandidateListPosition(osContext.getContextId()) != trimListUnusedPosition) {

            DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation =", allocation, "on trimCandidateList");
            osContext.get()->getResidencyController().removeFromTrimCandidateList(allocation, false);
        } else {

            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                fragmentResidency[allocationId] = allocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident;

                DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "fragment handle =",
                        allocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle,
                        fragmentResidency[allocationId] ? "resident" : "not resident");
            }
        }

        if (allocation->fragmentsStorage.fragmentCount == 0) {
            if (!mainResidency)
                handlesForResidency[totalHandlesCount++] = allocation->handle;
        } else {
            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                if (!fragmentResidency[allocationId])
                    handlesForResidency[totalHandlesCount++] = allocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle;
            }
        }
    }

    bool result = true;
    if (totalHandlesCount) {
        uint64_t bytesToTrim = 0;
        while ((result = wddm->makeResident(handlesForResidency.get(), totalHandlesCount, false, &bytesToTrim)) == false) {
            this->memoryBudgetExhausted = true;
            bool trimmingDone = trimResidencyToBudget(bytesToTrim);
            bool cantTrimFurther = !trimmingDone;
            if (cantTrimFurther) {
                result = wddm->makeResident(handlesForResidency.get(), totalHandlesCount, true, &bytesToTrim);
                break;
            }
        }
    }

    if (result == true) {
        for (uint32_t i = 0; i < residencyCount; i++) {
            WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(allocationsForResidency[i]);
            // Update fence value not to early destroy / evict allocation
            auto currentFence = osContext.get()->getResidencyController().getMonitoredFence().currentFenceValue;
            allocation->getResidencyData().updateCompletionData(currentFence, osContext.getContextId());
            allocation->getResidencyData().resident = true;

            for (uint32_t allocationId = 0; allocationId < allocation->fragmentsStorage.fragmentCount; allocationId++) {
                auto residencyData = allocation->fragmentsStorage.fragmentStorageData[allocationId].residency;
                // Update fence value not to remove the fragment referenced by different GA in trimming callback
                residencyData->updateCompletionData(currentFence, osContext.getContextId());
                residencyData->resident = true;
            }
        }
    }

    osContext.get()->getResidencyController().releaseLock();

    return result;
}

void WddmMemoryManager::makeNonResidentEvictionAllocations(ResidencyContainer &evictionAllocations, OsContext &osContext) {

    osContext.get()->getResidencyController().acquireLock();

    size_t residencyCount = evictionAllocations.size();

    for (uint32_t i = 0; i < residencyCount; i++) {
        WddmAllocation *allocation = reinterpret_cast<WddmAllocation *>(evictionAllocations[i]);
        osContext.get()->getResidencyController().addToTrimCandidateList(allocation);
    }

    osContext.get()->getResidencyController().releaseLock();
}

void WddmMemoryManager::trimResidency(D3DDDI_TRIMRESIDENCYSET_FLAGS flags, uint64_t bytes) {
    OsContext &osContext = *getRegisteredOsContext(0);
    if (flags.PeriodicTrim) {
        bool periodicTrimDone = false;
        D3DKMT_HANDLE fragmentEvictHandles[3] = {0};
        uint64_t sizeToTrim = 0;

        osContext.get()->getResidencyController().acquireLock();

        WddmAllocation *wddmAllocation = nullptr;
        while ((wddmAllocation = osContext.get()->getResidencyController().getTrimCandidateHead()) != nullptr) {

            DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "lastPeriodicTrimFenceValue = ", osContext.get()->getResidencyController().getLastTrimFenceValue());

            // allocation was not used from last periodic trim
            if (wddmAllocation->getResidencyData().getFenceValueForContextId(0) <= osContext.get()->getResidencyController().getLastTrimFenceValue()) {

                DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "allocation: handle =", wddmAllocation->handle, "lastFence =", (wddmAllocation)->getResidencyData().getFenceValueForContextId(0));

                uint32_t fragmentsToEvict = 0;

                if (wddmAllocation->fragmentsStorage.fragmentCount == 0) {
                    DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "Evict allocation: handle =", wddmAllocation->handle, "lastFence =", (wddmAllocation)->getResidencyData().getFenceValueForContextId(0));
                    wddm->evict(&wddmAllocation->handle, 1, sizeToTrim);
                }

                for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                    if (wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->getFenceValueForContextId(0) <= osContext.get()->getResidencyController().getLastTrimFenceValue()) {

                        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "Evict fragment: handle =", wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle, "lastFence =", wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->getFenceValueForContextId(0));

                        fragmentEvictHandles[fragmentsToEvict++] = wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].osHandleStorage->handle;
                        wddmAllocation->fragmentsStorage.fragmentStorageData[allocationId].residency->resident = false;
                    }
                }

                if (fragmentsToEvict != 0) {
                    wddm->evict((D3DKMT_HANDLE *)fragmentEvictHandles, fragmentsToEvict, sizeToTrim);
                }

                wddmAllocation->getResidencyData().resident = false;

                osContext.get()->getResidencyController().removeFromTrimCandidateList(wddmAllocation, false);
            } else {
                periodicTrimDone = true;
                break;
            }
        }

        if (osContext.get()->getResidencyController().checkTrimCandidateListCompaction()) {
            osContext.get()->getResidencyController().compactTrimCandidateList();
        }

        osContext.get()->getResidencyController().releaseLock();
    }

    if (flags.TrimToBudget) {

        osContext.get()->getResidencyController().acquireLock();

        trimResidencyToBudget(bytes);

        osContext.get()->getResidencyController().releaseLock();
    }

    if (flags.PeriodicTrim || flags.RestartPeriodicTrim) {
        const auto newPeriodicTrimFenceValue = *osContext.get()->getResidencyController().getMonitoredFence().cpuAddress;
        osContext.get()->getResidencyController().setLastTrimFenceValue(newPeriodicTrimFenceValue);
        DBG_LOG(ResidencyDebugEnable, "Residency:", __FUNCTION__, "updated lastPeriodicTrimFenceValue =", newPeriodicTrimFenceValue);
    }
}

bool WddmMemoryManager::trimResidencyToBudget(uint64_t bytes) {
    bool trimToBudgetDone = false;
    D3DKMT_HANDLE fragmentEvictHandles[3] = {0};
    uint64_t numberOfBytesToTrim = bytes;
    WddmAllocation *wddmAllocation = nullptr;
    auto &osContext = *getRegisteredOsContext(0);

    trimToBudgetDone = (numberOfBytesToTrim == 0);

    while (!trimToBudgetDone) {
        uint64_t lastFence = 0;
        wddmAllocation = osContext.get()->getResidencyController().getTrimCandidateHead();

        if (wddmAllocation == nullptr) {
            break;
        }

        lastFence = wddmAllocation->getResidencyData().getFenceValueForContextId(0);
        auto &monitoredFence = osContext.get()->getResidencyController().getMonitoredFence();

        if (lastFence <= monitoredFence.lastSubmittedFence) {
            uint32_t fragmentsToEvict = 0;
            uint64_t sizeEvicted = 0;
            uint64_t sizeToTrim = 0;

            if (lastFence > *monitoredFence.cpuAddress) {
                wddm->waitFromCpu(lastFence, *osContext.get());
            }

            if (wddmAllocation->fragmentsStorage.fragmentCount == 0) {
                wddm->evict(&wddmAllocation->handle, 1, sizeToTrim);

                sizeEvicted = wddmAllocation->getAlignedSize();
            } else {
                auto &fragmentStorageData = wddmAllocation->fragmentsStorage.fragmentStorageData;
                for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                    if (fragmentStorageData[allocationId].residency->getFenceValueForContextId(0) <= monitoredFence.lastSubmittedFence) {
                        fragmentEvictHandles[fragmentsToEvict++] = fragmentStorageData[allocationId].osHandleStorage->handle;
                    }
                }

                if (fragmentsToEvict != 0) {
                    wddm->evict((D3DKMT_HANDLE *)fragmentEvictHandles, fragmentsToEvict, sizeToTrim);

                    for (uint32_t allocationId = 0; allocationId < wddmAllocation->fragmentsStorage.fragmentCount; allocationId++) {
                        if (fragmentStorageData[allocationId].residency->getFenceValueForContextId(0) <= monitoredFence.lastSubmittedFence) {
                            fragmentStorageData[allocationId].residency->resident = false;
                            sizeEvicted += fragmentStorageData[allocationId].fragmentSize;
                        }
                    }
                }
            }

            if (sizeEvicted >= numberOfBytesToTrim) {
                numberOfBytesToTrim = 0;
            } else {
                numberOfBytesToTrim -= sizeEvicted;
            }

            wddmAllocation->getResidencyData().resident = false;
            osContext.get()->getResidencyController().removeFromTrimCandidateList(wddmAllocation, false);
            trimToBudgetDone = (numberOfBytesToTrim == 0);
        } else {
            trimToBudgetDone = true;
        }
    }

    if (bytes > numberOfBytesToTrim && osContext.get()->getResidencyController().checkTrimCandidateListCompaction()) {
        osContext.get()->getResidencyController().compactTrimCandidateList();
    }

    return numberOfBytesToTrim == 0;
}

bool WddmMemoryManager::mapAuxGpuVA(GraphicsAllocation *graphicsAllocation) {
    return wddm->updateAuxTable(graphicsAllocation->getGpuAddress(), graphicsAllocation->gmm, true);
}

AlignedMallocRestrictions *WddmMemoryManager::getAlignedMallocRestrictions() {
    return &mallocRestrictions;
}

bool WddmMemoryManager::createWddmAllocation(WddmAllocation *allocation, AllocationOrigin allocationOrigin) {
    bool useHeap1 = (allocationOrigin == AllocationOrigin::INTERNAL_ALLOCATION);
    auto wddmSuccess = wddm->createAllocation(allocation);
    if (wddmSuccess == STATUS_GRAPHICS_NO_VIDEO_MEMORY && deferredDeleter) {
        deferredDeleter->drain(true);
        wddmSuccess = wddm->createAllocation(allocation);
    }
    if (wddmSuccess == STATUS_SUCCESS) {
        bool mapSuccess = wddm->mapGpuVirtualAddress(allocation, allocation->getAlignedCpuPtr(), allocation->is32BitAllocation, false, useHeap1);
        if (!mapSuccess && deferredDeleter) {
            deferredDeleter->drain(true);
            mapSuccess = wddm->mapGpuVirtualAddress(allocation, allocation->getAlignedCpuPtr(), allocation->is32BitAllocation, false, useHeap1);
        }
        if (!mapSuccess) {
            wddm->destroyAllocations(&allocation->handle, 1, allocation->resourceHandle);
            wddmSuccess = STATUS_UNSUCCESSFUL;
        }
        allocation->setGpuAddress(allocation->gpuPtr);
    }
    return (wddmSuccess == STATUS_SUCCESS);
}

} // namespace OCLRT
