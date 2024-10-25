/*
* 用于公共API的XDMA设备文件接口
* ===============================
*
* Copyright 2017 Xilinx Inc.
* Copyright 2010-2012 Sidebranch
* Copyright 2010-2012 Leon Woestenberg <leon@sidebranch.com>
*
* 维护者:
* -----------
* Alexander Hornburg <alexande@xilinx.com>
*
* IO请求流程图:
* ------------------------
* 用户操作 (e.g. ReadFile())
* |
* |->  IO请求    -> EvtIoRead()--> ReadBarToRequest()                   // PCI BAR访问
*               |            |---> EvtIoReadDma()                       // 正常dma c2h传输
*               |            |---> EvtIoReadEngineRing()                // 用于流式接口
*               |            |---> CopyDescriptorsToRequestMemory()     // 将dma描述符获取到用户空间
*               |            |---> ServiceUserEvent()                   // 等待用户中断
*               |
*               |-> EvtIoWrite()-> WriteBarFromRequest()                // PCI BAR访问
*                             |--> EvtIoWriteDma()                      // 正常DMA H2C传输
*                             |--> WriteBypassDescriptor()              // 从用户空间写入描述符以绕过BAR
*/

// ========================= include dependencies =================================================

#include "driver.h"
#include "dma_engine.h"
#include "xdma_public.h"
#include "file_io.h"

#include "trace.h"
#ifdef DBG
/*
在任何 WPP 宏调用之前和定义 WPP_CONTROL_GUIDS 宏（在 trace.h 中定义）之后，
必须将跟踪消息标头 (.tmh) 文件包含在源文件中。请参阅 trace.h
*/
#include "file_io.tmh"
#endif

EVT_WDF_REQUEST_CANCEL      EvtCancelDma;

// ====================== 设备文件节点 =======================================================
// 静态常量结构体数组 FileNameLUT
// 这种写法常用于查找表（lookup table），可以根据设备类型快速找到对应的文件名和通道号，便于后续的操作。
const static struct {
    DEVNODE_TYPE devType;
    const wchar_t *wstr;
    ULONG channel;
} FileNameLUT[] = {
    { DEVNODE_TYPE_H2C,         XDMA_FILE_H2C_0,        0 },
    { DEVNODE_TYPE_C2H,         XDMA_FILE_C2H_0,        0 },
    { DEVNODE_TYPE_H2C,         XDMA_FILE_H2C_1,        1 },
    { DEVNODE_TYPE_C2H,         XDMA_FILE_C2H_1,        1 },
    { DEVNODE_TYPE_H2C,         XDMA_FILE_H2C_2,        2 },
    { DEVNODE_TYPE_C2H,         XDMA_FILE_C2H_2,        2 },
    { DEVNODE_TYPE_H2C,         XDMA_FILE_H2C_3,        3 },
    { DEVNODE_TYPE_C2H,         XDMA_FILE_C2H_3,        3 },
    { DEVNODE_TYPE_USER,        XDMA_FILE_USER,         0 },
    { DEVNODE_TYPE_CONTROL,     XDMA_FILE_CONTROL,      0 },
    { DEVNODE_TYPE_BYPASS,      XDMA_FILE_BYPASS,       0 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_0,      0 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_1,      1 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_2,      2 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_3,      3 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_4,      4 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_5,      5 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_6,      6 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_7,      7 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_8,      8 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_9,      9 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_10,     10 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_11,     11 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_12,     12 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_13,     13 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_14,     14 },
    { DEVNODE_TYPE_EVENTS,      XDMA_FILE_EVENT_15,     15 },
};

// 从文件名转换为设备节点
static VOID GetDevNodeType(PUNICODE_STRING fileName, PFILE_CONTEXT file, ULONG* index)
{
    for (UINT i = 0; i < sizeof(FileNameLUT) / sizeof(FileNameLUT[0]); ++i) {
        if (!wcscmp(fileName->Buffer, FileNameLUT[i].wstr)) {
            // 进到这里说明fileName->Buffer等于FileNameLUT[i].wstr
            file->devType = FileNameLUT[i].devType;
            *index = FileNameLUT[i].channel;
            return;
        }
    }
    TraceError(DBG_IO, "GetDevNodeID() returns ID_DEVNODE_UNKNOWN");
    file->devType = ID_DEVNODE_UNKNOWN;
}

