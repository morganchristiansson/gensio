/*
 *  gtlssh - A program for shell over TLS with gensios
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 *
 *  In addition, as a special exception, the copyright holders of
 *  gensio give you permission to combine gensio with free software
 *  programs or libraries that are released under the GNU LGPL and
 *  with code included in the standard release of OpenSSL under the
 *  OpenSSL license (or modified versions of such code, with unchanged
 *  license). You may copy and distribute such a system following the
 *  terms of the GNU GPL for gensio and the licenses of the other code
 *  concerned, provided that you include the source code of that
 *  other code when and as the GNU GPL requires distribution of source
 *  code.
 *
 *  Note that people who make modified versions of gensio are not
 *  obligated to grant this special exception for their modified
 *  versions; it is their choice whether to do so. The GNU General
 *  Public License gives permission to release a modified version
 *  without this exception; this exception also makes it possible to
 *  release a modified version which carries forward this exception.
 */

#include "config.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <termios.h>
#include <gensio/gensio.h>
#include <gensio/gensio_mdns.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#ifdef HAVE_PRCTL
#include <sys/prctl.h>
#endif
#include <assert.h>

#include <openssl/x509.h>
#include <openssl/pem.h>

#include "ioinfo.h"
#include "localports.h"
#include "ser_ioinfo.h"
#include "utils.h"
#include "gtlssh.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define X509_get0_notAfter(x) X509_get_notAfter(x)
#endif

unsigned int debug;

struct gdata {
    struct gensio_os_funcs *o;
    struct gensio_waiter *waiter;
    struct gensio *user_io;
    struct gensio *io;
    char *ios;
    bool can_close;

    /* The following are only used on ioinfo2. */
    bool interactive;
    bool got_oob;
};

static void winch_ready(int fd, void *cb_data);
static void winch_sent(void *cb_data);

static int winch_pipe[2];
static unsigned char winch_buf[11];
static struct ioinfo_oob winch_oob = { .buf = winch_buf };
static bool winch_oob_sending;
static bool winch_oob_pending;

static void
gshutdown(struct ioinfo *ioinfo, bool user_req)
{
    struct gdata *ginfo = ioinfo_userdata(ioinfo);

    ginfo->o->wake(ginfo->waiter);
}

