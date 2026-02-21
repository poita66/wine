/*
 * wineusb0 Unix library interface
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

#ifndef __WINE_WINEUSB0_UNIXLIB_H
#define __WINE_WINEUSB0_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/unixlib.h"

/* libusb-win32 IOCTL codes (matching libusb-win32 v1.2.6 ABI) */
#define LIBUSB0_IOCTL_SET_CONFIGURATION    0x00222004  /* CTL_CODE(0x22, 0x801, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_GET_CONFIGURATION    0x00222008  /* CTL_CODE(0x22, 0x802, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_SET_INTERFACE        0x0022200C  /* CTL_CODE(0x22, 0x803, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_GET_INTERFACE        0x00222010  /* CTL_CODE(0x22, 0x804, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_CLAIM_INTERFACE      0x00222014  /* CTL_CODE(0x22, 0x805, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_RELEASE_INTERFACE    0x00222018  /* CTL_CODE(0x22, 0x806, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_SET_DEBUG_LEVEL      0x0022201C  /* CTL_CODE(0x22, 0x807, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_GET_VERSION          0x00222020  /* CTL_CODE(0x22, 0x808, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_GET_DESCRIPTOR       0x00222024  /* CTL_CODE(0x22, 0x809, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_BULK_WRITE           0x00222029  /* CTL_CODE(0x22, 0x80A, METHOD_IN_DIRECT,   0) */
#define LIBUSB0_IOCTL_BULK_READ            0x0022202A  /* CTL_CODE(0x22, 0x80A, METHOD_OUT_DIRECT,  0) */
#define LIBUSB0_IOCTL_ABORT_ENDPOINT       0x0022202C  /* CTL_CODE(0x22, 0x80B, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_RESET_ENDPOINT       0x00222030  /* CTL_CODE(0x22, 0x80C, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_RESET_DEVICE         0x00222034  /* CTL_CODE(0x22, 0x80D, METHOD_BUFFERED,    0) */
#define LIBUSB0_IOCTL_VENDOR_WRITE         0x00222039  /* CTL_CODE(0x22, 0x80E, METHOD_IN_DIRECT,   0) */
#define LIBUSB0_IOCTL_VENDOR_READ          0x0022203A  /* CTL_CODE(0x22, 0x80E, METHOD_OUT_DIRECT,  0) */
#define LIBUSB0_IOCTL_CONTROL_WRITE        0x0022203D  /* CTL_CODE(0x22, 0x80F, METHOD_IN_DIRECT,   0) */
#define LIBUSB0_IOCTL_CONTROL_READ         0x0022203E  /* CTL_CODE(0x22, 0x80F, METHOD_OUT_DIRECT,  0) */

/* libusb-win32 request struct - matches ABI exactly (24 bytes) */
typedef struct
{
    unsigned int timeout;
    union
    {
        struct
        {
            unsigned int configuration;
        } configuration;
        struct
        {
            unsigned int interface_number;
            unsigned int altsetting_number;
        } interface;
        struct
        {
            unsigned int endpoint;
            unsigned int packet_size;
        } endpoint;
        struct
        {
            unsigned int type;
            unsigned int recipient;
            unsigned int request;
            unsigned int value;
            unsigned int index;
        } vendor;
        struct
        {
            unsigned int type;
            unsigned int recipient;
            unsigned int index;
            unsigned int language_id;
        } descriptor;
        struct
        {
            unsigned int level;
        } debug;
        struct
        {
            unsigned int major;
            unsigned int minor;
            unsigned int micro;
            unsigned int nano;
        } version;
        struct
        {
            unsigned int request;
            unsigned int value;
            unsigned int index;
        } control;
    };
} libusb_request;

/* Fake libusb-win32 driver version */
#define LIBUSB0_VERSION_MAJOR 1
#define LIBUSB0_VERSION_MINOR 2
#define LIBUSB0_VERSION_MICRO 6
#define LIBUSB0_VERSION_NANO  0

/* Event types for PE/Unix communication */
enum usb0_event_type
{
    USB0_EVENT_ADD_DEVICE,
    USB0_EVENT_REMOVE_DEVICE,
    USB0_EVENT_TRANSFER_COMPLETE,
};

struct usb0_event
{
    enum usb0_event_type type;

    union
    {
        struct usb0_add_device_event
        {
            struct unix_device *device;
            UINT16 vendor, product;
            char product_name[64];
        } added_device;
        struct unix_device *removed_device;
        struct
        {
            IRP *irp;
            NTSTATUS status;
            ULONG length;
        } completed_transfer;
    } u;
};

/* Parameter structs for unix calls */

struct usb0_main_loop_params
{
    struct usb0_event *event;
};

struct usb0_get_descriptor_params
{
    struct unix_device *device;
    unsigned int type;
    unsigned int recipient;
    unsigned int index;
    unsigned int language_id;
    void *buffer;
    ULONG buffer_length;
    ULONG *actual_length;
};

struct usb0_bulk_transfer_params
{
    struct unix_device *device;
    unsigned int endpoint;
    void *buffer;
    ULONG length;
    unsigned int timeout;
    IRP *irp;
};

struct usb0_claim_interface_params
{
    struct unix_device *device;
    unsigned int interface_number;
};

struct usb0_release_interface_params
{
    struct unix_device *device;
    unsigned int interface_number;
};

struct usb0_set_configuration_params
{
    struct unix_device *device;
    unsigned int configuration;
};

struct usb0_get_configuration_params
{
    struct unix_device *device;
    int *configuration;
};

struct usb0_set_interface_params
{
    struct unix_device *device;
    unsigned int interface_number;
    unsigned int altsetting;
};

struct usb0_reset_endpoint_params
{
    struct unix_device *device;
    unsigned int endpoint;
};

struct usb0_reset_device_params
{
    struct unix_device *device;
};

struct usb0_control_transfer_params
{
    struct unix_device *device;
    unsigned int request_type;
    unsigned int request;
    unsigned int value;
    unsigned int index;
    void *buffer;
    unsigned int length;
    unsigned int timeout;
    ULONG *actual_length;
};

struct usb0_cancel_transfer_params
{
    void *transfer;
};

struct usb0_destroy_device_params
{
    struct unix_device *device;
};

struct usb0_open_device_params
{
    struct unix_device *device;
};

struct usb0_close_device_params
{
    struct unix_device *device;
};

struct usb0_abort_endpoint_params
{
    struct unix_device *device;
    unsigned int endpoint;
};

enum unix_funcs
{
    unix_usb0_init,
    unix_usb0_exit,
    unix_usb0_main_loop,
    unix_usb0_get_descriptor,
    unix_usb0_bulk_transfer_async,
    unix_usb0_claim_interface,
    unix_usb0_release_interface,
    unix_usb0_set_configuration,
    unix_usb0_get_configuration,
    unix_usb0_set_interface,
    unix_usb0_reset_endpoint,
    unix_usb0_reset_device,
    unix_usb0_control_transfer,
    unix_usb0_cancel_transfer,
    unix_usb0_destroy_device,
    unix_usb0_abort_transfers,
    unix_usb0_open_device,
    unix_usb0_close_device,
};

#endif
