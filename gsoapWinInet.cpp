/*  gsoapWinInet 2.1 implementation.
    See the header file for usage.
    This file is distributed under the MIT licence. 
 */

#include <winsock2.h>
#include <windows.h>
#include <crtdbg.h>
#include <wininet.h>
#include <stdio.h>
#include <stdarg.h>

#include "gsoapWinInet.h"

#pragma comment(lib, "ws2_32.lib")      /* link the winsock2 library */
#pragma comment(lib, "wininet.lib")     /* link the wininet library */
#pragma warning(disable : 4996)         /* disable deprecation warnings */

#ifndef UNUSED_ARG
# define UNUSED_ARG(x) (void)(x)
#endif

#define INVALID_BUFFER_LENGTH  ((DWORD)-1)

#define ROUND_UP(value, step) (((value) % (step) == 0) ? (value) : ((((value) / (step)) + 1) * (step)))

/* plugin private data */

#define WININET_VERSION "wininet-2.1"
static const char wininet_id[] = WININET_VERSION;

enum LogFormat { LOGTYPE_UNKNOWN, LOGTYPE_TEXT, LOGTYPE_XML, LOGTYPE_HEX };

struct wininet_data
{
    HINTERNET            hInternet;         /* internet session handle */
    HINTERNET            hConnection;       /* current connection handle */
    HINTERNET            hRequest;          /* current request handle */
    BOOL                 bDisconnect;       /* connection is disconnected */
    DWORD                dwRequestFlags;    /* extra request flags from user */
    char *               pUrlPath;          /* current URL path to use */
    char *               pszErrorMessage;   /* wininet/system error message */
    char *               pUserAgent;        /* user agent header */
    char *               pBuffer;           /* send buffer */
    size_t               uiBufferSize;      /* current size of the buffer */
    size_t               uiBufferLenMax;    /* total length of the message */
    size_t               uiBufferLen;       /* length of data in buffer */
    BOOL                 bIsChunkSize;      /* expecting a chunk size buffer */
    enum LogFormat       nLogFormat;        /* log data format */
    wininet_rse_callback pRseCallback;      /* wininet_resolve_send_error callback.  Allows clients to resolve ssl errors programatically */
    FILE *               hLog;              /* debug log file */
};

/*=============================================================================
  Local Functions
 ============================================================================*/

/*  create an incrementing semi-random, semi-unique value to use in request 
    headers. It doesn't matter if this is really unique or random, it just
    provides a reference for matching debug logs together. It is assumed that 
    srand() has been called. */
static void
wininet_getreqid(
    char * aBuf,
    size_t aBufSiz
    )
{
    static LONG counter = (LONG) GetTickCount();
    LONG curr = InterlockedIncrement(&counter);
    _snprintf(aBuf, aBufSiz, "%lX%lX%lX", 
        (ULONG)GetCurrentProcessId(), (ULONG)rand(), (ULONG)curr);
}

/* NOTE: call via the WININET_LOGx macros for checking of a_pData */
static void
wininet_log(
    struct wininet_data *   a_pData,
    const char *            a_pFormat,
    ...
    )
{
    va_list args;
    struct timeb tb;
    struct tm * ptm;

    va_start(args, a_pFormat);

    ftime(&tb);
    ptm = localtime(&tb.time);
    fprintf(a_pData->hLog, "%02u/%02u %02u:%02u:%02u.%03u %p ",
        ptm->tm_mon + 1, ptm->tm_mday,
        ptm->tm_hour, ptm->tm_min, ptm->tm_sec, tb.millitm,
        a_pData);
    vfprintf(a_pData->hLog, a_pFormat, args);
    fputc('\n', a_pData->hLog);

    fflush(a_pData->hLog); /* as it is a debug log, flush it always */
}

/* hex */
static void
wininet_log_data_hex(
    struct wininet_data *   a_pData,
    const void *            a_pBuf,
    const size_t            a_nBufLen
    )
{
#define cols        24
#define linelen     (7 + cols*3 + cols/3 + 2 + cols + 2) /* offset + hex + space + space + ascii + LF NUL */

    char line[linelen]; 
    const unsigned char * pBuf = (const unsigned char *) a_pBuf;
    const char hex[] = "0123456789abcdef";
    size_t n, idx = 0;

    line[sizeof(line)-1] = 0;
    line[sizeof(line)-2] = '\n';

    while (idx < a_nBufLen) {
        size_t count = idx + cols > a_nBufLen ? a_nBufLen - idx : cols;
        memset(line, ' ', sizeof(line)-2);
        for (n = 0; n < count; ++n) {
            register unsigned char ch = pBuf[idx+n];
            line[0] = '0' + (char) ((idx / 10000) % 10);
            line[1] = '0' + (char) ((idx / 1000) % 10);
            line[2] = '0' + (char) ((idx / 100) % 10);
            line[3] = '0' + (char) ((idx / 10) % 10);
            line[4] = '0' + (char) ((idx / 1) % 10);
            line[5] = ':';
            line[7+n*3+0+n/8] = hex[ch >> 4];
            line[7+n*3+1+n/8] = hex[ch & 0xF];
            line[7+3+cols*3+2+n] = ch >= 32 && ch < 127 ? ch : '.';
        }
        idx += count;
        fwrite(line, 1, sizeof(line)-1, a_pData->hLog);
    }

#undef linelen
#undef cols
}

/* wrapped text */
static void
wininet_log_data_text(
    struct wininet_data *   a_pData,
    const void *            a_pBuf,
    const size_t            a_nBufLen
    )
{
    size_t len, idx;
    const unsigned char * pBuf = (const unsigned char *) a_pBuf;
    const unsigned char * pBufEnd = pBuf + a_nBufLen;

    while (pBuf < pBufEnd) {
        const size_t linelen = 120;
        for (len = idx = 0; len < linelen && pBuf + idx < pBufEnd; ++idx, ++len) {
            if (pBuf[idx] == '\n') len = 0;
        }
        fwrite(pBuf, 1, idx, a_pData->hLog);
        fputc('\n', a_pData->hLog);
        pBuf += idx;
    }
}

typedef enum XmlItemType_t {
    XMLTYPE_TEXT,
    XMLTYPE_OPEN,
    XMLTYPE_CLOSE, 
    XMLTYPE_OTHER   /* e.g. XMLPI, self-closing, things that don't change the indent */
} XmlItemType;

typedef struct XmlItem_t
{
    XmlItemType     nType;
    const char *    pStart;
    unsigned        nLen;
    BOOL            nNewLine;
    BOOL            nSplitAttribs;
} XmlItem;

static BOOL
wininet_parsexml(
    const char * a_pBuf,
    const char * a_pBufEnd,
    XmlItem *    a_pItem
    )
{
    unsigned char ch;

    if (a_pBuf >= a_pBufEnd) {
        return FALSE;
    }
    a_pItem->pStart = a_pBuf;
    a_pItem->nNewLine = FALSE;
    a_pItem->nSplitAttribs = FALSE;

    /* text starts if we are not a tag */
    if (*a_pBuf != '<') {
        a_pItem->nType = XMLTYPE_TEXT;
        while (a_pBuf < a_pBufEnd && *a_pBuf != '<') ++a_pBuf;
        a_pItem->nLen = (unsigned) (a_pBuf - a_pItem->pStart);
        a_pItem->nNewLine = (*(a_pBuf-1) == '\n');
        return TRUE;
    }

    a_pItem->nType = XMLTYPE_OPEN;
    while (a_pBuf < a_pBufEnd && *a_pBuf != '>') ++a_pBuf;
    a_pItem->nLen = (unsigned) (a_pBuf - a_pItem->pStart);
    if (a_pBuf < a_pBufEnd) ++a_pItem->nLen; /* include the tag end */

    /* anything that doesn't look like a tag is text */
    if (a_pItem->pStart + 2 >= a_pBuf || a_pBuf >= a_pBufEnd) {
        a_pItem->nType = XMLTYPE_TEXT;
        return TRUE;
    }

    /* look for a closing tag */
    ch = *(a_pItem->pStart+1);
    if (ch == '/') {
        a_pItem->nType = XMLTYPE_CLOSE;
        return TRUE;
    }

    /* look for a self-closing or XMLPI tag */
    ch = *(a_pBuf-1);
    if (ch == '/' || ch == '?') {
        a_pItem->nType = XMLTYPE_OTHER;
        return TRUE;
    }

    /* look for the soap envelope to mark it for splitting attribs to a new line */
#define SOAP_ENVELOPE "<SOAP-ENV:Envelope "
    if (a_pItem->nLen > sizeof(SOAP_ENVELOPE) 
        && !strnicmp(a_pItem->pStart, SOAP_ENVELOPE, sizeof(SOAP_ENVELOPE)-1)) 
    {
        a_pItem->nSplitAttribs = TRUE;
    }

    return TRUE;
}

