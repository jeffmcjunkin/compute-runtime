/*
 * Copyright (C) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "unit_tests/fixtures/device_fixture.h"
#include "unit_tests/mocks/mock_kernel.h"
#include "test.h"

using namespace OCLRT;

typedef Test<DeviceFixture> KernelSubstituteTest;

TEST_F(KernelSubstituteTest, givenKernelWhenSubstituteKernelHeapWithGreaterSizeThenAllocatesNewKernelAllocation) {
    MockKernelWithInternals kernel(*pDevice);
    auto pHeader = const_cast<SKernelBinaryHeaderCommon *>(kernel.kernelInfo.heapInfo.pKernelHeader);
    const size_t initialHeapSize = 0x40;
    pHeader->KernelHeapSize = initialHeapSize;

    EXPECT_EQ(nullptr, kernel.kernelInfo.kernelAllocation);
    kernel.kernelInfo.createKernelAllocation(pDevice->getMemoryManager());
    auto firstAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, firstAllocation);
    auto firstAllocationSize = firstAllocation->getUnderlyingBufferSize();
    EXPECT_EQ(initialHeapSize, firstAllocationSize);

    auto firstAllocationId = static_cast<MemoryAllocation *>(firstAllocation)->id;

    const size_t newHeapSize = initialHeapSize + 1;
    char newHeap[newHeapSize];

    kernel.mockKernel->substituteKernelHeap(newHeap, newHeapSize);
    auto secondAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, secondAllocation);
    auto secondAllocationSize = secondAllocation->getUnderlyingBufferSize();
    EXPECT_NE(initialHeapSize, secondAllocationSize);
    EXPECT_EQ(newHeapSize, secondAllocationSize);
    auto secondAllocationId = static_cast<MemoryAllocation *>(secondAllocation)->id;

    EXPECT_NE(firstAllocationId, secondAllocationId);

    pDevice->getMemoryManager()->checkGpuUsageAndDestroyGraphicsAllocations(secondAllocation);
}

TEST_F(KernelSubstituteTest, givenKernelWhenSubstituteKernelHeapWithSameSizeThenDoesNotAllocateNewKernelAllocation) {
    MockKernelWithInternals kernel(*pDevice);
    auto pHeader = const_cast<SKernelBinaryHeaderCommon *>(kernel.kernelInfo.heapInfo.pKernelHeader);
    const size_t initialHeapSize = 0x40;
    pHeader->KernelHeapSize = initialHeapSize;

    EXPECT_EQ(nullptr, kernel.kernelInfo.kernelAllocation);
    kernel.kernelInfo.createKernelAllocation(pDevice->getMemoryManager());
    auto firstAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, firstAllocation);
    auto firstAllocationSize = firstAllocation->getUnderlyingBufferSize();
    EXPECT_EQ(initialHeapSize, firstAllocationSize);

    auto firstAllocationId = static_cast<MemoryAllocation *>(firstAllocation)->id;

    const size_t newHeapSize = initialHeapSize;
    char newHeap[newHeapSize];

    kernel.mockKernel->substituteKernelHeap(newHeap, newHeapSize);
    auto secondAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, secondAllocation);
    auto secondAllocationSize = secondAllocation->getUnderlyingBufferSize();
    EXPECT_EQ(initialHeapSize, secondAllocationSize);
    auto secondAllocationId = static_cast<MemoryAllocation *>(secondAllocation)->id;

    EXPECT_EQ(firstAllocationId, secondAllocationId);

    pDevice->getMemoryManager()->checkGpuUsageAndDestroyGraphicsAllocations(secondAllocation);
}

TEST_F(KernelSubstituteTest, givenKernelWhenSubstituteKernelHeapWithSmallerSizeThenDoesNotAllocateNewKernelAllocation) {
    MockKernelWithInternals kernel(*pDevice);
    auto pHeader = const_cast<SKernelBinaryHeaderCommon *>(kernel.kernelInfo.heapInfo.pKernelHeader);
    const size_t initialHeapSize = 0x40;
    pHeader->KernelHeapSize = initialHeapSize;

    EXPECT_EQ(nullptr, kernel.kernelInfo.kernelAllocation);
    kernel.kernelInfo.createKernelAllocation(pDevice->getMemoryManager());
    auto firstAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, firstAllocation);
    auto firstAllocationSize = firstAllocation->getUnderlyingBufferSize();
    EXPECT_EQ(initialHeapSize, firstAllocationSize);

    auto firstAllocationId = static_cast<MemoryAllocation *>(firstAllocation)->id;

    const size_t newHeapSize = initialHeapSize - 1;
    char newHeap[newHeapSize];

    kernel.mockKernel->substituteKernelHeap(newHeap, newHeapSize);
    auto secondAllocation = kernel.kernelInfo.kernelAllocation;
    EXPECT_NE(nullptr, secondAllocation);
    auto secondAllocationSize = secondAllocation->getUnderlyingBufferSize();
    EXPECT_EQ(initialHeapSize, secondAllocationSize);
    auto secondAllocationId = static_cast<MemoryAllocation *>(secondAllocation)->id;

    EXPECT_EQ(firstAllocationId, secondAllocationId);

    pDevice->getMemoryManager()->checkGpuUsageAndDestroyGraphicsAllocations(secondAllocation);
}

TEST_F(KernelSubstituteTest, givenKernelWithUsedKernelAllocationWhenSubstituteKernelHeapAndAllocateNewMemoryThenStoreOldAllocationOnTemporaryList) {
    MockKernelWithInternals kernel(*pDevice);
    auto pHeader = const_cast<SKernelBinaryHeaderCommon *>(kernel.kernelInfo.heapInfo.pKernelHeader);
    auto memoryManager = pDevice->getMemoryManager();

    const size_t initialHeapSize = 0x40;
    pHeader->KernelHeapSize = initialHeapSize;

    kernel.kernelInfo.createKernelAllocation(memoryManager);
    auto firstAllocation = kernel.kernelInfo.kernelAllocation;

    firstAllocation->taskCount = ObjectNotUsed - 1;

    const size_t newHeapSize = initialHeapSize + 1;
    char newHeap[newHeapSize];

    EXPECT_TRUE(memoryManager->getCommandStreamReceiver(0)->getTemporaryAllocations().peekIsEmpty());

    kernel.mockKernel->substituteKernelHeap(newHeap, newHeapSize);
    auto secondAllocation = kernel.kernelInfo.kernelAllocation;

    EXPECT_FALSE(memoryManager->getCommandStreamReceiver(0)->getTemporaryAllocations().peekIsEmpty());
    EXPECT_EQ(memoryManager->getCommandStreamReceiver(0)->getTemporaryAllocations().peekHead(), firstAllocation);
    memoryManager->checkGpuUsageAndDestroyGraphicsAllocations(secondAllocation);
    memoryManager->cleanAllocationList(firstAllocation->taskCount, TEMPORARY_ALLOCATION);
}
