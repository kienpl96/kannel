/* conn.c - implement Connection type
 *
 * This file implements the interface defined in conn.h.
 *
 * Richard Braakman
 */

/* TODO: unlocked_close() on error */
/* TODO: have I/O functions check if connection is open */
/* TODO: have conn_open_tcp do a non-blocking connect() */

#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>

#include "gwlib/gwlib.h"

/*
 * This used to be 4096.  It is now 0 so that callers don't have to
 * deal with the complexities of buffering (i.e. deciding when to
 * flush) unless they want to.
 * FIXME: Figure out how to combine buffering sensibly with use of
 * conn_register.
 */
#define DEFAULT_OUTPUT_BUFFERING 0

struct Connection {
	/* We use separate locks for input and ouput fields, so that
	 * read and write activities don't have to get in each other's
	 * way.  If you need both, then acquire the outlock first. */
	Mutex *inlock;
	Mutex *outlock;
	volatile sig_atomic_t claimed;
#ifndef NO_GWASSERT
	long claiming_thread;
#endif

	/* fd value is read-only and is not locked */
	int fd;

	/* Protected by outlock */
	Octstr *outbuf;
	long outbufpos;  /* start of unwritten data in outbuf */

	/* Try to buffer writes until there are this many octets to send.
	 * Set it to 0 to get an unbuffered connection. */
	unsigned int output_buffering;

	/* Protected by inlock */
	Octstr *inbuf;
	long inbufpos;   /* start of unread data in inbuf */

	int read_eof;    /* we encountered eof on read */
	int read_error;  /* we encountered error on read */

	/* Protected by both locks when updating, so you need only one
	 * of the locks when reading. */
        FDSet *registered;
        conn_callback_t *callback;
        void *callback_data;
	/* Protected by inlock */
	int listening_pollin;
	/* Protected by outlock */
	int listening_pollout;
};

static void unlocked_register_pollin(Connection *conn, int onoff);
static void unlocked_register_pollout(Connection *conn, int onoff);

/* There are a number of functions that play with POLLIN and POLLOUT flags.
 * The general rule is that we always want to poll for POLLIN except when
 * we have detected eof (which may be reported as eternal POLLIN), and
 * we want to poll for POLLOUT only if there's data waiting in the
 * output buffer.  If output buffering is set, we may not want to poll for
 * POLLOUT if there's not enough data waiting, which is why we have
 * unlocked_try_write. */

/* Lock a Connection's read direction, if the Connection is unclaimed */
static void lock_in(Connection *conn) {
	gw_assert(conn != NULL);

	if (conn->claimed)
		gw_assert(gwthread_self() == conn->claiming_thread);
	else
		mutex_lock(conn->inlock);
}

/* Unlock a Connection's read direction, if the Connection is unclaimed */
static void unlock_in(Connection *conn) {
	gw_assert(conn != NULL);

	if (!conn->claimed)
		mutex_unlock(conn->inlock);
}

/* Lock a Connection's write direction, if the Connection is unclaimed */
static void lock_out(Connection *conn) {
	gw_assert(conn != NULL);

	if (conn->claimed)
		gw_assert(gwthread_self() == conn->claiming_thread);
	else
		mutex_lock(conn->outlock);
}

/* Unlock a Connection's write direction, if the Connection is unclaimed */
static void unlock_out(Connection *conn) {
	gw_assert(conn != NULL);

	if (!conn->claimed)
		mutex_unlock(conn->outlock);
}

/* Return the number of bytes in the Connection's output buffer */
static long unlocked_outbuf_len(Connection *conn) {
	return octstr_len(conn->outbuf) - conn->outbufpos;
}

/* Return the number of bytes in the Connection's input buffer */
static long unlocked_inbuf_len(Connection *conn) {
	return octstr_len(conn->inbuf) - conn->inbufpos;
}

/* Send as much data as can be sent without blocking.  Return the number
 * of bytes written, or -1 in case of error. */
static long unlocked_write(Connection *conn) {
	long ret;

	ret = octstr_write_data(conn->outbuf, conn->fd, conn->outbufpos);

	if (ret < 0)
		return -1;

	conn->outbufpos += ret;

	/* Heuristic: Discard the already-written data if it's more than
	 * half of the total.  This should keep the buffer size small
	 * without wasting too many cycles on moving data around. */
	if (conn->outbufpos > octstr_len(conn->outbuf) / 2) {
		octstr_delete(conn->outbuf, 0, conn->outbufpos);
		conn->outbufpos = 0;
	}

        if (conn->registered)
		unlocked_register_pollout(conn, unlocked_outbuf_len(conn) > 0);

	return ret;
}