static BOOL
wininet_haschild(
    const char * a_pBuf,
    const char * a_pBufEnd
    )
{
    XmlItem item;
    while (wininet_parsexml(a_pBuf, a_pBufEnd, &item)) {
        a_pBuf = item.pStart + item.nLen;
        if (item.nType == XMLTYPE_OPEN) return TRUE;
        if (item.nType == XMLTYPE_CLOSE) return FALSE;
    }
    return FALSE;
}

static void 
wininet_indent(
    struct wininet_data *   a_pData,
    int                     a_nIndent
    )
{
    char line[400];
    int len;

    if (a_nIndent < 1) return;
    for (len = 0; len < a_nIndent; ++len) {
        line[len] = ' ';
    }
    line[len] = 0;
    fwrite(line, 1, len, a_pData->hLog);
}

static void
wininet_writetag(
    struct wininet_data *   a_pData,
    XmlItem *               a_pItem,
    int                     a_nIndent
    )
{
    const char * pCurr;
    const char * pBuf    = a_pItem->pStart;
    const char * pBufEnd = pBuf + a_pItem->nLen;

    /* wrap text at a maximum line length, convert CRLF into LF only */
    if (a_pItem->nType == XMLTYPE_TEXT) {
        const size_t linelen = 120;
        size_t len, idx;
        BOOL newline = FALSE;

        while (pBuf < pBufEnd) {
            /* find end of buffer or end of line */
            for (len = idx = 0; len < linelen && pBuf + idx < pBufEnd; ++idx, ++len) {
                if (pBuf[idx] == '\r' || pBuf[idx] == '\n') {
                    newline = TRUE;
                    break;
                }
            }
            fwrite(pBuf, 1, idx, a_pData->hLog);
            pBuf += idx;
            if (pBuf < pBufEnd || newline) {
                fputc('\n', a_pData->hLog);
                if (*pBuf == '\r') ++pBuf;
                if (*pBuf == '\n') ++pBuf;
                newline = FALSE;
            }
            if (pBuf < pBufEnd) {
                wininet_indent(a_pData, a_nIndent);
            }
        }
        return;
    }

    if (!a_pItem->nSplitAttribs) {
        fwrite(a_pItem->pStart, 1, a_pItem->nLen, a_pData->hLog);
        return;
    }

    /* find the first attribute */
    for (pCurr = pBuf; pCurr < pBufEnd && *pCurr != ' '; ++pCurr);
    for (; pCurr < pBufEnd && *pCurr == ' '; ++pCurr);
    fwrite(pBuf, 1, pCurr - pBuf, a_pData->hLog);
    
    a_nIndent += 4;
    pBuf = pCurr;
    while (pBuf < pBufEnd) {
        /* find end of attrib */
        for (; pCurr < pBufEnd && *pCurr != '='; ++pCurr);
        if (pCurr < pBufEnd && *pCurr == '=') {
            ++pCurr;
            if (pCurr < pBufEnd && *pCurr == '"') {
                for (++pCurr; pCurr < pBufEnd && *pCurr != '"'; ++pCurr);
            }
            if (pCurr < pBufEnd && *pCurr == '"') ++pCurr;
        }
        if (pCurr < pBufEnd && *pCurr == ' ') ++pCurr;
        if (pCurr < pBufEnd && *pCurr == '>') ++pCurr;

        /* output this attrib */
        fputc('\n', a_pData->hLog);
        wininet_indent(a_pData, a_nIndent);
        fwrite(pBuf, 1, pCurr - pBuf, a_pData->hLog);
        pBuf = pCurr;
    }
}

/* pretty xml */
static void
wininet_log_data_xml(
    struct wininet_data *   a_pData,
    const void *            a_pBuf,
    const size_t            a_nBufLen
    )
{
    const char * pBuf = (const char *) a_pBuf;
    const char * pBufEnd = pBuf + a_nBufLen;
    XmlItem item;
    int indent = 0;
    BOOL skipNewLine = FALSE;

    while (wininet_parsexml(pBuf, pBufEnd, &item)) {
        pBuf = item.pStart + item.nLen;

        if (item.nType == XMLTYPE_CLOSE) {
            indent -= 2;
        }
        if (!skipNewLine) {
            wininet_indent(a_pData, indent);
        }
        if (item.nType == XMLTYPE_CLOSE) {
            skipNewLine = FALSE;
        }
        wininet_writetag(a_pData, &item, indent);
        if (item.nType == XMLTYPE_OPEN) {
            indent += 2;
            if (!wininet_haschild(pBuf, pBufEnd)) {
                skipNewLine = TRUE;
            }
        }
        if (!item.nNewLine && !skipNewLine) {
            fputc('\n', a_pData->hLog);
        }
    }
}

/* log data as hex dumps */
static void
wininet_log_data(
    struct wininet_data *   a_pData,
    const char *            a_pInfo,
    const void *            a_pBuf,
    const size_t            a_nBufLen
    )
{
    wininet_log(a_pData, "%s (%ld bytes):\n", a_pInfo, a_nBufLen);

    switch (a_pData->nLogFormat) {
    case LOGTYPE_XML:
        wininet_log_data_xml(a_pData, a_pBuf, a_nBufLen);
        break;
    case LOGTYPE_TEXT:
        wininet_log_data_text(a_pData, a_pBuf, a_nBufLen);
        break;
    case LOGTYPE_UNKNOWN:
    case LOGTYPE_HEX:
    default:
        wininet_log_data_hex(a_pData, a_pBuf, a_nBufLen);
        break;
    }

    fputc('\n', a_pData->hLog);
    fflush(a_pData->hLog);
}

#define WININET_LOG0(data, format)              if (data && data->hLog) wininet_log(data, format)           
#define WININET_LOG1(data, format, a)           if (data && data->hLog) wininet_log(data, format, a)           
#define WININET_LOG2(data, format, a,b)         if (data && data->hLog) wininet_log(data, format, a,b)         
#define WININET_LOG3(data, format, a,b,c)       if (data && data->hLog) wininet_log(data, format, a,b,c)       
#define WININET_LOG4(data, format, a,b,c,d)     if (data && data->hLog) wininet_log(data, format, a,b,c,d)     

static void
wininet_free_error_message(
    struct wininet_data * a_pData
    )
{
    if (a_pData->pszErrorMessage) {
        LocalFree(a_pData->pszErrorMessage);
        a_pData->pszErrorMessage = NULL;
    }
}

