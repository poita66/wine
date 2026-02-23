/*
 * Wine libusb0 bridge driver
 *
 * Bridges libusb-win32 IOCTLs (\\.\libusb0-XXXX) to Linux native libusb,
 * enabling applications with statically-linked libusb-win32 code to
 * communicate with USB devices.
 *
 * Copyright 2026 Peter Stewart
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winioctl.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineusb0);

#define DECLARE_CRITICAL_SECTION(cs) \
    static CRITICAL_SECTION cs; \
    static CRITICAL_SECTION_DEBUG cs##_debug = \
    { 0, 0, &cs, { &cs##_debug.ProcessLocksList, &cs##_debug.ProcessLocksList }, \
      0, 0, { (DWORD_PTR)(__FILE__ ": " # cs) }}; \
    static CRITICAL_SECTION cs = { &cs##_debug, -1, 0, 0, 0, 0 };

DECLARE_CRITICAL_SECTION(wineusb0_cs);

static struct list device_list = LIST_INIT(device_list);
static DRIVER_OBJECT *driver_obj;
static HANDLE event_thread;

struct usb0_device
{
    struct list entry;
    BOOL removed;

    DEVICE_OBJECT *device_obj;
    struct unix_device *unix_device;
    UNICODE_STRING symlink_name;

    UINT16 vendor, product;

    LIST_ENTRY irp_list;
};

static void add_usb0_device(const struct usb0_add_device_event *event)
{
    static unsigned int device_index = 1;
    struct usb0_device *device;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING dev_name, link_name;
    NTSTATUS status;
    WCHAR dev_name_buf[32];
    WCHAR link_name_buf[32];

    TRACE("Adding new device %p, vendor %04x, product %04x.\n",
            event->device, event->vendor, event->product);

    swprintf(dev_name_buf, ARRAY_SIZE(dev_name_buf), L"\\Device\\libusb0-%04u", device_index);
    swprintf(link_name_buf, ARRAY_SIZE(link_name_buf), L"\\DosDevices\\libusb0-%04u", device_index);
    device_index++;

    RtlInitUnicodeString(&dev_name, dev_name_buf);
    if ((status = IoCreateDevice(driver_obj, sizeof(*device), &dev_name,
            FILE_DEVICE_UNKNOWN, 0, FALSE, &device_obj)))
    {
        ERR("Failed to create device, status %#lx.\n", status);
        return;
    }

    device = device_obj->DeviceExtension;
    device->device_obj = device_obj;
    device->unix_device = event->device;
    device->removed = FALSE;
    device->vendor = event->vendor;
    device->product = event->product;
    InitializeListHead(&device->irp_list);

    /* Allocate and store symlink name for later deletion */
    device->symlink_name.Length = wcslen(link_name_buf) * sizeof(WCHAR);
    device->symlink_name.MaximumLength = device->symlink_name.Length + sizeof(WCHAR);
    device->symlink_name.Buffer = ExAllocatePool(PagedPool, device->symlink_name.MaximumLength);
    if (!device->symlink_name.Buffer)
    {
        ERR("Failed to allocate symlink name.\n");
        IoDeleteDevice(device_obj);
        return;
    }
    memcpy(device->symlink_name.Buffer, link_name_buf, device->symlink_name.MaximumLength);

    RtlInitUnicodeString(&link_name, link_name_buf);
    if ((status = IoCreateSymbolicLink(&link_name, &dev_name)))
    {
        ERR("Failed to create symbolic link, status %#lx.\n", status);
        ExFreePool(device->symlink_name.Buffer);
        IoDeleteDevice(device_obj);
        return;
    }

    /* Direct I/O for METHOD_IN_DIRECT / METHOD_OUT_DIRECT IOCTLs */
    device_obj->Flags |= DO_DIRECT_IO;
    device_obj->Flags &= ~DO_DEVICE_INITIALIZING;

    EnterCriticalSection(&wineusb0_cs);
    list_add_tail(&device_list, &device->entry);
    LeaveCriticalSection(&wineusb0_cs);

    TRACE("Created device %s -> %s for VID_%04x&PID_%04x.\n",
            debugstr_w(dev_name_buf), debugstr_w(link_name_buf),
            event->vendor, event->product);
}

