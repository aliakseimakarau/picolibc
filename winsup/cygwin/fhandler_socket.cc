/* fhandler_socket.cc. See fhandler.h for a description of the fhandler classes.

   Copyright 2000, 2001, 2002, 2003, 2004, 2005 Red Hat, Inc.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

/* #define DEBUG_NEST_ON 1 */

#define  __INSIDE_CYGWIN_NET__

#include "winsup.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <asm/byteorder.h>

#include <stdlib.h>
#define USE_SYS_TYPES_FD_SET
#include <winsock2.h>
#include <iphlpapi.h>
#include "cygerrno.h"
#include "security.h"
#include "cygwin/version.h"
#include "perprocess.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "shared_info.h"
#include "sigproc.h"
#include "cygthread.h"
#include "select.h"
#include "wininfo.h"
#include <unistd.h>
#include <sys/acl.h>
#include "cygtls.h"
#include "cygwin/in6.h"

#define ASYNC_MASK (FD_READ|FD_WRITE|FD_OOB|FD_ACCEPT|FD_CONNECT)
#define EVENT_MASK (FD_READ|FD_WRITE|FD_OOB|FD_ACCEPT|FD_CONNECT|FD_CLOSE)

extern bool fdsock (cygheap_fdmanip& fd, const device *, SOCKET soc);
extern "C" {
int sscanf (const char *, const char *, ...);
} /* End of "C" section */

fhandler_dev_random* entropy_source;