static const char *
wininet_error_message(
    struct wininet_data *   a_pData,
    DWORD                   a_dwErrorMsgId
    )
{
    HINSTANCE   hModule;
    DWORD       dwResult;
    DWORD       dwFormatFlags;

    /* free any existing error message */
    wininet_free_error_message(a_pData);

    dwFormatFlags = 
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_FROM_SYSTEM;

    /* load wininet.dll for the error messages */
    hModule = LoadLibraryExA("wininet.dll", NULL,
        LOAD_LIBRARY_AS_DATAFILE | DONT_RESOLVE_DLL_REFERENCES);
    if (hModule) {
        dwFormatFlags |= FORMAT_MESSAGE_FROM_HMODULE;
    }

    /* format the messages */
    dwResult = FormatMessageA(
        dwFormatFlags, 
        hModule, 
        a_dwErrorMsgId, 
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR) &a_pData->pszErrorMessage,
        0,
        NULL);

    /* free the library if we loaded it */
    if (hModule) {
        FreeLibrary(hModule);
    }

    /* remove the CR LF from the error message */
    if (dwResult > 2) {
        a_pData->pszErrorMessage[dwResult-2] = 0;
        return a_pData->pszErrorMessage;
    }
    else {
        const static char szUnknown[] = "(unknown)";
        return szUnknown;
    }
}

static BOOL
wininet_flag_set_option(
    struct wininet_data *   a_pData,
    DWORD                   a_dwOption,
    DWORD                   a_dwFlag
    )
{
    DWORD dwBuffer;
    DWORD dwBufferLength = sizeof(DWORD);
    BOOL bSuccess;

    _ASSERTE(a_pData->hRequest != NULL);

    bSuccess = InternetQueryOption(a_pData->hRequest, a_dwOption, &dwBuffer, &dwBufferLength);
    if (!bSuccess) {
        DWORD dwErrorCode = GetLastError();
        WININET_LOG3(a_pData, "flag_set_option: failed to get option %X, error %d (%s)",
            a_dwOption, dwErrorCode, wininet_error_message(a_pData, dwErrorCode));
        return bSuccess;
    }

    WININET_LOG4(a_pData, 
        "flag_set_option: %X, original value %d, adding flag %d, new value %d",
        a_dwOption, dwBuffer, a_dwFlag, dwBuffer | a_dwFlag);

    dwBuffer |= a_dwFlag;
    bSuccess = InternetSetOption(a_pData->hRequest, a_dwOption, &dwBuffer, dwBufferLength);
    if (!bSuccess) {
        DWORD dwErrorCode = GetLastError();
        WININET_LOG3(a_pData, "flag_set_option: failed to set option %X, error %d (%s)",
            a_dwOption, dwErrorCode, wininet_error_message(a_pData, dwErrorCode));
    }

    return bSuccess;
}

static wininet_rseReturn
wininet_resolve_send_error(
    struct wininet_data *   a_pData,
    DWORD                   a_dwErrorCode
    )
{
    wininet_rseReturn nRetVal = rseFalse;
    DWORD dwResult, dwLastError;

    WININET_LOG2(a_pData, "resolve_send_error: code = %d (%s)",
        a_dwErrorCode, wininet_error_message(a_pData, a_dwErrorCode));

    dwResult = InternetErrorDlg(GetDesktopWindow(), a_pData->hRequest, a_dwErrorCode,
        FLAGS_ERROR_UI_FILTER_FOR_ERRORS |
        FLAGS_ERROR_UI_FLAGS_GENERATE_DATA |
        FLAGS_ERROR_UI_FLAGS_CHANGE_OPTIONS,
        NULL);

    dwLastError = GetLastError();
    if (a_dwErrorCode == ERROR_INTERNET_INCORRECT_PASSWORD) {
        nRetVal = (dwResult == ERROR_INTERNET_FORCE_RETRY) ? rseTrue : rseFalse;
    }
    else {
        nRetVal = (dwResult == ERROR_SUCCESS) ? rseTrue : rseFalse;
    }

    if (nRetVal == rseTrue) {
        WININET_LOG0(a_pData, "resolve_send_error: result = true");

        /* Ignore errors once they have been handled or ignored once */
        switch (a_dwErrorCode) {
        case ERROR_INTERNET_SEC_CERT_CN_INVALID:
           /*   ignore invalid SSL certificate dates on this connection if the 
                client has indicated to ignore them this time 
            */
            WININET_LOG0(a_pData, "resolve_send_error: ignoring "
                "ERROR_INTERNET_SEC_CERT_CN_INVALID in future");
            wininet_flag_set_option(a_pData, INTERNET_OPTION_SECURITY_FLAGS, 
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID);
            break;
        }
    }
    else {
        WININET_LOG2(a_pData, "resolve_send_error: result = false, last error = %lu, %s",
            dwLastError, wininet_error_message(a_pData, dwLastError));
    }

    return nRetVal;
}

static DWORD
wininet_set_timeout(
    struct wininet_data *   a_pData,
    const char *            a_pszTimeout,
    DWORD                   a_dwOption,
    int                     a_nTimeout
    )
{
    DWORD dwTimeout;
    BOOL bSuccess;

    if (a_nTimeout < 1) {
        a_nTimeout = 60 * 60; /* unlimited = 1 hour = 60 mins x 60 secs */
    }
    dwTimeout = a_nTimeout * 1000;

    WININET_LOG3(a_pData, "set_timeout: %s = %d seconds (%lu ms)", a_pszTimeout, a_nTimeout, dwTimeout);
    bSuccess = InternetSetOption(a_pData->hInternet, a_dwOption, &dwTimeout, sizeof(DWORD));
    if (!bSuccess) {
        DWORD dwErrorCode = GetLastError();
        WININET_LOG3(a_pData, "set_timeout: failed to set %s timeout, error %d (%s)", 
            a_pszTimeout, dwErrorCode, wininet_error_message(a_pData, dwErrorCode));
        return dwErrorCode;
    }

    return 0;
}

/* check to ensure that our connection hasn't been disconnected 
    and disconnect remaining handles if necessary.
 */
static BOOL
wininet_have_connection(
    struct soap *           soap,
    struct wininet_data *   a_pData
    )
{
    /* close the connection */
    BOOL bCloseConnection = a_pData->bDisconnect || !a_pData->hConnection;
    if (bCloseConnection) {
        if (a_pData->hRequest) {
            WININET_LOG0(a_pData, "have_connection: have request = true, close connection = true -> closing request");
            InternetCloseHandle(a_pData->hRequest);
            a_pData->hRequest = NULL;
        }

        if (a_pData->hConnection) {
            WININET_LOG0(a_pData, "have_connection: have connection = true, close connection = true -> closing connection");
            InternetCloseHandle(a_pData->hConnection);
            a_pData->hConnection = NULL;
        }

        soap->socket = SOAP_INVALID_SOCKET;
        a_pData->bDisconnect = FALSE;
    }

    /* are we still connected? */
    if (!a_pData->hConnection) {
        WININET_LOG0(a_pData, "have_connection: false");
        return FALSE;
    }

    return TRUE;
}

static void CALLBACK
wininet_callback(
    HINTERNET   hInternet,
    DWORD_PTR   dwContext,
    DWORD       dwInternetStatus,
    LPVOID      lpvStatusInformation,
    DWORD       dwStatusInformationLength
    )
{
    struct soap * soap = (struct soap *) dwContext;
    char buf[500] = { 0 };
    const DWORD * pdw = (const DWORD *) lpvStatusInformation; /* sometimes */
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    UNUSED_ARG(hInternet);
    UNUSED_ARG(dwStatusInformationLength);

    switch (dwInternetStatus) {
    case INTERNET_STATUS_RESOLVING_NAME:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_RESOLVING_NAME: %s", 
            (const char *) lpvStatusInformation);
        break;
    case INTERNET_STATUS_NAME_RESOLVED:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_NAME_RESOLVED: %s", 
            (const char *) lpvStatusInformation);
        break;
    case INTERNET_STATUS_CONNECTING_TO_SERVER: 
        WININET_LOG0(pData, "callback: INTERNET_STATUS_CONNECTING_TO_SERVER");
        break;
    case INTERNET_STATUS_CONNECTED_TO_SERVER:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_CONNECTED_TO_SERVER");
        break;
    case INTERNET_STATUS_SENDING_REQUEST:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_SENDING_REQUEST");
        break;
    case INTERNET_STATUS_REQUEST_SENT:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_REQUEST_SENT, bytes sent = %lu", *pdw);
        break;
    case INTERNET_STATUS_RECEIVING_RESPONSE:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_RECEIVING_RESPONSE");
        break;
    case INTERNET_STATUS_RESPONSE_RECEIVED:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_RESPONSE_RECEIVED, bytes received = %lu", *pdw);
        break;
    case INTERNET_STATUS_CTL_RESPONSE_RECEIVED:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_CTL_RESPONSE_RECEIVED");
        break;
    case INTERNET_STATUS_PREFETCH:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_PREFETCH");
        break;
    case INTERNET_STATUS_CLOSING_CONNECTION:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_CLOSING_CONNECTION");
        break;
    case INTERNET_STATUS_CONNECTION_CLOSED:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_CONNECTION_CLOSED");
        if (pData->hConnection) {
            /*  the connection has been closed, so we close the handle here.
                however only mark this for disconnection otherwise errors 
                will occur when reading the data from the handle. In every 
                function that uses the connection, first check to  see if 
                it has been disconnected.
             */
            WININET_LOG0(pData, "callback: marking connection for disconnect");
            pData->bDisconnect = TRUE;
        }
        break;
    case INTERNET_STATUS_HANDLE_CREATED:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_HANDLE_CREATED");
        break;
    case INTERNET_STATUS_HANDLE_CLOSING:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_HANDLE_CLOSING");
        break;
