/* Stand-in for Apple's CoreFoundation, for a compile check on Linux only.
 * Declarations follow the macOS SDK's signatures. */
#ifndef STUB_COREFOUNDATION_H
#define STUB_COREFOUNDATION_H
#include <stdint.h>

typedef unsigned char UInt8;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef signed long CFIndex;
typedef unsigned long CFOptionFlags;
typedef unsigned char Boolean;
typedef double CFTimeInterval;
typedef CFTimeInterval CFAbsoluteTime;
typedef const void *CFTypeRef;
typedef unsigned long CFTypeID;
typedef UInt32 CFStringEncoding;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFSet *CFSetRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef CFStringRef CFRunLoopMode;

typedef enum { kCFNumberIntType = 9 } CFNumberType;
enum { kCFStringEncodingUTF8 = 0x08000100 };

typedef struct { CFIndex version; void *retain; void *release; void *copyDescription; void *equal; void *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; void *retain; void *release; void *copyDescription; void *equal; } CFDictionaryValueCallBacks;
typedef struct { CFIndex version; void *retain; void *release; void *copyDescription; void *equal; } CFArrayCallBacks;
typedef struct { CFIndex version; void *info; const void *(*retain)(const void *); void (*release)(const void *); CFStringRef (*copyDescription)(const void *); } CFRunLoopTimerContext;
typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef timer, void *info);

extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFRunLoopMode kCFRunLoopDefaultMode;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;

CFStringRef __CFStringMakeConstantString(const char *cStr);
#define CFSTR(cStr) __CFStringMakeConstantString("" cStr "")

CFTypeRef CFRetain(CFTypeRef cf);
void CFRelease(CFTypeRef cf);
CFTypeID CFGetTypeID(CFTypeRef cf);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFDataGetTypeID(void);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef allocator, CFIndex capacity,
                                                 const CFDictionaryKeyCallBacks *keyCallBacks,
                                                 const CFDictionaryValueCallBacks *valueCallBacks);
void CFDictionarySetValue(CFMutableDictionaryRef theDict, const void *key, const void *value);
CFNumberRef CFNumberCreate(CFAllocatorRef allocator, CFNumberType theType, const void *valuePtr);
Boolean CFNumberGetValue(CFNumberRef number, CFNumberType theType, void *valuePtr);
Boolean CFStringGetCString(CFStringRef theString, char *buffer, CFIndex bufferSize, CFStringEncoding encoding);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFArrayCallBacks *callBacks);
void CFArrayAppendValue(CFMutableArrayRef theArray, const void *value);
CFIndex CFSetGetCount(CFSetRef theSet);
void CFSetGetValues(CFSetRef theSet, const void **values);
const UInt8 *CFDataGetBytePtr(CFDataRef theData);
CFIndex CFDataGetLength(CFDataRef theData);
CFRunLoopRef CFRunLoopGetCurrent(void);
void CFRunLoopRun(void);
void CFRunLoopStop(CFRunLoopRef rl);
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval,
                                       CFOptionFlags flags, CFIndex order, CFRunLoopTimerCallBack callout,
                                       CFRunLoopTimerContext *context);
void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer, CFRunLoopMode mode);
void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer, CFRunLoopMode mode);

/* In macOS's <stdlib.h> and <string.h>. */
#include <stddef.h>
uint32_t arc4random(void);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

#endif