VOID EvtDeviceFileCreate(IN WDFDEVICE device, IN WDFREQUEST Request, IN WDFFILEOBJECT WdfFile) {
    PUNICODE_STRING fileName = WdfFileObjectGetFileName(WdfFile);
    DeviceContext* ctx = GetDeviceContext(device);
    PXDMA_DEVICE xdma = &(ctx->xdma);
    PFILE_CONTEXT devNode = GetFileContext(WdfFile);
    NTSTATUS status = STATUS_SUCCESS;

    // 没有给出文件名？
    if (fileName == NULL) {
        TraceError(DBG_IO, "Error: no filename given.");
        status = STATUS_INVALID_PARAMETER;
        goto ErrExit;
    }

    // 设备节点的长度为零？
    ASSERTMSG("fileName is empty string", fileName->Length != 0);
    ULONG index = 0;
    GetDevNodeType(fileName, devNode, &index);
    if (devNode->devType == ID_DEVNODE_UNKNOWN) {
        TraceError(DBG_IO, "Error: invalid device node given: %wZ", fileName);
        status = STATUS_INVALID_PARAMETER;
        goto ErrExit;
    }

    // 根据设备类型进行额外检查/设置
    switch (devNode->devType) {
    case DEVNODE_TYPE_CONTROL:
        devNode->u.bar = xdma->bar[xdma->configBarIdx];
        break;
    case DEVNODE_TYPE_USER:
        if (xdma->userBarIdx < 0) {
            TraceError(DBG_IO, "Failed to create 'user' device file. User BAR does not exist!");
            status = STATUS_INVALID_PARAMETER;
            goto ErrExit;
        }
        devNode->u.bar = xdma->bar[xdma->userBarIdx];
        break;
    case DEVNODE_TYPE_BYPASS:
        if (xdma->bypassBarIdx < 0) {
            TraceError(DBG_IO, "Failed to create 'bypass' device file. User BAR does not exist!");
            status = STATUS_INVALID_PARAMETER;
            goto ErrExit;
        }
        devNode->u.bar = xdma->bar[xdma->bypassBarIdx];
        break;
    case DEVNODE_TYPE_H2C:
    case DEVNODE_TYPE_C2H:
    {
        DirToDev dir = devNode->devType == DEVNODE_TYPE_H2C ? H2C : C2H;
        XDMA_ENGINE* engine = &(xdma->engines[index][dir]);

        if (engine->enabled == FALSE) {
            TraceError(DBG_IO, "Error: engine %s_%d not enabled in XDMA IP core",
                       dir == H2C ? "h2c" : "c2h", index);
            status = STATUS_INVALID_PARAMETER;
            goto ErrExit;
        }

        if ((engine->type == EngineType_ST) && (dir == C2H)) {
            EngineRingSetup(engine);
        }

        devNode->u.engine = engine;
        devNode->queue = ctx->engineQueue[dir][index];
        TraceVerbose(DBG_IO, "pollMode=%u", devNode->u.engine->poll);
        if (devNode->u.engine->poll) {
            EngineDisableInterrupt(devNode->u.engine);
        } else {
            EngineEnableInterrupt(devNode->u.engine);
        }
        break;
    }
    case DEVNODE_TYPE_EVENTS:
        devNode->u.event = &(xdma->userEvents[index]);
        break;
    default:
        break;
    }
    TraceInfo(DBG_IO, "Created %wZ device file", fileName);

ErrExit:
    WdfRequestComplete(Request, status);
    TraceVerbose(DBG_IO, "returns %!STATUS!", status);
}

VOID EvtFileClose(IN WDFFILEOBJECT FileObject) {
    PUNICODE_STRING fileName = WdfFileObjectGetFileName(FileObject);
    TraceInfo(DBG_IO, "Closing file %wZ", fileName);
}

VOID EvtFileCleanup(IN WDFFILEOBJECT FileObject) {
    PUNICODE_STRING fileName = WdfFileObjectGetFileName(FileObject);
    PFILE_CONTEXT file = GetFileContext(FileObject);
    if (file->devType == DEVNODE_TYPE_C2H) {
        if (file->u.engine->type == EngineType_ST) {
            EngineRingTeardown(file->u.engine);
        }
    }
    TraceVerbose(DBG_IO, "Cleanup %wZ", fileName);
}

