/*
===============================================================================
GSOAP WININET 2.1 PLUGIN
-------------------------------------------------------------------------------

Allow gsoap clients (not servers) to direct all communications through the 
WinInet API. This automatically provides all of the proxy and authentication 
features supported by the control panel 'Internet Options' dialog to the 
client. As these options are shared by IE, this means that "if IE works, 
gsoap works."

Project Home: http://code.google.com/p/gsoapwininet/

-------------------------------------------------------------------------------
Features
-------------------------------------------------------------------------------

 + gsoap plugin - extremely easy to use
 + complete support for:
     - HTTP/1.0 and HTTP/1.1
     - HTTPS (no extra libraries are required)
     - HTTP authentication
     - Proxy servers (simple, automatic discovery, etc)
     - Proxy authentication (basic, NTLM, etc)
 + authentication prompts and HTTPS warnings (e.g. invalid HTTPS CA) 
     can be resolved by the user via standard system dialog boxes.
 + message size is limited only by available memory
 + connect, receive and send timeouts are used 
 + supports all SOAP_IO types (see limitations)
 + written completely in C, can be used in C, C++, and MFC projects
     without modification (anywhere that gsoap is used)
 + can be used in both MBCS and UNICODE projects
 + compiles cleanly at warning level 4 (if gsoap uses SOAP_SOCKET
     for the definition of sockets instead of int, it will also
     compile without win64 warnings).
 + all debug trace goes to the gsoap TEST.log file 
 + supports multiple threads (all plugin data is stored in the 
     soap structure - no static variables)

-------------------------------------------------------------------------------
Limitations
-------------------------------------------------------------------------------
 - may internally buffer the entire outgoing message before sending
     (if the serialized message is larger then SOAP_BUFLEN, or if 
     SOAP_IO_CHUNK mode is being used then the entire message will 
     be buffered)

-------------------------------------------------------------------------------
Usage
-------------------------------------------------------------------------------

Add the gsoapWinInet2.h and gsoapWinInet2.cpp files to your project (if you 
have a C project, rename gsoapWinInet2.cpp to .c and use it as is). Ensure 
that you turn off precompiled headers for the .cpp file.

In your source, just after calling soap_init(), register this plugin with 
soap_register_plugin( soap, wininet_register ). 

For example:
     struct soap soap;
     soap_init( &soap );
     soap_register_plugin( &soap, wininet_register );
     soap.connect_timeout = 5; // this will be used by wininet too
     ...
     soap_done(&soap);

-------------------------------------------------------------------------------
Creating a logfile
-------------------------------------------------------------------------------

A logfile may be created at plugin registration by passing the full path to 
the logfile as the argument. The logfile will contain details of all calls, 
callbacks, data and status updates received by the plugin.

For example:
     struct soap soap;
     soap_init( &soap );
     soap_register_plugin_arg( &soap, wininet_register,
         "c:\\Temp\\wininet.log" );

The logfile can be set to NULL or an empty string to disable logging.

Alternatively, the logfile can be set or changed after registration using the
wininet_setlog() function. Since some settings may have already been set by 
the time wininet_setlog is called, it is recommended that the logfile is 
created at registration.

Note that if the log file cannot be created then no error is generated. In 
particular, if the gsoap plugin is running in the context of IE protected mode
then note that only some directories are writable (FOLDERID_LocalAppDataLow).

-------------------------------------------------------------------------------
Adding extra flags
-------------------------------------------------------------------------------

Extra flags can be passed to HttpOpenRequest by calling the wininet_setflags 
function after the plugin is registered. 

For example:
     struct soap soap;
     soap_init( &soap );
     soap_register_plugin( &soap, wininet_register );
     wininet_setflags( &soap, INTERNET_FLAG_IGNORE_CERT_CN_INVALID );

See the MSDN documentation on HttpOpenRequest for details of available flags. 
The <wininet.h> header file is required for the definitions of the flags. 
Some flags which may be useful are:

INTERNET_FLAG_KEEP_CONNECTION

     Uses keep-alive semantics, if available, for the connection. 
     This flag is required for Microsoft Network (MSN), NT LAN 
     Manager (NTLM), and other types of authentication. 

     ++ Note that this flag is used automatically when soap.omode 
     has the SOAP_IO_KEEPALIVE flag set. ++

INTERNET_FLAG_IGNORE_CERT_CN_INVALID

     Disables Microsoft Win32 Internet function checking of SSL/PCT-
     based certificates that are returned from the server against 
     the host name given in the request. 

INTERNET_FLAG_IGNORE_CERT_DATE_INVALID

     Disables Win32 Internet function checking of SSL/PCT-based 
     certificates for proper validity dates.

-------------------------------------------------------------------------------
Using other plugins
-------------------------------------------------------------------------------

This plugin uses the following callback functions and is most likely not going
to be compatible with any other plugin that uses these functions.

     soap->fopen
     soap->fpoll
     soap->fposthdr
     soap->fsend
     soap->frecv
     soap->fclose

The only function that may be modified is the soap->fposthdr function. To 
override this function, replace it with the customized version after the 
wininet plugin has been registered, and make sure that you call the original
soap->fposthdr function from your own.

For example:

     struct soap soap;
     soap_init( &soap );
     soap_register_plugin( &soap, wininet_register );
     orig_fposthdr = soap.fposthdr;
     soap.fposthdr = my_fposthdr;

    int my_fposthdr(struct soap *soap, const char *key, const char *val) {
        // my processing
        return orig_fposthdr(soap, key, val);
    }

-------------------------------------------------------------------------------
Memory Usage
-------------------------------------------------------------------------------

All messages are buffered internally to gsoapWinInet2 plugin it is 
recommended that the SOAP_IO_STORE flag is not used otherwise the message will
be buffered twice on every send. Use the default flag SOAP_IO_BUFFER, 
or SOAP_IO_FLUSH.

-------------------------------------------------------------------------------
Error Handling
-------------------------------------------------------------------------------

If there are errors in sending the HTTP request which would cause a dialog 
box to be displayed in IE (for instance, invalid certificates on an HTTPS 
connection), then by default, a dialog will also be displayed by this library. 

To disable the dialog, supply your own custom error resolving callback using 
wininet_set_rse_callback() function. It is possible to disable some warning
dialogs by setting the appropriate flags with wininet_setflags().

-------------------------------------------------------------------------------
License 
-------------------------------------------------------------------------------

The licence text below is the boilerplate "MIT Licence" used from:
http://www.opensource.org/licenses/mit-license.php

Copyright (c) 2009-2011, Brodie Thiesfield

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

-------------------------------------------------------------------------------
Developers
-------------------------------------------------------------------------------

26 May 2003: Jack Kustanowitz (jackk@atomica.com)
    Original version

29 September 2003: Brodie Thiesfield (code@jellycan.com)
    Rewritten as C plugin for gsoap. Bugs fixed and features added.

14 January 2004: Brodie Thiesfield (code@jellycan.com)
    Bug fix.

17 March 2009: Brodie Thiesfield (code@jellycan.com)
    Clean up and re-release.

10 February 2011: Brodie Thiesfield (code@jellycan.com)
    Rewrite, fix some bugs, clean up the code, add full (optional) logging 
    even in release builds, ensure that no-cache flags are used.
*/

