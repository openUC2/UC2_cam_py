#include "MvsError.h"

#include <cstdio>

#include "MvErrorDefine.h"

namespace hik {

std::string mvsErrorString(int code)
{
    // The SDK error codes above 0x7fffffff are unsigned; compare as unsigned so
    // the case labels (e.g. 0x80000000) are not narrowed to int.
    switch (static_cast<unsigned int>(code))
    {
    case MV_OK:                  return "success";
    // General SDK errors
    case MV_E_HANDLE:            return "error or invalid handle";
    case MV_E_SUPPORT:           return "function not supported";
    case MV_E_BUFOVER:           return "buffer overflow";
    case MV_E_CALLORDER:         return "function calling order error";
    case MV_E_PARAMETER:         return "incorrect parameter";
    case MV_E_RESOURCE:          return "applying resource failed";
    case MV_E_NODATA:            return "no data";
    case MV_E_PRECONDITION:      return "precondition error or environment changed";
    case MV_E_VERSION:           return "version mismatch";
    case MV_E_NOENOUGH_BUF:      return "insufficient memory";
    case MV_E_ABNORMAL_IMAGE:    return "abnormal (incomplete) image, maybe lost packets";
    case MV_E_LOAD_LIBRARY:      return "load dynamic library failed";
    case MV_E_NOOUTBUF:          return "no available output buffer";
    case MV_E_UNKNOW:            return "unknown error";
    // GenICam
    case MV_E_GC_ACCESS:         return "node access condition error";
    case MV_E_GC_TIMEOUT:        return "GenICam timeout";
    // Device / network
    case MV_E_NOT_IMPLEMENTED:   return "command not supported by device";
    case MV_E_ACCESS_DENIED:     return "no access permission (device in use?)";
    case MV_E_BUSY:              return "device busy or network disconnected";
    case MV_E_NETER:             return "network error";
    case MV_E_NORESPONSE:        return "no response from device";
    case MV_E_IP_CONFLICT:       return "device IP conflict";
    // USB
    case MV_E_USB_READ:          return "USB read error";
    case MV_E_USB_WRITE:         return "USB write error";
    case MV_E_USB_DEVICE:        return "USB device exception";
    case MV_E_USB_BANDWIDTH:     return "insufficient USB bandwidth";
    case MV_E_USB_DRIVER:        return "USB driver mismatch or not installed";
    default:                     return "unmapped error";
    }
}

std::string mvsErrorLabel(int code)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (0x%08x)", static_cast<unsigned int>(code));
    return mvsErrorString(code) + buf;
}

} // namespace hik
