/*
* XDMA Device
* ===========
*
* Copyright 2017 Xilinx Inc.
* Copyright 2010-2012 Sidebranch
* Copyright 2010-2012 Leon Woestenberg <leon@sidebranch.com>
*
* Maintainer:
* -----------
* Alexander Hornburg <alexande@xilinx.com>
*
* References:
* -----------
*	[1] pg195-pcie-dma.pdf - DMA/Bridge Subsystem for PCI Express v3.0 - Product Guide
*   [2] Windows Dev - Using Automatic Synchronization - https://msdn.microsoft.com/en-us/windows/hardware/drivers/wdf/using-automatic-synchronization
*/

// ====================== include dependancies ========================================================

#include <ntddk.h>
#include <initguid.h> // required for GUID definitions
#include <wdmguid.h> // required for WMILIB_CONTEXT

#include "device.h"
#include "interrupt.h"
#include "dma_engine.h"
#include "xdma_public.h"

#include "trace.h"
#ifdef DBG
// The trace message header (.tmh) file must be included in a source file before any WPP macro 
// calls and after defining a WPP_CONTROL_GUIDS macro (defined in trace.h). see trace.h
#include "device.tmh"
#endif

// ====================== Function declarations ========================================================

// �����º�������Ϊ�ɷ�ҳ����
#ifdef ALLOC_PRAGMA

#endif

/*
WDK 10 ��̬�����������Ԥ����� Windows 10��
��˽��鲻Ҫʹ�� MmMapIoSpace�������� MmMapIoSpaceEx��
���Ǵ˹����� Win 10 ֮ǰ�İ汾�в����á��������ô˾��档
*/
// see https://social.msdn.microsoft.com/Forums/en-US/f8a3fb63-10de-481c-b629-8b5f3d254c5e/unexpected-code-analysis-behavior?forum=wdk
#pragma warning (disable : 30029) 

// ====================== constants ========================================================

// XMDA IP�˵İ汾����
typedef enum XDMA_IP_VERSION_T {
    v2015_4 = 1,
    v2016_1 = 2,
    v2016_2 = 3,
    v2016_3 = 4,
    v2016_4 = 5,
    v2017_1 = 6,
    v2017_2 = 7,
    v2017_3 = 8
} XDMA_IP_VERSION;

// ====================== static functions ========================================================

// ��ȡ XDMA IP�˰汾
static XDMA_IP_VERSION GetVersion(IN OUT PXDMA_DEVICE xdma) {
    XDMA_IP_VERSION version = xdma->configRegs->identifier & 0x000000ffUL;
    TraceVerbose(DBG_INIT, "version is 0x%x", version);
    return version;
}

// ʹ��Ĭ��ֵ��ʼ�� XDMA_DEVICE �ṹ
static void DeviceDefaultInitialize(IN PXDMA_DEVICE xdma) {
    ASSERT(xdma != NULL);

    // bars
    xdma->numBars = 0;
    for (UINT32 i = 0; i < XDMA_MAX_NUM_BARS; i++) {
        xdma->bar[i] = NULL;
        xdma->barLength[i] = 0;
    }
    xdma->configBarIdx = 0;
    xdma->userBarIdx = -1;
    xdma->bypassBarIdx = -1;

    // registers 
    xdma->configRegs = NULL;
    xdma->interruptRegs = NULL;
    xdma->sgdmaRegs = NULL;

    // engines
    for (UINT dir = H2C; dir < 2; dir++) { // 0=H2C, 1=C2H
        for (ULONG ch = 0; ch < XDMA_MAX_NUM_CHANNELS; ch++) {
            xdma->engines[ch][dir].enabled = FALSE;
            xdma->engines[ch][dir].poll = FALSE;
        }
    }

    // interrupts - nothing to do

    // user events
    for (int i = 0; i < XDMA_MAX_USER_IRQ; i++) {
        xdma->userEvents[i].work = NULL;
        xdma->userEvents[i].userData = NULL;
    }
}

