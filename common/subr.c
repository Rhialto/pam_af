/*-
 * Copyright (c) 2004-2005 Stanislav Sedov
 * Copyright (c) 2005 MBSD labs
 * Copyright (c) 2005 by 310.ru [Tridesyatoe], Moscow, Russian Federation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: subr.c,v 1.3 2005/08/15 23:46:41 stas Exp $
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <paths.h>
#include <assert.h>
#include <err.h>
#include <ndbm.h>
#include <netdb.h>
#include <sysexits.h>

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "pam_af.h"
#include "subr.h"

const char *stdb = STATDB;
const char *cfgdb = CFGDB;

#define IPV4_ADDR(sockaddr) \
    ((char *)&(((struct sockaddr_in *)sockaddr)->sin_addr.s_addr))
#define IPV6_ADDR(sockaddr) \
    ((char *)((struct sockaddr_in6 *)sockaddr)->sin6_addr.s6_addr)
#define IPV4SZ sizeof(struct in_addr)
#define IPV6SZ sizeof(struct in6_addr)

void
my_freeaddrinfo(mai0)
	myaddrinfo_t	*mai0;
{
	myaddrinfo_t *mai, *mai1;

	for(mai = mai0; mai; mai = mai1) {
		mai1 = mai->next;
		free(mai->addr);
		free(mai);
	}
}

const char *
my_gai_strerror(err)
	int err;
{
	return(gai_strerror(err));
}

int
my_getaddrinfo(host, family, pmai)
	char	*host;
	int	family;
	myaddrinfo_t	**pmai;
{
	struct addrinfo hints, *res, *res0;
	myaddrinfo_t *mai, **last;
	int ret;
	
	if (strncmp(host, DEFRULE, strlen(DEFRULE)) == 0) {
		*pmai = (myaddrinfo_t *)malloc(sizeof(myaddrinfo_t));
		if (*pmai == NULL)
			return EAI_MEMORY;
		mai = *pmai;
		mai->next = NULL;
		mai->addr = (char *)malloc(strlen(DEFRULE));
		if (mai->addr == NULL)
			return EAI_MEMORY;
		bcopy(DEFRULE, mai->addr, strlen(DEFRULE));
		mai->addrlen = strlen(DEFRULE);
		return 0;
	}

	bzero(&hints, sizeof(hints));
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_family = family;

        if ((ret = getaddrinfo(host, NULL, &hints, &res0)) != 0)
		return ret;

        for (res = res0, last = pmai; res; res = res->ai_next) {
		*last = (myaddrinfo_t *)malloc(sizeof(myaddrinfo_t));
		mai = *last;
		if (mai == NULL) {
			my_freeaddrinfo(*pmai);
			return EAI_MEMORY;
		}
		mai->next = NULL;

                switch (res->ai_family) {
                case PF_INET:
				mai->addr = (char *)malloc(IPV4SZ);
				if (mai->addr == NULL) {
					my_freeaddrinfo(*pmai);
					return EAI_MEMORY;
				}
                                bcopy(IPV4_ADDR(res->ai_addr), mai->addr, \
				    IPV4SZ);
                                mai->addrlen = IPV4SZ;
                                break;

                case PF_INET6:
				mai->addr = (char *)malloc(IPV6SZ);
				if (mai->addr == NULL) {
					my_freeaddrinfo(*pmai);
					return EAI_MEMORY;
				}
                                bcopy(IPV6_ADDR(res->ai_addr), mai->addr, \
				    IPV6SZ);
                                mai->addrlen = IPV6SZ;
                                break;

                default:
				mai->addr = (char *)malloc(res->ai_addrlen);
				if (mai->addr == NULL) {
					my_freeaddrinfo(*pmai);
					return EAI_MEMORY;
				}
                                bcopy(res->ai_addr, mai->addr, \
				    res->ai_addrlen);
                                mai->addrlen = res->ai_addrlen;
                                break;
                }
		last = &mai->next;
	}

	freeaddrinfo(res0);

	return 0;
}
 
hostrule_t *
find_host_rule(dbp, host)
	DBM	*dbp;
	char	*host;
{
	datum			key, data;
	struct			myaddrinfo *res0, *res;
	static hostrule_t	hstent;
	char			buf[1024];
	int			found = 0;
	int			mask;
	int			ret;
	char			defrule[] = DEFRULE;

	if ((ret = my_getaddrinfo(host, PF_UNSPEC, &res0)) != 0)
		errx(EX_DATAERR, "can't resolve hostname %s: %s", \
		    host, my_gai_strerror(ret));

	for (res = res0; res && !found; res = res->next) {
		ret = my_getnameinfo(res->addr, res->addrlen, buf, sizeof(buf));
		if (ret != 0)
			err(EX_OSERR, "can't get numeric address");

		for (key = dbm_firstkey(dbp); key.dptr; key = dbm_nextkey(dbp))
		{
			if ((unsigned)key.dsize != res->addrlen)
				continue;

			data = dbm_fetch(dbp, key);
			if (data.dsize != sizeof(hstent))
				errx(EX_DATAERR, "database seriously broken");
			mask = ((hostrule_t *)data.dptr)->mask;
			if (mask == 0)
				mask = res->addrlen * 8;
			if (addr_cmp(key.dptr, res->addr, res->addrlen, mask) \
			    == 0) {
				found = 1;
				break;
			}
		}
	}
	if (found == 0) {
		key.dptr = defrule;
		key.dsize = strlen(DEFRULE) + 1;
		data = dbm_fetch(dbp, key);
	}

	if (data.dptr != NULL) {
		if (data.dsize != sizeof(hstent))
			errx(EX_DATAERR, "database seriously broken");
			
		bcopy(data.dptr, &hstent, sizeof(hstent));
	}
	else {
		hstent.mask = 0;
		hstent.attempts = DEFAULT_ATTEMPTS;
		hstent.locktime = DEFAULT_LOCKTIME;
		*hstent.lock_cmd = 0;
		*hstent.unlock_cmd = 0;
	}

	return &hstent;
}

int
lock_host(hstrec, hstent, fflag)
	hostrec_t	*hstrec;
	hostrule_t	*hstent;
	int		fflag;
{
	int ret;
	char ebuf[1024];

	if ((hstrec->num >= hstent->attempts && hstent->attempts != 0) || \
	    fflag != 0) {
		hstrec->locked_for = hstent->locktime;
		hstrec->last_attempt = time(NULL);
		if (hstent->lock_cmd != NULL) {
			ret = exec_cmd(hstent->lock_cmd, NULL, ebuf, \
			    sizeof(ebuf));
			if (ret != 0)
				warnx("error executing lock cmd: %s", ebuf);
		}
		return 0;
	}
	
	return 1;
}

int
unlock_host(hstrec, hstent, fflag)
	hostrec_t	*hstrec;
	hostrule_t	*hstent;
	int		fflag;
{
	int ret;
	char ebuf[1024];

	if ((hstrec->last_attempt + hstrec->locked_for < time(NULL) || \
	    fflag != 0) && hstrec->last_attempt != 0) {
		hstrec->locked_for = 0;
		if (hstent->unlock_cmd != NULL) {
			ret = exec_cmd(hstent->unlock_cmd, NULL, ebuf, \
			    sizeof(ebuf));
			if (ret != 0)
				warnx("error executing unlock cmd: %s", ebuf);
		}
		return 0;
	}
	
	return 1;
}

int
addr_cmp(addr1, addr2, addrlen, mask)
	const void	*addr1;
	const void	*addr2;
	size_t		addrlen;
	int32_t		mask;
{
	register int bytes = mask / 8;
	register int left = mask % 8;
	register int8_t byte1 = 0, byte2 = 0;

	if (mask > (signed)addrlen * 8)
		return 1;
	
	if (bcmp(addr1, addr2, bytes) != 0)
		return 1;

	if (left != 0) {
		byte1 = ((const int8_t *)addr1)[bytes];
		byte1 = ((const int8_t *)addr2)[bytes];

		byte1 >>= (8 - left);
		byte2 >>= (8 - left);
	}

	if (byte1 == byte2) {
		return 0;
	}
	else
		return 1;
}

int
parse_time(str, ptime)
	const char	*str;
	long		*ptime;
{
	register long rettime = 0;
	register int i;
	char *p;
	
	ASSERT(str);
	ASSERT(ptime);

	for (i = strtol(str, &p, 0); *p != '\0'; i = strtol(++p, &p, 0)) {
		if (i <= 0)
			return 1;
		switch (*p) {
		case 'y':
			rettime += i * 12 * 30 * 24 * 3600;
			break;
		case 'm':
			rettime += i * 30 * 24 * 3600;
			break;
		case 'd':
			rettime += i * 24 * 3600;
			break;
		case 'H':
			rettime += i * 3600;
			break;
		case 'M':
			rettime += i * 60;
			break;
		case 'S':
			rettime += i;
			break;
		default:
			return 1;
		}
	}

	*ptime = rettime + i;
	return 0;
}

/*
 * Execute external cmd, return error message in ebuf, if not NULL. 
 */