#ifdef INTERNET_STATUS_DETECTING_PROXY
    case INTERNET_STATUS_DETECTING_PROXY:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_DETECTING_PROXY");
        break;
#endif
    case INTERNET_STATUS_REQUEST_COMPLETE:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_REQUEST_COMPLETE");
        break;
    case INTERNET_STATUS_REDIRECT:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_REDIRECT, new url = %s", 
            (const char *) lpvStatusInformation);
        break;
    case INTERNET_STATUS_INTERMEDIATE_RESPONSE:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_INTERMEDIATE_RESPONSE");
        break;
#ifdef INTERNET_STATUS_USER_INPUT_REQUIRED
    case INTERNET_STATUS_USER_INPUT_REQUIRED:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_USER_INPUT_REQUIRED");
        break;
#endif
    case INTERNET_STATUS_STATE_CHANGE:
        if (*pdw & INTERNET_STATE_CONNECTED)            strcat(buf, ", CONNECTED");
        if (*pdw & INTERNET_STATE_DISCONNECTED)         strcat(buf, ", DISCONNECTED");
        if (*pdw & INTERNET_STATE_DISCONNECTED_BY_USER) strcat(buf, ", DISCONNECTED_BY_USER");
        if (*pdw & INTERNET_STATE_IDLE)                 strcat(buf, ", IDLE");
        if (*pdw & INTERNET_STATE_BUSY)                 strcat(buf, ", BUSY");
        if (*pdw & INTERNET_STATUS_USER_INPUT_REQUIRED) strcat(buf, ", USER_INPUT_REQUIRED");
        WININET_LOG1(pData, "callback: INTERNET_STATUS_STATE_CHANGE: %s", &buf[2]);
        break;
#ifdef INTERNET_STATUS_COOKIE_SENT
    case INTERNET_STATUS_COOKIE_SENT:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_COOKIE_SENT: count = %lu", *pdw);
        break;
#endif
#ifdef INTERNET_STATUS_COOKIE_RECEIVED
    case INTERNET_STATUS_COOKIE_RECEIVED:
        WININET_LOG1(pData, "callback: INTERNET_STATUS_COOKIE_RECEIVED: count = %lu", *pdw);
        break;
#endif
#ifdef INTERNET_STATUS_PRIVACY_IMPACTED
    case INTERNET_STATUS_PRIVACY_IMPACTED:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_PRIVACY_IMPACTED");
        break;
#endif
#ifdef INTERNET_STATUS_P3P_HEADER
    case INTERNET_STATUS_P3P_HEADER:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_P3P_HEADER");
        break;
#endif
#ifdef INTERNET_STATUS_P3P_POLICYREF
    case INTERNET_STATUS_P3P_POLICYREF:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_P3P_POLICYREF");
        break;
#endif
#ifdef INTERNET_STATUS_COOKIE_HISTORY
    case INTERNET_STATUS_COOKIE_HISTORY:
        WININET_LOG0(pData, "callback: INTERNET_STATUS_COOKIE_HISTORY");
        break;
#endif
    default:
        WININET_LOG1(pData, "callback: dwInternetStatus %d is unknown", 
            dwInternetStatus);
    }
}

/* gsoap documentation:
    Called by client proxy multiple times, to close a socket connection before
    a new socket connection is established and at the end of communications 
    when the SOAP_IO_KEEPALIVE flag is not set and soap.keep_alive = 0 
    (indicating that the other party supports keep alive). Should return 
    SOAP_OK, or a gSOAP error code. Built-in gSOAP function: tcp_disconnect
 */
static int 
wininet_fclose(struct soap * soap)
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    soap->error = SOAP_OK;

    WININET_LOG0(pData, "fclose: setting disconnect to true");

    /* force a disconnect by setting the disconnect flag to TRUE */
    pData->bDisconnect = TRUE;
    wininet_have_connection(soap, pData);

    return SOAP_OK;
}

/* copy the private data structure */
static int  
wininet_copy(
    struct soap *           soap, 
    struct soap_plugin *    a_pDst, 
    struct soap_plugin *    a_pSrc
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    UNUSED_ARG(a_pDst);
    UNUSED_ARG(a_pSrc);

    WININET_LOG0(pData, "copy: not supported");
    _ASSERTE(!"wininet doesn't support copy");

    return SOAP_FATAL_ERROR;
}

/* deallocate of our private structure */
static void 
wininet_delete(
    struct soap *           soap, 
    struct soap_plugin *    a_pPluginData
    )
{
    struct wininet_data * pData = 
        (struct wininet_data *) a_pPluginData->data;

    UNUSED_ARG(soap);

    WININET_LOG0(pData, "delete: delete private data");

    /* force a disconnect of any existing connection */
    pData->bDisconnect = TRUE;
    wininet_have_connection(soap, pData);
    if (pData->hInternet) {
        InternetCloseHandle(pData->hInternet);
    }

    /* free our data */
    wininet_free_error_message(pData);
    if (pData->pBuffer) {
        free(pData->pBuffer);
    }
    if (pData->pUrlPath) {
        free(pData->pUrlPath);
    }
    if (pData->pUserAgent) {
        free(pData->pUserAgent);
    }
    if (pData->hLog) {
        fclose(pData->hLog);
    }
    free(pData);
}

