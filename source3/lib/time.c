/*
   Unix SMB/CIFS implementation.
   time handling functions

   Copyright (C) Andrew Tridgell 		1992-2004
   Copyright (C) Stefan (metze) Metzmacher	2002
   Copyright (C) Jeremy Allison			2007

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"

/**
 * @file
 * @brief time handling functions
 */


#define NTTIME_INFINITY (NTTIME)0x8000000000000000LL

#if (SIZEOF_LONG == 8)
#define TIME_FIXUP_CONSTANT_INT 11644473600L
#elif (SIZEOF_LONG_LONG == 8)
#define TIME_FIXUP_CONSTANT_INT 11644473600LL
#endif

/**************************************************************
 Handle conversions between time_t and uint32, taking care to
 preserve the "special" values.
**************************************************************/

uint32_t convert_time_t_to_uint32_t(time_t t)
{
#if (defined(SIZEOF_TIME_T) && (SIZEOF_TIME_T == 8))
	/* time_t is 64-bit. */
	if (t == 0x8000000000000000LL) {
		return 0x80000000;
	} else if (t == 0x7FFFFFFFFFFFFFFFLL) {
		return 0x7FFFFFFF;
	}
#endif
	return (uint32_t)t;
}

time_t convert_uint32_t_to_time_t(uint32_t u)
{
#if (defined(SIZEOF_TIME_T) && (SIZEOF_TIME_T == 8))
	/* time_t is 64-bit. */
	if (u == 0x80000000) {
		return (time_t)0x8000000000000000LL;
	} else if (u == 0x7FFFFFFF) {
		return (time_t)0x7FFFFFFFFFFFFFFFLL;
	}
#endif
	return (time_t)u;
}

/****************************************************************************
 Check if NTTIME is 0.
****************************************************************************/

bool nt_time_is_zero(const NTTIME *nt)
{
	return (*nt == 0);
}

/****************************************************************************
 Convert ASN.1 GeneralizedTime string to unix-time.
 Returns 0 on failure; Currently ignores timezone.
****************************************************************************/

