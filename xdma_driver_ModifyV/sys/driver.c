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

// ��Щ���͵ı����Ǻ���ָ�룬ָ��ʵ�ʵ�ʵ�ֺ�������Ȼ������ṹ��������ǲ���
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD           EvtDeviceAdd;
EVT_WDF_DEVICE_CONTEXT_CLEANUP      EvtDeviceCleanup;
EVT_WDF_DEVICE_PREPARE_HARDWARE     EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     EvtDeviceReleaseHardware;

static NTSTATUS EngineCreateQueue(WDFDEVICE device, XDMA_ENGINE* engine, WDFQUEUE* queue);

// ����Щ�������Ϊ�ɷ�ҳ����
#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, DriverUnload)
#pragma alloc_text (PAGE, EvtDeviceAdd)
#pragma alloc_text (PAGE, EvtDevicePrepareHardware)
#pragma alloc_text (PAGE, EvtDeviceReleaseHardware)
#pragma alloc_text (PAGE, EngineCreateQueue)
#endif

// ========================= �������� =================================================

const char * const dateTimeStr = "Built " __DATE__ ", " __TIME__ ".";

// ��Windowsע����л�ȡ��Ϊ POLL_MODE �Ĳ���ֵ��������洢�� pollMode ��������С� 
static NTSTATUS GetPollModeParameter(IN PULONG pollMode) {
    // ��ȡ��ǰ�� WDFDRIVER ���󣬸ö����������
    WDFDRIVER driver = WdfGetDriver();
    WDFKEY key;
    // �������������ڵ�ע����,STANDARD_RIGHTS_ALL ��ʾ����Լ�����ȫ����Ȩ��
    NTSTATUS status = WdfDriverOpenParametersRegistryKey(driver, STANDARD_RIGHTS_ALL,
                                                         WDF_NO_OBJECT_ATTRIBUTES, &key);
    ULONG tracepollmode;
    
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDriverOpenParametersRegistryKey failed: %!STATUS!", status);
        WdfRegistryClose(key);
        return status;
    }

    // ����һ�������� Unicode �ַ���������Ϊ valueName������Ϊ "POLL_MODE"��
    DECLARE_CONST_UNICODE_STRING(valueName, L"POLL_MODE");

    // ��ѯע����е� POLL_MODE ֵ
    status = WdfRegistryQueryULong(key, &valueName, pollMode);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfRegistryQueryULong failed: %!STATUS!", status);
        WdfRegistryClose(key);
        return status;
    }

    tracepollmode = *pollMode;
    TraceVerbose(DBG_INIT, "pollMode=%u", tracepollmode);

    // �ر�ע����
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
    ����������������豸�Ķ�ȡ��д�������е����ݻ������ķ���
    �����豸ʹ��ֱ�� I/O (Direct I/O) ģʽ,
    ��δ����Ǹ����豸�� I/O ����ʱ����ʹ��ֱ�� I/O ��ʽ�����Ч�ʣ������ݾ���������ܻ��˻ص����� I/O��
    */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoDirect);

    // Ϊ���Ǹ���Ȥ���κκ������ûص������û�����ûص�����ܽ����в�ȡĬ�ϲ�����
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_POWER_POLICY_EVENT_CALLBACKS powerPolicyCallbacks;
    WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&powerPolicyCallbacks);
    WdfDeviceInitSetPowerPolicyEventCallbacks(DeviceInit, &powerPolicyCallbacks);

    // ע���ļ�����ص�
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtDeviceFileCreate, EvtFileClose, EvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&fileAttributes);
    fileAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&fileAttributes, FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    // ָ������Ҫ�������豸�����������ͺʹ�С��
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DeviceContext);
    /*
    �����ɾ���豸ʱ������ContextCleanup����ˣ��ڴ����豸��
    ���EvtDeviceAdd�����κδ����������Ƴ��ͷŷ��������ص����κ���Դ��
    */
    deviceAttributes.EvtCleanupCallback = EvtDeviceCleanup;
    WDFDEVICE device;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDeviceCreate failed: %!STATUS!", status);
        return status;
    }

    // Ϊָ���豸(GUID)�����豸�ӿ�
    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_DEVINTERFACE_XDMA, NULL);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfDeviceCreateDeviceInterface failed %!STATUS!", status);
        return status;
    }

    /*
    ������I/O���󵽴�ʱ����Ĭ�϶��н��ܶ���������е�I/O������Щ�������˳��
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

// �κ��ض����豸������ - TODO �豸���ã�
VOID EvtDeviceCleanup(IN WDFOBJECT device) {
    UNREFERENCED_PARAMETER(device);
    TraceInfo(DBG_INIT, "%!FUNC!");
}

// ��ʼ���豸Ӳ����������������
// �ɼ��弴�ù���������
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

    // ��ȡ��ѯģʽ������������Ҫʱ����������Ϊ��ѯģʽ
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

    // Ϊÿ�����洴��һ������
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

    // ѭ������ XDMA_MAX_USER_IRQ��Ϊÿ���û��жϳ�ʼ��һ���¼�����
    for (UINT i = 0; i < XDMA_MAX_USER_IRQ; ++i) {
        // ��ʼ���¼���NotificationEvent ��ʾ����һ��֪ͨ�¼���FALSE ��ʾ�¼���ʼ״̬Ϊ���ź�״̬��
        KeInitializeEvent(&ctx->eventSignals[i], NotificationEvent, FALSE);
        
        // ע���жϷ������̣�ISR�������жϷ���ʱ������� HandleUserEvent ������������Ӧ���¼����󴫵ݸ�����
        XDMA_UserIsrRegister(xdma, i, HandleUserEvent, &ctx->eventSignals[i]);
    }

    TraceVerbose(DBG_INIT, "<--Exit returning %!STATUS!", status);
    return status;
}

// ȡ��ӳ��PCIe��Դ
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
�ú�����������ΪDMA���洴��һ��˳����ȵ�WDF IO���У�
������DMA���䷽�����ö�Ӧ�Ļص����������д�����
��ص����������ݣ��� XDMA_ENGINE ָ�룩�����浽���е��������С�
*/
NTSTATUS EngineCreateQueue(WDFDEVICE device, XDMA_ENGINE* engine, WDFQUEUE* queue)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attribs;
    PQUEUE_CONTEXT context;

    PAGED_CODE();

    // ��ʼ��IO�������ã�������Ϊ˳�����ģʽ WdfIoQueueDispatchSequential����IO����˳���Ŷ�ִ�С�
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchSequential);

    // ����DMA����ķ��� engine->dir���жϵ�ǰ�Ǵ��豸��������C2H�����Ǵ��������豸��H2C���Ĵ���
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

    // ��ʼ�� attribs������ͬ����Χ����Ϊ WdfSynchronizationScopeQueue����ʾ������ö�����صĻص��ᱻ���л�
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.SynchronizationScope = WdfSynchronizationScopeQueue;
    // ���ö�������������Ϊ QUEUE_CONTEXT���Ա�洢�������ص����������ݡ�
    WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attribs, QUEUE_CONTEXT);

    // ����IO���У����豸���󡢶������á��������Դ��ݸ��������������Ķ��о���洢�� queue �С�
    status = WdfIoQueueCreate(device, &config, &attribs, queue);
    if (!NT_SUCCESS(status)) {
        TraceError(DBG_INIT, "WdfIoQueueCreate failed %d", status);
        return status;
    }

    // ��ȡ���е������ģ����� engine ����洢���������У����������ڴ������ʱ���ʶ�Ӧ��DMA���档
    context = GetQueueContext(*queue);
    context->engine = engine;

    return status;
}