/* gsoap documentation:
    Called from a client proxy to open a connection to a Web Service located 
    at endpoint. Input parameters host and port are micro-parsed from endpoint.
    Should return a valid file descriptor, or SOAP_INVALID_SOCKET and 
    soap->error set to an error code. Built-in gSOAP function: tcp_connect
*/
static SOAP_SOCKET  
wininet_fopen(
    struct soap *   soap, 
    const char *    a_pszEndpoint, 
    const char *    a_pszHost, 
    int             a_nPort
    )
{
    URL_COMPONENTSA urlComponents;
    char            szHost[MAX_PATH];
    char            szUrlPath[MAX_PATH];
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    soap->error = SOAP_OK;

    /* we parse the URL ourselves so we don't use these parameters */
    UNUSED_ARG(a_pszHost);
    UNUSED_ARG(a_nPort);

    WININET_LOG1(pData, "fopen: endpoint = '%s'", a_pszEndpoint);

    if (!pData->hInternet) {
        WININET_LOG0(pData, "fopen: not initialized");
        soap->error = SOAP_ERR;
        return SOAP_INVALID_SOCKET;
    }
    if (pData->hRequest) {
        WININET_LOG0(pData, "fopen: closing existing request handle");
        InternetCloseHandle(pData->hRequest);
        pData->hRequest = NULL;
    }
    if (pData->hConnection) {
        WININET_LOG0(pData, "fopen: closing existing connection handle");
        InternetCloseHandle(pData->hConnection);
        pData->hConnection = NULL;
    }

    /* parse out the url path */
    memset(&urlComponents, 0, sizeof(urlComponents));
    urlComponents.dwStructSize = sizeof(urlComponents);
    urlComponents.lpszHostName      = szHost;
    urlComponents.dwHostNameLength  = MAX_PATH;
    urlComponents.lpszUrlPath       = szUrlPath;
    urlComponents.dwUrlPathLength   = MAX_PATH;
    if (!InternetCrackUrlA(a_pszEndpoint, 0, 0, &urlComponents)) {
        soap->error = GetLastError();
        WININET_LOG2(pData, 
            "fopen: error %d (%s) in InternetCrackUrl", 
            soap->error, wininet_error_message(pData, soap->error));
        return SOAP_INVALID_SOCKET;
    }

    /* keep the URL path for when we create a request */
    if (pData->pUrlPath) {
        free(pData->pUrlPath);
    }
    pData->pUrlPath = strdup(szUrlPath);

    /* add or remove the HTTPS flag as necessary */
    if (urlComponents.nScheme == INTERNET_SCHEME_HTTPS) {
        pData->dwRequestFlags |= INTERNET_FLAG_SECURE;
    }
    else {
        pData->dwRequestFlags &= ~INTERNET_FLAG_SECURE;
    }

    /* update our timeouts to the latest value */
    wininet_set_timeout(pData, "connect", 
        INTERNET_OPTION_CONNECT_TIMEOUT, soap->connect_timeout);
    wininet_set_timeout(pData, "send",    
        INTERNET_OPTION_SEND_TIMEOUT, soap->send_timeout);
    wininet_set_timeout(pData, "recv", 
        INTERNET_OPTION_RECEIVE_TIMEOUT, soap->recv_timeout);

    /* connect to the target url, if we haven't connected yet 
       or if it was dropped */
    pData->hConnection = InternetConnectA(pData->hInternet, 
        szHost, urlComponents.nPort, "", "", INTERNET_SERVICE_HTTP, 
        0, (DWORD_PTR) soap);
    if (!pData->hConnection) {
        soap->error = GetLastError();
        WININET_LOG2(pData, "fopen: error %d (%s) in InternetConnect", 
            soap->error, wininet_error_message(pData, soap->error));
        return SOAP_INVALID_SOCKET;
    }

    /* return 0 as our "connected" socket type */
    WININET_LOG0(pData, "fopen: connected");
    return 0;
}

static int 
wininet_create_request(
    struct soap *   soap
    )
{
    DWORD dwFlags;
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    /*  Set up the flags for this connection */
    dwFlags = INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD;
    dwFlags |= pData->dwRequestFlags;
    if (soap->imode & SOAP_IO_KEEPALIVE || soap->omode & SOAP_IO_KEEPALIVE) {
        dwFlags |= INTERNET_FLAG_KEEP_CONNECTION;
    }

    if (pData->hLog) {
        char buf[1000] = { 0 };

#define LOGFLAG(flag) if (dwFlags & INTERNET_FLAG_ ## flag) strcat(buf, ", " #flag);
        LOGFLAG(RELOAD)
        LOGFLAG(RAW_DATA)
        LOGFLAG(EXISTING_CONNECT)
        LOGFLAG(ASYNC)
        LOGFLAG(PASSIVE)
        LOGFLAG(NO_CACHE_WRITE)
        LOGFLAG(MAKE_PERSISTENT)
        LOGFLAG(FROM_CACHE)
        LOGFLAG(SECURE)
        LOGFLAG(KEEP_CONNECTION)
        LOGFLAG(NO_AUTO_REDIRECT)
        LOGFLAG(READ_PREFETCH)
        LOGFLAG(NO_COOKIES)
        LOGFLAG(NO_AUTH)
        LOGFLAG(CACHE_IF_NET_FAIL)
        LOGFLAG(IGNORE_CERT_CN_INVALID)
        LOGFLAG(IGNORE_CERT_DATE_INVALID)
        LOGFLAG(IGNORE_REDIRECT_TO_HTTPS)
        LOGFLAG(IGNORE_REDIRECT_TO_HTTP)
        LOGFLAG(RESYNCHRONIZE)
        LOGFLAG(HYPERLINK)
        LOGFLAG(NO_UI)
        LOGFLAG(PRAGMA_NOCACHE)
        LOGFLAG(CACHE_ASYNC)
        LOGFLAG(FORMS_SUBMIT)
        LOGFLAG(NEED_FILE)
        LOGFLAG(RESTRICTED_ZONE)
        LOGFLAG(TRANSFER_BINARY)
        LOGFLAG(TRANSFER_ASCII)
        LOGFLAG(FWD_BACK)
        /*LOGFLAG(INTERNET_FLAG_BGUPDATE)*/
#undef LOGFLAG

        wininet_log(pData, "create_request: using INTERNET_FLAG_xxx = %s", &buf[2]);
    }

    /*  Note that although we specify HTTP/1.1 for the connection here, the 
        actual connection may be HTTP/1.0 depending on the settings in the 
        control panel. See the "Internet Options", "HTTP 1.1 settings".
     */
    pData->hRequest = HttpOpenRequestA(
        pData->hConnection, "POST", pData->pUrlPath, "HTTP/1.1", NULL, NULL, 
        dwFlags, (DWORD_PTR) soap);
    if (!pData->hRequest) {
        soap->error = GetLastError();
        WININET_LOG2(pData, "create_request: error %d (%s) in HttpOpenRequest", 
            soap->error, wininet_error_message(pData, soap->error));
        return SOAP_ERR;
    }

    WININET_LOG0(pData, "create_request: success");
    return SOAP_OK;
}

/* gsoap documentation:
    Used by clients to check if the server is still responsive. 
    Built-in gSOAP function: soap_poll.
*/
static int 
wininet_fpoll(
    struct soap * soap
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    WININET_LOG0(pData, "fpoll");

    /* ensure that our connection hasn't been disconnected */
    if (!wininet_have_connection(soap, pData)) {
        return SOAP_EOF;
    }

    return SOAP_OK; 
}

static int
wininet_growbuffer(
    struct wininet_data *   a_pData,
    const char *            a_pModule,
    size_t					a_nSize
    )
{
    a_pData->uiBufferSize = ROUND_UP(a_nSize, 4096); /* round up to 4096 boundary */
    WININET_LOG2(a_pData, "%s: growing internal buffer to %lu bytes", 
        a_pModule, a_pData->uiBufferSize);

    a_pData->pBuffer = (char *) realloc(a_pData->pBuffer, a_pData->uiBufferSize);
    if (!a_pData->pBuffer) {
        WININET_LOG1(a_pData, "%s: realloc failed", a_pModule);
        a_pData->uiBufferSize = 0;
        return SOAP_EOM;
    }

    return SOAP_OK;
}

/* gsoap documentation:
    Called by http_post and http_response (through the callbacks). Emits HTTP 
    key: val header entries. Should return SOAP_OK, or a gSOAP error code. 
    Built-in gSOAP function: http_post_header.
 */