// 验证Bar参数
static NTSTATUS ValidateBarParams(IN PXDMA_DEVICE xdma, ULONG nBar, size_t offset, size_t length) {
    if (length == 0) {
        TraceError(DBG_IO, "Error: attempting to read 0 bytes");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // BAR 索引无效？
    if (nBar >= xdma->numBars) {
        TraceError(DBG_IO, "Error: attempting to read BAR %u but only 2 exist", nBar);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // 访问有效 BAR 地址范围之外？
    if (offset + length >= xdma->barLength[nBar]) {
        TraceError(DBG_IO, "Error: attempting to read BAR %u offset=%llu size=%llu",
                   nBar, offset, length);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ReadBarToRequest(WDFREQUEST request, PVOID bar)
// 从 PCIe mmap 内存读取到 IO 请求中
{
    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(request, &params);
    size_t offset = (size_t)params.Parameters.Read.DeviceOffset;
    size_t length = params.Parameters.Read.Length;

    // 静态驱动程序验证程序不够智能，无法看到在 ValidateBarParams 中检查长度
    // 因此我们也需要在这里检查
    if (length == 0) {
        TraceError(DBG_IO, "Error: attempting to read 0 bytes");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // 计算 mmap 的 BAR 位置的虚拟地址
    PUCHAR readAddr = (PUCHAR)bar + offset;

    // 获取IO请求内存的句柄，该内存将保存读取的数据
    WDFMEMORY requestMemory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        return status;
    }

    // 获取缓冲区requestMemory的指针
    PVOID reqBuffer = WdfMemoryGetBuffer(requestMemory, NULL);

    // read to BAR
    if (length % sizeof(ULONG) == 0) {
        READ_REGISTER_BUFFER_ULONG((volatile ULONG*)readAddr, (PULONG)reqBuffer, (ULONG)length / sizeof(ULONG));
    } else if (length % sizeof(USHORT) == 0) {
        READ_REGISTER_BUFFER_USHORT((volatile USHORT*)readAddr, (PUSHORT)reqBuffer, (ULONG)length / sizeof(USHORT));
    } else {
        READ_REGISTER_BUFFER_UCHAR((volatile UCHAR*)readAddr, (PUCHAR)reqBuffer, (ULONG)length);
    }

    return status;
}

static NTSTATUS WriteBarFromRequest(WDFREQUEST request, PVOID bar)
// 将 IO 请求写入 PCIe mmap 内存
{
    WDF_REQUEST_PARAMETERS params;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(request, &params);
    size_t offset = (size_t)params.Parameters.Read.DeviceOffset;
    size_t length = params.Parameters.Read.Length;

    // 静态驱动程序验证程序不够智能，无法看到在 ValidateBarParams 中检查长度
    // 因此我们也需要在这里检查
    if (length == 0) {
        TraceError(DBG_IO, "Error: attempting to read 0 bytes");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // 计算 mmap 的 BAR 位置的虚拟地址
    PUCHAR writeAddr = (PUCHAR)bar + offset;

    WDFMEMORY requestMemory;
    //获取IO请求内存的句柄，该内存保存要写入的数据
    NTSTATUS status = WdfRequestRetrieveInputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveInputMemory failed: %!STATUS!", status);
        return status;
    }

    // 获取缓冲区requestMemory的指针
    PVOID reqBuffer = WdfMemoryGetBuffer(requestMemory, NULL);

    // write to BAR
    if (length % sizeof(ULONG) == 0) {
        WRITE_REGISTER_BUFFER_ULONG((volatile ULONG*)writeAddr, (PULONG)reqBuffer, (ULONG)length / sizeof(ULONG));
    } else if (length % sizeof(USHORT) == 0) {
        WRITE_REGISTER_BUFFER_USHORT((volatile USHORT*)writeAddr, (PUSHORT)reqBuffer, (ULONG)length / sizeof(USHORT));
    } else {
        WRITE_REGISTER_BUFFER_UCHAR((volatile UCHAR*)writeAddr, (PUCHAR)reqBuffer, (ULONG)length);
    }


    return status;
}

VOID EvtIoRead(IN WDFQUEUE queue, IN WDFREQUEST request, IN size_t length)
// 设备节点 ReadFile 上的回调函数
{
    NTSTATUS status = STATUS_SUCCESS;
    PFILE_CONTEXT file = GetFileContext(WdfRequestGetFileObject(request));

    UNREFERENCED_PARAMETER(length); // 长度可以从请求中提取

    TraceVerbose(DBG_IO, "(Queue=%p, Request=%p, Length=%llu)", queue, request, length);
    TraceVerbose(DBG_IO, "devNodeType %d", file->devType);

    switch (file->devType) {
    case DEVNODE_TYPE_USER:
    case DEVNODE_TYPE_CONTROL:
    case DEVNODE_TYPE_BYPASS:
        ASSERTMSG("no BAR ptr attached to file context", file->u.bar != NULL);
        // 在这里处理请求而不转发 - 从 PCIe BAR 读取到请求内存中
        status = ReadBarToRequest(request, file->u.bar);
        if (NT_SUCCESS(status)) {
            // 完成请求 - 读取的字节在 requestMemory 中
            WdfRequestCompleteWithInformation(request, status, length);
        }
        break;
    case DEVNODE_TYPE_EVENTS:
        ASSERTMSG("no event attached to file context", file->u.event != NULL);
        // 将请求转发到引擎队列-稍后由 EvtIoReadDma 完成
        status = EvtReadUserEvent(request, length);
        break;
    case DEVNODE_TYPE_C2H:
    {
        ASSERTMSG("no engine attached to file context", file->u.engine != NULL);

        // 将请求转发到引擎队列-稍后由 EvtIoReadDma 完成
        status = WdfRequestForwardToIoQueue(request, file->queue);
        break;
    }
    case DEVNODE_TYPE_H2C:
    default:
        TraceError(DBG_IO, "fails with invalid DevNodeID %d", file->devType);
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "request complete with %!STATUS!", status);
        WdfRequestComplete(request, status);
    }

    return; // 请求已直接完成或转发到队列
}

VOID EvtIoWrite(IN WDFQUEUE queue, IN WDFREQUEST request, IN size_t length)
// 设备节点写操作的回调函数
{
    NTSTATUS status = STATUS_SUCCESS;
    PFILE_CONTEXT file = GetFileContext(WdfRequestGetFileObject(request));

    UNREFERENCED_PARAMETER(length); // 可以从请求中提取长度

    TraceVerbose(DBG_IO, "(Queue=%p, Request=%p)", queue, request);
    TraceVerbose(DBG_IO, "DevNodeID %d", file->devType);

    switch (file->devType) {
    case DEVNODE_TYPE_USER:
    case DEVNODE_TYPE_CONTROL:
    case DEVNODE_TYPE_BYPASS:
        ASSERTMSG("no BAR ptr attached to file context", file->u.bar != NULL);
        // 在此处理请求而不转发。从请求内存写入 PCIe BAR
        status = WriteBarFromRequest(request, file->u.bar);
        if (NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(request, status, length);  // complete the request        }
        }
        break;
    case DEVNODE_TYPE_H2C:
        ASSERTMSG("no engine attached to file context", file->u.engine != NULL);

        // 将请求转发到写引擎队列，稍后它会进入 EvtIoWriteDma()
        status = WdfRequestForwardToIoQueue(request, file->queue);
        break;
    case DEVNODE_TYPE_C2H:
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        TraceInfo(DBG_IO, "Error Request 0x%p: %!STATUS!", request, status);
    }

    return; // 请求已直接完成或转发到队列
}

static NTSTATUS IoctlGetPerf(IN WDFREQUEST request, IN XDMA_ENGINE* engine) {

    ASSERT(engine != NULL);
    XDMA_PERF_DATA perfData = { 0 };
    EngineGetPerf(engine, &perfData);

    // 获取保存读取数据的 IO 请求内存的句柄
    WDFMEMORY requestMemory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        return status;
    }

    // 从 perfData 复制到请求内存中
    status = WdfMemoryCopyFromBuffer(requestMemory, 0, &perfData, sizeof(perfData));
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfMemoryCopyFromBuffer failed: %!STATUS!", status);
        return status;
    }

    return status;
}

static NTSTATUS IoctlGetAddrMode(IN WDFREQUEST request, IN XDMA_ENGINE* engine) {

    ASSERT(engine != NULL);
    ULONG addrMode = (engine->regs->control & XDMA_CTRL_NON_INCR_ADDR) != 0; // 0 = inc, 1=non-inc
    TraceVerbose(DBG_IO, "addrMode=%u", addrMode);

    // 获取IO请求内存的句柄，该内存将保存读取的数据
    WDFMEMORY requestMemory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        return status;
    }

    // 从perfData复制到请求内存中
    status = WdfMemoryCopyFromBuffer(requestMemory, 0, &addrMode, sizeof(addrMode));
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfMemoryCopyFromBuffer failed: %!STATUS!", status);
        return status;
    }

    return status;
}