/* Try to empty the output buffer without blocking.  Return 0 for success,
 * 1 if there is still data left in the buffer, and -1 for errors. */
static int unlocked_try_write(Connection *conn) {
	long len;

	len = unlocked_outbuf_len(conn);
	if (len == 0)
		return 0;

	if (len < (long) conn->output_buffering)
		return 1;

	if (unlocked_write(conn) < 0)
		return -1;

	if (unlocked_outbuf_len(conn) > 0)
		return 1;

	return 0;
}

/* Read whatever data is currently available, up to an internal maximum. */
static void unlocked_read(Connection *conn) {
	unsigned char buf[4096];
	long len;

	if (conn->inbufpos > 0) {
		octstr_delete(conn->inbuf, 0, conn->inbufpos);
		conn->inbufpos = 0;
	}

	len = read(conn->fd, buf, sizeof(buf));
	if (len < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		error(errno, "Error reading from fd %d:", conn->fd);
		conn->read_error = 1;
		if (conn->registered)
			unlocked_register_pollin(conn, 0);
		return;
	} else if (len == 0) {
		conn->read_eof = 1;
		if (conn->registered)
			unlocked_register_pollin(conn, 0);
	} else {
		octstr_append_data(conn->inbuf, buf, len);
	}
}

/* Cut "length" octets from the input buffer and return them as an Octstr */
static Octstr *unlocked_get(Connection *conn, long length) {
	Octstr *result = NULL;

	gw_assert(unlocked_inbuf_len(conn) >= length);
	result = octstr_copy(conn->inbuf, conn->inbufpos, length);
	conn->inbufpos += length;

	return result;
}

/* Tell the fdset whether we are interested in POLLIN events, but only
 * if the status changed.  (Calling fdset_listen can be expensive if
 * it requires synchronization with the polling thread.)
 * We must already have the inlock.
 */
static void unlocked_register_pollin(Connection *conn, int onoff) {
	gw_assert(conn->registered);

	if (onoff == 1 && !conn->listening_pollin) {
		/* Turn it on */
		conn->listening_pollin = 1;
		fdset_listen(conn->registered, conn->fd, POLLIN, POLLIN);
	} else if (onoff == 0 && conn->listening_pollin) {
		/* Turn it off */
		conn->listening_pollin = 0;
		fdset_listen(conn->registered, conn->fd, POLLIN, 0);
	}
}

/* Tell the fdset whether we are interested in POLLOUT events, but only
 * if the status changed.  (Calling fdset_listen can be expensive if
 * it requires synchronization with the polling thread.)
 * We must already have the outlock.
 */
static void unlocked_register_pollout(Connection *conn, int onoff) {
	gw_assert(conn->registered);

	if (onoff == 1 && !conn->listening_pollout) {
		/* Turn it on */
		conn->listening_pollout = 1;
		fdset_listen(conn->registered, conn->fd, POLLOUT, POLLOUT);
	} else if (onoff == 0 && conn->listening_pollout) {
		/* Turn it off */
		conn->listening_pollout = 0;
		fdset_listen(conn->registered, conn->fd, POLLOUT, 0);
	}
}

Connection *conn_open_tcp(Octstr *host, int port) {
	int sockfd;

	sockfd = tcpip_connect_to_server(octstr_get_cstr(host), port);
	if (sockfd < 0)
		return NULL;

	return conn_wrap_fd(sockfd);
}

Connection *conn_wrap_fd(int fd) {
	Connection *conn;

	if (socket_set_blocking(fd, 0) < 0)
		return NULL;

	conn = gw_malloc(sizeof(*conn));
	conn->inlock = mutex_create();
	conn->outlock = mutex_create();
	conn->claimed = 0;

	conn->outbuf = octstr_create("");
	conn->outbufpos = 0;
	conn->inbuf = octstr_create("");
	conn->inbufpos = 0;

	conn->fd = fd;
	conn->read_eof = 0;
	conn->read_error = 0;
	conn->output_buffering = DEFAULT_OUTPUT_BUFFERING;

	conn->registered = NULL;
	conn->callback = NULL;
	conn->callback_data = NULL;
	conn->listening_pollin = 0;
	conn->listening_pollout = 0;

	return conn;
}