static int 
wininet_fposthdr(
    struct soap *   soap, 
    const char *    a_pszKey, 
    const char *    a_pszValue
    )  
{
    int     rc;
    char    szHeader[4096];
    int     nLen;
    BOOL    bResult = FALSE;
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    soap->error = SOAP_OK;

    /* ensure that our connection hasn't been disconnected */
    if (!wininet_have_connection(soap, pData)) {
        return SOAP_EOF;
    }

    /* initialize a new request */
    if (a_pszKey && !a_pszValue) {
        WININET_LOG0(pData, "fposthdr: initialize request");

        /* if we are using chunk output then we start with a chunk size */
        pData->bIsChunkSize = ((soap->omode & SOAP_IO) == SOAP_IO_CHUNK);
        pData->uiBufferLen = 0;
        pData->uiBufferLenMax = INVALID_BUFFER_LENGTH;
        pData->nLogFormat = LOGTYPE_UNKNOWN;

        /* create new request for these headers */
        rc = wininet_create_request(soap);
        if (rc != SOAP_OK) return rc;

        return SOAP_OK;
    }

    /* completed request headers */
    if (!a_pszKey) {
        WININET_LOG0(pData, "fposthdr: complete headers");
        return SOAP_OK;
    }

    /* add a header */
    if (a_pszValue) { 
        char szTemp[200];

        if (!strcmp(a_pszKey, "User-Agent")) {
            if (pData->pUserAgent) {
                a_pszValue = pData->pUserAgent;
            }

            /* so that the request id shows up in IIS logs, add the request ID to the user agent */
            if (pData->hLog) {
                char szRequestId[50];
                wininet_getreqid(szRequestId, sizeof(szRequestId));
                nLen = _snprintf(szTemp, sizeof(szTemp), "%s [%s]", a_pszValue, szRequestId);
                if (nLen < 0) {
                    WININET_LOG0(pData, "fposthdr: EOM");
                    return SOAP_EOM;
                }
                a_pszValue = szTemp;
            }
        }
        
        WININET_LOG2(pData, "fposthdr: header '%s: %s'", a_pszKey, a_pszValue);

        /* determine the maximum length of this message so that we can
           correctly determine when we have completed the send */
        if (!strcmp(a_pszKey, "Content-Length")) {
            _ASSERTE(pData->uiBufferLenMax == INVALID_BUFFER_LENGTH);
            pData->uiBufferLenMax = strtoul(a_pszValue, NULL, 10);
            if (pData->uiBufferSize < pData->uiBufferLenMax) {
                int rc = wininet_growbuffer(pData, "fposthdr", pData->uiBufferLenMax);
                if (rc != SOAP_OK) return rc;
            }
        }

        /*! check to see what sort of data with have */
        else if (!strcmp(a_pszKey, "Content-Encoding")) {
            pData->nLogFormat = LOGTYPE_HEX;
        }

        else if (pData->nLogFormat == LOGTYPE_UNKNOWN && !strcmp(a_pszKey, "Content-Type")) {
            if (strstr(a_pszValue, "text/xml")) {
                pData->nLogFormat = LOGTYPE_XML;
            }
            else if (strstr(a_pszValue, "text/")) {
                pData->nLogFormat = LOGTYPE_TEXT;
            }
            else {
                pData->nLogFormat = LOGTYPE_HEX;
            }
        }

        nLen = _snprintf(szHeader, sizeof(szHeader), "%s: %s\r\n", a_pszKey, a_pszValue);
        if (nLen < 0) {
            WININET_LOG0(pData, "fposthdr: EOM");
            return SOAP_EOM;
        }

        _ASSERTE(pData->hRequest != NULL);
        bResult = HttpAddRequestHeadersA(pData->hRequest, szHeader, nLen, HTTP_ADDREQ_FLAG_ADD);
        if (!bResult) {
            /* not a critical error, so just log it */
            WININET_LOG2(pData, "fposthdr: error %d (%s) in HttpAddRequestHeaders", 
                soap->error, wininet_error_message(pData, GetLastError()));
        }
    }

    return SOAP_OK; 
}

static int
wininet_get_headers(
    struct wininet_data *   a_pData,
    const char *            a_pModule,
    DWORD                   a_dwFlags
    )
{
    DWORD dwLen;

    /* retrieve all of the response headers as NULL separated strings */
    _ASSERTE(a_pData->uiBufferSize > 0);
    dwLen = (DWORD) a_pData->uiBufferSize;
    a_pData->pBuffer[0] = 0;
    while (!HttpQueryInfoA(a_pData->hRequest, HTTP_QUERY_RAW_HEADERS | a_dwFlags, a_pData->pBuffer, &dwLen, 0) 
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        int rc = wininet_growbuffer(a_pData, a_pModule, dwLen);
        if (rc != SOAP_OK) return rc;
        dwLen = (DWORD) a_pData->uiBufferSize;
    }

    return SOAP_OK;
}

static void
wininet_log_headers(
    struct wininet_data *   a_pData,
    const char *            a_pModule
    )
{
    const char * pHeader;

    /* log all of the headers */
    for (pHeader = a_pData->pBuffer; *pHeader; pHeader += strlen(pHeader)+1) {
        WININET_LOG2(a_pData, "%s header '%s'", a_pModule, pHeader);

        /*! check to see what sort of data with have */
        if (!strnicmp(pHeader, "Content-Encoding:", 17)) {
            a_pData->nLogFormat = LOGTYPE_HEX;
        }

        else if (a_pData->nLogFormat == LOGTYPE_UNKNOWN && !strnicmp(pHeader, "Content-Type:", 13)) {
            if (strstr(pHeader, "text/xml")) {
                a_pData->nLogFormat = LOGTYPE_XML;
            }
            else if (strstr(pHeader, "text/")) {
                a_pData->nLogFormat = LOGTYPE_TEXT;
            }
            else {
                a_pData->nLogFormat = LOGTYPE_HEX;
            }
        }
    }
}