time_t generalized_to_unix_time(const char *str)
{
	struct tm tm;

	ZERO_STRUCT(tm);

	if (sscanf(str, "%4d%2d%2d%2d%2d%2d",
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
		return 0;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	return timegm(&tm);
}

/*******************************************************************
 Accessor function for the server time zone offset.
 set_server_zone_offset() must have been called first.
******************************************************************/

static int server_zone_offset;

int get_server_zone_offset(void)
{
	return server_zone_offset;
}

/*******************************************************************
 Initialize the server time zone offset. Called when a client connects.
******************************************************************/

int set_server_zone_offset(time_t t)
{
	server_zone_offset = get_time_zone(t);
	return server_zone_offset;
}

/***************************************************************************
 Server versions of the above functions.
***************************************************************************/

void srv_put_dos_date(char *buf,int offset,time_t unixdate)
{
	push_dos_date((uint8_t *)buf, offset, unixdate, server_zone_offset);
}

void srv_put_dos_date2(char *buf,int offset, time_t unixdate)
{
	push_dos_date2((uint8_t *)buf, offset, unixdate, server_zone_offset);
}

void srv_put_dos_date3(char *buf,int offset,time_t unixdate)
{
	push_dos_date3((uint8_t *)buf, offset, unixdate, server_zone_offset);
}

void round_timespec(enum timestamp_set_resolution res, struct timespec *ts)
{
	if (is_omit_timespec(ts)) {
		return;
	}

	switch (res) {
		case TIMESTAMP_SET_SECONDS:
			round_timespec_to_sec(ts);
			break;
		case TIMESTAMP_SET_MSEC:
			round_timespec_to_usec(ts);
			break;
		case TIMESTAMP_SET_NT_OR_BETTER:
			/* No rounding needed. */
			break;
        }
}

/****************************************************************************
 Take a Unix time and convert to an NTTIME structure and place in buffer
 pointed to by p, rounded to the correct resolution.
****************************************************************************/

void put_long_date_timespec(enum timestamp_set_resolution res, char *p, struct timespec ts)
{
	NTTIME nt;
	round_timespec(res, &ts);
	nt = unix_timespec_to_nt_time(ts);
	SBVAL(p, 0, nt);
}

void put_long_date_full_timespec(enum timestamp_set_resolution res,
				 char *p,
				 const struct timespec *_ts)
{
	struct timespec ts = *_ts;
	NTTIME nt;

	round_timespec(res, &ts);
	nt = full_timespec_to_nt_time(&ts);
	SBVAL(p, 0, nt);
}

struct timespec pull_long_date_full_timespec(const char *p)
{
	NTTIME nt = BVAL(p, 0);

	return nt_time_to_full_timespec(nt);
}

void put_long_date(char *p, time_t t)
{
	struct timespec ts;
	ts.tv_sec = t;
	ts.tv_nsec = 0;
	put_long_date_timespec(TIMESTAMP_SET_SECONDS, p, ts);
}

void dos_filetime_timespec(struct timespec *tsp)
{
	tsp->tv_sec &= ~1;
	tsp->tv_nsec = 0;
}

/*******************************************************************
 Create a unix date (int GMT) from a dos date (which is actually in
 localtime).
********************************************************************/

time_t make_unix_date(const void *date_ptr, int zone_offset)
{
	return pull_dos_date(date_ptr, zone_offset);
}

/*******************************************************************
 Like make_unix_date() but the words are reversed.
********************************************************************/

time_t make_unix_date2(const void *date_ptr, int zone_offset)
{
	return pull_dos_date2(date_ptr, zone_offset);
}

/*******************************************************************
 Create a unix GMT date from a dos date in 32 bit "unix like" format
 these generally arrive as localtimes, with corresponding DST.
******************************************************************/

time_t make_unix_date3(const void *date_ptr, int zone_offset)
{
	return pull_dos_date3(date_ptr, zone_offset);
}

time_t srv_make_unix_date(const void *date_ptr)
{
	return make_unix_date(date_ptr, server_zone_offset);
}

time_t srv_make_unix_date2(const void *date_ptr)
{
	return make_unix_date2(date_ptr, server_zone_offset);
}

time_t srv_make_unix_date3(const void *date_ptr)
{
	return make_unix_date3(date_ptr, server_zone_offset);
}

/****************************************************************************
 Interprets an nt time into a unix struct timespec.
 Differs from nt_time_to_unix in that an 8 byte value of 0xffffffffffffffff
 will be returned as (time_t)-1, whereas nt_time_to_unix returns 0 in this case.
****************************************************************************/

struct timespec interpret_long_date(NTTIME nt)
{
	if (nt == (uint64_t)-1) {
		struct timespec ret;
		ret.tv_sec = (time_t)-1;
		ret.tv_nsec = 0;
		return ret;
	}
	return nt_time_to_full_timespec(nt);
}

/*******************************************************************
 Re-read the smb serverzone value.
******************************************************************/

static struct timeval start_time_hires;

void TimeInit(void)
{
	set_server_zone_offset(time(NULL));

	DEBUG(4,("TimeInit: Serverzone is %d\n", server_zone_offset));

	/* Save the start time of this process. */
	if (start_time_hires.tv_sec == 0 && start_time_hires.tv_usec == 0) {
		GetTimeOfDay(&start_time_hires);
	}
}

/**********************************************************************
 Return a timeval struct of the uptime of this process. As TimeInit is
 done before a daemon fork then this is the start time from the parent
 daemon start. JRA.
***********************************************************************/

void get_process_uptime(struct timeval *ret_time)
{
	struct timeval time_now_hires;

	GetTimeOfDay(&time_now_hires);
	ret_time->tv_sec = time_now_hires.tv_sec - start_time_hires.tv_sec;
	if (time_now_hires.tv_usec < start_time_hires.tv_usec) {
		ret_time->tv_sec -= 1;
		ret_time->tv_usec = 1000000 + (time_now_hires.tv_usec - start_time_hires.tv_usec);
	} else {
		ret_time->tv_usec = time_now_hires.tv_usec - start_time_hires.tv_usec;
	}
}

/**
 * @brief Get the startup time of the server.
 *
 * @param[out] ret_time A pointer to a timveal structure to set the startup
 *                      time.
 */
void get_startup_time(struct timeval *ret_time)
{
	ret_time->tv_sec = start_time_hires.tv_sec;
	ret_time->tv_usec = start_time_hires.tv_usec;
}


/****************************************************************************
 Convert a NTTIME structure to a time_t.
 It's originally in "100ns units".

 This is an absolute version of the one above.
 By absolute I mean, it doesn't adjust from 1/1/1601 to 1/1/1970
 if the NTTIME was 5 seconds, the time_t is 5 seconds. JFM
****************************************************************************/

time_t nt_time_to_unix_abs(const NTTIME *nt)
{
	uint64_t d;

	if (*nt == 0) {
		return (time_t)0;
	}

	if (*nt == (uint64_t)-1) {
		return (time_t)-1;
	}

	if (*nt == NTTIME_INFINITY) {
		return (time_t)-1;
	}

	/* reverse the time */
	/* it's a negative value, turn it to positive */
	d=~*nt;

	d += 1000*1000*10/2;
	d /= 1000*1000*10;

	if (!(TIME_T_MIN <= ((time_t)d) && ((time_t)d) <= TIME_T_MAX)) {
		return (time_t)0;
	}

	return (time_t)d;
}

/****************************************************************************
 Convert a time_t to a NTTIME structure

 This is an absolute version of the one above.
 By absolute I mean, it doesn't adjust from 1/1/1970 to 1/1/1601
 If the time_t was 5 seconds, the NTTIME is 5 seconds. JFM
****************************************************************************/

void unix_to_nt_time_abs(NTTIME *nt, time_t t)
{
	double d;

	if (t==0) {
		*nt = 0;
		return;
	}

	if (t == TIME_T_MAX) {
		*nt = 0x7fffffffffffffffLL;
		return;
	}

	if (t == (time_t)-1) {
		/* that's what NT uses for infinite */
		*nt = NTTIME_INFINITY;
		return;
	}

	d = (double)(t);
	d *= 1.0e7;

	*nt = (NTTIME)d;

	/* convert to a negative value */
	*nt=~*nt;
}


/****************************************************************************
 Utility function that always returns a const string even if localtime
 and asctime fail.
****************************************************************************/

const char *time_to_asc(const time_t t)
{
	const char *asct;
	struct tm *lt = localtime(&t);

	if (!lt) {
		return "unknown time\n";
	}

	asct = asctime(lt);
	if (!asct) {
		return "unknown time\n";
	}
	return asct;
}

const char *display_time(NTTIME nttime)
{
	float high;
	float low;
	int sec;
	int days, hours, mins, secs;

	if (nttime==0)
		return "Now";

	if (nttime==NTTIME_INFINITY)
		return "Never";

	high = 65536;
	high = high/10000;
	high = high*65536;
	high = high/1000;
	high = high * (~(nttime >> 32));

	low = ~(nttime & 0xFFFFFFFF);
	low = low/(1000*1000*10);

	sec=(int)(high+low);

	days=sec/(60*60*24);
	hours=(sec - (days*60*60*24)) / (60*60);
	mins=(sec - (days*60*60*24) - (hours*60*60) ) / 60;
	secs=sec - (days*60*60*24) - (hours*60*60) - (mins*60);

	return talloc_asprintf(talloc_tos(), "%u days, %u hours, %u minutes, "
			       "%u seconds", days, hours, mins, secs);
}

bool nt_time_is_set(const NTTIME *nt)
{
	if (*nt == 0x7FFFFFFFFFFFFFFFLL) {
		return false;
	}

	if (*nt == NTTIME_INFINITY) {
		return false;
	}

	return true;
}