static NTSTATUS IoctlSetAddrMode(IN WDFREQUEST request, IN XDMA_ENGINE* engine) {

    ASSERT(engine != NULL);

    // 获取保存读取数据的 IO 请求内存的句柄
    WDFMEMORY requestMemory;
    NTSTATUS status = WdfRequestRetrieveInputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        return status;
    }
    ULONG addrMode = 0;

    // 从 perfData 复制到请求内存中
    status = WdfMemoryCopyToBuffer(requestMemory, 0, &addrMode, sizeof(addrMode));
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfMemoryCopyFromBuffer failed: %!STATUS!", status);
        return status;
    }

    if (addrMode) {
        engine->regs->controlW1S = XDMA_CTRL_NON_INCR_ADDR;
    } else {
        engine->regs->controlW1C = XDMA_CTRL_NON_INCR_ADDR;
    }
    engine->addressMode = addrMode;

    TraceVerbose(DBG_IO, "addrMode=%u", addrMode);

    return status;
}

// 待办事项：为 SGDMA 和其他功能分离 ioctl 函数？
VOID EvtIoDeviceControl(IN WDFQUEUE Queue, IN WDFREQUEST request, IN size_t OutputBufferLength,
                        IN size_t InputBufferLength, IN ULONG IoControlCode) {

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PFILE_CONTEXT file = GetFileContext(WdfRequestGetFileObject(request));
    PQUEUE_CONTEXT queue = GetQueueContext(file->queue);
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    ASSERT(queue != NULL);
    if (queue->engine == NULL) {
        TraceError(DBG_IO, "IOCTL only supported on DMA files (hc2_* or c2h_* devices)");
        status = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    // xdma_public.h 中定义的 ioctl 代码

    switch (IoControlCode) {
    case IOCTL_XDMA_PERF_START:
        TraceInfo(DBG_IO, "%s_%u IOCTL_XDMA_PERF_START",
                  queue->engine->dir == H2C ? "H2C" : "C2H", queue->engine->channel);
        EngineStartPerf(queue->engine);
        status = STATUS_SUCCESS;
        WdfRequestComplete(request, status);
        break;
    case IOCTL_XDMA_PERF_GET:
        TraceInfo(DBG_IO, "%s_%u IOCTL_XDMA_PERF_GET",
                  queue->engine->dir == H2C ? "H2C" : "C2H", queue->engine->channel);
        status = IoctlGetPerf(request, queue->engine);
        if (NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(request, status, sizeof(XDMA_PERF_DATA));
        }
        break;
    case IOCTL_XDMA_ADDRMODE_GET:
        TraceInfo(DBG_IO, "%s_%u IOCTL_XDMA_ADDRMODE_GET",
                  queue->engine->dir == H2C ? "H2C" : "C2H", queue->engine->channel);
        status = IoctlGetAddrMode(request, queue->engine);
        if (NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(request, status, sizeof(ULONG));
        }
        break;
    case IOCTL_XDMA_ADDRMODE_SET:
        TraceInfo(DBG_IO, "%s_%u IOCTL_XDMA_ADDRMODE_SET",
                  queue->engine->dir == H2C ? "H2C" : "C2H", queue->engine->channel);
        status = IoctlSetAddrMode(request, queue->engine);
        if (NT_SUCCESS(status)) {
            WdfRequestComplete(request, STATUS_SUCCESS);
        }
        break;
    default:
        TraceError(DBG_IO, "Unknown IOCTL code!");
        status = STATUS_NOT_SUPPORTED;
        break;
    }

exit:
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
    }
    TraceVerbose(DBG_IO, "exit with status: %!STATUS!", status);
}