/* gsoap documentation:
    Called for all send operations to emit contents of s of length n. 
    Should return SOAP_OK, or a gSOAP error code. Built-in gSOAP 
    function: fsend

   Notes:
    I do a heap of buffering here because we need the entire message available
    in a single buffer in order to iterate through the sending loop. I had 
    hoped that the SOAP_IO_STORE flag would have worked to do the same, however
    this still breaks the messages up into blocks. Although there were a number
    of ways this could've been implemented, this works and supports all of the
    possible SOAP_IO flags, even though the entire message is still buffered 
    the same as if SOAP_IO_STORE was used.
*/
static int 
wininet_fsend(
    struct soap *   soap, 
    const char *    a_pBuffer, 
    size_t          a_uiBufferLen
    )
{
    BOOL        bResult;
    BOOL        bRetryPost;
    DWORD       dwStatusCode;
    DWORD       dwStatusCodeLen;
    wininet_rseReturn errorResolved;
    int         nResult = SOAP_OK;
    int         nAttempt = 1;
    size_t      nSendSize = 0;
    char *      pSendBuf = NULL;
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    _ASSERTE(pData->hRequest != NULL);

    WININET_LOG1(pData, "fsend: data len = %lu bytes", a_uiBufferLen);

    /* ensure that our connection hasn't been disconnected */
    soap->error = SOAP_OK;
    if (!wininet_have_connection(soap, pData)) {
        return SOAP_EOF;
    }

    /*  on the first time, if we don't know the size of the buffer, then we are either using
        chunked send, or we need to slowly allocate the buffer as it comes.
    */
    if (pData->uiBufferLen == 0 && pData->uiBufferLenMax == INVALID_BUFFER_LENGTH) {
        /*  If we are using chunked sending, then we don't know how big the
            buffer will need to be. So we start with a 0 length buffer and
            grow it later to ensure that it is always large enough.
         */
        if ((soap->mode & SOAP_IO) == SOAP_IO_CHUNK) {
            /* ensure we have a buffer large enough for this chunksize 
               buffer, plus the next chunk of actual data, and a few extra 
               bytes for the final "0" chunksize block. */
            size_t uiChunkSize = strtoul(a_pBuffer, NULL, 16);
            _ASSERTE(a_uiBufferLen > 0);
            pData->uiBufferLenMax = uiChunkSize + a_uiBufferLen + 16;
        }

        /* empty bodies will set the buffer length to 0, permit this */
        if (a_uiBufferLen == 0) {
            pData->uiBufferLenMax = 0;
        }
    }

    /*  If the currently supplied buffer from gsoap holds the entire message then just use 
        that buffer as is. This will only be true when (1) not using chunked send (so 
        uiBufferLenMax has been previously set to the Content-Length header length), and 
        (2) gsoap is sending the entire message at one time. 
     */
    if (a_uiBufferLen == pData->uiBufferLenMax) {
        WININET_LOG0(pData, "fsend: using gsoap supplied data buffer");
        nSendSize = a_uiBufferLen;
        pSendBuf  = (char *) a_pBuffer;
    }

    /*  If not using the gsoap buffer, then ensure that the current allocation
        is large enough for the entire message. This is because authentication 
        may require the entire message to be sent multiple times. Since this 
        send is only a part of the message, we need to buffer until we have the 
        entire message.
    */
    if (!pSendBuf) {
        size_t uiNewBufferLen = pData->uiBufferLen + a_uiBufferLen;
        if (!pData->pBuffer || uiNewBufferLen > pData->uiBufferSize) {
            int rc = wininet_growbuffer(pData, "fsend", uiNewBufferLen);
            if (rc != SOAP_OK) return rc;
        }

        memcpy(pData->pBuffer + pData->uiBufferLen, a_pBuffer, a_uiBufferLen);
        pData->uiBufferLen = uiNewBufferLen;

        /*  when doing chunked transfers, and this is a chunk size block, and it is 
            "0", then it is the last block. set the maximum size to continue to the 
            actual send. */
        if ((soap->mode & SOAP_IO) == SOAP_IO_CHUNK
             && pData->bIsChunkSize 
             && a_pBuffer[2] == '0' && !isalnum(a_pBuffer[3]))
        {
            pData->uiBufferLenMax = pData->uiBufferLen;
        }

        /* continue if the entire message is not complete */
        if (pData->uiBufferLen < pData->uiBufferLenMax) {
            pData->bIsChunkSize = !pData->bIsChunkSize
                && ((soap->mode & SOAP_IO) == SOAP_IO_CHUNK);
            return SOAP_OK;
        }

        /* message is complete, set the sending buffer */
        nSendSize = pData->uiBufferLen;
        pSendBuf  = pData->pBuffer;

        WININET_LOG3(pData, "fsend: using %d%% (%lu bytes) of %lu byte internal buffer", 
            (int)((pData->uiBufferLen * 100) / pData->uiBufferSize), 
            pData->uiBufferLen, pData->uiBufferSize);
    }

    /* output the data we are sending */
    WININET_LOG1(pData, "fsend: sending message %lu bytes", nSendSize);
    if (pData->hLog && nSendSize > 0) {
        wininet_log_data(pData, "fsend: message data", pSendBuf, nSendSize);
    }

    /* we've now got the entire message, now we can enter our sending loop */
    bRetryPost = TRUE;
    while (bRetryPost) {
        bRetryPost = FALSE;

        WININET_LOG1(pData, "fsend: sending message, attempt %d", nAttempt++);
        bResult = HttpSendRequestA(
            pData->hRequest, NULL, 0, pSendBuf, (DWORD)nSendSize);
        if (!bResult) {
            soap->error = GetLastError();
            WININET_LOG2(pData, "fsend: error %d (%s) in HttpSendRequest", 
                soap->error, wininet_error_message(pData, soap->error));

            /* see if we can handle this error, see the MSDN documentation
               for InternetErrorDlg for details */
            switch (soap->error) {
            case ERROR_INTERNET_HTTP_TO_HTTPS_ON_REDIR:
            case ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR:
            case ERROR_INTERNET_INCORRECT_PASSWORD:
            case ERROR_INTERNET_INVALID_CA:
            case ERROR_INTERNET_POST_IS_NON_SECURE:
            case ERROR_INTERNET_SEC_CERT_CN_INVALID:
            case ERROR_INTERNET_SEC_CERT_DATE_INVALID:
            case ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED:
                errorResolved = rseDisplayDlg;
                if (pData->pRseCallback) {
                    WININET_LOG0(pData, "fsend: calling client supplied error callback");
                    errorResolved = pData->pRseCallback(pData->hRequest, soap->error);
                }
                if (errorResolved == rseDisplayDlg) {
                    errorResolved = wininet_resolve_send_error(pData, soap->error);
                    if (errorResolved == rseTrue) {
                        WININET_LOG1(pData, "fsend: error %d has been resolved (retrying)", soap->error);

                        /*  we may have been disconnected by the error. Since we 
                            are going to try again, we will automatically be 
                            reconnected. Therefore we want to disregard any previous
                            disconnection messages. 
                            */
                        pData->bDisconnect = FALSE; 
                        bRetryPost = TRUE;
                        nResult = SOAP_OK;
                        continue;
                    }
                }
                break;
            }

            /* if the error wasn't handled then we exit */
            nResult = SOAP_HTTP_ERROR;
            break;
        }

        /* get the status code from the response to determine if we need 
           to authorize */
        dwStatusCodeLen = sizeof(dwStatusCode);
        bResult = HttpQueryInfo(
            pData->hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, 
            &dwStatusCode, &dwStatusCodeLen, NULL);
        if (!bResult) {
            soap->error = GetLastError();
            WININET_LOG2(pData, "fsend: error %d (%s) in HttpQueryInfo", 
                soap->error, wininet_error_message(pData, soap->error));
            nResult = SOAP_HTTP_ERROR;
            break;
        }

        WININET_LOG1(pData, "fsend: HTTP status code = %lu", dwStatusCode);

        /*  if we need authentication, then request the user for the 
            appropriate data. Their reply is saved into the request so 
            that we can use it later.
         */
        switch (dwStatusCode) {
        case HTTP_STATUS_DENIED:            /* 401 */
        case HTTP_STATUS_PROXY_AUTH_REQ:    /* 407 */
            errorResolved = rseDisplayDlg;
            WININET_LOG0(pData, "fsend: user authentication required");
            if (pData->pRseCallback) {
                WININET_LOG0(pData, "fsend: calling client supplied error callback");
                errorResolved = pData->pRseCallback(pData->hRequest, dwStatusCode);
            }
            if (errorResolved == rseDisplayDlg) {
                errorResolved = wininet_resolve_send_error(pData, ERROR_INTERNET_INCORRECT_PASSWORD);
            }
            if (errorResolved == rseTrue) {
                WININET_LOG0(pData, "fsend: authentication has been provided (retrying)");

                /*  we may have been disconnected by the error. Since we 
                    are going to try again, we will automatically be 
                    reconnected. Therefore we want to disregard any previous
                    disconnection messages. 
                    */
                pData->bDisconnect = FALSE; 
                bRetryPost = TRUE;
                nResult = SOAP_OK;
                continue;
            }
            break;
        }
    }

    /* log the actual headers used */
    if (pData->hLog) {
        int rc = wininet_get_headers(pData, "fsend: actual", HTTP_QUERY_FLAG_REQUEST_HEADERS);
        if (rc != SOAP_OK) return rc;
        wininet_log_headers(pData, "fsend: actual");
        WININET_LOG0(pData, "fsend: complete");
    }

    /* signal to frecv that nothing has been received yet */
    pData->uiBufferLenMax = INVALID_BUFFER_LENGTH;

    return nResult; 
}

/* gsoap documentation:
    Called for all receive operations to fill buffer s of maximum length n. 
    Should return the number of bytes read or 0 in case of an error, e.g. EOF.
    Built-in gSOAP function: frecv
 */
