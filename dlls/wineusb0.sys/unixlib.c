/*
 * Wine libusb0 bridge driver - Unix libusb backend
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

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <libusb.h>
#include <pthread.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineusb0);

struct unix_device
{
    struct list entry;
    libusb_device_handle *handle;  /* NULL until opened via usb0_open_device */
    libusb_device *libusb_dev;
    pthread_mutex_t lock;
    int open_count;
};

static libusb_hotplug_callback_handle hotplug_cb_handle;
static volatile bool thread_shutdown;

static struct usb0_event *usb0_events;
static size_t usb0_event_count, usb0_events_capacity;

static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list device_list = LIST_INIT(device_list);

static bool array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    unsigned int new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return true;

    max_capacity = ~(size_t)0 / size;
    if (count > max_capacity)
        return false;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return false;

    *elements = new_elements;
    *capacity = new_capacity;

    return true;
}

static void queue_event(const struct usb0_event *event)
{
    if (array_reserve((void **)&usb0_events, &usb0_events_capacity, usb0_event_count + 1, sizeof(*usb0_events)))
        usb0_events[usb0_event_count++] = *event;
    else
        ERR("Failed to queue event.\n");
}

static bool get_event(struct usb0_event *event)
{
    if (!usb0_event_count) return false;

    *event = usb0_events[0];
    if (--usb0_event_count)
        memmove(usb0_events, usb0_events + 1, usb0_event_count * sizeof(*usb0_events));

    return true;
}

static NTSTATUS ntstatus_from_libusb(int error)
{
    switch (error)
    {
        case LIBUSB_SUCCESS:             return STATUS_SUCCESS;
        case LIBUSB_ERROR_IO:            return STATUS_IO_DEVICE_ERROR;
        case LIBUSB_ERROR_INVALID_PARAM: return STATUS_INVALID_PARAMETER;
        case LIBUSB_ERROR_ACCESS:        return STATUS_ACCESS_DENIED;
        case LIBUSB_ERROR_NO_DEVICE:     return STATUS_DEVICE_NOT_CONNECTED;
        case LIBUSB_ERROR_NOT_FOUND:     return STATUS_NOT_FOUND;
        case LIBUSB_ERROR_BUSY:          return STATUS_DEVICE_BUSY;
        case LIBUSB_ERROR_TIMEOUT:       return STATUS_IO_TIMEOUT;
        case LIBUSB_ERROR_PIPE:          return STATUS_IO_DEVICE_ERROR;
        case LIBUSB_ERROR_NO_MEM:        return STATUS_INSUFFICIENT_RESOURCES;
        default:
            ERR("Unhandled libusb error %d: %s\n", error, libusb_strerror(error));
            return STATUS_UNSUCCESSFUL;
    }
}

static NTSTATUS ntstatus_from_transfer_status(enum libusb_transfer_status status)
{
    switch (status)
    {
        case LIBUSB_TRANSFER_COMPLETED:  return STATUS_SUCCESS;
        case LIBUSB_TRANSFER_ERROR:      return STATUS_IO_DEVICE_ERROR;
        case LIBUSB_TRANSFER_TIMED_OUT:  return STATUS_IO_TIMEOUT;
        case LIBUSB_TRANSFER_CANCELLED:  return STATUS_CANCELLED;
        case LIBUSB_TRANSFER_STALL:      return STATUS_IO_DEVICE_ERROR;
        case LIBUSB_TRANSFER_NO_DEVICE:  return STATUS_DEVICE_NOT_CONNECTED;
        case LIBUSB_TRANSFER_OVERFLOW:   return STATUS_BUFFER_OVERFLOW;
        default:
            ERR("Unhandled transfer status %d.\n", status);
            return STATUS_UNSUCCESSFUL;
    }
}

static void add_usb_device(libusb_device *libusb_device)
{
    struct libusb_device_descriptor device_desc;
    struct unix_device *unix_device;
    struct usb0_event event;

    libusb_get_device_descriptor(libusb_device, &device_desc);

    TRACE("Adding new device %p, vendor %04x, product %04x.\n", libusb_device,
            device_desc.idVendor, device_desc.idProduct);

    if (!(unix_device = calloc(1, sizeof(*unix_device))))
        return;

    /* Lazy open: don't call libusb_open() here to avoid holding handles to
     * every USB device on the system. The handle is opened on first
     * IRP_MJ_CREATE and closed on last IRP_MJ_CLOSE. */
    unix_device->handle = NULL;
    unix_device->libusb_dev = libusb_ref_device(libusb_device);
    pthread_mutex_init(&unix_device->lock, NULL);
    unix_device->open_count = 0;

    pthread_mutex_lock(&device_mutex);
    list_add_tail(&device_list, &unix_device->entry);
    pthread_mutex_unlock(&device_mutex);

    event.type = USB0_EVENT_ADD_DEVICE;
    event.u.added_device.device = unix_device;
    event.u.added_device.vendor = device_desc.idVendor;
    event.u.added_device.product = device_desc.idProduct;
    event.u.added_device.product_name[0] = 0;
    queue_event(&event);
}

