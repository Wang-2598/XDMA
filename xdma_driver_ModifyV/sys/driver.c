/*
* XDMA Device Driver for Windows
* ===============================
*
* Copyright 2017 Xilinx Inc.
* Copyright 2010-2012 Sidebranch
* Copyright 2010-2012 Leon Woestenberg <leon@sidebranch.com>
*
* Maintainer:
* -----------
* Alexander Hornburg <alexander.hornburg@xilinx.com>
*
* Description:
* ------------
* This is a sample driver for the Xilinx Inc. 'DMA/Bridge Subsystem for PCIe v3.0' (XDMA) IP.
*
* References:
* -----------
*	[1] pg195-pcie-dma.pdf - DMA/Bridge Subsystem for PCI Express v3.0 - Product Guide
*/

// ========================= include dependencies =================================================

#include "driver.h"
#include "file_io.h"
#include "trace.h"

#ifdef DBG
// The trace message header (.tmh) file must be included in a source file before any WPP macro 
// calls and after defining a WPP_CONTROL_GUIDS macro (defined in trace.h). see trace.h
#include "driver.tmh"
#endif

// ========================= declarations ================================================= 

// 这些类型的变量是函数指针，指向实际的实现函数，虽然看着像结构体变量但是不是
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD           EvtDeviceAdd;
EVT_WDF_DEVICE_CONTEXT_CLEANUP      EvtDeviceCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE     EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     EvtDeviceReleaseHardware;

static NTSTATUS EngineCreateQueue(WDFDEVICE device, XDMA_ENGINE* engine, WDFQUEUE* queue);

// 将这些函数标记为可分页代码
#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, DriverUnload)
#pragma alloc_text (PAGE, EvtDeviceAdd)
#pragma alloc_text (PAGE, EvtDevicePrepareHardware)
#pragma alloc_text (PAGE, EvtDeviceReleaseHardware)
#pragma alloc_text (PAGE, EngineCreateQueue)
#endif

// ========================= 定义日期 =================================================

const char * const dateTimeStr = "Built " __DATE__ ", " __TIME__ ".";

// 从Windows注册表中获取名为 POLL_MODE 的参数值，并将其存储在 pollMode 输出参数中。 
static NTSTATUS GetPollModeParameter(IN PULONG pollMode) {
    // 获取当前的 WDFDRIVER 对象，该对象代表驱动
    WDFDRIVER driver = WdfGetDriver();
    WDFKEY key;
    // 打开驱动参数所在的注册表键,STANDARD_RIGHTS_ALL 表示请求对键的完全访问权限
    NTSTATUS status = WdfDriverOpenParametersRegistryKey(driver, STANDARD_RIGHTS_ALL,
                                                         WDF_NO_OBJECT_ATTRIBUTES, &key);
    ULONG tracepollmode;
    
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDriverOpenParametersRegistryKey failed: %!STATUS!", status);
        WdfRegistryClose(key);
        return status;
    }

    // 声明一个常量的 Unicode 字符串，名字为 valueName，内容为 "POLL_MODE"。
    DECLARE_CONST_UNICODE_STRING(valueName, L"POLL_MODE");

    // 查询注册表中的 POLL_MODE 值
    status = WdfRegistryQueryULong(key, &valueName, pollMode);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfRegistryQueryULong failed: %!STATUS!", status);
        WdfRegistryClose(key);
        return status;
    }

    tracepollmode = *pollMode;
    TraceVerbose(DBG_INIT, "pollMode=%u", tracepollmode);

    // 关闭注册表键
    WdfRegistryClose(key);
    return status;
}

// main entry point - Called when driver is installed
NTSTATUS DriverEntry(IN PDRIVER_OBJECT driverObject, IN PUNICODE_STRING registryPath) {
    NTSTATUS			status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG	DriverConfig;
    WDFDRIVER			Driver;

    // Initialize WPP Tracing
    WPP_INIT_TRACING(driverObject, registryPath);
    TraceInfo(DBG_INIT, "XDMA Driver - %s", dateTimeStr);

    // Initialize the Driver Config; register the device add event callback
    // EvtDeviceAdd() will be called when a device is found
    WDF_DRIVER_CONFIG_INIT(&DriverConfig, EvtDeviceAdd);

    // Creates a WDFDRIVER object, the top of our device's tree of objects
    status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &DriverConfig,
                             &Driver);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDriverCreate failed: %!STATUS!", status);
        WPP_CLEANUP(driverObject);
        return status;
    }

    driverObject->DriverUnload = DriverUnload;

    return status;
}