VOID EvtIoWriteDma(IN WDFQUEUE wdfQueue, IN WDFREQUEST Request, IN size_t length)
// 写入 I/O 请求进入 SGDMA 写队列时的回调
{

    UNREFERENCED_PARAMETER(length);
    NTSTATUS status = STATUS_INTERNAL_ERROR;
    PQUEUE_CONTEXT  queue = GetQueueContext(wdfQueue);

    TraceVerbose(DBG_IO, "%!FUNC!(queue=%p, request=%p, length=%llu)", wdfQueue, Request, length);

    XDMA_ENGINE* engine = queue->engine;
    TraceInfo(DBG_IO, "%s_%u writing %llu bytes to device",
              DirectionToString(engine->dir), engine->channel, length);

    // 根据请求初始化 DMA 事务 
    status = WdfDmaTransactionInitializeUsingRequest(queue->engine->dmaTransaction, Request,
                                                     XDMA_EngineProgramDma,
                                                     WdfDmaDirectionWriteToDevice);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfDmaTransactionInitializeUsingRequest failed: %!STATUS!", status);
        goto ErrExit;
    }
    status = WdfRequestMarkCancelableEx(Request, EvtCancelDma);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestMarkCancelableEx failed: %!STATUS!", status);
        goto ErrExit;
    }

    // supply the Queue as context for EvtProgramDma 
    status = WdfDmaTransactionExecute(queue->engine->dmaTransaction, queue->engine);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfDmaTransactionExecute failed: %!STATUS!", status);
        goto ErrExit;
    }

    if (queue->engine->poll) {
        status = EnginePollTransfer(queue->engine);
        if (!NT_SUCCESS(status)) {
            TraceError(DBG_IO, "EnginePollTransfer failed: %!STATUS!", status);
            // EnginePollTransfer 在发生错误时清理/完成请求，因此无需转到 ErrExit
        }
    }

    return; // success