void conn_destroy(Connection *conn) {
	int ret;

	if (conn == NULL)
		return;

	/* No locking done here.  conn_destroy should not be called
	 * if any thread might still be interested in the connection. */

	if (conn->registered)
		fdset_unregister(conn->registered, conn->fd);

	if (conn->fd >= 0) {
		/* Try to flush any remaining data */
		unlocked_write(conn);
		ret = close(conn->fd);
		if (ret < 0)
			error(errno, "conn_destroy: error on close");
		conn->fd = -1;
	}

	octstr_destroy(conn->outbuf);
	octstr_destroy(conn->inbuf);
	mutex_destroy(conn->inlock);
	mutex_destroy(conn->outlock);

	gw_free(conn);
}

void conn_claim(Connection *conn) {
	gw_assert(conn != NULL);

	if (conn->claimed)
		panic(0, "Connection is being claimed twice!");
	conn->claimed = 1;
#ifndef NO_GWASSERT
	conn->claiming_thread = gwthread_self();
#endif
}

long conn_outbuf_len(Connection *conn) {
	long len;

	lock_out(conn);
	len = unlocked_outbuf_len(conn);
	unlock_out(conn);

	return len;
}

long conn_inbuf_len(Connection *conn) {
	long len;

	lock_in(conn);
	len = unlocked_inbuf_len(conn);
	unlock_in(conn);

	return len;
}

int conn_eof(Connection *conn) {
	int eof;

	lock_in(conn);
	eof = conn->read_eof;
	unlock_in(conn);

	return eof;
}

int conn_read_error(Connection *conn) {
	int err;

	lock_in(conn);
	err = conn->read_error;
	unlock_in(conn);

	return err;
}

void conn_set_output_buffering(Connection *conn, unsigned int size) {
	lock_out(conn);
	conn->output_buffering = size;
	/* If the buffer size is smaller, we may have to write immediately. */
	unlocked_try_write(conn);
	unlock_out(conn);
}

static void poll_callback(int fd, int revents, void *data) {
	Connection *conn;

	conn = data;

	if (conn == NULL) {
		error(0, "poll_callback called with NULL connection.");
		return;
	}

	if (conn->fd != fd) {
		error(0, "poll_callback called on wrong connection.");
		return;
	}

	/* If unlocked_write manages to write all pending data, it will
	 * tell the fdset to stop listening for POLLOUT. */
	if (revents & POLLOUT) {
		lock_out(conn);
		unlocked_write(conn);
		unlock_out(conn);
	}

	/* If unlocked_read hits eof or error, it will tell the fdset to
         * stop listening for POLLIN. */
	if (revents & POLLIN) {
		lock_in(conn);
		unlocked_read(conn);
		unlock_in(conn);
		conn->callback(conn, conn->callback_data);
	}
}

int conn_register(Connection *conn, FDSet *fdset,
		  conn_callback_t callback, void *data) {
	int events;
	int result = 0;

	gw_assert(conn != NULL);

	if (conn->fd < 0)
		return -1;

	/* We need both locks if we want to update the registration
	 * information. */
	lock_out(conn);
	lock_in(conn);

        if (conn->registered == fdset) {
            /* Re-registering.  Change only the callback info. */
	    conn->callback = callback;
            conn->callback_data = data;
            result = 0;
        } else if (conn->registered) {
            /* Already registered to a different fdset. */
            result = -1;
        } else {
	    events = 0;
  	    if (conn->read_eof == 0 && conn->read_error == 0)
		    events |= POLLIN;
            if (unlocked_outbuf_len(conn) > 0)
            	events |= POLLOUT;
    
            conn->registered = fdset;
            conn->callback = callback;
            conn->callback_data = data;
            conn->listening_pollin = (events & POLLIN) != 0;
            conn->listening_pollout = (events & POLLOUT) != 0;
            fdset_register(fdset, conn->fd, events, poll_callback, conn);
            result = 0;
        }

        unlock_in(conn);
        unlock_out(conn);

	return result;
}

