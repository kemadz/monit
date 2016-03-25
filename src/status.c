/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "monit.h"
#include "net.h"
#include "socket.h"
#include "process.h"
#include "device.h"

// libmonit
#include "exceptions/AssertException.h"

/**
 *  Obtain status from the monit daemon
 *
 *  @file
 */


/* ------------------------------------------------------------------ Public */


/**
 * Show all services in the service list.
 */
boolean_t status(const char *level, const char *group, const char *service) {
        boolean_t status = false;
        if (! exist_daemon()) {
                LogError("Status not available -- the monit daemon is not running\n");
                return status;
        }
        Socket_T S = NULL;
        if (Run.httpd.flags & Httpd_Net) {
                // FIXME: Monit HTTP support IPv4 only currently ... when IPv6 is implemented change the family to Socket_Ip
                SslOptions_T options = {
                        .flags = (Run.httpd.flags & Httpd_Ssl) ? SSL_Enabled : SSL_Disabled,
                        .clientpemfile = Run.httpd.socket.net.ssl.clientpem,
                        .allowSelfSigned = Run.httpd.flags & Httpd_AllowSelfSignedCertificates
                };
                S = Socket_create(Run.httpd.socket.net.address ? Run.httpd.socket.net.address : "localhost", Run.httpd.socket.net.port, Socket_Tcp, Socket_Ip4, options, Run.limits.networkTimeout);
        } else if (Run.httpd.flags & Httpd_Unix) {
                S = Socket_createUnix(Run.httpd.socket.unix.path, Socket_Tcp, Run.limits.networkTimeout);
        } else {
                LogError("Status not available - monit http interface is not enabled, please add the 'set httpd' statement\n");
        }
        if (S) {
                Socket_print(S, "GET /_status?format=text&level=%s", level);
                if (group) {
                        char *_group = Util_urlEncode((char *)group);
                        Socket_print(S, "&group=%s", _group);
                        FREE(_group);
                }
                if (service) {
                        char *_service = Util_urlEncode((char *)service);
                        Socket_print(S, "&service=%s", _service);
                        FREE(_service);
                }
                char *_auth = Util_getBasicAuthHeaderMonit();
                Socket_print(S, " HTTP/1.0\r\n%s\r\n", _auth ? _auth : "");
                FREE(_auth);
                char buf[1024];
                TRY
                {
                        Util_parseMonitHttpResponse(S);
                        while (Socket_readLine(S, buf, sizeof(buf)))
                                printf("%s", buf);
                        status = true;
                }
                ELSE
                {
                        LogError("%s\n", Exception_frame.message);
                }
                END_TRY;
                Socket_free(&S);
        }
        return status;
}