/* cygwin internal: map sockaddr into internet domain address */
static int
get_inet_addr (const struct sockaddr *in, int inlen,
	       struct sockaddr_storage *out, int *outlen,
	       int *type = NULL, int *secret = NULL)
{
  int secret_buf [4];
  int* secret_ptr = (secret ? : secret_buf);

  if (in->sa_family == AF_INET || in->sa_family == AF_INET6)
    {
      memcpy (out, in, inlen);
      *outlen = inlen;
      return 1;
    }
  else if (in->sa_family == AF_LOCAL)
    {
      path_conv pc (in->sa_data, PC_SYM_FOLLOW);
      if (pc.error)
	{
	  set_errno (pc.error);
	  return 0;
	}
      if (!pc.exists ())
	{
	  set_errno (ENOENT);
	  return 0;
	}
      if (!pc.issocket ())
	{
	  set_errno (EBADF);
	  return 0;
	}
      HANDLE fh = CreateFile (pc, GENERIC_READ, wincap.shared (), &sec_none,
			      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
      if (fh == INVALID_HANDLE_VALUE)
	{
	  __seterrno ();
	  return 0;
	}
      int ret = 0;
      DWORD len = 0;
      char buf[128];
      memset (buf, 0, sizeof buf);
      if (ReadFile (fh, buf, 128, &len, 0))
	{
	  struct sockaddr_in sin;
	  char ctype;
	  sin.sin_family = AF_INET;
	  sscanf (buf + strlen (SOCKET_COOKIE), "%hu %c %08x-%08x-%08x-%08x",
		  &sin.sin_port,
		  &ctype,
		  secret_ptr, secret_ptr + 1, secret_ptr + 2, secret_ptr + 3);
	  sin.sin_port = htons (sin.sin_port);
	  sin.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
	  memcpy (out, &sin, sizeof sin);
	  *outlen = sizeof sin;
	  if (type)
	    *type = (ctype == 's' ? SOCK_STREAM :
		     ctype == 'd' ? SOCK_DGRAM
				  : 0);
	  ret = 1;
	}
      else
	__seterrno ();
      CloseHandle (fh);
      return ret;
    }
  else
    {
      set_errno (EAFNOSUPPORT);
      return 0;
    }
}

/**********************************************************************/
/* fhandler_socket */

fhandler_socket::fhandler_socket () :
  fhandler_base (),
  wsock_mtx (NULL),
  wsock_evt (NULL),
  wsock_events (NULL),
  sun_path (NULL),
  status ()
{
  need_fork_fixup (true);
  prot_info_ptr = (LPWSAPROTOCOL_INFOA) cmalloc (HEAP_BUF,
						 sizeof (WSAPROTOCOL_INFOA));
#if 0
  if (pc.is_fs_special ())
    {
      fhandler_socket * fhs = (fhandler_socket *) fh;
      fhs->set_addr_family (AF_LOCAL);
      fhs->set_sun_path (posix_path);
    }
#endif
}

fhandler_socket::~fhandler_socket ()
{
  if (prot_info_ptr)
    cfree (prot_info_ptr);
  if (sun_path)
    cfree (sun_path);
}

char *
fhandler_socket::get_proc_fd_name (char *buf)
{
  __small_sprintf (buf, "socket:[%d]", get_socket ());
  return buf;
}

int
fhandler_socket::open (int flags, mode_t mode)
{
  set_errno (ENXIO);
  return 0;
}

void
fhandler_socket::af_local_set_sockpair_cred ()
{
  sec_pid = sec_peer_pid = getpid ();
  sec_uid = sec_peer_uid = geteuid32 ();
  sec_gid = sec_peer_gid = getegid32 ();
}

void
fhandler_socket::af_local_setblocking (bool &async, bool &nonblocking)
{
  async = async_io ();
  nonblocking = is_nonblocking ();
  if (async)
    {
      WSAAsyncSelect (get_socket (), winmsg, 0, 0);
      WSAEventSelect (get_socket (), wsock_evt, EVENT_MASK);
    }
  set_nonblocking (false);
  async_io (false);
}

void
fhandler_socket::af_local_unsetblocking (bool async, bool nonblocking)
{
  if (nonblocking)
    set_nonblocking (true);
  if (async)
    {
      WSAAsyncSelect (get_socket (), winmsg, WM_ASYNCIO, ASYNC_MASK);
      async_io (true);
    }
}

bool
fhandler_socket::af_local_recv_secret ()
{
  int out[4] = { 0, 0, 0, 0 };
  int rest = sizeof out;
  char *ptr = (char *) out;
  while (rest > 0)
    {
      int ret = recvfrom (ptr, rest, 0, NULL, NULL);
      if (ret <= 0)
	break;
      rest -= ret;
      ptr += ret;
    }
  if (rest == 0)
    {
      debug_printf ("Received af_local secret: %08x-%08x-%08x-%08x",
		    out[0], out[1], out[2], out[3]);
      if (out[0] != connect_secret[0] || out[1] != connect_secret[1]
	  || out[2] != connect_secret[2] || out[3] != connect_secret[3])
	{
	  debug_printf ("Receiving af_local secret mismatch");
	  return false;
	}
    }
  else
    debug_printf ("Receiving af_local secret failed");
  return rest == 0;
}

bool
fhandler_socket::af_local_send_secret ()
{
  int rest = sizeof connect_secret;
  char *ptr = (char *) connect_secret;
  while (rest > 0)
    {
      int ret = sendto (ptr, rest, 0, NULL, 0);
      if (ret <= 0)
	break;
      rest -= ret;
      ptr += ret;
    }
  debug_printf ("Sending af_local secret %s", rest == 0 ? "succeeded"
							: "failed");
  return rest == 0;
}

bool
fhandler_socket::af_local_recv_cred ()
{
  struct ucred out = { (pid_t) 0, (__uid32_t) -1, (__gid32_t) -1 };
  int rest = sizeof out;
  char *ptr = (char *) &out;
  while (rest > 0)
    {
      int ret = recvfrom (ptr, rest, 0, NULL, NULL);
      if (ret <= 0)
	break;
      rest -= ret;
      ptr += ret;
    }
  if (rest == 0)
    {
      debug_printf ("Received eid credentials: pid: %d, uid: %d, gid: %d",
		    out.pid, out.uid, out.gid);
      sec_peer_pid = out.pid;
      sec_peer_uid = out.uid;
      sec_peer_gid = out.gid;
    }
  else
    debug_printf ("Receiving eid credentials failed");
  return rest == 0;
}

bool
fhandler_socket::af_local_send_cred ()
{
  struct ucred in = { sec_pid, sec_uid, sec_gid };
  int rest = sizeof in;
  char *ptr = (char *) &in;
  while (rest > 0)
    {
      int ret = sendto (ptr, rest, 0, NULL, 0);
      if (ret <= 0)
	break;
      rest -= ret;
      ptr += ret;
    }
  if (rest == 0)
    debug_printf ("Sending eid credentials succeeded");
  else
    debug_printf ("Sending eid credentials failed");
  return rest == 0;
}

int
fhandler_socket::af_local_connect ()
{
  /* This keeps the test out of select. */
  if (get_addr_family () != AF_LOCAL || get_socket_type () != SOCK_STREAM)
    return 0;

  debug_printf ("af_local_connect called");
  bool orig_async_io, orig_is_nonblocking;
  af_local_setblocking (orig_async_io, orig_is_nonblocking);
  if (!af_local_send_secret () || !af_local_recv_secret ()
      || !af_local_send_cred () || !af_local_recv_cred ())
    {
      debug_printf ("accept from unauthorized server");
      ::shutdown (get_socket (), SD_BOTH);
      WSASetLastError (WSAECONNREFUSED);
      return -1;
    }
  af_local_unsetblocking (orig_async_io, orig_is_nonblocking);
  return 0;
}

int
fhandler_socket::af_local_accept ()
{
  debug_printf ("af_local_accept called");
  bool orig_async_io, orig_is_nonblocking;
  af_local_setblocking (orig_async_io, orig_is_nonblocking);
  if (!af_local_recv_secret () || !af_local_send_secret ()
      || !af_local_recv_cred () || !af_local_send_cred ())
    {
      debug_printf ("connect from unauthorized client");
      ::shutdown (get_socket (), SD_BOTH);
      ::closesocket (get_socket ());
      WSASetLastError (WSAECONNABORTED);
      return -1;
    }
  af_local_unsetblocking (orig_async_io, orig_is_nonblocking);
  return 0;
}

void
fhandler_socket::af_local_set_cred ()
{
  sec_pid = getpid ();
  sec_uid = geteuid32 ();
  sec_gid = getegid32 ();
  sec_peer_pid = (pid_t) 0;
  sec_peer_uid = (__uid32_t) -1;
  sec_peer_gid = (__gid32_t) -1;
}

void
fhandler_socket::af_local_copy (fhandler_socket *sock)
{
  sock->connect_secret[0] = connect_secret[0];
  sock->connect_secret[1] = connect_secret[1];
  sock->connect_secret[2] = connect_secret[2];
  sock->connect_secret[3] = connect_secret[3];
  sock->sec_pid = sec_pid;
  sock->sec_uid = sec_uid;
  sock->sec_gid = sec_gid;
  sock->sec_peer_pid = sec_peer_pid;
  sock->sec_peer_uid = sec_peer_uid;
  sock->sec_peer_gid = sec_peer_gid;
}

void
fhandler_socket::af_local_set_secret (char *buf)
{
  if (!entropy_source)
    {
      void *buf = malloc (sizeof (fhandler_dev_random));
      entropy_source = new (buf) fhandler_dev_random ();
      entropy_source->dev () = *urandom_dev;
    }
  if (entropy_source &&
      !entropy_source->open (O_RDONLY))
    {
      delete entropy_source;
      entropy_source = NULL;
    }
  if (entropy_source)
    {
      size_t len = sizeof (connect_secret);
      entropy_source->read (connect_secret, len);
      if (len != sizeof (connect_secret))
	bzero ((char*) connect_secret, sizeof (connect_secret));
    }
  __small_sprintf (buf, "%08x-%08x-%08x-%08x",
		   connect_secret [0], connect_secret [1],
		   connect_secret [2], connect_secret [3]);
}

struct wsa_event
{
  LONG serial_number;
  long events;
  int  connect_errorcode;
};

/* Maximum number of concurrently opened sockets from all Cygwin processes
   on a machine.  Note that shared sockets (through dup/fork/exec) are
   counted as one socket. */
#define NUM_SOCKS	(65536 / sizeof (wsa_event))

static wsa_event wsa_events[NUM_SOCKS] __attribute__((section (".cygwin_dll_common"), shared)) = { 0 };

static LONG socket_serial_number __attribute__((section (".cygwin_dll_common"), shared)) = 0;

static HANDLE wsa_slot_mtx;

static wsa_event *
search_wsa_event_slot (LONG new_serial_number)
{
  char name[CYG_MAX_PATH], searchname[CYG_MAX_PATH];

  if (!wsa_slot_mtx)
    {
      wsa_slot_mtx = CreateMutex (&sec_all, FALSE,
      				  shared_name (name, "sock", 0));
      if (!wsa_slot_mtx)
	api_fatal ("Couldn't create/open shared socket mutex, %E");
    }
  switch (WaitForSingleObject (wsa_slot_mtx, INFINITE))
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
      break;
    default:
      api_fatal ("WFSO failed for shared socket mutex, %E");
      break;
    }
  unsigned int slot = new_serial_number % NUM_SOCKS;
  while (wsa_events[slot].serial_number)
    {
      HANDLE searchmtx = OpenMutex (STANDARD_RIGHTS_READ, FALSE,
	    shared_name (searchname, "sock", wsa_events[slot].serial_number));
      if (!searchmtx)
        break;
      /* Mutex still exists, attached socket is active, try next slot. */
      CloseHandle (searchmtx);
      slot = (slot + 1) % NUM_SOCKS;
      if (slot == (new_serial_number % NUM_SOCKS))
        {
	  /* Did the whole array once.   Too bad. */
	  debug_printf ("No free socket slot");
	  ReleaseMutex (wsa_slot_mtx);
	  return NULL;
	}
    }
  wsa_events[slot].serial_number = new_serial_number;
  ReleaseMutex (wsa_slot_mtx);
  return wsa_events + slot;
}