// Called before the driver is removed
VOID DriverUnload(IN PDRIVER_OBJECT driverObject) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(driverObject);
    TraceVerbose(DBG_INIT, "%!FUNC!");

    WPP_CLEANUP(driverObject); // cleanup tracing

    return;
}

NTSTATUS EvtDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit) {
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    TraceVerbose(DBG_INIT, "(Driver=0x%p)", Driver);

    /*
    设置驱动程序访问设备的读取和写入请求中的数据缓冲区的方法
    设置设备使用直接 I/O (Direct I/O) 模式,
    这段代码是告诉设备在 I/O 操作时优先使用直接 I/O 方式来提高效率，但根据具体情况可能会退回到缓冲 I/O。
    */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);

    // 为我们感兴趣的任何函数设置回调。如果没有设置回调，框架将自行采取默认操作。
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_POWER_POLICY_EVENT_CALLBACKS powerPolicyCallbacks;
    WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&powerPolicyCallbacks);
    WdfDeviceInitSetPowerPolicyEventCallbacks(DeviceInit, &powerPolicyCallbacks);

    // 注册文件对象回调
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtDeviceFileCreate, EvtFileClose, EvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&fileAttributes);
    fileAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&fileAttributes, FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    // 指定我们要创建的设备的上下文类型和大小。
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DeviceContext);
    /*
    框架在删除设备时将调用ContextCleanup。因此，在创建设备后，
    如果EvtDeviceAdd返回任何错误，您可以推迟释放分配给清理回调的任何资源。
    */
    deviceAttributes.EvtCleanupCallback = EvtDeviceCleanup;
    WDFDEVICE device;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDeviceCreate failed: %!STATUS!", status);
        return status;
    }

    // 为指定设备(GUID)创建设备接口
    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_DEVINTERFACE_XDMA, NULL);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDeviceCreateDeviceInterface failed %!STATUS!", status);
        return status;
    }

    /*
    在所有I/O请求到达时创建默认队列接受多个并行运行的I/O请求，这些请求随后被顺序化
    */
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl; // callback handler for control requests
    queueConfig.EvtIoRead = EvtIoRead; // callback handler for read requests
    queueConfig.EvtIoWrite = EvtIoWrite; // callback handler for write requests
    WDFQUEUE entryQueue;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &entryQueue);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfIoQueueCreate failed: %!STATUS!", status);
        return status;
    }

    TraceVerbose(DBG_INIT, "returns %!STATUS!", status);
    return status;
}

// 任何特定于设备的清理 - TODO 设备重置？
VOID EvtDeviceCleanup(IN WDFOBJECT device) {
    UNREFERENCED_PARAMETER(device);
    TraceInfo(DBG_INIT, "%!FUNC!");
}