ErrExit:
    WdfDmaTransactionRelease(queue->engine->dmaTransaction);
    WdfRequestComplete(Request, status);
    TraceError(DBG_IO, "Error Request 0x%p: %!STATUS!", Request, status);
}

VOID EvtIoReadDma(IN WDFQUEUE wdfQueue, IN WDFREQUEST Request, IN size_t length)
// 
{
    UNREFERENCED_PARAMETER(length);
    NTSTATUS status = STATUS_INTERNAL_ERROR;
    PQUEUE_CONTEXT queue = GetQueueContext(wdfQueue);

    TraceVerbose(DBG_IO, "%!FUNC!(queue=%p, request=%p, length=%llu)", wdfQueue, Request, length);

    XDMA_ENGINE* engine = queue->engine;
    TraceInfo(DBG_IO, "%s_%u reading %llu bytes from device",
              DirectionToString(engine->dir), engine->channel, length);

    // 根据请求初始化DMA事务
    status = WdfDmaTransactionInitializeUsingRequest(queue->engine->dmaTransaction, Request,
                                                     XDMA_EngineProgramDma,
                                                     WdfDmaDirectionReadFromDevice);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfDmaTransactionInitializeUsingRequest failed: %!STATUS!",
                   status);
        goto ErrExit;
    }
    status = WdfRequestMarkCancelableEx(Request, EvtCancelDma);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestMarkCancelableEx failed: %!STATUS!", status);
        goto ErrExit;
    }

    // supply the Queue as context for EvtProgramDma
    status = WdfDmaTransactionExecute(queue->engine->dmaTransaction, queue->engine);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfDmaTransactionExecute failed: %!STATUS!", status);
        goto ErrExit;
    }

    if (queue->engine->poll) {
        status = EnginePollTransfer(queue->engine);
        if (!NT_SUCCESS(status)) {
            TraceError(DBG_IO, "EnginePollTransfer failed: %!STATUS!", status);
            // EnginePollTransfer 在发生错误时清理/完成请求，因此无需转到 ErrExit
        }
    }

    return; // success
ErrExit:
    WdfDmaTransactionRelease(queue->engine->dmaTransaction);
    WdfRequestComplete(Request, status);
    TraceError(DBG_IO, "Error Request 0x%p: %!STATUS!", Request, status);
}

VOID EvtIoReadEngineRing(IN WDFQUEUE wdfQueue, IN WDFREQUEST Request, IN size_t length) {
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PQUEUE_CONTEXT queue = GetQueueContext(wdfQueue);
    XDMA_ENGINE* engine = queue->engine;

    // 获取输出缓冲区
    WDFMEMORY outputMem;
    status = WdfRequestRetrieveOutputMemory(Request, &outputMem);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    TraceInfo(DBG_IO, "%s_%u requesting %llu bytes from ring buffer",
              DirectionToString(engine->dir), engine->channel, length);

    LARGE_INTEGER timeout;
    timeout.QuadPart = -3 * 10000000; // 3 second timeout
    size_t numBytes = 0;
    status = EngineRingCopyBytesToMemory(engine, outputMem, length, timeout, &numBytes);

    WdfRequestCompleteWithInformation(Request, status, numBytes);
}