bool
fhandler_socket::prepare ()
{
  LONG new_serial_number;
  char name[CYG_MAX_PATH];
  DWORD err = 0;

  do
    {
      new_serial_number = InterlockedIncrement (&socket_serial_number);
      if (!new_serial_number)	/* 0 is reserved for global mutex */
	InterlockedIncrement (&socket_serial_number);
      wsock_mtx = CreateMutex (&sec_all, FALSE,
			       shared_name (name, "sock", new_serial_number));
      if (!wsock_mtx)
	{
	  debug_printf ("CreateMutex, %E");
	  set_errno (ENOBUFS);
	  return false;
	}
      err = GetLastError ();
      if (err == ERROR_ALREADY_EXISTS)
        CloseHandle (wsock_mtx);
    }
  while (err == ERROR_ALREADY_EXISTS);
  if ((wsock_evt = CreateEvent (&sec_all, TRUE, FALSE, NULL))
      == WSA_INVALID_EVENT)
    {
      debug_printf ("WSACreateEvent, %E");
      set_errno (ENOBUFS);
      CloseHandle (wsock_mtx);
      return false;
    }
  if (WSAEventSelect (get_socket (), wsock_evt, EVENT_MASK) == SOCKET_ERROR)
    {
      debug_printf ("WSAEventSelect, %E");
      set_winsock_errno ();
      CloseHandle (wsock_evt);
      CloseHandle (wsock_mtx);
      return false;
    }
  wsock_events = search_wsa_event_slot (new_serial_number);
  memset (wsock_events, 0, sizeof *wsock_events);
  return true;
}

int
fhandler_socket::wait (long event_mask)
{
  int ret = SOCKET_ERROR;
  int wsa_err = 0;
  DWORD timeout = (is_nonblocking () ? 0 : INFINITE); 
  long events;

  if (async_io ())
    return 0;

  WaitForSingleObject (wsock_mtx, INFINITE);
  WSAEVENT ev[2] = { wsock_evt, signal_arrived };

sa_rerun:

  if ((events = (wsock_events->events & event_mask)) != 0)
    {
      if (events & FD_CONNECT)
	{
	  wsa_err = wsock_events->connect_errorcode;
	  wsock_events->connect_errorcode = 0;
	}
      wsock_events->events &= ~(events & ~FD_CLOSE);
      if (!wsa_err)
	ret = 0;
      else
	WSASetLastError (wsa_err);
      ReleaseMutex (wsock_mtx);
      return ret;
    }
  ReleaseMutex (wsock_mtx);

/* If WSAWaitForMultipleEvents is interrupted by a signal, and the signal
   has the SA_RESTART flag set, return to this label and... restart. */
sa_restart:

  WSANETWORKEVENTS evts = { 0 };
  switch (WSAWaitForMultipleEvents (2, ev, FALSE, timeout, FALSE))
    {
      case WSA_WAIT_TIMEOUT:
	WSASetLastError (WSAEINPROGRESS);
	break;
      case WSA_WAIT_EVENT_0:
	WaitForSingleObject (wsock_mtx, INFINITE);
	if (!WSAEnumNetworkEvents (get_socket (), wsock_evt, &evts))
	  {
	    if (!evts.lNetworkEvents)
	      {
		if (timeout == INFINITE)
		  goto sa_rerun;
		ReleaseMutex (wsock_mtx);
		WSASetLastError (WSAEINPROGRESS);
		break;
	      }
	    wsock_events->events |= evts.lNetworkEvents;
	    if (evts.lNetworkEvents & FD_CONNECT)
	      wsock_events->connect_errorcode = evts.iErrorCode[FD_CONNECT_BIT];
	    if ((evts.lNetworkEvents & FD_OOB)
	    	&& !evts.iErrorCode[FD_OOB_BIT]
	    	&& owner ())
	      {
		siginfo_t si = {0};
		si.si_signo = SIGURG;
		si.si_code = SI_KERNEL;
		sig_send (myself_nowait, si);
		if (_my_tls.call_signal_handler ())
		  {
		    sig_dispatch_pending ();
		    goto sa_rerun;
		  }
		if (evts.lNetworkEvents & event_mask)
		  goto sa_rerun;
		WSASetLastError (WSAEINTR);
	      }
	    else
	      {
		if (timeout == INFINITE || (evts.lNetworkEvents & event_mask))
		  goto sa_rerun;
		WSASetLastError (WSAEINPROGRESS);
	      }
	  }
	ReleaseMutex (wsock_mtx);
	break;
      case WSA_WAIT_EVENT_0 + 1:
	if (_my_tls.call_signal_handler ())
	  {
	    sig_dispatch_pending ();
	    goto sa_restart;
	  }
	WSASetLastError (WSAEINTR);
	break;
      default:
	WSASetLastError (WSAEFAULT);
	break;
    }
  return ret;
}

void
fhandler_socket::release ()
{
  CloseHandle (wsock_evt);
  CloseHandle (wsock_mtx);
}

void
fhandler_socket::fixup_before_fork_exec (DWORD win_proc_id)
{
  if (!WSADuplicateSocketA (get_socket (), win_proc_id, prot_info_ptr))
    debug_printf ("WSADuplicateSocket went fine, sock %p, win_proc_id %d, prot_info_ptr %p",
		  get_socket (), win_proc_id, prot_info_ptr);
  else
    {
      debug_printf ("WSADuplicateSocket error, sock %p, win_proc_id %d, prot_info_ptr %p",
		    get_socket (), win_proc_id, prot_info_ptr);
      set_winsock_errno ();
    }
}