static void remove_usb0_device(struct unix_device *unix_device)
{
    struct usb0_device *device;

    TRACE("Removing device %p.\n", unix_device);

    EnterCriticalSection(&wineusb0_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, struct usb0_device, entry)
    {
        if (device->unix_device == unix_device)
        {
            if (!device->removed)
            {
                LIST_ENTRY *irp_entry;

                device->removed = TRUE;
                list_remove(&device->entry);

                /* Cancel all pending IRPs */
                while ((irp_entry = RemoveHeadList(&device->irp_list)) != &device->irp_list)
                {
                    IRP *irp = CONTAINING_RECORD(irp_entry, IRP, Tail.Overlay.ListEntry);
                    struct usb0_cancel_transfer_params params =
                    {
                        .transfer = irp->Tail.Overlay.DriverContext[0],
                    };
                    WINE_UNIX_CALL(unix_usb0_cancel_transfer, &params);
                }
            }
            break;
        }
    }
    LeaveCriticalSection(&wineusb0_cs);
}

static void complete_transfer(IRP *irp, NTSTATUS status, ULONG length)
{
    EnterCriticalSection(&wineusb0_cs);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    LeaveCriticalSection(&wineusb0_cs);

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = length;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static DWORD CALLBACK event_thread_proc(void *arg)
{
    struct usb0_event event;
    struct usb0_main_loop_params params =
    {
        .event = &event,
    };

    TRACE("Starting event thread.\n");

    if (WINE_UNIX_CALL(unix_usb0_init, NULL) != STATUS_SUCCESS)
        return 0;

    while (WINE_UNIX_CALL(unix_usb0_main_loop, &params) == STATUS_PENDING)
    {
        switch (event.type)
        {
            case USB0_EVENT_ADD_DEVICE:
                add_usb0_device(&event.u.added_device);
                break;

            case USB0_EVENT_REMOVE_DEVICE:
                remove_usb0_device(event.u.removed_device);
                break;

            case USB0_EVENT_TRANSFER_COMPLETE:
                complete_transfer(event.u.completed_transfer.irp,
                        event.u.completed_transfer.status,
                        event.u.completed_transfer.length);
                break;
        }
    }

    TRACE("Shutting down event thread.\n");
    return 0;
}

static NTSTATUS WINAPI driver_create(DEVICE_OBJECT *device_obj, IRP *irp)
{
    struct usb0_device *device = device_obj->DeviceExtension;
    struct usb0_open_device_params params = { .device = device->unix_device };
    NTSTATUS status;

    TRACE("device_obj %p, irp %p.\n", device_obj, irp);

    if (device->removed)
    {
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    /* Lazy open: open the libusb handle on first IRP_MJ_CREATE */
    status = WINE_UNIX_CALL(unix_usb0_open_device, &params);
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS WINAPI driver_close(DEVICE_OBJECT *device_obj, IRP *irp)
{
    struct usb0_device *device = device_obj->DeviceExtension;
    struct usb0_close_device_params params = { .device = device->unix_device };

    TRACE("device_obj %p, irp %p.\n", device_obj, irp);

    /* Close the libusb handle when all file handles are closed */
    WINE_UNIX_CALL(unix_usb0_close_device, &params);

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_cleanup(DEVICE_OBJECT *device_obj, IRP *irp)
{
    struct usb0_device *device = device_obj->DeviceExtension;
    LIST_ENTRY *entry;

    TRACE("device_obj %p, irp %p.\n", device_obj, irp);

    /* Cancel all pending async transfers for this device */
    EnterCriticalSection(&wineusb0_cs);
    for (entry = device->irp_list.Flink; entry != &device->irp_list; entry = entry->Flink)
    {
        IRP *queued_irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        struct usb0_cancel_transfer_params params =
        {
            .transfer = queued_irp->Tail.Overlay.DriverContext[0],
        };
        WINE_UNIX_CALL(unix_usb0_cancel_transfer, &params);
    }
    LeaveCriticalSection(&wineusb0_cs);

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS handle_get_version(IRP *irp, IO_STACK_LOCATION *stack)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;

    if (out_len < sizeof(libusb_request))
        return STATUS_BUFFER_TOO_SMALL;

    req->version.major = LIBUSB0_VERSION_MAJOR;
    req->version.minor = LIBUSB0_VERSION_MINOR;
    req->version.micro = LIBUSB0_VERSION_MICRO;
    req->version.nano  = LIBUSB0_VERSION_NANO;

    irp->IoStatus.Information = sizeof(libusb_request);
    TRACE("GET_VERSION -> %u.%u.%u.%u\n",
            LIBUSB0_VERSION_MAJOR, LIBUSB0_VERSION_MINOR,
            LIBUSB0_VERSION_MICRO, LIBUSB0_VERSION_NANO);
    return STATUS_SUCCESS;
}

static NTSTATUS handle_get_descriptor(struct usb0_device *device, IRP *irp, IO_STACK_LOCATION *stack)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG actual_length = 0;
    struct usb0_get_descriptor_params params;
    NTSTATUS status;

    TRACE("GET_DESCRIPTOR type %u, recipient %u, index %u, language_id %u, out_len %lu.\n",
            req->descriptor.type, req->descriptor.recipient,
            req->descriptor.index, req->descriptor.language_id, out_len);

    params.device = device->unix_device;
    params.type = req->descriptor.type;
    params.recipient = req->descriptor.recipient;
    params.index = req->descriptor.index;
    params.language_id = req->descriptor.language_id;
    params.buffer = irp->AssociatedIrp.SystemBuffer;
    params.buffer_length = out_len;
    params.actual_length = &actual_length;

    status = WINE_UNIX_CALL(unix_usb0_get_descriptor, &params);

    irp->IoStatus.Information = actual_length;
    return status;
}

static NTSTATUS handle_claim_interface(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_claim_interface_params params =
    {
        .device = device->unix_device,
        .interface_number = req->interface.interface_number,
    };

    TRACE("CLAIM_INTERFACE %u.\n", req->interface.interface_number);
    return WINE_UNIX_CALL(unix_usb0_claim_interface, &params);
}

static NTSTATUS handle_release_interface(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_release_interface_params params =
    {
        .device = device->unix_device,
        .interface_number = req->interface.interface_number,
    };

    TRACE("RELEASE_INTERFACE %u.\n", req->interface.interface_number);
    return WINE_UNIX_CALL(unix_usb0_release_interface, &params);
}

static NTSTATUS handle_set_configuration(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_set_configuration_params params =
    {
        .device = device->unix_device,
        .configuration = req->configuration.configuration,
    };

    TRACE("SET_CONFIGURATION %u.\n", req->configuration.configuration);
    return WINE_UNIX_CALL(unix_usb0_set_configuration, &params);
}

static NTSTATUS handle_get_configuration(struct usb0_device *device, IRP *irp, IO_STACK_LOCATION *stack)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG out_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    int configuration = 0;
    struct usb0_get_configuration_params params =
    {
        .device = device->unix_device,
        .configuration = &configuration,
    };
    NTSTATUS status;

    if (out_len < sizeof(libusb_request))
        return STATUS_BUFFER_TOO_SMALL;

    status = WINE_UNIX_CALL(unix_usb0_get_configuration, &params);
    if (status == STATUS_SUCCESS)
    {
        req->configuration.configuration = configuration;
        irp->IoStatus.Information = sizeof(libusb_request);
    }

    TRACE("GET_CONFIGURATION -> %d, status %#lx.\n", configuration, status);
    return status;
}

static NTSTATUS handle_set_interface(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_set_interface_params params =
    {
        .device = device->unix_device,
        .interface_number = req->interface.interface_number,
        .altsetting = req->interface.altsetting_number,
    };

    TRACE("SET_INTERFACE %u alt %u.\n", req->interface.interface_number,
            req->interface.altsetting_number);
    return WINE_UNIX_CALL(unix_usb0_set_interface, &params);
}

static NTSTATUS handle_reset_endpoint(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_reset_endpoint_params params =
    {
        .device = device->unix_device,
        .endpoint = req->endpoint.endpoint,
    };

    TRACE("RESET_ENDPOINT %u.\n", req->endpoint.endpoint);
    return WINE_UNIX_CALL(unix_usb0_reset_endpoint, &params);
}

static NTSTATUS handle_abort_endpoint(struct usb0_device *device, IRP *irp)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    struct usb0_abort_endpoint_params params =
    {
        .device = device->unix_device,
        .endpoint = req->endpoint.endpoint,
    };

    TRACE("ABORT_ENDPOINT %u.\n", req->endpoint.endpoint);

    /* Cancel all pending transfers for this endpoint on the unix side */
    return WINE_UNIX_CALL(unix_usb0_abort_transfers, &params);
}

static NTSTATUS handle_reset_device(struct usb0_device *device)
{
    struct usb0_reset_device_params params =
    {
        .device = device->unix_device,
    };

    TRACE("RESET_DEVICE.\n");
    return WINE_UNIX_CALL(unix_usb0_reset_device, &params);
}

static NTSTATUS handle_bulk_transfer(struct usb0_device *device, IRP *irp,
        IO_STACK_LOCATION *stack, ULONG ioctl_code)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG data_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    unsigned int endpoint = req->endpoint.endpoint;
    void *data_buffer;
    struct usb0_bulk_transfer_params params;
    NTSTATUS status;

    /* For BULK_READ (METHOD_OUT_DIRECT) and BULK_WRITE (METHOD_IN_DIRECT),
     * the data buffer is accessed via MdlAddress. */
    if (!irp->MdlAddress)
    {
        WARN("No MDL for bulk transfer.\n");
        return STATUS_INVALID_PARAMETER;
    }

    data_buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
    if (!data_buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Set direction bit based on IOCTL: BULK_READ = IN (0x80), BULK_WRITE = OUT (0x00) */
    if (ioctl_code == LIBUSB0_IOCTL_BULK_READ)
        endpoint |= 0x80;
    else
        endpoint &= ~0x80;

    TRACE("BULK_%s endpoint %#x, length %lu, timeout %u.\n",
            (ioctl_code == LIBUSB0_IOCTL_BULK_READ) ? "READ" : "WRITE",
            endpoint, data_len, req->timeout);

    params.device = device->unix_device;
    params.endpoint = endpoint;
    params.buffer = data_buffer;
    params.length = data_len;
    params.timeout = req->timeout;
    params.irp = irp;

    /* Hold lock while submitting and queuing to prevent race with completion */
    EnterCriticalSection(&wineusb0_cs);
    status = WINE_UNIX_CALL(unix_usb0_bulk_transfer_async, &params);
    if (status == STATUS_PENDING)
    {
        IoMarkIrpPending(irp);
        InsertTailList(&device->irp_list, &irp->Tail.Overlay.ListEntry);
    }
    LeaveCriticalSection(&wineusb0_cs);

    return status;
}

static NTSTATUS handle_vendor_transfer(struct usb0_device *device, IRP *irp,
        IO_STACK_LOCATION *stack, ULONG ioctl_code)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG data_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    unsigned int request_type;
    void *data_buffer = NULL;
    ULONG actual_length = 0;
    struct usb0_control_transfer_params params;
    NTSTATUS status;

    /* Build USB request type byte: direction | type | recipient */
    request_type = (req->vendor.type & 0x03) << 5;
    request_type |= (req->vendor.recipient & 0x1f);

    if (ioctl_code == LIBUSB0_IOCTL_VENDOR_READ)
        request_type |= 0x80; /* ENDPOINT_IN */

    /* For vendor transfers with data, the data buffer is via MdlAddress */
    if (data_len > 0 && irp->MdlAddress)
    {
        data_buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
        if (!data_buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    TRACE("VENDOR_%s request_type %#x, request %#x, value %#x, index %#x, length %lu.\n",
            (ioctl_code == LIBUSB0_IOCTL_VENDOR_READ) ? "READ" : "WRITE",
            request_type, req->vendor.request, req->vendor.value,
            req->vendor.index, data_len);

    params.device = device->unix_device;
    params.request_type = request_type;
    params.request = req->vendor.request;
    params.value = req->vendor.value;
    params.index = req->vendor.index;
    params.buffer = data_buffer;
    params.length = data_len;
    params.timeout = req->timeout;
    params.actual_length = &actual_length;

    status = WINE_UNIX_CALL(unix_usb0_control_transfer, &params);
    irp->IoStatus.Information = actual_length;
    return status;
}

static NTSTATUS handle_control_transfer(struct usb0_device *device, IRP *irp,
        IO_STACK_LOCATION *stack, ULONG ioctl_code)
{
    libusb_request *req = irp->AssociatedIrp.SystemBuffer;
    ULONG data_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    unsigned int request_type;
    void *data_buffer = NULL;
    ULONG actual_length = 0;
    struct usb0_control_transfer_params params;
    NTSTATUS status;

    /* For generic control transfers, the request type is constructed differently.
     * The control struct has: request, value, index (no explicit type/recipient).
     * The direction is implied by the IOCTL code. */
    request_type = 0;
    if (ioctl_code == LIBUSB0_IOCTL_CONTROL_READ)
        request_type |= 0x80; /* ENDPOINT_IN */

    if (data_len > 0 && irp->MdlAddress)
    {
        data_buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
        if (!data_buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    TRACE("CONTROL_%s request %#x, value %#x, index %#x, length %lu.\n",
            (ioctl_code == LIBUSB0_IOCTL_CONTROL_READ) ? "READ" : "WRITE",
            req->control.request, req->control.value,
            req->control.index, data_len);

    params.device = device->unix_device;
    params.request_type = request_type;
    params.request = req->control.request;
    params.value = req->control.value;
    params.index = req->control.index;
    params.buffer = data_buffer;
    params.length = data_len;
    params.timeout = req->timeout;
    params.actual_length = &actual_length;

    status = WINE_UNIX_CALL(unix_usb0_control_transfer, &params);
    irp->IoStatus.Information = actual_length;
    return status;
}

static NTSTATUS WINAPI driver_ioctl(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    struct usb0_device *device = device_obj->DeviceExtension;
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;

    TRACE("device_obj %p, irp %p, code %#lx.\n", device_obj, irp, code);

    if (device->removed)
    {
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    irp->IoStatus.Information = 0;

    switch (code)
    {
        case LIBUSB0_IOCTL_GET_VERSION:
            status = handle_get_version(irp, stack);
            break;

        case LIBUSB0_IOCTL_GET_DESCRIPTOR:
            status = handle_get_descriptor(device, irp, stack);
            break;

        case LIBUSB0_IOCTL_CLAIM_INTERFACE:
            status = handle_claim_interface(device, irp);
            break;

        case LIBUSB0_IOCTL_RELEASE_INTERFACE:
            status = handle_release_interface(device, irp);
            break;

        case LIBUSB0_IOCTL_SET_CONFIGURATION:
            status = handle_set_configuration(device, irp);
            break;

        case LIBUSB0_IOCTL_GET_CONFIGURATION:
            status = handle_get_configuration(device, irp, stack);
            break;

        case LIBUSB0_IOCTL_SET_INTERFACE:
            status = handle_set_interface(device, irp);
            break;

        case LIBUSB0_IOCTL_GET_INTERFACE:
            FIXME("GET_INTERFACE not implemented.\n");
            status = STATUS_NOT_IMPLEMENTED;
            break;

        case LIBUSB0_IOCTL_RESET_ENDPOINT:
            status = handle_reset_endpoint(device, irp);
            break;

        case LIBUSB0_IOCTL_ABORT_ENDPOINT:
            status = handle_abort_endpoint(device, irp);
            break;

        case LIBUSB0_IOCTL_RESET_DEVICE:
            status = handle_reset_device(device);
            break;

        case LIBUSB0_IOCTL_BULK_READ:
        case LIBUSB0_IOCTL_BULK_WRITE:
            status = handle_bulk_transfer(device, irp, stack, code);
            break;

        case LIBUSB0_IOCTL_VENDOR_READ:
        case LIBUSB0_IOCTL_VENDOR_WRITE:
            status = handle_vendor_transfer(device, irp, stack, code);
            break;

        case LIBUSB0_IOCTL_CONTROL_READ:
        case LIBUSB0_IOCTL_CONTROL_WRITE:
            status = handle_control_transfer(device, irp, stack, code);
            break;

        case LIBUSB0_IOCTL_SET_DEBUG_LEVEL:
            TRACE("SET_DEBUG_LEVEL (ignored).\n");
            status = STATUS_SUCCESS;
            break;

        default:
            FIXME("Unhandled ioctl %#lx (device %#lx, access %#lx, function %#lx, method %#lx).\n",
                    code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    return status;
}

static void WINAPI driver_unload(DRIVER_OBJECT *driver)
{
    struct usb0_device *device, *cursor;

    TRACE("driver %p.\n", driver);

    WINE_UNIX_CALL(unix_usb0_exit, NULL);
    WaitForSingleObject(event_thread, INFINITE);
    CloseHandle(event_thread);

    EnterCriticalSection(&wineusb0_cs);
    LIST_FOR_EACH_ENTRY_SAFE(device, cursor, &device_list, struct usb0_device, entry)
    {
        struct usb0_destroy_device_params params =
        {
            .device = device->unix_device,
        };
        WINE_UNIX_CALL(unix_usb0_destroy_device, &params);
        IoDeleteSymbolicLink(&device->symlink_name);
        ExFreePool(device->symlink_name.Buffer);
        list_remove(&device->entry);
        IoDeleteDevice(device->device_obj);
    }
    LeaveCriticalSection(&wineusb0_cs);
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    NTSTATUS status;

    TRACE("driver %p, path %s.\n", driver, debugstr_w(path->Buffer));

    if ((status = __wine_init_unix_call()))
    {
        ERR("Failed to initialize Unix library, status %#lx.\n", status);
        return status;
    }

    driver_obj = driver;

    driver->DriverUnload = driver_unload;
    driver->MajorFunction[IRP_MJ_CREATE] = driver_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = driver_close;
    driver->MajorFunction[IRP_MJ_CLEANUP] = driver_cleanup;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver_ioctl;

    event_thread = CreateThread(NULL, 0, event_thread_proc, NULL, 0, NULL);
    if (!event_thread)
    {
        ERR("Failed to create event thread.\n");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}
