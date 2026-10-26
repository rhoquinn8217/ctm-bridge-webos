/* Stand-in for Apple's IOKit/hid/IOHIDKeys.h, for a compile check on Linux only. */
#ifndef STUB_IOHIDKEYS_H
#define STUB_IOHIDKEYS_H
#include <stdint.h>

#define kIOHIDTransportKey "Transport"
#define kIOHIDVendorIDKey "VendorID"
#define kIOHIDProductIDKey "ProductID"
#define kIOHIDManufacturerKey "Manufacturer"
#define kIOHIDProductKey "Product"
#define kIOHIDSerialNumberKey "SerialNumber"
#define kIOHIDReportDescriptorKey "ReportDescriptor"

typedef enum {
    kIOHIDReportTypeInput = 0,
    kIOHIDReportTypeOutput,
    kIOHIDReportTypeFeature,
    kIOHIDReportTypeCount
} IOHIDReportType;

enum {
    kIOHIDOptionsTypeNone = 0x00,
    kIOHIDOptionsTypeSeizeDevice = 0x01
};
typedef uint32_t IOHIDOptionsType;

#endif