void
fhandler_socket::fixup_after_fork (HANDLE parent)
{
  SOCKET new_sock;

  debug_printf ("WSASocket begin, dwServiceFlags1=%d",
		prot_info_ptr->dwServiceFlags1);

  if ((new_sock = WSASocketA (FROM_PROTOCOL_INFO,
				   FROM_PROTOCOL_INFO,
				   FROM_PROTOCOL_INFO,
				   prot_info_ptr, 0, 0)) == INVALID_SOCKET)
    {
      debug_printf ("WSASocket error");
      set_io_handle ((HANDLE)INVALID_SOCKET);
      set_winsock_errno ();
    }
  else
    {
      debug_printf ("WSASocket went fine new_sock %p, old_sock %p", new_sock, get_socket ());

      /* Go figure!  Even though the original socket was not inheritable,
	 the duplicated socket is inheritable again.  This can lead to all
	 sorts of trouble, apparently.  Note that there's no way to prevent
	 this on 9x, not even by trying to reset socket inheritance using
	 DuplicateHandle and closing the original socket. */
      if (wincap.has_set_handle_information ())
	SetHandleInformation ((HANDLE) new_sock, HANDLE_FLAG_INHERIT, 0);

      set_io_handle ((HANDLE) new_sock);
    }
}

void
fhandler_socket::fixup_after_exec ()
{
  if (!close_on_exec ())
    fixup_after_fork (NULL);
  else
    {
      CloseHandle (wsock_evt);
      CloseHandle (wsock_mtx);
    }
}

int
fhandler_socket::dup (fhandler_base *child)
{
  HANDLE nh;

  debug_printf ("here");
  fhandler_socket *fhs = (fhandler_socket *) child;

  if (!DuplicateHandle (hMainProc, wsock_mtx, hMainProc, &fhs->wsock_mtx, 0,
			TRUE, DUPLICATE_SAME_ACCESS))
    {
      system_printf ("DuplicateHandle(%x) failed, %E", wsock_mtx);
      __seterrno ();
      return -1;
    }
  if (!DuplicateHandle (hMainProc, wsock_evt, hMainProc, &fhs->wsock_evt, 0,
			TRUE, DUPLICATE_SAME_ACCESS))
    {
      system_printf ("DuplicateHandle(%x) failed, %E", wsock_evt);
      __seterrno ();
      CloseHandle (fhs->wsock_mtx);
      return -1;
    }
  fhs->wsock_events = wsock_events;

  fhs->addr_family = addr_family;
  fhs->set_socket_type (get_socket_type ());
  if (get_addr_family () == AF_LOCAL)
    {
      fhs->set_sun_path (get_sun_path ());
      if (get_socket_type () == SOCK_STREAM)
	{
	  fhs->sec_pid = sec_pid;
	  fhs->sec_uid = sec_uid;
	  fhs->sec_gid = sec_gid;
	  fhs->sec_peer_pid = sec_peer_pid;
	  fhs->sec_peer_uid = sec_peer_uid;
	  fhs->sec_peer_gid = sec_peer_gid;
	}
    }
  fhs->connect_state (connect_state ());

  /* Since WSADuplicateSocket() fails on NT systems when the process
     is currently impersonating a non-privileged account, we revert
     to the original account before calling WSADuplicateSocket() and
     switch back afterwards as it's also in fork().
     If WSADuplicateSocket() still fails for some reason, we fall back
     to DuplicateHandle(). */
  WSASetLastError (0);
  cygheap->user.deimpersonate ();
  fhs->set_io_handle (get_io_handle ());
  fhs->fixup_before_fork_exec (GetCurrentProcessId ());
  cygheap->user.reimpersonate ();
  if (!WSAGetLastError ())
    {
      fhs->fixup_after_fork (hMainProc);
      if (fhs->get_io_handle() != (HANDLE) INVALID_SOCKET)
	{
	  cygheap->fdtab.inc_need_fixup_before ();
	  return 0;
	}
    }
  debug_printf ("WSADuplicateSocket failed, trying DuplicateHandle");

  /* We don't call fhandler_base::dup here since that requires
     having winsock called from fhandler_base and it creates only
     inheritable sockets which is wrong for winsock2. */

  if (!DuplicateHandle (hMainProc, get_io_handle (), hMainProc, &nh, 0,
			FALSE, DUPLICATE_SAME_ACCESS))
    {
      system_printf ("DuplicateHandle(%x) failed, %E", get_io_handle ());
      __seterrno ();
      CloseHandle (fhs->wsock_evt);
      CloseHandle (fhs->wsock_mtx);
      return -1;
    }
  VerifyHandle (nh);
  fhs->set_io_handle (nh);
  cygheap->fdtab.inc_need_fixup_before ();
  return 0;
}

int __stdcall
fhandler_socket::fstat (struct __stat64 *buf)
{
  int res;
  if (get_device () == FH_UNIX)
    {
      res = fhandler_base::fstat_fs (buf);
      if (!res)
	{
	  buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
	}
    }
  else
    {
      res = fhandler_base::fstat (buf);
      if (!res)
	{
	  buf->st_dev = 0;
	  buf->st_ino = (__ino64_t) ((DWORD) get_handle ());
	  buf->st_mode = S_IFSOCK | S_IRWXU | S_IRWXG | S_IRWXO;
	}
    }
  return res;
}

int
fhandler_socket::fchmod (mode_t mode)
{
  if (get_device () == FH_UNIX)
    {
      fhandler_disk_file fh (pc);
      fh.get_device () = FH_FS;
      int ret = fh.fchmod (mode);
      SetFileAttributes	(pc, GetFileAttributes (pc) | FILE_ATTRIBUTE_SYSTEM);
      return ret;
    }
  return 0;
}

int
fhandler_socket::fchown (__uid32_t uid, __gid32_t gid)
{
  if (get_device () == FH_UNIX)
    {
      fhandler_disk_file fh (pc);
      return fh.fchown (uid, gid);
    }
  return 0;
}

int
fhandler_socket::facl (int cmd, int nentries, __aclent32_t *aclbufp)
{
  if (get_device () == FH_UNIX)
    {
      fhandler_disk_file fh (pc);
      return fh.facl (cmd, nentries, aclbufp);
    }
  return fhandler_base::facl (cmd, nentries, aclbufp);
}

int
fhandler_socket::link (const char *newpath)
{
  if (get_device () == FH_UNIX)
    {
      fhandler_disk_file fh (pc);
      return fh.link (newpath);
    }
  return fhandler_base::link (newpath);
}