int
exec_cmd(str, env, ebuf, ebufsz)
	const char	*str;
	char * const env[];
	char		*ebuf;
	size_t		ebufsz;
{
	int pid, ret = 0, status;
			
	switch (pid = vfork()) {
	case 0:
		(void)execle(_PATH_BSHELL, "sh", "-c", str, NULL,
		    (char * const *)env);
		ret = errno;
		_exit(1);
		break;
	case -1:
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "can't fork: %s", \
			    strerror(errno));
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
		break;
	default:
		break;
	}

	if (waitpid(pid, &status, 0) == -1) {
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "waitpid(): %s", \
			    strerror(errno));
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
	}
	
	/* Check child exit value */
	if (ret != 0) {
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "execle(): %s", \
			    strerror(errno));
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
	}

	if (WIFSIGNALED(status)) {
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "cmd caught signal %d%s", \
			    WTERMSIG(status),
		    	    WCOREDUMP(status) ? " (core dumped)" : "");
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
	}
	if (WIFEXITED(status) == 0) {
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "unknown status 0x%x", status);
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
	}
	if (WEXITSTATUS(status) != 0) {
		if (ebuf != NULL) {
			snprintf(ebuf, ebufsz, "cmd returned code %d", \
			    WEXITSTATUS(status));
			ebuf[ebufsz - 1] = 0;
		}
		return 1;
	}

	return 0;	
}	
