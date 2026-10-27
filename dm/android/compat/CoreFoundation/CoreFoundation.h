// Android stand-in for the CoreFoundation the host uses: a mutable dictionary with pointer keys and values (no retain / release callbacks),
// as hle_nvn.c's program key table makes it (CFDictionaryCreateMutable(NULL, 0, NULL, NULL)). plat_android.c: an open-addressing hash table
#pragma once
#include "dm_android.h"
typedef struct dm_cfdict *CFMutableDictionaryRef, *CFDictionaryRef;
typedef long CFIndex;
typedef const void *CFTypeRef;
typedef const void *CFAllocatorRef;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb);
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
void CFRelease(CFTypeRef o);