void conn_unregister(Connection *conn) {
	gw_assert(conn != NULL);

	if (conn->fd < 0)
		return;

	/* We need both locks to update the registration information */
	lock_out(conn);
	lock_in(conn);

	if (conn->registered) {
		fdset_unregister(conn->registered, conn->fd);
		conn->registered = NULL;
		conn->callback = NULL;
		conn->callback_data = NULL;
		conn->listening_pollin = 0;
		conn->listening_pollout = 0;
	}

	unlock_in(conn);
	unlock_out(conn);
}

int conn_wait(Connection *conn, double seconds) {
	int events;
	int ret;
	int fd;

	lock_out(conn);

	/* Try to write any data that might still be waiting to be sent */
	ret = unlocked_write(conn);
	if (ret < 0) {
		unlock_out(conn);
		return -1;
	}
	if (ret > 0) {
		/* We did something useful.  No need to poll or wait now. */
		unlock_out(conn);
		return 0;
	}

	fd = conn->fd;

	/* Normally, we block until there is more data available.  But
	 * if any data still needs to be sent, we block until we can
	 * send it (or there is more data available).  We always block
	 * for reading, unless we know there is no more data coming.
	 * (Because in that case, poll will keep reporting POLLIN to
	 * signal the end of the file).  If the caller explicitly wants
	 * to wait even though there is no data to write and we're at
	 * end of file, then poll for new data anyway because the caller
	 * apparently doesn't trust eof. */
	events = 0;
	if (unlocked_outbuf_len(conn) > 0)
		events |= POLLOUT;
	/* Don't keep the connection locked while we wait */
	unlock_out(conn);

	/* We need the in lock to query read_eof */
	lock_in(conn);
	if ((conn->read_eof == 0 && conn->read_error == 0) || events == 0)
		events |= POLLIN;
	unlock_in(conn);

	ret = gwthread_pollfd(fd, events, seconds);
	if (ret < 0) {
		if (errno == EINTR)
			return 0;
		error(0, "conn_wait: poll failed on fd %d:", fd);
		return -1;
	}

	if (ret == 0)
		return 1;

	if (ret & POLLNVAL) {
		error(0, "conn_wait: fd %d not open.", fd);
		return -1;
	}

	if (ret & (POLLERR | POLLHUP)) {
		/* Call unlocked_read to report the specific error,
		 * and handle the results of the error.  We can't be
		 * certain that the error still exists, because we
		 * released the lock for a while. */
		lock_in(conn);
		unlocked_read(conn);
		unlock_in(conn);
		return -1;
	}

	/* If POLLOUT is on, then we must have wanted
	 * to write something. */
	if (ret & POLLOUT) {
		lock_out(conn);
		unlocked_write(conn);
		unlock_out(conn);
	}

	/* Since we normally select for reading, we must
	 * try to read here.  Otherwise, if the caller loops
	 * around conn_wait without making conn_read* calls
	 * in between, we will keep polling this same data. */
	if (ret & POLLIN) {
		lock_in(conn);
		unlocked_read(conn);
		unlock_in(conn);
	}

	return 0;
}

int conn_flush(Connection *conn) {
	int ret;
	int revents;
	int fd;

	lock_out(conn);
	ret = unlocked_write(conn);
	if (ret < 0) {
		unlock_out(conn);
		return -1;
	}

	while (unlocked_outbuf_len(conn) != 0) {
		fd = conn->fd;

		unlock_out(conn);
		revents = gwthread_pollfd(fd, POLLOUT, -1.0);

		/* Note: Make sure we have the "out" lock when
		 * going through the loop again, because the 
		 * loop condition needs it. */

		if (revents < 0) {
			if (errno == EINTR)
				return 1;
			error(0, "conn_flush: poll failed on fd %d:", fd);
			return -1;
		}

		if (revents == 0) {
			/* We were woken up */
			return 1;
		}

		if (revents & POLLNVAL) {
			error(0, "conn_flush: fd %d not open.", fd);
			return -1;
		}

		lock_out(conn);

		if (revents & (POLLOUT | POLLERR | POLLHUP)) {
			ret = unlocked_write(conn);
			if (ret < 0) {
				unlock_out(conn);
				return -1;
			}
		}
	}

	return 0;
}

int conn_write(Connection *conn, Octstr *data) {
	int ret;

	lock_out(conn);
	octstr_append(conn->outbuf, data);
	ret = unlocked_try_write(conn);
	unlock_out(conn);

	return ret;
}

int conn_write_data(Connection *conn, unsigned char *data, long length) {
	int ret;

	lock_out(conn);
	octstr_append_data(conn->outbuf, data, length);
	ret = unlocked_try_write(conn);
	unlock_out(conn);

	return ret;
}