static void
gerr(struct ioinfo *ioinfo, char *fmt, va_list ap)
{
    struct gdata *ginfo = ioinfo_userdata(ioinfo);

    fprintf(stderr, "Error on %s: \n", ginfo->ios);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

static void
gout(struct ioinfo *ioinfo, char *fmt, va_list ap)
{
    struct gdata *ginfo = ioinfo_userdata(ioinfo);
    char str[200];

    vsnprintf(str, sizeof(str), fmt, ap);
    gensio_write(ginfo->user_io, NULL, str, strlen(str), NULL);
}

static void handle_rem_req(struct gensio *io, const char *service);

static int gevent(struct ioinfo *ioinfo, struct gensio *io, int event,
		  int ierr, unsigned char *buf, gensiods *buflen,
		  const char *const *auxdata)
{
    if (event != GENSIO_EVENT_NEW_CHANNEL)
	return GE_NOTSUP;
    handle_rem_req((struct gensio *) buf, auxdata[0]);
    return 0;
}

/*
 * We wait until the other end has signalled us to say that it is
 * ready before sending the winch.  Otherwise the winch will mess up
 * the password login process.
 */
static void
goobdata(struct ioinfo *ioinfo, unsigned char *buf, gensiods *buflen)
{
    struct gdata *ginfo = ioinfo_userdata(ioinfo);

    if (ginfo->got_oob)
	return;

    ginfo->got_oob = true;
    if (ginfo->interactive)
	winch_ready(winch_pipe[0], ioinfo);
}

static struct ioinfo_user_handlers guh = {
    .shutdown = gshutdown,
    .err = gerr,
    .event = gevent,
    .out = gout,
    .oobdata = goobdata
};

static const char *username, *hostname, *keyfile, *certfile, *CAdir;
static char *tlssh_dir = NULL;
static int port = 852;

static int
getpassword(char *pw, gensiods *len)
{
    int fd = open("/dev/tty", O_RDWR);
    struct termios old_termios, new_termios;
    int err = 0;
    gensiods pos = 0;
    char c = 0;
    static char *prompt = "Password: ";

    if (fd == -1) {
	err = errno;
	fprintf(stderr, "Unable to open controlling terminal: %s\n",
		strerror(err));
	return err;
    }

    err = tcgetattr(fd, &old_termios);
    if (err == -1) {
	err = errno;
	fprintf(stderr, "Unable to get terminal information: %s\n",
		strerror(err));
	goto out_close;
    }

    new_termios = old_termios;
    new_termios.c_lflag &= ~ECHO;

    err = tcsetattr(fd, TCSANOW, &new_termios);
    if (err == -1) {
	err = errno;
	fprintf(stderr, "Unable to set terminal information: %s\n",
		strerror(err));
	goto out_close;
    }

    err = write(fd, prompt, strlen(prompt));
    if (err == -1) {
	fprintf(stderr, "Error writing password prompt, giving up: %s\n",
		strerror(errno));
	exit(1);
    }
    while (true) {
	err = read(fd, &c, 1);
	if (err < 0) {
	    err = errno;
	    fprintf(stderr, "Error reading password: %s\n", strerror(err));
	    goto out;
	}
	if (c == '\r' || c == '\n')
	    break;
	if (pos < *len)
	    pw[pos++] = c;
    }
    err = 0;
    printf("\n");
    if (pos < *len)
	pw[pos++] = '\0';
    *len = pos;

 out:
    tcsetattr(fd, TCSANOW, &old_termios);
 out_close:
    close(fd);
    return err;
}

static void
io_close(struct gensio *io, void *close_data)
{
    struct ioinfo *ioinfo = gensio_get_user_data(io);
    struct gdata *ginfo = ioinfo_userdata(ioinfo);
    struct gensio_waiter *closewaiter = close_data;

    ginfo->o->wake(closewaiter);
}

static const char *progname;
static char *io1_default_tty = "serialdev,/dev/tty";
static char *io1_default_notty = "stdio(self)";

static void
help(int err)
{
    printf("%s [options] hostname [program]\n", progname);
    printf("\nA program to connect to a remote system over TLS.  The\n");
    printf("hostname is the remote system.  If no program is given and\n");
    printf("if stdin is a tty, the connection is interactive.  Otherwise\n");
    printf("the connection is not interactive and buffered.\n");
    printf("\noptions are:\n");
    printf("  -p, --port <port> - Use the given port instead of the\n"
	   "    default.\n");
    printf("  -i, --keyfile <file> - Use the given file for the key instead\n"
	   "    of the default.  The certificate will default to the same\n"
	   "    name ending in .crt\n");
    printf("  --certfile <file> - Set the certificate to use.\n");
    printf("  -r, --telnet - Do telnet processing with RFC2217 handling.\n");
    printf("  -e, --escchar - Set the local terminal escape character.\n"
	   "    Set to -1 to disable the escape character\n"
	   "    Default is ^\\ for tty stdin and disabled for non-tty stdin\n");
    printf("  --nosctp - Disable SCTP support (default).\n");
    printf("  --sctp - Disable SCTP support.\n");
    printf("  --notcp - Disable TCP support.\n");
    printf("  --transport - Use the given gensio instead of TCP or SCTP.\n");
    printf("    hostname is ignored in this case, except for the username\n");
    printf("    part, but is required.\n");
    printf("  -m, --mdns - Look up the name using mDNS.  This will fetch\n");
    printf("    then IP address, IPv4 or IPv6, the port number and whether\n");
    printf("    telnet is required and make the connection.\n");
    printf("  --mdns-type - Set the type used for the lookup.  See\n");
    printf("    the gmdns(1) man page under 'STRING VALUES FOR QUERIES'\n");
    printf("    for detail on how to do regex, glob, etc.\n");
    printf("  -d, --debug - Enable debug.  Specify more than once to increase\n"
	   "    the debug level\n");
    printf("  -L <accept addr>:<connect addr> - Listen at the <accept addr>\n"
	   "    on the local machine, and if a connection comes in forward it\n"
	   "    to the <connect addr> from the remote machine on the gtlssh\n"
	   "    connection.  A local address is in the form:\n"
	   "      [<bind addr>:][sctp|tcp,]port\n"
	   "    or:\n"
	   "      <unix socket path>\n"
	   "    Remote addresses are in the form:\n"
	   "      <hostname>:[sctp|tcp,]port\n"
	   "    or:\n"
	   "      <unix socket path>\n"
	   "    If a name begins with '/' it is a unix socket path.  hostname\n"
	   "    and bind addr are standard internet names or addresses.\n");
    printf("  -R <accept addr>:<connect addr> - Like -L, except the\n"
	   "    <accept addr> is on the remote machine and <connect addr> is\n"
	   "    done from the local machine\n");
    printf("  -4 - Do IPv4 only.\n");
    printf("  -6 - Do IPv6 only.\n");
    printf("  -h, --help - This help\n");
    exit(err);
}

static void
do_vlog(struct gensio_os_funcs *f, enum gensio_log_levels level,
	const char *log, va_list args)
{
    if (!debug)
	return;
    fprintf(stderr, "gensio %s log: ", gensio_log_level_to_str(level));
    vfprintf(stderr, log, args);
    fprintf(stderr, "\r\n");
}

/*
 * This would be a lot simpler if we could use ASN1_TIME_diff(), but
 * it's not available on libressl.  Keep things as basic as possible,
 * code to calculate the difference in time between now and an ASN1
 * date.
 */
static bool is_leap_year(int year)
{
    return (year % 4 == 0) && !((year % 100 == 0) && !(year % 400 == 0));
}

#define SECS_IN_DAY 86400

/* Number of days to the beginning of the given month. */
static int mdays[12] ={ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

static int get_yearday(struct tm *t)
{
    int yday;

    assert(t->tm_mon >= 0 && t->tm_mon <= 11);
    yday = mdays[t->tm_mon];
    if (t->tm_mon > 1 && is_leap_year(t->tm_year))
	yday++;
    yday += t->tm_mday - 1; /* tm_mday ranges from 1 - 31, need to adjust */
    return yday;
}

/*
 * Calculate the number of days between two dates assuming
 *   t1.tm_year < t2.tm_year.
 */
static int
tmdaydiff(struct tm *t1, struct tm *t2)
{
    int days, v;

    if (t2->tm_year - t1->tm_year > 2) {
	/*
	 * Just short-circuit this if more than two years apart.
	 * We don't care about the actual value at this point.
	 */
	return 730;
    }

    if (is_leap_year(t1->tm_year))
	days = 366 - t1->tm_yday;
    else
	days = 365 - t1->tm_yday;
    v = t1->tm_year + 1;
    while (v < t1->tm_year - 1) {
	days += 365;
	if (is_leap_year(v))
	    days++;
	v++;
    }
    days += t2->tm_yday;
    return days;
}

static int
second_in_day(struct tm *t)
{
    return t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static void
tmdiff(int *rdays, int *rseconds, struct tm *from, struct tm *to)
{
    int days, seconds;

    if (to->tm_year < from->tm_year) {
	days = -tmdaydiff(to, from);
    } else if (to->tm_year > from->tm_year) {
	days = tmdaydiff(from, to);
    } else {
	days = to->tm_yday - from->tm_yday;
    }

    seconds = second_in_day(to) - second_in_day(from);
    if (seconds > 0 && days < 0) {
	seconds -= SECS_IN_DAY;
	days++;
    } else if (seconds < 0 && days > 0) {
	seconds += SECS_IN_DAY;
	days--;
    }

    *rdays = days;
    *rseconds = seconds;
}

static int
calc_timediff(int *days, int *seconds, const ASN1_TIME *t)
{
    struct asn1_string_st *g = (struct asn1_string_st *)
	ASN1_TIME_to_generalizedtime(t, NULL);
    struct tm tm, now_tm;
    time_t now;
    int rv;

    if (!g)
	return GE_NOMEM;

    /* Per rfc5280 generalized time is in the form: YYYYMMDDHHMMSSZ. */
    if (g->length != 15) {
	ASN1_STRING_free((ASN1_STRING *) g);
	return -1;
    }

    /* Extract all the fields into integers. */
    rv = sscanf((char *) g->data, "%4d%2d%2d%2d%2d%2d",
		&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		&tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    ASN1_STRING_free((ASN1_STRING *) g);
    if (rv != 6)
	return -1;

    /*
     * Month is 1-12 in ISO8601, so adjust to tm's 0-11. The day is,
     * for some odd reason, 1-31 in a tm, same as ISO8601.
     */
    tm.tm_mon -= 1;

    /* We need this later. */
    tm.tm_yday = get_yearday(&tm);

    /*
     * rfc5280 generalized time is always GMT/UTC.  Get the current
     * time in GMT/UTC.
     */
    now = time(NULL);
    if (gmtime_r(&now, &now_tm) == NULL)
	return -1;

    /* tm_year starts with 0 == 1900, adjust so leap year calcs work. */
    now_tm.tm_year += 1900;

    tmdiff(days, seconds, &now_tm, &tm);
    return 0;
}

static int
has_time_passed(const ASN1_TIME *t, int *rdays)
{
    int days, seconds;

    /*
     * Get the time from "t" to now.  If "t" is in the past, the value will
     * be negative.
     */
    if (calc_timediff(&days, &seconds, t))
	return GE_IOERR;

    if (days < 0 || seconds < 0)
	return GE_TIMEDOUT;

    *rdays = days;

    return 0;
}

static int
check_cert_expiry(const char *name, const char *filename,
		  const char *cert, gensiods certlen)
{
    X509 *x = NULL;
    const ASN1_TIME *t = NULL;
    int err = 0, days;

    if (filename) {
	FILE *fp = fopen(filename, "r");

	if (!fp) {
	    fprintf(stderr,
		    "Unable to open %s certificate file for "
		    "expiry verification: %s\n", name, strerror(errno));
	    return GE_NOTFOUND;
	}
	x = PEM_read_X509(fp, NULL, NULL, NULL);
	fclose(fp);
    } else {
	BIO *cert_bio = BIO_new_mem_buf(cert, certlen);

	if (!cert_bio) {
	    fprintf(stderr, "Unable to create %s certificate BIO\n", name);
	    return GE_IOERR;
	}

	x = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
	BIO_free(cert_bio);
    }
    if (!x) {
	fprintf(stderr,
		"Unable to load %s certificate for expiry verification\n",
		name);
	return GE_IOERR;
    }

    t = X509_get0_notAfter(x);
    if (!t) {
	fprintf(stderr, "Unable to get certificate expiry time\n");
	err = GE_IOERR;
	goto out;
    }

    err = has_time_passed(t, &days);
    if (err == GE_TIMEDOUT) {
	fprintf(stderr, "***Error: %s certificate has expired\n", name);
	err = GE_CERTEXPIRED;
	goto out;
    } else if (err) {
	fprintf(stderr, "Unable to compare certificate expiry time\n");
	goto out;
    }

    if (days < 30)
	fprintf(stderr, "***WARNING: %s certificate will expire in %d days\n",
		name, days);

 out:
    if (x)
	X509_free(x);

    return err;
}

static int
lookup_certfiles(const char *tlssh_dir, const char *username,
		 const char *hostname, int port,
		 char **rCAdir, char **rcertfile, char **rkeyfile)
{
    int err = GE_NOMEM;
    char *tcertfile, *tkeyfile;

    if (!CAdir) {
	CAdir = alloc_sprintf("%s/server_certs", tlssh_dir);
	if (!CAdir) {
	    fprintf(stderr, "Error allocating memory for CAdir\n");
	    return GE_NOMEM;
	}
    }

    if (!certfile) {
	tcertfile = alloc_sprintf("%s/keycerts/%s,%d.crt", tlssh_dir,
				  hostname, port);
	if (!tcertfile)
	    goto cert_nomem;
	if (file_is_readable(tcertfile)) {
	    tkeyfile = alloc_sprintf("%s/keycerts/%s,%d.key", tlssh_dir,
				     hostname, port);
	    goto found_cert;
	}
	free(tcertfile);
	tcertfile = alloc_sprintf("%s/keycerts/%s.crt", tlssh_dir, hostname);
	if (!tcertfile)
	    goto cert_nomem;
	if (file_is_readable(tcertfile)) {
	    tkeyfile = alloc_sprintf("%s/keycerts/%s.key", tlssh_dir, hostname);
	    goto found_cert;
	}
	free(tcertfile);

	tcertfile = alloc_sprintf("%s/default.crt", tlssh_dir);
	if (!tcertfile) {
	cert_nomem:
	    fprintf(stderr, "Error allocating memory for certificate file\n");
	    goto out_err;
	}
	tkeyfile = alloc_sprintf("%s/default.key", tlssh_dir);
    found_cert:
	if (!tkeyfile) {
	    fprintf(stderr, "Error allocating memory for private key file\n");
	    free(tcertfile);
	    tcertfile = NULL;
	    goto out_err;
	}
	certfile = tcertfile;
	keyfile = tkeyfile;
    }

    err = checkout_file(CAdir, true, false);
    if (err)
	goto out_err;

    err = checkout_file(certfile, false, false);
    if (err)
	goto out_err;

    err = checkout_file(keyfile, false, true);
    if (err)
	goto out_err;

    check_cert_expiry("local", certfile, NULL, 0);

    err = GE_NOMEM;
    *rCAdir = alloc_sprintf("CA=%s/", CAdir);
    if (!*rCAdir)
	goto out_err;
    *rcertfile = alloc_sprintf(",cert=%s", certfile);
    if (!*rcertfile) {
	free(*rCAdir);
	*rCAdir = NULL;
	goto out_err;
    }
    *rkeyfile = alloc_sprintf(",key=%s", keyfile);
    if (!*rkeyfile) {
	free(*rcertfile);
	*rcertfile = NULL;
	free(*rCAdir);
	*rCAdir = NULL;
	goto out_err;
    }

    err = 0;

 out_err:
    return err;
}

static int
add_certfile(struct gensio_os_funcs *o, const char *cert, const char *fmt, ...)
{
    int rv;
    va_list va;
    char *filename;

    va_start(va, fmt);
    filename = alloc_vsprintf(fmt, va);
    va_end(va);
    if (!filename) {
	fprintf(stderr, "Out of memory allocating filename");
	return GE_NOMEM;
    }

    rv = open(filename, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (rv == -1 && errno == EEXIST) {
	fprintf(stderr,
		"Certificate file %s already exists, this means the\n"
		"certificate has changed.  Someone may be trying to\n"
		"intercept your communications.  Giving up, remove the\n"
		"file if it is incorrect and try again\n", filename);
	rv = GE_KEYINVALID;
    } else if (rv == -1) {
	rv = errno;
	fprintf(stderr, "Error opening '%s', could not save certificate: %s\n",
		filename, strerror(rv));
	rv = gensio_os_err_to_err(o, rv);
    } else {
	int fd = rv, len = strlen(cert);

    retry:
	rv = write(fd, cert, len);
	if (rv == -1) {
	    rv = errno;
	    fprintf(stderr, "Error writing '%s', could not save certificate:"
		    " %s\n", filename, strerror(rv));
	    rv = gensio_os_err_to_err(o, rv);
	    goto out;
	} else if (rv != len) {
	    len -= rv;
	    cert += rv;
	    goto retry;
	}
	rv = 0;

    out:
	close(fd);
    }

    free(filename);
    return rv;
}

static int
verify_certfile(struct gensio_os_funcs *o,
		const char *cert, const char *fmt, ...)
{
    int rv;
    va_list va;
    char *filename;
    char cmpcert[16384];

    va_start(va, fmt);
    filename = alloc_vsprintf(fmt, va);
    va_end(va);
    if (!filename) {
	fprintf(stderr, "Out of memory allocating filename");
	return GE_NOMEM;
    }

    rv = open(filename, O_RDONLY);
    if (rv == -1) {
	fprintf(stderr,
		"Unable to open certificate file at %s: %s\n", filename,
		strerror(errno));
	rv = GE_CERTNOTFOUND;
    } else {
	int fd = rv, len = strlen(cert);

	rv = read(fd, cmpcert, sizeof(cmpcert));
	if (rv == -1) {
	    rv = errno;
	    fprintf(stderr, "Error reading '%s', could not verify certificate:"
		    " %s\n", filename, strerror(rv));
	    rv = gensio_os_err_to_err(o, rv);
	    goto out;
	} else if (rv != len) {
	    fprintf(stderr, "Certificate at '%s': length mismatch\n", filename);
	    rv = GE_CERTINVALID;
	    goto out;
	} else if (memcmp(cert, cmpcert, len) != 0) {
	    fprintf(stderr, "Certificate at '%s': compare failure\n", filename);
	    rv = GE_CERTINVALID;
	    goto out;
	}
	rv = 0;

    out:
	close(fd);
    }

    free(filename);
    return rv;
}

static void
translate_raddr(char *raddr)
{
    while (*raddr) {
	if (*raddr == '/')
	    *raddr = '-';
	raddr++;
    }
}

static int
auth_event(struct gensio *io, void *user_data, int event, int ierr,
	   unsigned char *ibuf, gensiods *buflen,
	   const char *const *auxdata)
{
    struct ioinfo *ioinfo = user_data;
    struct gdata *ginfo = ioinfo_userdata(ioinfo);
    struct gensio *ssl_io;
    char raddr[256];
    char fingerprint[256];
    char cert[16384];
    char buf[100];
    char *cmd;
    gensiods len, certlen;
    int err, err1, err2;

    switch (event) {
    case GENSIO_EVENT_POSTCERT_VERIFY:
	ssl_io = io;
	while (ssl_io) {
	    if (strcmp(gensio_get_type(ssl_io, 0), "ssl") == 0)
		break;
	    ssl_io = gensio_get_child(ssl_io, 1);
	}
	if (!ssl_io) {
	    fprintf(stderr, "SSL was not in the gensio stack?\n");
	    return GE_INVAL;
	}

	certlen = sizeof(cert);
	err = gensio_control(ssl_io, 0, true, GENSIO_CONTROL_CERT,
			     cert, &certlen);
	if (err) {
	    fprintf(stderr, "Error getting certificate: %s\n",
		    gensio_err_to_str(err));
	    return GE_NOMEM;
	}
	if (certlen >= sizeof(cert)) {
	    fprintf(stderr, "certificate is too large");
	    return GE_NOMEM;
	}

	strcpy(raddr, "0");
	len = sizeof(raddr);
	err = gensio_control(ssl_io, GENSIO_CONTROL_DEPTH_FIRST, true,
			     GENSIO_CONTROL_CONNECT_ADDR_STR, raddr, &len);
	if (err) {
	    strcpy(raddr, "0");
	    len = sizeof(raddr);
	    err = gensio_control(ssl_io, GENSIO_CONTROL_DEPTH_FIRST, true,
				 GENSIO_CONTROL_RADDR, raddr, &len);
	}
	if (err) {
	    fprintf(stderr, "Could not get connections remote address: %s\n",
		    gensio_err_to_str(err));
	    return err;
	}
	translate_raddr(raddr);

	if (!ierr) {
	    /* Found a certificate, make sure it's the right one. */
	    err1 = verify_certfile(ginfo->o,
				   cert, "%s/%s,%d.crt", CAdir, hostname, port);
	    err2 = verify_certfile(ginfo->o,
				   cert, "%s/%s.crt", CAdir, raddr);
	    if ((err1 == GE_CERTNOTFOUND && err1 == err2) ||
			(err1 == GE_CERTNOTFOUND && !err2) ||
			(err2 == GE_CERTNOTFOUND && !err1)) {
		if (err1)
		    printf("\nCertificate for %s found and correct, but"
			   " address file was\nmissing for it.\n", hostname);
		if (err2)
		    printf("\nCertificate for %s found and correct, but"
			   " address file was\nmissing for\n  %s\n",
			   hostname, raddr);
		printf("It is possible that the same key is used for"
		       " different connections,\nbut"
		       " there may also be a man in the middle\n");
		printf("Verify carefully, add if it is ok.\n");
		do {
		    char *s;

		    printf("Add this certificate? (y/n): ");
		    s = fgets(buf, sizeof(buf), stdin);
		    if (s == NULL) {
			printf("Error reading input, giving up\n");
			exit(1);
		    }
		    if (buf[0] == 'y') {
			err = 0;
			break;
		    } else if (buf[0] == 'n') {
			err = GE_AUTHREJECT;
			break;
		    } else {
			printf("Invalid input: %s", buf);
		    }
		} while (true);
		if (!err) {
		    if (err1)
			err = add_certfile(ginfo->o, cert,
					   "%s/%s,%d.crt", CAdir, hostname,
					   port);
		    if (!err && err2)
			err = add_certfile(ginfo->o, cert,
					   "%s/%s.crt", CAdir, raddr);
		}
	    }

	    goto postcert_done;
	}

	/*
	 * Called from the SSL layer if the certificate provided by
	 * the server didn't have a match.
	 */
	if (ierr != GE_CERTNOTFOUND) {
	    const char *errstr = "probably didn't match host certificate.";
	    if (ierr == GE_CERTREVOKED)
		errstr = "is revoked";
	    else if (ierr == GE_CERTEXPIRED)
		errstr = "is expired";
	    fprintf(stderr, "Certificate for %s failed validation: %s\n",
		    hostname, auxdata[0]);
	    fprintf(stderr,
		    "Certificate from remote, and possibly in\n"
		    "  %s/%s,%d.crt\n"
		    "or\n"
		    "  %s/%s.crt\n"
		    "%s\n",
		    CAdir, hostname, port, CAdir, raddr, errstr);
	    return ierr;
	}

	/* Key was not present, ask the user if that is ok. */
	len = sizeof(fingerprint);
	err = gensio_control(ssl_io, 0, true, GENSIO_CONTROL_CERT_FINGERPRINT,
			     fingerprint, &len);
	if (err) {
	    fprintf(stderr, "Error getting fingerprint: %s\n",
		    gensio_err_to_str(err));
	    return GE_CERTINVALID;
	}
	if (len >= sizeof(fingerprint)) {
	    fprintf(stderr, "fingerprint is too large\n");
	    return GE_CERTINVALID;
	}

	printf("Certificate for %s", hostname);
	if (strcmp(hostname, raddr) != 0)
	    printf(" %s\n", raddr);
	printf(" is not present, fingerprint is:\n%s\n", fingerprint);
	printf("Please validate the fingerprint and verify if you want it\n"
	       "added to the set of valid servers.\n");
	do {
	    char *s;

	    printf("Add this certificate? (y/n): ");
	    s = fgets(buf, sizeof(buf), stdin);
	    if (s == NULL) {
		printf("Error reading input, giving up\n");
		exit(1);
	    }
	    if (buf[0] == 'y') {
		err = 0;
		break;
	    } else if (buf[0] == 'n') {
		err = GE_AUTHREJECT;
		break;
	    } else {
		printf("Invalid input: %s", buf);
	    }
	} while (true);

	if (err)
	    return err;

	len = sizeof(cert);
	err = gensio_control(ssl_io, 0, true, GENSIO_CONTROL_CERT,
			     cert, &len);
	if (err) {
	    fprintf(stderr, "Error getting certificate: %s\n",
		    gensio_err_to_str(err));
	    return GE_NOMEM;
	}
	if (len >= sizeof(cert)) {
	    fprintf(stderr, "certificate is too large");
	    return GE_NOMEM;
	}

	err = add_certfile(ginfo->o,
			   cert, "%s/%s,%d.crt", CAdir, hostname, port);
	if (!err)
	    err = add_certfile(ginfo->o, cert, "%s/%s.crt", CAdir, raddr);

	cmd = alloc_sprintf("gtlssh-keygen rehash %s", CAdir);
	if (!cmd) {
	    fprintf(stderr, "Could not allocate memory for rehash, skipping\n");
	} else {
	    int rv = system(cmd);

	    if (rv != 0)
		fprintf(stderr, "Error from %s, rehash skipped\n", cmd);

	    free(cmd);
	}

    postcert_done:
	if (!err)
	    err = check_cert_expiry("remote host", NULL, cert, certlen);
	return err;

    case GENSIO_EVENT_REQUEST_PASSWORD:
	return getpassword((char *) ibuf, buflen);

    default:
	return GE_NOTSUP;
    }
}

static void
send_winch(struct ioinfo *ioinfo)
{
    struct winsize win;
    int rv;

    rv = ioctl(0, TIOCGWINSZ, &win);
    if (rv == -1)
	return;

    /* See goobdata() comment for message format. */
    winch_oob.buf[0] = 'w';
    gensio_u16_to_buf(winch_oob.buf + 1, 8); /* Number of following bytes. */
    gensio_u16_to_buf(winch_oob.buf + 3, win.ws_row);
    gensio_u16_to_buf(winch_oob.buf + 5, win.ws_col);
    gensio_u16_to_buf(winch_oob.buf + 7, win.ws_xpixel);
    gensio_u16_to_buf(winch_oob.buf + 9, win.ws_ypixel);
    winch_oob.len = 11;
    winch_oob.cb_data = ioinfo;
    winch_oob.send_done = winch_sent;
    ioinfo_sendoob(ioinfo, &winch_oob);
    winch_oob_sending = true;
}

static void
winch_sent(void *cb_data)
{
    struct ioinfo *ioinfo = cb_data;

    winch_oob_sending = false;
    if (winch_oob_pending) {
	winch_oob_pending = false;
	send_winch(ioinfo);
    }
}

static void
winch_ready(int fd, void *cb_data)
{
    struct ioinfo *ioinfo = cb_data;
    char dummy;
    int rv = 1;

    /* Clear out the pipe. */
    while (rv == 1)
	rv = read(winch_pipe[0], &dummy, 1);
    /* errno should be EAGAIN here. */

    if (!isatty(0))
	return;

    if (winch_oob_sending)
	winch_oob_pending = true;
    else
	send_winch(ioinfo);
}

static void
handle_sigwinch(int signum)
{
    int rv;

    rv = write(winch_pipe[1], "w", 1);
    if (rv != 1) {
	/* What can be done here? */
    }
}

static void
pr_localport(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
}

struct remote_portinfo {
    char *accepter_str;
    char *connecter_str;
    char *id_str;

    char service[5];

    struct ioinfo_oob oob;

    struct gensio_os_funcs *o;

    struct remote_portinfo *next;
};

static unsigned int curr_service;
static struct remote_portinfo *remote_ports;

static void
handle_rem_req(struct gensio *io, const char *service)
{
    struct remote_portinfo *pi = remote_ports;

    for (; pi; pi = pi->next) {
	if (strcmp(pi->service, service) == 0)
	    break;
    }
    if (!pi) {
	fprintf(stderr, "Unknown remote service request: %s\n", service);
	gensio_free(io);
	return;
    }

    remote_port_new_con(pi->o, io, pi->connecter_str, pi->id_str);
}

static int
add_remote_port(struct gensio_os_funcs *o,
		const char *accepter_str, const char *connecter_str,
		const char *id_str)
{
    struct remote_portinfo *np = NULL;
    int err = GE_NOMEM;

    np = malloc(sizeof(*np));
    if (!np) {
	fprintf(stderr, "Out of memory allocating port info\n");
	goto out_err;
    }
    memset(np, 0, sizeof(*np));
    np->o = o;
    snprintf(np->service, sizeof(np->service), "%4.4d", curr_service++);

    np->accepter_str = alloc_sprintf("r  %s%s", np->service, accepter_str);
    if (!np->accepter_str) {
	fprintf(stderr, "Out of memory allocating accept string: %s\n",
		accepter_str);
	goto out_err;
    }
    np->oob.buf = (unsigned char *) np->accepter_str;
    np->oob.len = strlen(np->accepter_str) + 1; /* Send the nil, too. */
    if (np->oob.len > 65535) {
	fprintf(stderr, "Accepter string to long: %s\n", accepter_str);
	goto out_err;
    }
    np->oob.buf[1] = (np->oob.len - 3) >> 8;
    np->oob.buf[2] = (np->oob.len - 3) & 0xff;

    np->connecter_str = strdup(connecter_str);
    if (!np->connecter_str) {
	fprintf(stderr, "Out of memory allocating connecter string: %s\n",
		connecter_str);
	goto out_err;
    }

    np->id_str = strdup(id_str);
    if (!np->id_str) {
	fprintf(stderr, "Out of memory allocating id string: %s\n", id_str);
	goto out_err;
    }
    np->next = remote_ports;
    remote_ports = np;
    np = NULL;
    err = 0;

 out_err:
    if (np) {
	if (np->accepter_str)
	    free(np->accepter_str);
	if (np->connecter_str)
	    free(np->connecter_str);
	if (np->id_str)
	    free(np->id_str);
	free(np);
    }
    return err;
}

static void
start_remote_ports(struct ioinfo *ioinfo)
{
    struct remote_portinfo *pi = remote_ports;

    for (; pi; pi = pi->next)
	ioinfo_sendoob(ioinfo, &pi->oob);
}

static bool
validate_port(const char *host, const char *port, const char **rtype,
	      const char *addr)
{
    const char *type = "tcp";
    unsigned long val;
    char *end;

    if (strncmp(port, "tcp,", 4) == 0)
	port += 4;
    if (strncmp(port, "sctp,", 5) == 0) {
	port += 5;
	type = "sctp";
    }

    if (host && *host == '\0') {
	fprintf(stderr, "No host given in '%s'\n", addr);
	return false;
    }

    val = strtoul(port, &end, 10);
    if (*port == '\0' || *end != '\0' || val > 65535) {
	fprintf(stderr, "Invalid port given in '%s'\n", addr);
	return false;
    }

    *rtype = type;
    return true;
}

static int
handle_port(struct gensio_os_funcs *o, bool remote, const char *iaddr)
{
    char *s[4];
    const char *type = NULL;
    unsigned int num_s = 0, pos = 0;
    char *connecter_str = NULL, *accepter_str = NULL;
    char *addr = strdup(iaddr);
    int err = -1;
    bool has_bind = false;

    if (!addr) {
	fprintf(stderr, "Out of memory duplicating port '%s'\n", iaddr);
	goto out_err;
    }

    while (num_s < 4) {
	s[num_s++] = addr;
	addr = strchr(addr, ':');
	if (addr)
	    *addr++ = '\0';
	else
	    break;
    }

    if (num_s < 2)
	goto out_not_enough_fields;
    if (num_s > 4)
	goto out_too_many_fields;

    if (s[num_s - 1][0] == '/') { /* remote is a unix socket. */
	if (s[0][0] == '/') { /* local is a unix socket */
	    if (num_s > 2)
		goto out_too_many_fields;
	} else if (num_s > 3) {
	    goto out_too_many_fields;
	} else if (num_s == 3) {
	    has_bind = true;
	}
    } else if (s[0][0] == '/') { /* local is a unix socket */
	if (num_s > 3)
	    goto out_too_many_fields;
    } else if (num_s < 3) {
	goto out_not_enough_fields;
    } else if (num_s == 4) {
	has_bind = true;
    }

    if (has_bind) {
	if (!validate_port(s[pos], s[pos + 1], &type, iaddr))
	    goto out_err;
	accepter_str = alloc_sprintf("%s,%s,%s", type, s[pos], s[pos + 1]);
	pos += 2;
    } else if (s[pos][0] == '/') { /* accepter is a unix socket */
	accepter_str = alloc_sprintf("unix,%s", s[pos]);
	pos++;
    } else {
	if (!validate_port(NULL, s[pos], &type, iaddr))
	    goto out_err;
	accepter_str = alloc_sprintf("%s,%s", type, s[pos]);
	pos++;
    }

    if (s[pos][0] == '/') { /* remote is a unix socket */
	connecter_str = alloc_sprintf("unix,%s", s[pos]);
    } else {
	if (!validate_port(s[pos], s[pos + 1], &type, iaddr))
	    goto out_err;
	connecter_str = alloc_sprintf("%s,%s,%s", type, s[pos], s[pos + 1]);
    }

    if (remote)
	err = add_remote_port(o, accepter_str, connecter_str, iaddr);
    else
	err = add_local_port(o, accepter_str, connecter_str, iaddr);
    goto out_err;

 out_not_enough_fields:
    fprintf(stderr, "Not enough fields in port info '%s'\n", iaddr);
    goto out_err;
 out_too_many_fields:
    fprintf(stderr, "Too many fields in port info '%s'\n", iaddr);
    goto out_err;

 out_err:
    if (accepter_str)
	free(accepter_str);
    if (connecter_str)
	free(connecter_str);
    if (addr)
	free(addr);

    return err;
}

struct mdns_cb_data {
    bool done;
    int err;
    bool telnet;
    char *transport;
    struct gensio_os_funcs *o;
    struct gensio_waiter *wait;
};

static void
mdns_cb(struct gensio_mdns_watch *w,
	enum gensio_mdns_data_state state,
	int interface, int ipdomain,
	const char *name, const char *type,
	const char *domain, const char *host,
	const struct gensio_addr *addr, const char *txt[],
	void *userdata)
{
    struct mdns_cb_data *cb_data = userdata;
    struct gensio_os_funcs *o = cb_data->o;
    char *addrstr = NULL, *s;
    gensiods addrstrlen = 0, pos = 0;
    const char *protocol;
    static const char *stackstr = "gensiostack=";
    unsigned int i;

    if (cb_data->done)
	return;

    if (state == GENSIO_MDNS_ALL_FOR_NOW)
	goto out_wake;

    if (state != GENSIO_MDNS_NEW_DATA)
	return;

    /* Found it. */

    /* Look for the trailing protocol type. */
    s = strrchr(type, '.');
    if (!s)
	goto out_wake;
    s++;
    if (strcmp(s, "_tcp") == 0) {
	protocol = "tcp";
    } else if (strcmp(s, "_udp") == 0) {
	protocol = "udp";
    } else if (strcmp(s, "_sctp") == 0) {
	protocol = "sctp";
    } else {
	goto out_wake;
    }

    cb_data->err = gensio_addr_to_str(addr, NULL, &addrstrlen, 0);
    if (cb_data->err)
	goto out_wake;
    addrstr = o->zalloc(o, addrstrlen + 1);
    cb_data->err = gensio_addr_to_str(addr, addrstr, &pos, addrstrlen + 1);
    if (cb_data->err) {
	o->free(o, addrstr);
	goto out_wake;
    }

    cb_data->transport = gensio_alloc_sprintf(o, "%s,%s", protocol, addrstr);
    if (!cb_data->transport) {
	cb_data->err = GE_NOMEM;
	goto out_wake;
    }

    for (i = 0; txt && txt[i]; i++) {
	if (strncmp(txt[i], stackstr, strlen(stackstr)) == 0) {
	    if (strstr(txt[i], "telnet"))
		cb_data->telnet = true;
	    break;
	}
    }

 out_wake:
    if (addrstr)
	o->free(o, addrstr);
    cb_data->done = true;
    o->wake(cb_data->wait);
}

static void
mdns_freed(struct gensio_mdns *m, void *userdata)
{
    struct mdns_cb_data *cb_data = userdata;

    cb_data->o->wake(cb_data->wait);
}

static int
lookup_mdns_transport(struct gensio_os_funcs *o, const char *name,
		      const char *type, const char *iptypestr,
		      const char **transport, bool *telnet)
{
    struct gensio_mdns *mdns;
    struct gensio_mdns_watch *mdns_watch;
    int nettype = GENSIO_NETTYPE_UNSPEC;
    int err;
    struct mdns_cb_data cb_data;
    struct gensio_time timeout = { 30, 0 };

    memset(&cb_data, 0, sizeof(cb_data));
    cb_data.err = GE_NOTFOUND;
    cb_data.o = o;
    cb_data.wait = o->alloc_waiter(o);
    if (!cb_data.wait) {
	fprintf(stderr, "Unable to allocate wait: out of memory\n");
	return 1;
    }

    if (strcmp(iptypestr, "ipv4,") == 0)
	nettype = GENSIO_NETTYPE_IPV4;
    else if (strcmp(iptypestr, "ipv6,") == 0)
	nettype = GENSIO_NETTYPE_IPV6;

    err = gensio_alloc_mdns(o, &mdns);
    if (err) {
	o->free_waiter(cb_data.wait);
	fprintf(stderr, "Unable to allocate mdns data: %s\n",
		gensio_err_to_str(err));
	return 1;
    }

    err = gensio_mdns_add_watch(mdns, -1, nettype, name, type, NULL, NULL,
				mdns_cb, &cb_data, &mdns_watch);
    if (err) {
	o->free_waiter(cb_data.wait);
	gensio_free_mdns(mdns, NULL, NULL);
	fprintf(stderr, "Unable to add mdns watch: %s\n",
		gensio_err_to_str(err));
	return 1;
    }

    o->wait(cb_data.wait, 1, &timeout);

    gensio_mdns_remove_watch(mdns_watch, NULL, NULL);
    gensio_free_mdns(mdns, mdns_freed, &cb_data);

    o->wait(cb_data.wait, 1, NULL);

    if (cb_data.err) {
	fprintf(stderr, "Error looking up %s with mdns: %s\n",
		name, gensio_err_to_str(cb_data.err));
	return cb_data.err;
    }

    *transport = cb_data.transport;
    *telnet = cb_data.telnet;
    return 0;
}

int
main(int argc, char *argv[])
{
    int arg, rv;
    struct gensio_waiter *closewaiter;
    unsigned int closecount = 0;
    int escape_char = -1;
    struct gensio_os_funcs *o;
    struct ioinfo_sub_handlers *sh1 = NULL, *sh2 = NULL;
    void *subdata1 = NULL, *subdata2 = NULL;
    struct ioinfo *ioinfo1, *ioinfo2;
    struct gdata userdata1, userdata2;
    char *s;
    int err;
    char *do_telnet = "";
    unsigned int use_telnet = 0;
    char *CAdirspec = NULL, *certfilespec = NULL, *keyfilespec = NULL;
    char *service;
    gensiods service_len, len;
    const char *transport = "sctp";
    bool user_transport = false, mdns_transport = false;
    bool notcp = false, nosctp = true;
    const char *muxstr = "mux,";
    bool use_mux = true;
    struct sigaction sigact;
    const char *addr;
    const char *iptype = ""; /* Try both IPv4 and IPv6 by default. */
    const char *mdns_type = NULL;

    localport_err = pr_localport;

    memset(&userdata1, 0, sizeof(userdata1));
    memset(&userdata2, 0, sizeof(userdata2));
    userdata2.interactive = true;
    memset(&sigact, 0, sizeof(sigact));

    rv = gensio_default_os_hnd(0, &o);
    if (rv) {
	fprintf(stderr, "Could not allocate OS handler: %s\n",
		gensio_err_to_str(rv));
	return 1;
    }

    if (pipe(winch_pipe) == -1) {
	perror("Unable to allocate SIGWINCH pipe");
	return 1;
    }
    if (fcntl(winch_pipe[0], F_SETFL, O_NONBLOCK) == -1) {
	perror("Unable to set nonblock on SIGWINCH pipe[0]");
	return 1;
    }
    if (fcntl(winch_pipe[1], F_SETFL, O_NONBLOCK) == -1) {
	perror("Unable to set nonblock on SIGWINCH pipe[1]");
	return 1;
    }

    sigact.sa_handler = handle_sigwinch;
    err = sigaction(SIGWINCH, &sigact, NULL);
    if (err) {
	perror("Unable to setup SIGWINCH");
	return 1;
    }

    progname = argv[0];

    if (isatty(0)) {
	escape_char = 0x1c; /* ^\ */
	userdata1.ios = io1_default_tty;
    } else {
	userdata1.ios = io1_default_notty;
	userdata2.interactive = false;
    }

    for (arg = 1; arg < argc; arg++) {
	if (argv[arg][0] != '-')
	    break;
	if (strcmp(argv[arg], "--") == 0) {
	    arg++;
	    break;
	}
	if ((rv = cmparg(argc, argv, &arg, "-i", "--keyfile", &keyfile))) {
	    if (!certfile) {
		char *dotpos = strrchr(keyfile, '.');

		if (dotpos)
		    *dotpos = '\0';
		certfile = alloc_sprintf("%s.crt", keyfile);
		if (dotpos)
		    *dotpos = '.';
		if (!certfile) {
		    fprintf(stderr, "Unable to allocate memory for certfile\n");
		    exit(1);
		}
	    }
	} else if ((rv = cmparg_int(argc, argv, &arg, "-p", "--port",
				    &port))) {
	    ;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--certfile",
				&certfile))) {
	    ;
	} else if ((rv = cmparg_int(argc, argv, &arg, "-e", "--escchar",
			     &escape_char))) {
	    ;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--nomux", NULL))) {
	    muxstr = "";
	    use_mux = false;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--notcp", NULL))) {
	    notcp = true;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--nosctp", NULL))) {
	    nosctp = true;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--sctp", NULL))) {
	    nosctp = false;
	} else if ((rv = cmparg(argc, argv, &arg, "-r", "--telnet", NULL))) {
	    do_telnet = "telnet(rfc2217),";
	    use_telnet = 1;
	} else if ((rv = cmparg(argc, argv, &arg, "", "--transport",
				&transport))) {
	    user_transport = true;
	    nosctp = true;
	    notcp = true;
	} else if ((rv = cmparg(argc, argv, &arg, "-m", "--mdns", NULL))) {
	    mdns_transport = true;
	    user_transport = true;
	    nosctp = true;
	    notcp = true;
	} else if ((rv = cmparg(argc, argv, &arg, NULL, "--mdns-type",
				&mdns_type))) {
	    ;
	} else if ((rv = cmparg(argc, argv, &arg, "-L", NULL, &addr))) {
	    rv = handle_port(o, false, addr);
	} else if ((rv = cmparg(argc, argv, &arg, "-R", NULL, &addr))) {
	    rv = handle_port(o, true, addr);
	} else if ((rv = cmparg(argc, argv, &arg, "-4", NULL, NULL))) {
	    iptype = "ipv4,";
	} else if ((rv = cmparg(argc, argv, &arg, "-6", NULL, NULL))) {
	    iptype = "ipv6,";
	} else if ((rv = cmparg(argc, argv, &arg, "-d", "--debug", NULL))) {
	    debug++;
	    if (debug > 1)
		gensio_set_log_mask(GENSIO_LOG_MASK_ALL);
	} else if ((rv = cmparg(argc, argv, &arg, "-h", "--help", NULL))) {
	    help(0);
	} else {
	    fprintf(stderr, "Unknown argument: %s\n", argv[arg]);
	    help(1);
	}
	if (rv < 0)
	    return 1;
    }

    if (nosctp && notcp && !user_transport) {
	fprintf(stderr, "You cannot disable both TCP and SCTP\n");
	exit(1);
    }

    if (nosctp && !user_transport)
	transport = "tcp";

    if (!!certfile != !!keyfile) {
	fprintf(stderr,
		"If you specify a certfile, you must specify a keyfile\n");
	exit(1);
    }

    if (arg >= argc) {
	fprintf(stderr, "No string given to connect to\n");
	help(1);
    }

#if defined(HAVE_PRCTL) && defined(PR_SET_DUMPABLE)
    if (!debug) {
	if (prctl(PR_SET_DUMPABLE, 0) != 0) {
	    fprintf(stderr,
		    "Unable to disable ptrace attach, giving up.\n");
	    exit(1);
	}
    }
#endif

    s = strrchr(argv[arg], '@');
    if (s) {
	*s++ = '\0';
	username = argv[arg];
	hostname = s;
    } else {
	struct passwd *pw = getpwuid(getuid());

	if (!pw) {
	    fprintf(stderr, "no username given, and can't look up UID\n");
	    return 1;
	}
	username = strdup(pw->pw_name);
	if (!username) {
	    fprintf(stderr, "out of memory allocating username\n");
	    return 1;
	}
	hostname = argv[arg];
    }

    arg++;
    if (arg < argc) {
	int i;
	unsigned int len = 0;

	/* User gave us a remote program. */
	for (i = arg; i < argc; i++)
	    len += strlen(argv[i]) + 1; /* Extra space for nil at end */
	len += 9; /* Space for "program:" and final nil. */
	/* Note that ending '\0' is handled by final space. */

	service = malloc(len);
	if (!service) {
	    fprintf(stderr, "Unable to allocate remote program request\n");
	    return 1;
	}
	strcpy(service, "program:");
	len = 8;
	for (i = arg; i < argc; i++) {
	    len += sprintf(service + len, "%s", argv[i]);
	    service[len++] = '\0';
	}
	service[len++] = '\0';

	userdata1.ios = io1_default_notty;
	userdata2.interactive = false;
	service_len = len;
    } else {
	char *termvar = getenv("TERM");
	unsigned int len = 0;

	if (termvar) {
	    len = 6 + 5 + strlen(termvar) + 2;
	    service = malloc(len);
	    if (!service) {
		fprintf(stderr, "Unable to allocate remote login request\n");
		return 1;
	    }
	    len = sprintf(service, "login:TERM=%s", termvar);
	    service[len++] = '\0';
	    service[len++] = '\0';
	} else {
	    service = malloc(6 + 1);
	    if (!service) {
		fprintf(stderr, "Unable to allocate remote login request\n");
		return 1;
	    }
	    len = sprintf(service, "login:");
	    service[len++] = '\0';
	}
	service_len = len;
    }

    if (!tlssh_dir) {
	const char *home = getenv("HOME");

	if (!home) {
	    fprintf(stderr, "No home directory set\n");
	    return 1;
	}

	tlssh_dir = alloc_sprintf("%s/.gtlssh", home);
	if (!tlssh_dir) {
	    fprintf(stderr, "Out of memory allocating gtlssh dir\n");
	    return 1;
	}
    }

    err = checkout_file(tlssh_dir, true, true);
    if (err)
	return 1;

    o->vlog = do_vlog;

    userdata1.o = o;
    userdata2.o = o;

    userdata1.waiter = o->alloc_waiter(o);
    if (!userdata1.waiter) {
	fprintf(stderr, "Could not allocate OS waiter\n");
	return 1;
    }
    userdata2.waiter = userdata1.waiter;

    closewaiter = o->alloc_waiter(o);
    if (!closewaiter) {
	fprintf(stderr, "Could not allocate close waiter\n");
	return 1;
    }

    subdata1 = alloc_ser_ioinfo(0, "", &sh1);
    if (!subdata1) {
	fprintf(stderr, "Could not allocate subdata 1\n");
	return 1;
    }
    subdata2 = alloc_ser_ioinfo(0, "", &sh2);
    if (!subdata2) {
	fprintf(stderr, "Could not allocate subdata 2\n");
	return 1;
    }

    ioinfo1 = alloc_ioinfo(o, escape_char, sh1, subdata1, &guh, &userdata1);
    if (!ioinfo1) {
	fprintf(stderr, "Could not allocate ioinfo 1\n");
	return 1;
    }

    ioinfo2 = alloc_ioinfo(o, -1, sh2, subdata2, &guh, &userdata2);
    if (!ioinfo2) {
	fprintf(stderr, "Could not allocate ioinfo 2\n");
	return 1;
    }

    ioinfo_set_otherioinfo(ioinfo1, ioinfo2);

    rv = str_to_gensio(userdata1.ios, o, NULL, ioinfo1, &userdata1.io);
    if (rv) {
	fprintf(stderr, "Could not allocate %s: %s\n",
		userdata1.ios, gensio_err_to_str(rv));
	return 1;
    }

    userdata1.user_io = userdata1.io;
    userdata2.user_io = userdata1.io;

    err = lookup_certfiles(tlssh_dir, username, hostname, port,
			   &CAdirspec, &certfilespec, &keyfilespec);
    if (err)
	return 1;

    if (mdns_transport) {
	bool ltelnet;
	err = lookup_mdns_transport(o, hostname, mdns_type, iptype,
				    &transport, &ltelnet);
	if (err)
	    return 1;
	if (ltelnet) {
	    do_telnet = "telnet(rfc2217),";
	    use_telnet = 1;
	}
    }

 retry:
    if (user_transport)
	s = alloc_sprintf("%s%scertauth(enable-password,username=%s%s%s),"
			  "ssl(%s),%s",
			  do_telnet, muxstr, username, certfilespec,
			  keyfilespec, CAdirspec, transport);
    else
	s = alloc_sprintf("%s%scertauth(enable-password,username=%s%s%s),"
			  "ssl(%s),%s,%s%s,%d",
			  do_telnet, muxstr, username, certfilespec,
			  keyfilespec, CAdirspec,
			  transport, iptype, hostname, port);
    if (!s) {
	fprintf(stderr, "out of memory allocating IO string\n");
	return 1;
    }
    userdata2.ios = s;

    rv = str_to_gensio(userdata2.ios, o, auth_event, ioinfo2, &userdata2.io);
    if (rv) {
	fprintf(stderr, "Could not allocate %s: %s\n", userdata2.ios,
		gensio_err_to_str(rv));
	return 1;
    }

    if (use_mux) {
	len = 4;
	rv = gensio_control(userdata2.io, 1 + use_telnet, false,
			    GENSIO_CONTROL_SERVICE, "mux", &len);
	if (rv) {
	    fprintf(stderr, "Could not set mux service %s: %s\n",
		    userdata2.ios, gensio_err_to_str(rv));
	    return 1;
	}
    }

    rv = gensio_control(userdata2.io, 0 + use_telnet, false,
			GENSIO_CONTROL_SERVICE, service, &service_len);
    if (rv) {
	fprintf(stderr, "Could not set service %s: %s\n", userdata2.ios,
		gensio_err_to_str(rv));
	return 1;
    }

    if (userdata2.interactive) {
	rv = gensio_control(userdata2.io, GENSIO_CONTROL_DEPTH_ALL, false,
			    GENSIO_CONTROL_NODELAY, "1", NULL);
	if (rv) {
	    fprintf(stderr, "Could not set nodelay on %s: %s\n", userdata2.ios,
		    gensio_err_to_str(rv));
	    return 1;
	}
    }

    userdata2.can_close = true;
    rv = gensio_open_s(userdata2.io);
    if (rv) {
	userdata2.can_close = false;
	fprintf(stderr, "Could not open %s: %s\n", userdata2.ios,
		gensio_err_to_str(rv));
	if (!notcp && !user_transport && strcmp(transport, "sctp") == 0) {
	    fprintf(stderr, "Falling back to tcp\n");
	    free(userdata2.ios);
	    userdata2.ios = NULL;
	    transport = "tcp";
	    goto retry;
	}
	goto closeit;
    }

    if (mdns_transport)
	o->free(o, (char *) transport);

    userdata1.can_close = true;
    rv = gensio_open_s(userdata1.io);
    if (rv) {
	userdata1.can_close = false;
	fprintf(stderr, "Could not open %s: %s\n",
		userdata1.ios, gensio_err_to_str(rv));
	goto closeit;
    }

    ioinfo_set_ready(ioinfo1, userdata1.io);
    ioinfo_set_ready(ioinfo2, userdata2.io);

    rv = o->set_fd_handlers(o, winch_pipe[0], ioinfo2, winch_ready,
			    NULL, NULL, NULL);
    if (rv) {
	fprintf(stderr, "Could not set SIGWINCH fd handler: %s\n",
		gensio_err_to_str(rv));
	return 1;
    }
    o->set_read_handler(o, winch_pipe[0], true);

    start_local_ports(userdata2.io);
    start_remote_ports(ioinfo2);

    o->wait(userdata1.waiter, 1, NULL);

 closeit:
    free(service);

    if (userdata2.can_close) {
	rv = gensio_close(userdata2.io, io_close, closewaiter);
	if (rv)
	    printf("Unable to close %s: %s\n", userdata2.ios,
		   gensio_err_to_str(rv));
	else
	    closecount++;
    }

    if (userdata1.can_close) {
	rv = gensio_close(userdata1.io, io_close, closewaiter);
	if (rv)
	    printf("Unable to close %s: %s\n", userdata1.ios,
		   gensio_err_to_str(rv));
	else
	    closecount++;
    }

    if (closecount > 0)
	o->wait(closewaiter, closecount, NULL);

    if (CAdirspec)
	free(CAdirspec);
    if (certfilespec)
	free(certfilespec);
    if (keyfilespec)
	free(keyfilespec);

    gensio_free(userdata1.io);
    gensio_free(userdata2.io);

    if (userdata2.ios)
	free(userdata2.ios);
    o->free_waiter(closewaiter);
    o->free_waiter(userdata1.waiter);

    free_ioinfo(ioinfo1);
    free_ioinfo(ioinfo2);
    free_ser_ioinfo(subdata1);
    free_ser_ioinfo(subdata2);

    return 0;
}