// 初始化设备硬件和主机缓冲区。
// 由即插即用管理器调用
NTSTATUS EvtDevicePrepareHardware(IN WDFDEVICE device, IN WDFCMRESLIST Resources,
                                  IN WDFCMRESLIST ResourcesTranslated) {
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Resources);
    TraceVerbose(DBG_INIT, "-->Entry");

    DeviceContext* ctx = GetDeviceContext(device);
    PXDMA_DEVICE xdma = &(ctx->xdma);
    NTSTATUS status = XDMA_DeviceOpen(device, xdma, Resources, ResourcesTranslated);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "XDMA_DeviceOpen failed: %!STATUS!", status);
        return status;
    }

    // 获取轮询模式参数，并在需要时将引擎配置为轮询模式
    ULONG pollMode = 0;
    status = GetPollModeParameter(&pollMode);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "GetPollModeParameter failed: %!STATUS!", status);
        return status;
    }
    for (UINT dir = H2C; dir < 2; dir++) { // 0=H2C, 1=C2H
        for (ULONG ch = 0; ch < XDMA_MAX_NUM_CHANNELS; ch++) {
            XDMA_ENGINE* engine = &(xdma->engines[ch][dir]);
            XDMA_EngineSetPollMode(engine, (BOOLEAN)pollMode);
        }
    }

    // 为每个引擎创建一个队列
    for (UINT dir = H2C; dir < 2; dir++) { // 0=H2C, 1=C2H
        for (ULONG ch = 0; ch < XDMA_MAX_NUM_CHANNELS; ch++) {
            XDMA_ENGINE* engine = &(xdma->engines[ch][dir]);
            if (engine->enabled == TRUE) {
                status = EngineCreateQueue(device, engine, &(ctx->engineQueue[dir][ch]));
                if (!NT_SUCCESS(status)) {
                    TraceError(DBG_INIT, "EngineCreateQueue() failed: %!STATUS!", status);
                    return status;
                }
            }
        }
    }

    // 循环遍历 XDMA_MAX_USER_IRQ，为每个用户中断初始化一个事件对象
    for (UINT i = 0; i < XDMA_MAX_USER_IRQ; ++i) {
        // 初始化事件，NotificationEvent 表示这是一个通知事件，FALSE 表示事件初始状态为非信号状态。
        KeInitializeEvent(&ctx->eventSignals[i], NotificationEvent, FALSE);
        
        // 注册中断服务例程（ISR）。在中断发生时，会调用 HandleUserEvent 函数，并将对应的事件对象传递给它。
        XDMA_UserIsrRegister(xdma, i, HandleUserEvent, &ctx->eventSignals[i]);
    }

    TraceVerbose(DBG_INIT, "<--Exit returning %!STATUS!", status);
    return status;
}

// 取消映射PCIe资源
NTSTATUS EvtDeviceReleaseHardware(IN WDFDEVICE Device, IN WDFCMRESLIST ResourcesTranslated) {

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    TraceVerbose(DBG_INIT, "entry");
    
    DeviceContext* ctx = GetDeviceContext(Device);
    if (ctx != NULL) {
        XDMA_DeviceClose(&ctx->xdma);
    }

    TraceVerbose(DBG_INIT, "exit");
    return STATUS_SUCCESS;
}

/*
该函数的作用是为DMA引擎创建一个顺序调度的WDF IO队列，
并根据DMA传输方向配置对应的回调函数。队列创建后，
相关的上下文数据（即 XDMA_ENGINE 指针）被保存到队列的上下文中。
*/
NTSTATUS EngineCreateQueue(WDFDEVICE device, XDMA_ENGINE* engine, WDFQUEUE* queue)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attribs;
    PQUEUE_CONTEXT context;

    PAGED_CODE();

    // 初始化IO队列配置，并设置为顺序调度模式 WdfIoQueueDispatchSequential，即IO请求按顺序排队执行。
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchSequential);

    // 根据DMA引擎的方向 engine->dir，判断当前是从设备到主机（C2H）还是从主机到设备（H2C）的传输
    ASSERTMSG("direction is neither H2C nor C2H!", (engine->dir == C2H) || (engine->dir == H2C));

    if (engine->dir == H2C) 
    { 
        // callback handler for write requests
        config.EvtIoWrite = EvtIoWriteDma;
        TraceInfo(DBG_INIT, "EvtIoWrite=EvtIoWriteDma");
    } else if (engine->dir == C2H) 
    { 
        // callback handler for read requests

        if (engine->type == EngineType_ST) {
            config.EvtIoRead = EvtIoReadEngineRing;
            TraceInfo(DBG_INIT, "EvtIoRead=EvtIoReadEngineRing");
        } else {
            config.EvtIoRead = EvtIoReadDma;
            TraceInfo(DBG_INIT, "EvtIoRead=EvtIoReadDma");
        }
    }

    // 初始化 attribs，并将同步范围设置为 WdfSynchronizationScopeQueue，表示所有与该队列相关的回调会被序列化
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.SynchronizationScope = WdfSynchronizationScopeQueue;
    // 设置队列上下文类型为 QUEUE_CONTEXT，以便存储与队列相关的上下文数据。
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attribs, QUEUE_CONTEXT);

    // 创建IO队列，将设备对象、队列配置、队列属性传递给它，并将创建的队列句柄存储在 queue 中。
    status = WdfIoQueueCreate(device, &config, &attribs, queue);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfIoQueueCreate failed %d", status);
        return status;
    }

    // 获取队列的上下文，并将 engine 对象存储到上下文中，这样可以在处理队列时访问对应的DMA引擎。
    context = GetQueueContext(*queue);
    context->engine = engine;

    return status;
}
