/*
* XDMA Device Driver for Windows
* ===============================
*
* Copyright 2017 Xilinx Inc.
* Copyright 2010-2012 Sidebranch
* Copyright 2010-2012 Leon Woestenberg <leon@sidebranch.com>
*
* ά����:
* -----------
* Alexander Hornburg <alexander.hornburg@xilinx.com>
*
* ����:
* ------------
* ����Xilinx��˾�ġ�PCIe 3.0��(XDMA) IP��DMA/�Ž���ϵͳ����ʾ����������
*
* �ο�:
* -----------
*	[1] pg195-pcie-dma.pdf - DMA/Bridge Subsystem for PCI Express v3.0 - Product Guide
*/

#pragma once

#include "xdma.h"

typedef struct DeviceContext_t {
    XDMA_DEVICE xdma;
    WDFQUEUE engineQueue[2][XDMA_MAX_NUM_CHANNELS];
    KEVENT eventSignals[XDMA_MAX_USER_IRQ];

}DeviceContext;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)