VOID EvtCancelDma(IN WDFREQUEST request) {
    PQUEUE_CONTEXT queue = GetQueueContext(WdfRequestGetIoQueue(request));
    TraceInfo(DBG_IO, "Request 0x%p from Queue 0x%p", request, queue);
    EngineStop(queue->engine);
    NTSTATUS status = WdfRequestUnmarkCancelable(request);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestUnmarkCancelable failed: %!STATUS!", status);
    }
    status = WdfDmaTransactionRelease(queue->engine->dmaTransaction);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfDmaTransactionRelease failed: %!STATUS!", status);
    }
    WdfRequestComplete(request, STATUS_CANCELLED);
}

VOID EvtCancelReadUserEvent(IN WDFREQUEST request) {

    PFILE_CONTEXT file = GetFileContext(WdfRequestGetFileObject(request));
    KEVENT* event = (KEVENT*)file->u.event->userData;
    KePulseEvent(event, IO_NO_INCREMENT, FALSE);

    NTSTATUS status = WdfRequestUnmarkCancelable(request);
    if (status != STATUS_CANCELLED) {
        WdfRequestComplete(request, STATUS_CANCELLED);
    }
}

NTSTATUS EvtReadUserEvent(WDFREQUEST request, size_t length) {

    NTSTATUS status = 0;
    BOOLEAN eventValue = TRUE;

    if (length != sizeof(BOOLEAN)) {
        status = STATUS_INVALID_PARAMETER;
        TraceError(DBG_IO, "Error: %!STATUS!", status);
        goto Exit;
    }

    //status = WdfRequestMarkCancelableEx(request, EvtCancelReadUserEvent);
    //if (!NT_SUCCESS(status)) {
    //    TraceError(DBG_IO, "WdfRequestMarkCancelableEx failed: %!STATUS!", status);
    //    goto Exit;
    //}

    // 等待事件发生 - 超时返回错误
    PFILE_CONTEXT file = GetFileContext(WdfRequestGetFileObject(request));
    KEVENT* event = (KEVENT*)file->u.event->userData;
    KeClearEvent(event);
    LARGE_INTEGER timeout;
    timeout.QuadPart = -3 * 10000000; // 3 second timeout
    status = KeWaitForSingleObject(event, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        eventValue = FALSE;
    }

    // get output buffer
    WDFMEMORY outputMem;
    status = WdfRequestRetrieveOutputMemory(request, &outputMem);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfRequestRetrieveOutputMemory failed: %!STATUS!", status);
        goto Exit;
    }

    // get buffer size
    size_t bufSize = 0;
    WdfMemoryGetBuffer(outputMem, &bufSize);
    if (bufSize != sizeof(BOOLEAN)) {
        TraceError(DBG_IO, "Error: length is %llu but must be %llu", bufSize, sizeof(UINT32));
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    // 将用户中断事件从我们的内部缓冲区复制到 WdfMemory
    status = WdfMemoryCopyFromBuffer(outputMem, 0, &eventValue, bufSize);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_IO, "WdfMemoryCopyFromBuffer failed: %!STATUS!", status);
        goto Exit;
    }

    WdfRequestCompleteWithInformation(request, status, bufSize);
    TraceInfo(DBG_IO, "user events returned is 0x%08X", eventValue);

Exit:
    TraceVerbose(DBG_IO, "user EP=0x%08X", eventValue);
    return status;
}

VOID HandleUserEvent(ULONG eventId, void* userData) {

    ASSERTMSG("userData=NULL!", userData != NULL);
    KEVENT* event = (KEVENT*)userData;

    TraceInfo(DBG_IO, "event_%u signaling completion", eventId);
    /*
    KePulseEvent 函数对事件对象进行脉冲信号。这会通知等待该事件的线程，
    表示事件已发生。IO_NO_INCREMENT 参数表示不增加事件的计数，FALSE 表示不设置事件为信号状态。
    */
    KePulseEvent(event, IO_NO_INCREMENT, FALSE);
}