static inline bool
address_in_use (struct sockaddr_in *addr)
{
  PMIB_TCPTABLE tab;
  PMIB_TCPROW entry;
  DWORD size = 0, i;

  if (GetTcpTable (NULL, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER)
    {
      tab = (PMIB_TCPTABLE) alloca (size);
      if (!GetTcpTable (tab, &size, FALSE))
	{
	  for (i = tab->dwNumEntries, entry = tab->table; i > 0; --i, ++entry)
	    if (entry->dwLocalAddr == addr->sin_addr.s_addr
		&& entry->dwLocalPort == addr->sin_port
		&& entry->dwState >= MIB_TCP_STATE_LISTEN
		&& entry->dwState <= MIB_TCP_STATE_LAST_ACK)
	      return true;
	}
    }
  return false;
}

int
fhandler_socket::bind (const struct sockaddr *name, int namelen)
{
  int res = -1;

  if (name->sa_family == AF_LOCAL)
    {
#define un_addr ((struct sockaddr_un *) name)
      struct sockaddr_in sin;
      int len = sizeof sin;

      if (strlen (un_addr->sun_path) >= UNIX_PATH_LEN)
	{
	  set_errno (ENAMETOOLONG);
	  goto out;
	}
      sin.sin_family = AF_INET;
      sin.sin_port = 0;
      sin.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
      if (::bind (get_socket (), (sockaddr *) &sin, len))
	{
	  syscall_printf ("AF_LOCAL: bind failed %d", get_errno ());
	  set_winsock_errno ();
	  goto out;
	}
      if (::getsockname (get_socket (), (sockaddr *) &sin, &len))
	{
	  syscall_printf ("AF_LOCAL: getsockname failed %d", get_errno ());
	  set_winsock_errno ();
	  goto out;
	}

      sin.sin_port = ntohs (sin.sin_port);
      debug_printf ("AF_LOCAL: socket bound to port %u", sin.sin_port);

      path_conv pc (un_addr->sun_path, PC_SYM_FOLLOW);
      if (pc.error)
	{
	  set_errno (pc.error);
	  goto out;
	}
      if (pc.exists ())
	{
	  set_errno (EADDRINUSE);
	  goto out;
	}
      mode_t mode = (S_IRWXU | S_IRWXG | S_IRWXO) & ~cygheap->umask;
      DWORD attr = FILE_ATTRIBUTE_SYSTEM;
      if (!(mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
	attr |= FILE_ATTRIBUTE_READONLY;
      SECURITY_ATTRIBUTES sa = sec_none;
      security_descriptor sd;
      if (allow_ntsec && pc.has_acls ())
	set_security_attribute (mode, &sa, sd);
      HANDLE fh = CreateFile (pc, GENERIC_WRITE, 0, &sa, CREATE_NEW, attr, 0);
      if (fh == INVALID_HANDLE_VALUE)
	{
	  if (GetLastError () == ERROR_ALREADY_EXISTS)
	    set_errno (EADDRINUSE);
	  else
	    __seterrno ();
	}

      char buf[sizeof (SOCKET_COOKIE) + 80];
      __small_sprintf (buf, "%s%u %c ", SOCKET_COOKIE, sin.sin_port, get_socket_type () == SOCK_STREAM ? 's' : get_socket_type () == SOCK_DGRAM ? 'd' : '-');
      af_local_set_secret (strchr (buf, '\0'));
      DWORD blen = strlen (buf) + 1;
      if (!WriteFile (fh, buf, blen, &blen, 0))
	{
	  __seterrno ();
	  CloseHandle (fh);
	  DeleteFile (pc);
	}
      else
	{
	  CloseHandle (fh);
	  set_sun_path (un_addr->sun_path);
	  res = 0;
	}
#undef un_addr
    }
  else
    {
      /* If the application didn't explicitely call setsockopt (SO_REUSEADDR),
	 enforce exclusive local address use using the SO_EXCLUSIVEADDRUSE
	 socket option, to emulate POSIX socket behaviour more closely.

	 KB 870562: Note that this option is only available since NT4 SP4.
	 Also note that a bug in Win2K SP1-3 and XP up to SP1 only enables
	 this option for users in the local administrators group. */
      if (wincap.has_exclusiveaddruse ())
	{
	  if (!saw_reuseaddr ())
	    {
	      int on = 1;
	      int ret = ::setsockopt (get_socket (), SOL_SOCKET,
				      ~(SO_REUSEADDR),
				      (const char *) &on, sizeof on);
	      debug_printf ("%d = setsockopt (SO_EXCLUSIVEADDRUSE), %E", ret);
	    }
	  else
	    {
	      debug_printf ("SO_REUSEADDR set");
	      /* There's a bug in SO_REUSEADDR handling in WinSock.
		 Per standards, we must not be able to reuse a complete
		 duplicate of a local TCP address (same IP, same port),
		 even if SO_REUSEADDR has been set.  That's unfortunately
		 possible in WinSock.  So we're testing here if the local
		 address is already in use and don't bind, if so.  This
		 only works for OSes with IP Helper support. */
	      if (get_socket_type () == SOCK_STREAM
		  && wincap.has_ip_helper_lib ()
		  && address_in_use ((struct sockaddr_in *) name))
		{
		  debug_printf ("Local address in use, don't bind");
		  set_errno (EADDRINUSE);
		  goto out;
		}
	    }
	}
      if (::bind (get_socket (), name, namelen))
	set_winsock_errno ();
      else
	res = 0;
    }

out:
  return res;
}

int
fhandler_socket::connect (const struct sockaddr *name, int namelen)
{
  int res = -1;
  bool in_progress = false;
  struct sockaddr_storage sst;
  DWORD err;
  int type;

  if (!get_inet_addr (name, namelen, &sst, &namelen, &type, connect_secret))
    return -1;

  if (get_addr_family () == AF_LOCAL && get_socket_type () != type)
    {
      WSASetLastError (WSAEPROTOTYPE);
      set_winsock_errno ();
      return -1;
    }

  res = ::connect (get_socket (), (struct sockaddr *) &sst, namelen);
  if (!is_nonblocking ()
      && res == SOCKET_ERROR
      && WSAGetLastError () == WSAEWOULDBLOCK)
    res = wait (FD_CONNECT | FD_CLOSE);

  if (!res)
    err = 0;
  else
    {
      err = WSAGetLastError ();
      /* Special handling for connect to return the correct error code
	 when called on a non-blocking socket. */
      if (is_nonblocking ())
	{
	  if (err == WSAEWOULDBLOCK || err == WSAEALREADY)
	    in_progress = true;

	  if (err == WSAEWOULDBLOCK)
	    WSASetLastError (err = WSAEINPROGRESS);
	}
      if (err == WSAEINVAL)
	WSASetLastError (err = WSAEISCONN);
      set_winsock_errno ();
    }

  if (get_addr_family () == AF_LOCAL && (!res || in_progress))
    set_sun_path (name->sa_data);

  if (get_addr_family () == AF_LOCAL && get_socket_type () == SOCK_STREAM)
    {
      af_local_set_cred (); /* Don't move into af_local_connect since
			       af_local_connect is called from select,
			       possibly running under another identity. */
      if (!res && af_local_connect ())
	{
	  set_winsock_errno ();
	  return -1;
	}
    }

  if (err == WSAEINPROGRESS || err == WSAEALREADY)
    connect_state (connect_pending);
  else if (err)
    connect_state (connect_failed);
  else
    connect_state (connected);

  return res;
}

int
fhandler_socket::listen (int backlog)
{
  int res = ::listen (get_socket (), backlog);
  if (res && WSAGetLastError () == WSAEINVAL)
    {
      /* It's perfectly valid to call listen on an unbound INET socket.
	 In this case the socket is automatically bound to an unused
	 port number, listening on all interfaces.  On Winsock, listen
	 fails with WSAEINVAL when it's called on an unbound socket.
	 So we have to bind manually here to have POSIX semantics. */
      if (get_addr_family () == AF_INET)
        {
	  struct sockaddr_in sin;
	  sin.sin_family = AF_INET;
	  sin.sin_port = 0;
	  sin.sin_addr.s_addr = INADDR_ANY;
	  if (!::bind (get_socket (), (struct sockaddr *) &sin, sizeof sin))
	    res = ::listen (get_socket (), backlog);
	}
      else if (get_addr_family () == AF_INET6)
        {
	  struct sockaddr_in6 sin6 = 
	    {
	      sin6_family: AF_INET6,
	      sin6_port: 0,
	      sin6_flowinfo: 0,
	      sin6_addr: IN6ADDR_ANY_INIT,
	      sin6_scope_id: 0
	    };
	  if (!::bind (get_socket (), (struct sockaddr *) &sin6, sizeof sin6))
	    res = ::listen (get_socket (), backlog);
	}
    }
  if (!res)
    {
      if (get_addr_family () == AF_LOCAL && get_socket_type () == SOCK_STREAM)
	af_local_set_cred ();
      connect_state (connected);
      listener (true);
    }
  else
    set_winsock_errno ();
  return res;
}

int
fhandler_socket::accept (struct sockaddr *peer, int *len)
{
  /* Allows NULL peer and len parameters. */
  struct sockaddr_in peer_dummy;
  int len_dummy;
  if (!peer)
    peer = (struct sockaddr *) &peer_dummy;
  if (!len)
    {
      len_dummy = sizeof (struct sockaddr_in);
      len = &len_dummy;
    }

  /* accept on NT fails if len < sizeof (sockaddr_in)
   * some programs set len to
   * sizeof (name.sun_family) + strlen (name.sun_path) for UNIX domain
   */
  if (len && ((unsigned) *len < sizeof (struct sockaddr_in)))
    *len = sizeof (struct sockaddr_in);


  int res = 0;
  while (!(res = wait (FD_ACCEPT | FD_CLOSE))
	 && (res = ::accept (get_socket (), peer, len)) == WSAEWOULDBLOCK)
    ;

  if (res == (int) INVALID_SOCKET)
    set_winsock_errno ();
  else
    {
      cygheap_fdnew res_fd;
      if (res_fd >= 0 && fdsock (res_fd, &dev (), res))
	{
	  fhandler_socket *sock = (fhandler_socket *) res_fd;
	  sock->set_addr_family (get_addr_family ());
	  sock->set_socket_type (get_socket_type ());
	  sock->async_io (async_io ());
	  sock->set_nonblocking (is_nonblocking ());
	  if (get_addr_family () == AF_LOCAL)
	    {
	      sock->set_sun_path (get_sun_path ());
	      if (get_socket_type () == SOCK_STREAM)
		{
		  /* Don't forget to copy credentials from accepting
		     socket to accepted socket and start transaction
		     on accepted socket! */
		  af_local_copy (sock);
		  res = sock->af_local_accept ();
		  if (res == -1)
		    {
		      res_fd.release ();
		      set_winsock_errno ();
		      goto out;
		    }
		}
	    }
	  sock->connect_state (connected);
	  res = res_fd;
	}
      else
	{
	  closesocket (res);
	  res = -1;
	}
    }

out:
  debug_printf ("res %d", res);
  return res;
}

int
fhandler_socket::getsockname (struct sockaddr *name, int *namelen)
{
  int res = -1;

  if (get_addr_family () == AF_LOCAL)
    {
      struct sockaddr_un *sun = (struct sockaddr_un *) name;
      memset (sun, 0, *namelen);
      sun->sun_family = AF_LOCAL;

      if (!get_sun_path ())
	sun->sun_path[0] = '\0';
      else
	/* According to SUSv2 "If the actual length of the address is
	   greater than the length of the supplied sockaddr structure, the
	   stored address will be truncated."  We play it save here so
	   that the path always has a trailing 0 even if it's truncated. */
	strncpy (sun->sun_path, get_sun_path (),
		 *namelen - sizeof *sun + sizeof sun->sun_path - 1);

      *namelen = sizeof *sun - sizeof sun->sun_path
		 + strlen (sun->sun_path) + 1;
      res = 0;
    }
  else
    {
      res = ::getsockname (get_socket (), name, namelen);
      if (res)
	set_winsock_errno ();
    }

  return res;
}

int
fhandler_socket::getpeername (struct sockaddr *name, int *namelen)
{
  int res = ::getpeername (get_socket (), name, namelen);
  if (res)
    set_winsock_errno ();

  return res;
}

int
fhandler_socket::readv (const struct iovec *const iov, const int iovcnt,
			ssize_t tot)
{
  struct msghdr msg =
    {
      msg_name:		NULL,
      msg_namelen:	0,
      msg_iov:		(struct iovec *) iov, // const_cast
      msg_iovlen:	iovcnt,
      msg_control:	NULL,
      msg_controllen:	0,
      msg_flags:	0
    };

  return recvmsg (&msg, 0, tot);
}

inline ssize_t
fhandler_socket::recv_internal (WSABUF *wsabuf, DWORD wsacnt, DWORD flags,
				struct sockaddr *from, int *fromlen)
{
  ssize_t res = 0;
  DWORD ret = 0;

  flags &= MSG_WINMASK;
  if (flags & MSG_PEEK)
    res = WSARecvFrom (get_socket (), wsabuf, wsacnt, &ret,
		       &flags, from, fromlen, NULL, NULL);
  else
    {
      int evt_mask = FD_READ | FD_CLOSE
		     | ((flags & MSG_OOB) ? FD_OOB : 0);
      while ((res = WSARecvFrom (get_socket (), wsabuf, wsacnt, &ret,
				    &flags, from, fromlen, NULL, NULL)) == -1
	     && WSAGetLastError () == WSAEWOULDBLOCK
	     && !(res = wait (evt_mask)))
	;
    }

  if (res == SOCKET_ERROR)
    {
      /* According to SUSv3, errno isn't set in that case and no error
	 condition is returned. */
      if (WSAGetLastError () == WSAEMSGSIZE)
	return ret;

      /* ESHUTDOWN isn't defined for recv in SUSv3.  Simply EOF is returned
	 in this case. */
      if (WSAGetLastError () == WSAESHUTDOWN)
	return 0;

      set_winsock_errno ();
    }
  else
    res = ret;

  return res;
}

int
fhandler_socket::recvfrom (void *ptr, size_t len, int flags,
			   struct sockaddr *from, int *fromlen)
{
  WSABUF wsabuf = { len, (char *) ptr };
  return recv_internal (&wsabuf, 1, flags, from, fromlen);
}

int
fhandler_socket::recvmsg (struct msghdr *msg, int flags, ssize_t tot)
{
  if (CYGWIN_VERSION_CHECK_FOR_USING_ANCIENT_MSGHDR)
    ((struct OLD_msghdr *) msg)->msg_accrightslen = 0;
  else
    {
      msg->msg_controllen = 0;
      msg->msg_flags = 0;
    }
  if (get_addr_family () == AF_LOCAL)
    {
      /* On AF_LOCAL sockets the (fixed-size) name of the shared memory
	 area used for descriptor passing is transmitted first.
	 If this string is empty, no descriptors are passed and we can
	 go ahead recv'ing the normal data blocks.  Otherwise start
	 special handling for descriptor passing. */
      /*TODO*/
    }

  WSABUF wsabuf[msg->msg_iovlen];
  WSABUF *wsaptr = wsabuf + msg->msg_iovlen;
  const struct iovec *iovptr = msg->msg_iov + msg->msg_iovlen;
  while (--wsaptr >= wsabuf)
    {
      wsaptr->len = (--iovptr)->iov_len;
      wsaptr->buf = (char *) iovptr->iov_base;
    }

  struct sockaddr *from = (struct sockaddr *) msg->msg_name;
  int *fromlen = from ? &msg->msg_namelen : NULL;

  return recv_internal (wsabuf, msg->msg_iovlen, flags, from, fromlen);
}

int
fhandler_socket::writev (const struct iovec *const iov, const int iovcnt,
			 ssize_t tot)
{
  struct msghdr msg =
    {
      msg_name:		NULL,
      msg_namelen:	0,
      msg_iov:		(struct iovec *) iov, // const_cast
      msg_iovlen:	iovcnt,
      msg_control:	NULL,
      msg_controllen:	0,
      msg_flags:	0
    };

  return sendmsg (&msg, 0, tot);
}

inline ssize_t
fhandler_socket::send_internal (struct _WSABUF *wsabuf, DWORD wsacnt, int flags,
				const struct sockaddr *to, int tolen)
{
  int res = 0;
  DWORD ret = 0;
  while ((res = WSASendTo (get_socket (), wsabuf, wsacnt, &ret,
			   flags & MSG_WINMASK, to, tolen, NULL, NULL)) == -1
	 && WSAGetLastError () == WSAEWOULDBLOCK
	 && !(res = wait (FD_WRITE | FD_CLOSE)))
    ;

  if (res == SOCKET_ERROR)
    set_winsock_errno ();

  /* Special handling for EPIPE and SIGPIPE.

     EPIPE is generated if the local end has been shut down on a connection
     oriented socket.  In this case the process will also receive a SIGPIPE
     unless MSG_NOSIGNAL is set.  */
  if (res == SOCKET_ERROR && get_errno () == ESHUTDOWN
      && get_socket_type () == SOCK_STREAM)
    {
      set_errno (EPIPE);
      if (! (flags & MSG_NOSIGNAL))
	raise (SIGPIPE);
    }
  else
    res = ret;

  return res;
}

ssize_t
fhandler_socket::sendto (const void *ptr, size_t len, int flags,
			 const struct sockaddr *to, int tolen)
{
  struct sockaddr_storage sst;

  if (to && !get_inet_addr (to, tolen, &sst, &tolen))
    return SOCKET_ERROR;

  WSABUF wsabuf = { len, (char *) ptr };
  return send_internal (&wsabuf, 1, flags,
  			(to ? (const struct sockaddr *) &sst : NULL), tolen);
}

int
fhandler_socket::sendmsg (const struct msghdr *msg, int flags, ssize_t tot)
{
  if (get_addr_family () == AF_LOCAL)
    {
      /* For AF_LOCAL/AF_UNIX sockets, if descriptors are given, start
	 the special handling for descriptor passing.  Otherwise just
	 transmit an empty string to tell the receiver that no
	 descriptor passing is done. */
      /*TODO*/
    }

  WSABUF wsabuf[msg->msg_iovlen];
  WSABUF *wsaptr = wsabuf + msg->msg_iovlen;
  const struct iovec *iovptr = msg->msg_iov + msg->msg_iovlen;
  while (--wsaptr >= wsabuf)
    {
      wsaptr->len = (--iovptr)->iov_len;
      wsaptr->buf = (char *) iovptr->iov_base;
    }

  return send_internal (wsabuf, msg->msg_iovlen, flags,
  			(struct sockaddr *) msg->msg_name, msg->msg_namelen);
}

int
fhandler_socket::shutdown (int how)
{
  int res = ::shutdown (get_socket (), how);

  if (res)
    set_winsock_errno ();
  else
    switch (how)
      {
      case SHUT_RD:
	saw_shutdown_read (true);
	break;
      case SHUT_WR:
	saw_shutdown_write (true);
	break;
      case SHUT_RDWR:
	saw_shutdown_read (true);
	saw_shutdown_write (true);
	break;
      }
  return res;
}

int
fhandler_socket::close ()
{
  int res = 0;

  /* HACK to allow a graceful shutdown even if shutdown() hasn't been
     called by the application. Note that this isn't the ultimate
     solution but it helps in many cases. */
  struct linger linger;
  linger.l_onoff = 1;
  linger.l_linger = 240; /* secs. default 2MSL value according to MSDN. */
  setsockopt (get_socket (), SOL_SOCKET, SO_LINGER,
	      (const char *)&linger, sizeof linger);

  release ();
  while ((res = closesocket (get_socket ())) != 0)
    {
      if (WSAGetLastError () != WSAEWOULDBLOCK)
	{
	  set_winsock_errno ();
	  res = -1;
	  break;
	}
      if (WaitForSingleObject (signal_arrived, 10) == WAIT_OBJECT_0)
	{
	  set_errno (EINTR);
	  res = -1;
	  break;
	}
      WSASetLastError (0);
    }

  debug_printf ("%d = fhandler_socket::close()", res);
  return res;
}

int
fhandler_socket::ioctl (unsigned int cmd, void *p)
{
  extern int get_ifconf (struct ifconf *ifc, int what); /* net.cc */
  int res;
  struct ifconf ifc, *ifcp;
  struct ifreq *ifr, *ifrp;

  switch (cmd)
    {
    case SIOCGIFCONF:
      ifcp = (struct ifconf *) p;
      if (!ifcp)
	{
	  set_errno (EINVAL);
	  return -1;
	}
      res = get_ifconf (ifcp, cmd);
      if (res)
	debug_printf ("error in get_ifconf");
      break;
    case SIOCGIFFLAGS:
      ifr = (struct ifreq *) p;
      if (ifr == 0)
	{
	  set_errno (EINVAL);
	  return -1;
	}
      ifr->ifr_flags = IFF_NOTRAILERS | IFF_UP | IFF_RUNNING;
      if (!strncmp(ifr->ifr_name, "lo", 2)
	  || ntohl (((struct sockaddr_in *) &ifr->ifr_addr)->sin_addr.s_addr)
	  == INADDR_LOOPBACK)
	ifr->ifr_flags |= IFF_LOOPBACK;
      else
	ifr->ifr_flags |= IFF_BROADCAST;
      res = 0;
      break;
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFADDR:
    case SIOCGIFHWADDR:
    case SIOCGIFMETRIC:
    case SIOCGIFMTU:
      {
	ifc.ifc_len = 2048;
	ifc.ifc_buf = (char *) alloca (2048);

	ifr = (struct ifreq *) p;
	if (ifr == 0)
	  {
	    debug_printf ("ifr == NULL");
	    set_errno (EINVAL);
	    return -1;
	  }

	res = get_ifconf (&ifc, cmd);
	if (res)
	  {
	    debug_printf ("error in get_ifconf");
	    break;
	  }

	debug_printf ("    name: %s", ifr->ifr_name);
	for (ifrp = ifc.ifc_req;
	     (caddr_t) ifrp < ifc.ifc_buf + ifc.ifc_len;
	     ++ifrp)
	  {
	    debug_printf ("testname: %s", ifrp->ifr_name);
	    if (! strcmp (ifrp->ifr_name, ifr->ifr_name))
	      {
		switch (cmd)
		  {
		  case SIOCGIFADDR:
		    ifr->ifr_addr = ifrp->ifr_addr;
		    break;
		  case SIOCGIFBRDADDR:
		    ifr->ifr_broadaddr = ifrp->ifr_broadaddr;
		    break;
		  case SIOCGIFNETMASK:
		    ifr->ifr_netmask = ifrp->ifr_netmask;
		    break;
		  case SIOCGIFHWADDR:
		    ifr->ifr_hwaddr = ifrp->ifr_hwaddr;
		    break;
		  case SIOCGIFMETRIC:
		    ifr->ifr_metric = ifrp->ifr_metric;
		    break;
		  case SIOCGIFMTU:
		    ifr->ifr_mtu = ifrp->ifr_mtu;
		    break;
		  }
		break;
	      }
	  }
	if ((caddr_t) ifrp >= ifc.ifc_buf + ifc.ifc_len)
	  {
	    set_errno (EINVAL);
	    return -1;
	  }
	break;
      }
    case FIOASYNC:
      res = WSAAsyncSelect (get_socket (), winmsg, WM_ASYNCIO,
	      *(int *) p ? ASYNC_MASK : 0);
      syscall_printf ("Async I/O on socket %s",
	      *(int *) p ? "started" : "cancelled");
      async_io (*(int *) p != 0);
      /* If async_io is switched off, revert the event handling. */
      if (*(int *) p == 0)
        WSAEventSelect (get_socket (), wsock_evt, EVENT_MASK);
      break;
    case FIONREAD:
      res = ioctlsocket (get_socket (), FIONREAD, (unsigned long *) p);
      if (res == SOCKET_ERROR)
	set_winsock_errno ();
      break;
    default:
      /* Sockets are always non-blocking internally.  So we just note the
         state here. */
      if (cmd == FIONBIO)
	{
	  syscall_printf ("socket is now %sblocking",
			    *(int *) p ? "non" : "");
	  set_nonblocking (*(int *) p);
	  res = 0;
        }
      else
	res = ioctlsocket (get_socket (), cmd, (unsigned long *) p);
      break;
    }
  syscall_printf ("%d = ioctl_socket (%x, %x)", res, cmd, p);
  return res;
}

int
fhandler_socket::fcntl (int cmd, void *arg)
{
  int res = 0;
  int request, current;

  switch (cmd)
    {
    case F_SETOWN:
      {
	/* Urgh!  Bad hack! */
	pid_t pid = (pid_t) arg;
	owner (pid == getpid ());
	debug_printf ("owner set to %d", owner ());
      }
      break;
    case F_SETFL:
      {
	/* Carefully test for the O_NONBLOCK or deprecated OLD_O_NDELAY flag.
	   Set only the flag that has been passed in.  If both are set, just
	   record O_NONBLOCK.   */
	int new_flags = (int) arg & O_NONBLOCK_MASK;
	if ((new_flags & OLD_O_NDELAY) && (new_flags & O_NONBLOCK))
	  new_flags = O_NONBLOCK;
	current = get_flags () & O_NONBLOCK_MASK;
	request = new_flags ? 1 : 0;
	if (!!current != !!new_flags && (res = ioctl (FIONBIO, &request)))
	  break;
	set_flags ((get_flags () & ~O_NONBLOCK_MASK) | new_flags);
	break;
      }
    default:
      res = fhandler_base::fcntl (cmd, arg);
      break;
    }
  return res;
}

void
fhandler_socket::set_close_on_exec (bool val)
{
  close_on_exec (val);
  debug_printf ("set close_on_exec for %s to %d", get_name (), val);
}

void
fhandler_socket::set_sun_path (const char *path)
{
  sun_path = path ? cstrdup (path) : NULL;
}

int
fhandler_socket::getpeereid (pid_t *pid, __uid32_t *euid, __gid32_t *egid)
{
  if (get_addr_family () != AF_LOCAL || get_socket_type () != SOCK_STREAM)
    {
      set_errno (EINVAL);
      return -1;
    }
  if (connect_state () != connected)
    {
      set_errno (ENOTCONN);
      return -1;
    }
  if (sec_peer_pid == (pid_t) 0)
    {
      set_errno (ENOTCONN);	/* Usually when calling getpeereid on
				   accepting (instead of accepted) socket. */
      return -1;
    }

  myfault efault;
  if (efault.faulted (EFAULT))
    return -1;
  if (pid)
    *pid = sec_peer_pid;
  if (euid)
    *euid = sec_peer_uid;
  if (egid)
    *egid = sec_peer_gid;
  return 0;
}
