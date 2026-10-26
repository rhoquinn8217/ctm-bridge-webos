/* Stand-in for Apple's IOKit/hid/IOHIDManager.h (with what it pulls in from
 * IOHIDDevice.h, IOHIDBase.h and IOReturn.h), for a compile check on Linux only. */
#ifndef STUB_IOHIDMANAGER_H
#define STUB_IOHIDMANAGER_H
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>

typedef int kern_return_t;
typedef kern_return_t IOReturn;
typedef UInt32 IOOptionBits;

#define STUB_IOKIT_ERR(code) ((IOReturn)(0xe0000000u | (code)))
#define kIOReturnSuccess 0
#define kIOReturnNoDevice STUB_IOKIT_ERR(0x2c0)
#define kIOReturnNotPrivileged STUB_IOKIT_ERR(0x2c1)
#define kIOReturnBadArgument STUB_IOKIT_ERR(0x2c2)
#define kIOReturnExclusiveAccess STUB_IOKIT_ERR(0x2c5)
#define kIOReturnTimeout STUB_IOKIT_ERR(0x2d6)
#define kIOReturnNotPermitted STUB_IOKIT_ERR(0x2e2)

typedef struct __IOHIDManager *IOHIDManagerRef;
typedef struct __IOHIDDevice *IOHIDDeviceRef;
typedef void (*IOHIDCallback)(void *context, IOReturn result, void *sender);
typedef void (*IOHIDReportCallback)(void *context, IOReturn result, void *sender, IOHIDReportType type,
                                    uint32_t reportID, uint8_t *report, CFIndex reportLength);

IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef allocator, IOOptionBits options);
void IOHIDManagerSetDeviceMatchingMultiple(IOHIDManagerRef manager, CFArrayRef multiple);
CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef manager);

IOReturn IOHIDDeviceOpen(IOHIDDeviceRef device, IOOptionBits options);
IOReturn IOHIDDeviceClose(IOHIDDeviceRef device, IOOptionBits options);
CFTypeRef IOHIDDeviceGetProperty(IOHIDDeviceRef device, CFStringRef key);
IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID,
                              const uint8_t *report, CFIndex reportLength);
IOReturn IOHIDDeviceGetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID,
                              uint8_t *report, CFIndex *pReportLength);
void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef device, uint8_t *report, CFIndex reportLength,
                                            IOHIDReportCallback callback, void *context);
void IOHIDDeviceRegisterRemovalCallback(IOHIDDeviceRef device, IOHIDCallback callback, void *context);
void IOHIDDeviceScheduleWithRunLoop(IOHIDDeviceRef device, CFRunLoopRef runLoop, CFStringRef runLoopMode);
void IOHIDDeviceUnscheduleFromRunLoop(IOHIDDeviceRef device, CFRunLoopRef runLoop, CFStringRef runLoopMode);

#endif