#ifndef INCLUDED_gsoapWinInet2_h
#define INCLUDED_gsoapWinInet2_h

#include <wininet.h>
#include <stdsoap2.h>

#ifdef __cplusplus
extern "C" {
#endif 

/*! register the plugin and optionally set a logfile.
    NOTE: flags cannot be set using the registration function now. Use the 
    wininet_setflags() function! 
 */
extern int wininet_register(struct soap *a_pSoap, struct soap_plugin *a_pPluginData, void *a_pLogFile);

/*! set or cancel the logfile after plugin registration. Set to NULL or empty string
    to disable logging. */
extern int wininet_setlog(struct soap * soap, const char * a_pLogFile);

/*! set the extra flags after plugin registration. Set to 0 for default flags. */
extern int wininet_setflags(struct soap * soap, DWORD a_dwRequestFlags);

/*! set or cancel the string to send as the User-Agent header. Set to NULL to use the
    default supplied. */
extern int wininet_setagent(struct soap * soap, const char * a_pUserAgent);

/*! possible results from the RSE callback */
typedef enum {
    rseFalse = 0,   /*!< failed to resolve the error */
    rseTrue,        /*!< error has been resolved, retry the request */
    rseDisplayDlg   /*!< display the standard Windows dialog */
} wininet_rseReturn;

/*! RSE callback signature */
typedef wininet_rseReturn (*wininet_rse_callback)(HINTERNET a_hHttpRequest, DWORD a_dwErrorCode);

/*! set the ResolveSendError callback to the used. This can be used to resolve errors manually
    without using the built-in Windows dialogs. */
extern int wininet_set_rse_callback(struct soap *a_pSoap, wininet_rse_callback a_pRseCallback);

#ifdef __cplusplus
}
#endif 

#endif // INCLUDED_gsoapWinInet2_h