static size_t 
wininet_frecv(
    struct soap *   soap, 
    char *          a_pBuffer, 
    size_t          a_uiBufferLen
    ) 
{ 
    DWORD   dwBytesRead;
    size_t  uiTotalBytesRead;
    BOOL    bResult;
    int     rc;
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    soap->error = SOAP_OK;

    /*  NOTE: we do not check here that our connection hasn't been 
        disconnected because in HTTP/1.0 connections, it will always have been
        disconnected by now. This is because the response is checked by the 
        wininet_fsend function to ensure that we didn't need any special 
        authentication. At that time the connection would have been 
        disconnected. This is okay however as we can still read the response
        from the request handle.
     */

    if (!pData->hRequest) {
		soap->error = SOAP_ERR;
        return 0;
    }
    WININET_LOG1(pData, "frecv: available buffer len = %lu", a_uiBufferLen);

    uiTotalBytesRead = 0;
    if (pData->uiBufferLenMax == INVALID_BUFFER_LENGTH) {
        const char *pHeader, *pHeaderNext = NULL;

        pData->nLogFormat = LOGTYPE_UNKNOWN;
        pData->uiBufferLenMax = 0; 

        /* retrieve all of the response headers */
        rc = wininet_get_headers(pData, "frecv:", 0);
		if (rc != SOAP_OK) {
			soap->error = rc;
			return 0;
		}
        if (pData->hLog) {
            wininet_log_headers(pData, "frecv:");
        }

        /* size required for end of headers CRLF */
        if (a_uiBufferLen < 2) {
            WININET_LOG0(pData, "frecv: buffer too small for headers");
			soap->error = SOAP_EOM;
			return 0;
        }

        /* pass through some headers to gsoap */
        for (pHeader = pData->pBuffer; *pHeader; pHeader = pHeaderNext) {
            pHeaderNext = pHeader + strlen(pHeader)+1;

            /*  don't pass through headers related to transport or authentication 
                as this has been handled already by the wininet layer. */
            if (0 == strnicmp(pHeader, "Transfer-Encoding:", 18) ||
                0 == strnicmp(pHeader, "WWW-Authenticate:", 17))
            {
                continue;
            }

            dwBytesRead = (DWORD) (pHeaderNext - pHeader - 1);
            if (uiTotalBytesRead + dwBytesRead + 4 > a_uiBufferLen) {
                WININET_LOG0(pData, "frecv: buffer too small for headers");
				soap->error = SOAP_EOM;
				return 0;
            }

            memcpy(&a_pBuffer[uiTotalBytesRead], pHeader, dwBytesRead);
            uiTotalBytesRead += dwBytesRead;
            a_pBuffer[uiTotalBytesRead++] = '\r'; 
            a_pBuffer[uiTotalBytesRead++] = '\n';
        }

        /* terminate the headers */
        a_pBuffer[uiTotalBytesRead++] = '\r'; 
        a_pBuffer[uiTotalBytesRead++] = '\n';
    }

    do {
        /* read from the connection up to our maximum amount of data */
        _ASSERTE(a_uiBufferLen <= ULONG_MAX);
        bResult = InternetReadFile(
            pData->hRequest, 
            &a_pBuffer[uiTotalBytesRead], 
            (DWORD) a_uiBufferLen - uiTotalBytesRead, 
            &dwBytesRead);
        if (bResult) {
            uiTotalBytesRead += dwBytesRead;
        }
        else {
            soap->error = GetLastError();
            WININET_LOG2(pData, "frecv: error %d (%s) in InternetReadFile", 
                soap->error, wininet_error_message(pData, soap->error));
        }
    } 
    while (bResult && dwBytesRead && uiTotalBytesRead < a_uiBufferLen);

    WININET_LOG1(pData, "frecv: received %lu bytes", uiTotalBytesRead);

    /* output the data we received */
    if (pData->hLog && uiTotalBytesRead > 0) {
        wininet_log_data(pData, "frecv: message data", a_pBuffer, uiTotalBytesRead);
    }

    return uiTotalBytesRead;
} 

static int 
wininet_setlog_internal(
    struct wininet_data *   a_pData,
    const char *            a_pLogFile
    )
{
    if (!a_pData) {
        return SOAP_ERR;
    }

    if (a_pData->hLog) {
        fclose(a_pData->hLog);
        a_pData->hLog = NULL;
    }

    if (a_pLogFile && *a_pLogFile) {
        a_pData->hLog = fopen(a_pLogFile, "a");
        if (!a_pData->hLog) return SOAP_ERR;
        fputs("----------------------------------------------------------------------\n", a_pData->hLog);
    }

    return SOAP_OK;
}

/*=============================================================================
  API Functions
 ============================================================================*/

/* register and set the logfile */
int 
wininet_register(
    struct soap *           soap, 
    struct soap_plugin *    a_pPluginData, 
    void *                  a_pLogFile
    )
{
    struct wininet_data * pData;
    int rc;

    /* can't use WININET_LOG0 as we don't exist yet */
    DBGLOG(TEST, SOAP_MESSAGE(fdebug, "wininet %p: registration\n", soap));

    pData = (struct wininet_data *) malloc(sizeof(struct wininet_data));
    if (!pData) return SOAP_EOM;
    memset(pData, 0, sizeof(struct wininet_data));
    pData->nLogFormat = LOGTYPE_UNKNOWN;

    rc = wininet_setlog_internal(pData, (const char *) a_pLogFile);
    if (rc != SOAP_OK) {
        free(pData);
        return rc;
    }
    
    WININET_LOG0(pData, "init");

    if ((soap->omode & SOAP_IO) == SOAP_IO_STORE) {
        WININET_LOG0(pData, "use of SOAP_IO_STORE is not recommended");
    }

    /* guarantee that we have an internal buffer */
    rc = wininet_growbuffer(pData, "init", 1);
    if (rc != SOAP_OK) {
        wininet_delete(soap, a_pPluginData);
        return rc;
    }

    /* start our internet session using the standard IE proxy config */
    pData->hInternet = InternetOpenA("gsoap/" WININET_VERSION, 
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!pData->hInternet) {
        soap->error = GetLastError();
        WININET_LOG2(pData, "init: error %d (%s) in InternetOpen", 
            soap->error, wininet_error_message(pData, soap->error));
        wininet_delete(soap, a_pPluginData);
        return FALSE;
    }

    /* set up the callback function so we get notifications */
    InternetSetStatusCallbackA(pData->hInternet, wininet_callback);

    /* set all of our callbacks */
    soap->fopen    = wininet_fopen;
    soap->fpoll    = wininet_fpoll;
    soap->fposthdr = wininet_fposthdr;
    soap->fsend    = wininet_fsend;
    soap->frecv    = wininet_frecv;
    soap->fclose   = wininet_fclose;

    a_pPluginData->id        = wininet_id;
    a_pPluginData->fcopy     = wininet_copy;
    a_pPluginData->fdelete   = wininet_delete;
    a_pPluginData->data      = pData;

    return SOAP_OK;
}

/* start or stop logging */
int 
wininet_setlog(
    struct soap *   soap,
    const char *    a_pLogFile
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    if (!pData) return SOAP_ERR;
    return wininet_setlog_internal(pData, a_pLogFile);
}

/* set the flags */
int 
wininet_setflags(
    struct soap *   soap, 
    DWORD           a_dwRequestFlags
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);
    
    if (!pData) return SOAP_ERR;
    WININET_LOG1(pData, "wininet_setflags: new flags = %lu", a_dwRequestFlags);
    pData->dwRequestFlags = a_dwRequestFlags;
    return SOAP_OK;
}

 /* set the optional user-agent string */
extern int 
wininet_setagent(
    struct soap *   soap,
    const char *    a_pUserAgent
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    if (!pData) return SOAP_ERR;
    
    if (pData->pUserAgent) {
        free(pData->pUserAgent);
        pData->pUserAgent = NULL;
    }

    if (a_pUserAgent && *a_pUserAgent) {
        WININET_LOG1(pData, "setagent: User-Agent = '%s'", a_pUserAgent);
        pData->pUserAgent = strdup(a_pUserAgent);
    }
    else {
        WININET_LOG0(pData, "setagent: User-Agent = (use default)");
    }

    return SOAP_OK;
}

/* set the optional callback */
extern int 
wininet_set_rse_callback(
    struct soap *           soap,
    wininet_rse_callback    a_pRsecallback
    )
{
    struct wininet_data * pData = (struct wininet_data *) 
        soap_lookup_plugin(soap, wininet_id);

    if (!pData) return SOAP_ERR;
    WININET_LOG1(pData, "set_rse_callback: callback = '%p'", a_pRsecallback);
    pData->pRseCallback = a_pRsecallback;
    return SOAP_OK;
}