static void remove_usb_device(libusb_device *libusb_device)
{
    struct unix_device *unix_device;
    struct usb0_event event;

    TRACE("Removing device %p.\n", libusb_device);

    pthread_mutex_lock(&device_mutex);
    LIST_FOR_EACH_ENTRY(unix_device, &device_list, struct unix_device, entry)
    {
        if (unix_device->libusb_dev == libusb_device)
        {
            event.type = USB0_EVENT_REMOVE_DEVICE;
            event.u.removed_device = unix_device;
            queue_event(&event);
            break;
        }
    }
    pthread_mutex_unlock(&device_mutex);
}

static int LIBUSB_CALL hotplug_cb(libusb_context *context, libusb_device *device,
        libusb_hotplug_event event, void *user_data)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        add_usb_device(device);
    else
        remove_usb_device(device);

    return 0;
}

static NTSTATUS usb0_main_loop(void *args)
{
    const struct usb0_main_loop_params *params = args;
    int ret;

    while (!thread_shutdown)
    {
        if (get_event(params->event)) return STATUS_PENDING;

        if ((ret = libusb_handle_events(NULL)))
            ERR("Error handling events: %s\n", libusb_strerror(ret));
    }

    libusb_exit(NULL);
    free(usb0_events);
    usb0_events = NULL;
    usb0_event_count = usb0_events_capacity = 0;
    thread_shutdown = false;

    TRACE("USB0 main loop exiting.\n");
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_init(void *args)
{
    int ret;

    if ((ret = libusb_init(NULL)))
    {
        ERR("Failed to initialize libusb: %s\n", libusb_strerror(ret));
        return STATUS_UNSUCCESSFUL;
    }

    if ((ret = libusb_hotplug_register_callback(NULL,
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            LIBUSB_HOTPLUG_ENUMERATE,
            LIBUSB_HOTPLUG_MATCH_ANY,       /* any vendor */
            LIBUSB_HOTPLUG_MATCH_ANY,       /* any product */
            LIBUSB_HOTPLUG_MATCH_ANY,       /* any class */
            hotplug_cb, NULL, &hotplug_cb_handle)))
    {
        ERR("Failed to register hotplug callback: %s\n", libusb_strerror(ret));
        libusb_exit(NULL);
        return STATUS_UNSUCCESSFUL;
    }

    TRACE("Initialized libusb0 bridge, listening for all USB devices.\n");
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_exit(void *args)
{
    libusb_hotplug_deregister_callback(NULL, hotplug_cb_handle);
    thread_shutdown = true;
    libusb_interrupt_event_handler(NULL);

    return STATUS_SUCCESS;
}

static NTSTATUS usb0_get_descriptor(void *args)
{
    const struct usb0_get_descriptor_params *params = args;
    unsigned char *buffer;
    int ret;

    /* Use a control transfer to get the descriptor */
    uint8_t request_type = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD;

    /* Set recipient based on the descriptor recipient field */
    switch (params->recipient)
    {
        case 0: request_type |= LIBUSB_RECIPIENT_DEVICE; break;
        case 1: request_type |= LIBUSB_RECIPIENT_INTERFACE; break;
        case 2: request_type |= LIBUSB_RECIPIENT_ENDPOINT; break;
        default: request_type |= LIBUSB_RECIPIENT_OTHER; break;
    }

    buffer = malloc(params->buffer_length);
    if (!buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    ret = libusb_control_transfer(params->device->handle,
            request_type,
            LIBUSB_REQUEST_GET_DESCRIPTOR,
            (params->type << 8) | params->index,
            params->language_id,
            buffer,
            params->buffer_length,
            5000); /* 5 second timeout */

    if (ret < 0)
    {
        WARN("Failed to get descriptor type %u index %u: %s\n",
                params->type, params->index, libusb_strerror(ret));
        free(buffer);
        return ntstatus_from_libusb(ret);
    }

    memcpy(params->buffer, buffer, ret);
    *params->actual_length = ret;
    free(buffer);

    TRACE("Got descriptor type %u, %d bytes.\n", params->type, ret);
    return STATUS_SUCCESS;
}

struct transfer_ctx
{
    IRP *irp;
};

static void LIBUSB_CALL bulk_transfer_cb(struct libusb_transfer *transfer)
{
    struct transfer_ctx *ctx = transfer->user_data;
    struct usb0_event event;

    TRACE("Completing bulk transfer IRP %p, status %d, actual_length %d.\n",
            ctx->irp, transfer->status, transfer->actual_length);

    event.type = USB0_EVENT_TRANSFER_COMPLETE;
    event.u.completed_transfer.irp = ctx->irp;
    event.u.completed_transfer.status = ntstatus_from_transfer_status(transfer->status);
    event.u.completed_transfer.length = (transfer->status == LIBUSB_TRANSFER_COMPLETED)
            ? transfer->actual_length : 0;

    queue_event(&event);
    free(ctx);
}

static NTSTATUS usb0_bulk_transfer_async(void *args)
{
    const struct usb0_bulk_transfer_params *params = args;
    struct libusb_transfer *transfer;
    struct transfer_ctx *ctx;
    int ret;

    if (!(ctx = calloc(1, sizeof(*ctx))))
        return STATUS_NO_MEMORY;
    ctx->irp = params->irp;

    if (!(transfer = libusb_alloc_transfer(0)))
    {
        free(ctx);
        return STATUS_NO_MEMORY;
    }

    /* Store transfer handle in IRP for cancellation */
    params->irp->Tail.Overlay.DriverContext[0] = transfer;

    libusb_fill_bulk_transfer(transfer, params->device->handle,
            params->endpoint, params->buffer, params->length,
            bulk_transfer_cb, ctx, params->timeout);
    transfer->flags = LIBUSB_TRANSFER_FREE_TRANSFER;

    if ((ret = libusb_submit_transfer(transfer)) < 0)
    {
        ERR("Failed to submit bulk transfer: %s\n", libusb_strerror(ret));
        free(ctx);
        /* Don't free transfer since LIBUSB_TRANSFER_FREE_TRANSFER is set,
         * but we need to clear it since submit failed */
        transfer->flags = 0;
        libusb_free_transfer(transfer);
        return ntstatus_from_libusb(ret);
    }

    return STATUS_PENDING;
}

static NTSTATUS usb0_claim_interface(void *args)
{
    const struct usb0_claim_interface_params *params = args;
    int ret;

    ret = libusb_claim_interface(params->device->handle, params->interface_number);
    if (ret < 0)
    {
        ERR("Failed to claim interface %u: %s\n", params->interface_number, libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Claimed interface %u.\n", params->interface_number);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_release_interface(void *args)
{
    const struct usb0_release_interface_params *params = args;
    int ret;

    ret = libusb_release_interface(params->device->handle, params->interface_number);
    if (ret < 0)
    {
        WARN("Failed to release interface %u: %s\n", params->interface_number, libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Released interface %u.\n", params->interface_number);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_set_configuration(void *args)
{
    const struct usb0_set_configuration_params *params = args;
    int ret;

    ret = libusb_set_configuration(params->device->handle, params->configuration);
    if (ret < 0)
    {
        ERR("Failed to set configuration %u: %s\n", params->configuration, libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Set configuration %u.\n", params->configuration);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_get_configuration(void *args)
{
    const struct usb0_get_configuration_params *params = args;
    int ret;

    ret = libusb_get_configuration(params->device->handle, params->configuration);
    if (ret < 0)
    {
        ERR("Failed to get configuration: %s\n", libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Got configuration %d.\n", *params->configuration);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_set_interface(void *args)
{
    const struct usb0_set_interface_params *params = args;
    int ret;

    ret = libusb_set_interface_alt_setting(params->device->handle,
            params->interface_number, params->altsetting);
    if (ret < 0)
    {
        ERR("Failed to set interface %u alt %u: %s\n",
                params->interface_number, params->altsetting, libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Set interface %u alt %u.\n", params->interface_number, params->altsetting);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_reset_endpoint(void *args)
{
    const struct usb0_reset_endpoint_params *params = args;
    int ret;

    ret = libusb_clear_halt(params->device->handle, params->endpoint);
    if (ret < 0)
    {
        WARN("Failed to clear halt on endpoint %#x: %s\n", params->endpoint, libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Reset endpoint %#x.\n", params->endpoint);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_reset_device(void *args)
{
    const struct usb0_reset_device_params *params = args;
    int ret;

    ret = libusb_reset_device(params->device->handle);
    if (ret < 0)
    {
        ERR("Failed to reset device: %s\n", libusb_strerror(ret));
        return ntstatus_from_libusb(ret);
    }

    TRACE("Reset device.\n");
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_control_transfer(void *args)
{
    const struct usb0_control_transfer_params *params = args;
    unsigned char *buffer = NULL;
    int ret;
    unsigned int timeout = params->timeout ? params->timeout : 5000;

    if (params->length > 0)
    {
        buffer = malloc(params->length);
        if (!buffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        /* For OUT transfers, copy data from the caller's buffer */
        if (!(params->request_type & 0x80) && params->buffer)
            memcpy(buffer, params->buffer, params->length);
    }

    ret = libusb_control_transfer(params->device->handle,
            params->request_type, params->request,
            params->value, params->index,
            buffer, params->length, timeout);

    if (ret < 0)
    {
        WARN("Control transfer failed: %s (request_type=%#x request=%#x value=%#x index=%#x)\n",
                libusb_strerror(ret), params->request_type, params->request,
                params->value, params->index);
        free(buffer);
        return ntstatus_from_libusb(ret);
    }

    /* For IN transfers, copy data back to the caller's buffer */
    if ((params->request_type & 0x80) && params->buffer && ret > 0)
        memcpy(params->buffer, buffer, ret);

    *params->actual_length = ret;
    free(buffer);

    TRACE("Control transfer completed, %d bytes.\n", ret);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_cancel_transfer(void *args)
{
    const struct usb0_cancel_transfer_params *params = args;
    int ret;

    if ((ret = libusb_cancel_transfer(params->transfer)) < 0)
        ERR("Failed to cancel transfer: %s\n", libusb_strerror(ret));

    return STATUS_SUCCESS;
}

static NTSTATUS usb0_open_device(void *args)
{
    const struct usb0_open_device_params *params = args;
    struct unix_device *device = params->device;
    int ret;

    pthread_mutex_lock(&device->lock);

    if (!device->handle)
    {
        if ((ret = libusb_open(device->libusb_dev, &device->handle)))
        {
            pthread_mutex_unlock(&device->lock);
            ERR("Failed to open device: %s\n", libusb_strerror(ret));
            return ntstatus_from_libusb(ret);
        }
        libusb_set_auto_detach_kernel_driver(device->handle, 1);
    }
    device->open_count++;

    pthread_mutex_unlock(&device->lock);
    TRACE("Opened device %p, open_count %d.\n", device, device->open_count);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_close_device(void *args)
{
    const struct usb0_close_device_params *params = args;
    struct unix_device *device = params->device;

    pthread_mutex_lock(&device->lock);

    if (device->open_count > 0)
        device->open_count--;

    if (device->open_count == 0 && device->handle)
    {
        libusb_close(device->handle);
        device->handle = NULL;
        TRACE("Closed device %p.\n", device);
    }

    pthread_mutex_unlock(&device->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS usb0_destroy_device(void *args)
{
    const struct usb0_destroy_device_params *params = args;
    struct unix_device *device = params->device;

    pthread_mutex_lock(&device_mutex);
    list_remove(&device->entry);
    pthread_mutex_unlock(&device_mutex);

    if (device->handle)
        libusb_close(device->handle);
    libusb_unref_device(device->libusb_dev);
    pthread_mutex_destroy(&device->lock);
    free(device);

    return STATUS_SUCCESS;
}

static NTSTATUS usb0_abort_transfers(void *args)
{
    /* This is a no-op on the unix side; the PE side handles cancellation
     * by iterating the IRP list and calling usb0_cancel_transfer for each. */
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
#define X(name) [unix_ ## name] = name
    X(usb0_init),
    X(usb0_exit),
    X(usb0_main_loop),
    X(usb0_get_descriptor),
    X(usb0_bulk_transfer_async),
    X(usb0_claim_interface),
    X(usb0_release_interface),
    X(usb0_set_configuration),
    X(usb0_get_configuration),
    X(usb0_set_interface),
    X(usb0_reset_endpoint),
    X(usb0_reset_device),
    X(usb0_control_transfer),
    X(usb0_cancel_transfer),
    X(usb0_destroy_device),
    X(usb0_abort_transfers),
    X(usb0_open_device),
    X(usb0_close_device),
};