// ���� PCIe ��Դ���� BARS ӳ�䵽�����ڴ���
static NTSTATUS MapBARs(IN PXDMA_DEVICE xdma, IN WDFCMRESLIST ResourcesTranslated) {

    const ULONG numResources = WdfCmResourceListGetCount(ResourcesTranslated);
    TraceVerbose(DBG_INIT, "# PCIe resources = %d", numResources);

    for (UINT i = 0; i < numResources; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (!resource) {
            TraceError(DBG_INIT, "WdfResourceCmGetDescriptor() fails");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        if (resource->Type == CmResourceTypeMemory) {
            xdma->barLength[xdma->numBars] = resource->u.Memory.Length;
            xdma->bar[xdma->numBars] = MmMapIoSpace(resource->u.Memory.Start,
                                                    resource->u.Memory.Length, MmNonCached);
            if (xdma->bar[xdma->numBars] == NULL) {
                TraceError(DBG_INIT, "MmMapIoSpace returned NULL! for BAR%u", xdma->numBars);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            TraceInfo(DBG_INIT, "MM BAR %d (addr:0x%lld, length:%u) mapped at 0x%08p",
                      xdma->numBars, resource->u.Memory.Start.QuadPart,
                      resource->u.Memory.Length, xdma->bar[xdma->numBars]);
            xdma->numBars++;
        }
    }
    return STATUS_SUCCESS;
}

// Is the BAR at index 'idx' the config BAR?
static BOOLEAN IsConfigBAR(IN PXDMA_DEVICE xdma, IN UINT idx) {

    XDMA_IRQ_REGS* irqRegs = (XDMA_IRQ_REGS*)((PUCHAR)xdma->bar[idx] + IRQ_BLOCK_OFFSET);
    XDMA_CONFIG_REGS* cfgRegs = (XDMA_CONFIG_REGS*)((PUCHAR)xdma->bar[idx] + CONFIG_BLOCK_OFFSET);

    UINT32 interruptID = irqRegs->identifier & XDMA_ID_MASK;
    UINT32 configID = cfgRegs->identifier & XDMA_ID_MASK;

    return ((interruptID == XDMA_ID) && (configID == XDMA_ID)) ? TRUE : FALSE;
}

// ȷ���ĸ� BAR ������ BAR
static UINT FindConfigBAR(IN PXDMA_DEVICE xdma) {
    for (UINT i = 0; i < xdma->numBars; ++i) {
        if (IsConfigBAR(xdma, i)) {
            TraceInfo(DBG_INIT, "config BAR is %u", i);
            return i;
        }
    }
    return xdma->numBars; //not found - return past-the-end index
}

// ʶ������ BAR
static NTSTATUS IdentifyBars(IN PXDMA_DEVICE xdma) {

    // find DMA config BAR (usually BAR1, see section 'Target Bridge' in [1]) 
    xdma->configBarIdx = FindConfigBAR(xdma);
    if (xdma->configBarIdx == xdma->numBars) {
        TraceError(DBG_INIT, "findConfigBar() failed: bar is %d", xdma->configBarIdx);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    // if config bar is bar0 then user bar doesnt exit
    xdma->userBarIdx = xdma->configBarIdx == 1 ? 0 : -1;

    // if config bar is not the last bar then bypass bar exists
    xdma->bypassBarIdx = xdma->numBars - xdma->configBarIdx == 2 ? xdma->numBars - 1 : -1;

    TraceInfo(DBG_INIT, "%!FUNC!, BAR index: user=%d, control=%d, bypass=%d",
              xdma->userBarIdx, xdma->configBarIdx, xdma->bypassBarIdx);
    return STATUS_SUCCESS;
}

// Get the config, interrupt and sgdma module register offsets
static void GetRegisterModules(IN PXDMA_DEVICE xdma) {
    PUCHAR configBarAddr = (PUCHAR)xdma->bar[xdma->configBarIdx];
    xdma->configRegs = (XDMA_CONFIG_REGS*)(configBarAddr + CONFIG_BLOCK_OFFSET);
    xdma->interruptRegs = (XDMA_IRQ_REGS*)(configBarAddr + IRQ_BLOCK_OFFSET);
    xdma->sgdmaRegs = (XDMA_SGDMA_COMMON_REGS*)(configBarAddr + SGDMA_COMMON_BLOCK_OFFSET);
}

// ====================== API functions ========================================

NTSTATUS XDMA_DeviceOpen(WDFDEVICE wdfDevice,
                         PXDMA_DEVICE xdma,
                         WDFCMRESLIST ResourcesRaw,
                         WDFCMRESLIST ResourcesTranslated) {

    NTSTATUS status = STATUS_INTERNAL_ERROR;

    DeviceDefaultInitialize(xdma);

    xdma->wdfDevice = wdfDevice;

    // �� PCIe BAR ӳ�䵽�����ڴ�
    status = MapBARs(xdma, ResourcesTranslated);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "MapBARs() failed! %!STATUS!", status);
        return status;
    }

    // ʶ��BAR���� - user(optional), config, bypass(optional)
    status = IdentifyBars(xdma);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "IdentifyBars() failed! %!STATUS!", status);
        return status;
    }

    // ��ȡ�������е�ģ��ƫ����
    GetRegisterModules(xdma);

    // ȷ�� XDMA IP�˰汾�����������ƥ��
    UINT version = GetVersion(xdma);
    if (version != v2017_1) {
        TraceWarning(DBG_INIT, "Version mismatch! Expected 2017.1 (0x%x) but got (0x%x)",
                     v2017_1, version);
    }

    status = SetupInterrupts(xdma, ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "SetupInterrupts failed: %!STATUS!", status);
        return status;
    }

    // WDF DMA Enabler - ���� 8 �ֽڶ���
    WdfDeviceSetAlignmentRequirement(xdma->wdfDevice, 8 - 1); // TODO - ѡ����ȷ��ֵ
    WDF_DMA_ENABLER_CONFIG dmaConfig;
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather64Duplex, XDMA_MAX_TRANSFER_SIZE);
    status = WdfDmaEnablerCreate(xdma->wdfDevice, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &xdma->dmaEnabler);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, " WdfDmaEnablerCreate() failed: %!STATUS!", status);
        return status;
    }

    // ���ͳ�ʼ�� HW IP �����������
    status = ProbeEngines(xdma);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "ProbeEngines failed: %!STATUS!", status);
        return status;
    }

    return status;
}

void XDMA_DeviceClose(PXDMA_DEVICE xdma) {

    // todo - ֹͣ��������?

    // ���� irq vectors?
    if (xdma && xdma->interruptRegs) {
        xdma->interruptRegs->userVector[0] = 0;
        xdma->interruptRegs->userVector[1] = 0;
        xdma->interruptRegs->userVector[2] = 0;
        xdma->interruptRegs->userVector[3] = 0;
        xdma->interruptRegs->channelVector[0] = 0;
        xdma->interruptRegs->channelVector[1] = 0;
    }

    // ȡ��ӳ���κ� I/O �˿ڡ���ܽ��Զ��Ͽ��ж����ӡ�
    for (UINT i = 0; i < xdma->numBars; i++) {
        if (xdma->bar[i] != NULL) {
            TraceInfo(DBG_INIT, "Unmapping BAR%d, VA:(%p) Length %ul",
                      i, xdma->bar[i], xdma->barLength[i]);
            MmUnmapIoSpace(xdma->bar[i], xdma->barLength[i]);
            xdma->bar[i] = NULL;
        }
    }
}