int conn_write_withlen(Connection *conn, Octstr *data) {
	int ret;
	unsigned char lengthbuf[4];

	encode_network_long(lengthbuf, octstr_len(data));
	lock_out(conn);
	octstr_append_data(conn->outbuf, lengthbuf, 4);
	octstr_append(conn->outbuf, data);
	ret = unlocked_try_write(conn);
	unlock_out(conn);

	return ret;
}

Octstr *conn_read_everything(Connection *conn) {
	Octstr *result = NULL;

	lock_in(conn);
	if (unlocked_inbuf_len(conn) == 0) {
		unlocked_read(conn);
		if (unlocked_inbuf_len(conn) == 0) {
			unlock_in(conn);
			return NULL;
		}
	}

	result = unlocked_get(conn, unlocked_inbuf_len(conn));
	unlock_in(conn);

	return result;
}

Octstr *conn_read_fixed(Connection *conn, long length) {
	Octstr *result = NULL;

	/* See if the data is already available.  If not, try a read(),
	 * then see if we have enough data after that.  If not, give up. */
	lock_in(conn);
	if (unlocked_inbuf_len(conn) < length) {
		unlocked_read(conn);
		if (unlocked_inbuf_len(conn) < length) {
			unlock_in(conn);
			return NULL;
		}
	}
	result = unlocked_get(conn, length);
	unlock_in(conn);

	return result;
}

Octstr *conn_read_line(Connection *conn) {
	Octstr *result = NULL;
	long pos;

	lock_in(conn);
	/* 10 is the code for linefeed.  We don't rely on \n because that
	 * might be a different value on some (strange) systems, and
	 * we are reading from a network connection. */
	pos = octstr_search_char(conn->inbuf, 10, conn->inbufpos);
	if (pos < 0) {
		unlocked_read(conn);
		pos = octstr_search_char(conn->inbuf,
					10, conn->inbufpos);
		if (pos < 0) {
			unlock_in(conn);
			return NULL;
		}
	}
		
	result = unlocked_get(conn, pos - conn->inbufpos);

	/* Skip the LF, which we left in the buffer */
	conn->inbufpos++;

	/* If the line was terminated with CR LF, we have to remove
	 * the CR from the result. */
	if (octstr_len(result) > 0 &&
	    octstr_get_char(result, octstr_len(result) - 1) == 13)
		octstr_delete(result, octstr_len(result) - 1, 1);

	unlock_in(conn);
	return result;
}

Octstr *conn_read_withlen(Connection *conn) {
	Octstr *result = NULL;
	unsigned char lengthbuf[4];
	long length;
	int try;

	lock_in(conn);

	for (try = 1; try <= 2; try++) {
		if (try > 1)
			unlocked_read(conn);

	retry:
		/* First get the length. */
		if (unlocked_inbuf_len(conn) < 4)
			continue;
	
		octstr_get_many_chars(lengthbuf, conn->inbuf,
					conn->inbufpos, 4);
		length = decode_network_long(lengthbuf);

		if (length < 0) {
			warning(0, "conn_read_withlen: "
				"got negative length, skipping");
			conn->inbufpos += 4;
			goto retry;
		}

		/* Then get the data. */
		if (unlocked_inbuf_len(conn) - 4 < length)
			continue;

		conn->inbufpos += 4;
		result = unlocked_get(conn, length);
		break;
	}

	unlock_in(conn);
	return result;
}

Octstr *conn_read_packet(Connection *conn, int startmark, int endmark) {
	int startpos, endpos;
	Octstr *result = NULL;
	int try;

	lock_in(conn);

	for (try = 1; try <= 2; try++) {
		if (try > 1)
			unlocked_read(conn);

		/* Find startmark, and discard everything up to it */
		startpos = octstr_search_char(conn->inbuf, startmark,
				conn->inbufpos);
		if (startpos < 0) {
			conn->inbufpos = octstr_len(conn->inbuf);
			continue;
		} else {
			conn->inbufpos = startpos;
		}

		/* Find first endmark after startmark */
		endpos = octstr_search_char(conn->inbuf, endmark,
				conn->inbufpos);
		if (endpos < 0)
			continue;

		result = unlocked_get(conn, endpos - startpos + 1);
		break;
	}

	unlock_in(conn);
	return result;
}
