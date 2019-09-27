// Call Answer and recording based on FireBrick voip-record.c with permission
// (c) 2013 Andrews & Arnold Ltd Adrian Kennard
//
// This server acts as a SIP endpoint and is designed for use with FireBrick VoIP
// It is not a complete SIP implementation and assumes FireBrick behaviour
//
// This can be used with FireBrick recording, which sends an X-Record header
// A script is run to handle the recording and email it to the recipient.
//
// This can also be used as a playback server, to play audio files, and record
// The SIP URI used for the playback system consists of...
// Prefixes (in this order):-
// XXX=         This call is not to be answered, but call progress. XXX is final status
// =            This call is not to be answered, but call progress. End with constant ringing
// -            Add a ring, may be repeated
// !            Add a SIT, may be repeated
// N*           The playback sequence is to be repeated N times
// Filenames
// Following the prefix are dot separated filenames, assumed to be wav files, to play
// A filename can have a ? after it followed by another filename. This skips the second file if the first exists
// Each dot is also a small time delay, so several dots can be used for a pause
// Suffixes, one applies
// =filename    Record to file
// *            Silence
// #            Refer to #
// #NNN...      Refer to NNN...

typedef unsigned int ui32;

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <err.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <netdb.h>
#include <popt.h>
#include <syslog.h>
#include "sip_parsers.h"
#include "../deps/siptools.c"

int debug = 1;
const char *savescript = NULL;  // script for saved file
const char *recscript = NULL;   // script for recording

const char *
audio_in (int port, int s, ui8 * rx, ui8 * rxe, int nonanswer)
{                               // process incoming audio, then run script. Return NULL for not done, empty string for done, other string for REFER
   if (debug)
      fprintf (stderr, "%d Audio processing\n", port);
   char infilename[100];
   char *outfilename = NULL;
   char template[] = "/tmp/voip-answer-XXXXXX";
   int temp_fd = -1;
   ui32 seq = 0;
   ui32 ts = 0;
   ui32 id = port;
   int minute = 60 * 10;        // silence period

   ui8 *exrecord,
    *xrecord = sip_find_header (rx, rxe, "X-Record", NULL, &exrecord, NULL);
   ui8 *request = NULL,
      *erequest = NULL,
      *rp = NULL;
   int ring = 0,
      sit = 0,
      count = 1;
   int rf = -1;
   if (!xrecord)
   {                            // set up playback sequencing
      ui8 *e,
       *p = sip_find_request (rx, rxe, &e);
      p = sip_find_local (p, e, &e);
      if (p + 4 < e && !strncasecmp (p, "sip:", 4))
         p += 4;
      syslog (LOG_INFO, "%d Playback %.*s", port, (int) (e - p), p);
      // get prefixes
      request = p;
      read_unsigned (&p, e);
      if (p < e && *p == '=')
      {
         p++;
      } else
         p = request;
      while (p < e && *p == '-')
      {
         ring++;
         p++;
      }
      while (p < e && *p == '!')
      {
         sit++;
         p++;
      }
      request = p;
      int v = read_unsigned (&p, e);
      if (p < e && *p == '*')
      {
         p++;
         count = v;
      } else
         p = request;
      request = p;
      erequest = e;
   } else
   {                            // simple record, set up temp_fd
      temp_fd = mkstemp (outfilename = template);
      if (temp_fd < 0)
         err (1, "temp failed");
      lseek (temp_fd, 44, SEEK_SET);
      syslog (LOG_INFO, "%d Recording %s", port, outfilename);
   }
   int datalen = 0;
   ui8 channels = 0;
   const char *done = NULL;
   char saved = 0;              // saved a file - not to be deleted
   ui8 buf[1000];
   struct sockaddr_in6 from = { };
   socklen_t fromlen;
   struct timeval tv;
   gettimeofday (&tv, NULL);
   long long next = tv.tv_sec * 1000000LL + tv.tv_usec;
   long long timeout = next + (nonanswer ? 300 : 10) * 1000000LL;
   long long now = 0;
   while (!done)
   {
      gettimeofday (&tv, NULL);
      now = tv.tv_sec * 1000000LL + tv.tv_usec;
      if (now > timeout)
         break;
      long long delay = next - now;
      //syslog (LOG_INFO, "Delay %lld channels %d", delay, channels);
      if (delay > 0)
      {
         int ret;
         struct timeval to = { 0, delay };
         fd_set ss;
         FD_ZERO (&ss);
         FD_SET (s, &ss);
         ret = select (s + 1, &ss, 0, 0, &to);
         if (ret > 0)
         {
            int len = 0;
            fromlen = sizeof (from);
            len = recvfrom (s, buf, sizeof (buf) - 1, 0, (struct sockaddr *) &from, &fromlen);
            if (len > 12)
            {
               if (!channels)
                  channels = 1; // started
               if (channels == 1 && (buf[1] & 0x7F) == 9)
               {
                  channels = 2;
                  syslog (LOG_INFO, "%d Stereo", port);
               }
               if (temp_fd >= 0 && ((buf[1] & 0x7F) == 8 || (buf[1] & 0x7F) == 9))
               {                // Write to file - this is simple and assumes all arrive in order, which would be the case on a local network
                  if (write (temp_fd, buf + 12, len - 12) < 0)
                     err (1, "write");
                  datalen += len - 12;
               } else if ((buf[1] & 0x7F) == 101)
               {                // DTMF/key
                  syslog (LOG_INFO, "Key %d", buf[12]);
                  const char *keys[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#" };
                  if (erequest > request && (erequest[-1] == '*' || erequest[-1] == '#') && buf[12] <= 11)
                     done = keys[(int) buf[12]];        // Quit with key if end of playback is a * (wait) or # (exit at end)
               }
               timeout = now + (nonanswer ? 300 : 5) * 1000000LL;
            }
         }
         continue;
      }
      next += 20000LL;          // 20ms
      if (channels != 1)
         continue;
      // Send audio back
      int samples = 160;        // 20ms
      ui8 *p = buf;
      *p++ = 0x80;              // v2
      *p++ = 8;                 // alaw
      *p++ = (seq >> 8);
      *p++ = seq;
      *p++ = (ts >> 24);
      *p++ = (ts >> 16);
      *p++ = (ts >> 8);
      *p++ = (ts);
      *p++ = (id >> 24);
      *p++ = (id >> 16);
      *p++ = (id >> 8);
      *p++ = (id);
      ts += samples;
      seq++;
      while (samples && request)
      {
         int nextfile (void)
         {
            if (ring)
            {
               ring--;
               rp = "aai";
            } else if (sit)
            {
               sit--;
               rp = "sit";
            } else if (!rp || rp == erequest || !*rp || *rp == '=')
            {
               if (!count)
               {
                  if (rp != erequest && *rp == '=')
                  {             // recording
                     rp++;
                     if (rp != erequest)
                     {
                        saved = 1;
                        outfilename = malloc (erequest + 5 - rp);
                        memmove (outfilename, rp, erequest - rp);
                        strcpy (outfilename + (erequest - rp), ".wav");
                        temp_fd = open (outfilename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (debug)
                        {
                           if (temp_fd < 0)
                              warn ("%s", outfilename);
                           else
                              fprintf (stderr, "%d Recording to %s\n", port, outfilename);
                        }
                     } else
                        temp_fd = mkstemp (outfilename = strdup (template));
                     if (temp_fd >= 0)
                        request = NULL; // stop playing
                  } else
                  {
                     if (debug)
                        fprintf (stderr, "%d End of playback\n", port);
                     done = ""; // end of playback
                  }
                  return -1;
               }
               rp = request;
               count--;
            }
            if (!rp || rp == erequest || !*rp)
               return -1;
            if (*rp == '#')
            {
               if (rp + 1 < erequest && rp[1] && isdigit (rp[1]))
               {                // Refer to number
                  rp++;
                  static char refer[50],
                   *o = refer;
                  while (rp < erequest && isdigit (*rp) && o < refer + sizeof (refer) - 1)
                     *o++ = *rp++;
                  *o = 0;
                  done = refer;
               } else
                  done = "#";   // Refer to hash
               return -1;
            }
            if (*rp == '*' && !minute--)
            {                   // * is a one minute silence done as 100ms playback
               minute = 60 * 10;
               rp++;
            }
            void getfile (void)
            {
               char *o = infilename;
               while (rp != erequest
                      && ((isalnum (*rp) || *rp == '+' || (*rp == '/' && o != infilename) || *rp == '-')
                          && o < infilename + sizeof (infilename) - 5))
                  *o++ = *rp++;
               if (o == infilename)
                  o += sprintf (o, "100ms");
               strcpy (o, ".wav");
               if (debug)
                  fprintf (stderr, "%d File %s\n", port, infilename);
            }
            getfile ();
            int fn = open (infilename, O_RDONLY, 0);
            while (rp != erequest && *rp == '?')
            {                   // alternate file
               rp++;
               getfile ();
               if (fn < 0)
                  fn = open (infilename, O_RDONLY, 0);
            }
            if (rp != erequest && *rp == '.')
               rp++;
            if (fn < 0)
               return fn;
            // headers
            if (lseek (fn, 12, SEEK_SET) == (off_t) - 1)
            {
               close (fn);
               if (debug)
                  fprintf (stderr, "%d Bad file %s (seek 12)\n", port, infilename);
               return -1;
            }
            while (1)
            {
               unsigned char d[8];
               if (read (fn, d, 8) != 8)
               {
                  close (fn);
                  if (debug)
                     fprintf (stderr, "%d Bad file %s (read 8)\n", port, infilename);
                  return -1;
               }
               if (!memcmp (d, "data", 4))
                  return fn;
               if (lseek (fn, d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24), SEEK_CUR) == (off_t) - 1)
               {
                  close (fn);
                  if (debug)
                     fprintf (stderr, "%d Bad file %s (skip %.4s)\n", port, infilename, d);
                  return -1;
               }
            }
            return fn;
         }
         int l = -1;
         while (1)
         {
            if (rf < 0)
               rf = nextfile ();
            if (rf < 0)
               break;
            l = read (rf, p, samples);
            if (l > 0)
               break;
            if (l == 0)
            {
               close (rf);
               rf = -1;
            }
         }
         if (rf < 0)
            break;
         samples -= l;
         p += l;
      }
      while (samples)
      {
         *p++ = 0x55;
         samples--;
      }
      sendto (s, buf, p - buf, 0, &from, fromlen);
   }

   if (!channels)
   {
      if (temp_fd >= 0)
         close (temp_fd);
      syslog (LOG_INFO, "%d Audio finished %u bytes%s%s%s", port, datalen, now > timeout ? " (timeout)" : "", done ? " refer " : "",
              done ? : "");
      return done;
   }

   syslog (LOG_INFO, "%d Audio finished %us%s%s%s", port, datalen / channels / 8000, now > timeout ? " (timeout)" : "",
           done ? " refer " : "", done ? : "");

   if (temp_fd >= 0)
   {                            // Update header
      void writen (int c, int n)
      {
         unsigned char l[4];
         l[0] = n;
         l[1] = (n >> 8);
         l[2] = (n >> 16);
         l[3] = (n >> 24);
         if (write (temp_fd, l, c) != c)
            err (1, "write");
      }
      lseek (temp_fd, 0, SEEK_SET);
      if (write (temp_fd, "RIFF", 4) != 4)
         err (1, "write");      // ChunkID
      writen (4, datalen - 36); // ChunkSize
      if (write (temp_fd, "WAVE", 4) != 4)
         err (1, "write");      // Format
      if (write (temp_fd, "fmt ", 4) != 4)
         err (1, "write");      // Subchunk1ID
      writen (4, 16);           // Subchunk1Size
      writen (2, 6);            // AudioFormat
      writen (2, channels);     // NumChannels
      writen (4, 8000);         // SampleRate
      writen (4, 8000 * channels);      // ByteRate
      writen (2, channels);     // BlockAlign
      writen (2, 8);            // BitsPerSample
      if (write (temp_fd, "data", 4) != 4)
         err (1, "write");      // Subchunk2ID
      writen (4, datalen);      // Subchunk2Size
      close (temp_fd);
   }
   {
      // Some standard variables
      char temp[100];
      int s = datalen / channels / 8;
      sprintf (temp, "%u:%02u", s / 60000, s / 1000 % 60);
      setenv ("duration", temp, 1);
      sprintf (temp, "%u", channels);
      setenv ("channels", temp, 1);
      time_t now = time (0) - s / 1000;
      struct tm t = *localtime (&now);
      strftime (temp, sizeof (temp), "%FT%T", &t);
      sprintf (temp + 19, ".%03uZ", s % 1000);
      setenv ("calltime", temp, 1);
      //strftime (temp, sizeof (temp), "%FT%T", &t);
      strftime (temp, sizeof (temp), "%a, %e %b %Y %T %z", &t);
      setenv ("maildate", temp, 1);
      ui8 *e,
       *p = sip_find_header (rx, rxe, "Call-ID", "i", &e, NULL);
      if (p)
      {
         snprintf (temp, sizeof (temp), "%.*s", (int) (e - p), p);
         setenv ("i", temp, 1);
      }
   }
   if (outfilename)
   {                            // Run script
      // Get arguments: CLI, Dialled, Email address(es)
      char *args[20];
      int a = 0;
      args[a++] = "voip-answer";
      ui8 *q,
       *z,
       *e,
       *p = sip_find_header (rx, rxe, "From", "f", &e, NULL);
      p = sip_find_uri (p, e, &e);
      p = sip_find_local (p, e, &e);
      args[a] = strndup (p, e - p);
      setenv ("from", args[a], 1);
      a++;
      p = sip_find_header (rx, rxe, "To", "t", &e, NULL);
      p = sip_find_uri (p, e, &e);
      p = sip_find_local (p, e, &e);
      args[a] = strndup (p, e - p);
      setenv ("to", args[a], 1);
      a++;
      if (saved)
      {                         // Saved file
         if (!fork ())
         {
            close (s);
            if (debug)
               fprintf (stderr, "Script %s %s\n", savescript, outfilename);
            execl (savescript, savescript, outfilename, NULL);
            err (1, "%s", savescript);
         }
      } else
      {
         setenv("wavpath", outfilename, 1);
         if (datalen)
         {                      // Recording
            if (!recscript)
               return done;
            if (xrecord)
            {
               z = NULL;
               p = xrecord;
               e = exrecord;
               while (p < e)
               {
                  q = sip_find_uri (p, e, &z);
                  if (!q)
                     break;
                  if (z < e && *z == '>')
                     z++;
                  if (z < e && *z == ';')
                     break;
                  if (z < e && *z == ',')
                     z++;
                  p = z;
               }
               while (z && z < e && *z == ';')
               {                // parameters
                  z++;
                  ui8 *ts = z;
                  while (z < e && *z != '=')
                     z++;
                  if (z == e)
                     break;
                  ui8 *te = z;
                  z++;
                  ui8 *vs = z,
                     *ve;
                  if (z < e && *z == '"')
                  {
                     z++;
                     vs = z;
                     while (z < e && *z != '"')
                        z++;
                     ve = z;
                     if (z < e)
                        z++;
                  } else
                  {
                     while (z < e && *z != ';')
                        z++;
                     ve = z;
                  }
                  if (te > ts)
                  {
                     char *t = strndup (ts, te - ts);
                     char *v = strndup (vs, ve - vs);
                     setenv (t, v, 1);
                     if (debug)
                        fprintf (stderr, "%d Variable %s=%s\n", port, t, v);
                  }
               }
               p = xrecord;
               e = exrecord;
               while (p < e)
               {
                  q = sip_find_display (p, e, &z);
                  if (!q)
                     args[a] = "";
                  else
                  {
                     args[a] = strndup (q, z - q);
                  }
                  setenv ("name", args[a], 1);
                  a++;
                  q = sip_find_uri (p, e, &z);
                  if (!q)
                     break;
                  if (a < sizeof (args) / sizeof (*args) - 1)
                  {
                     if (debug)
                        fprintf (stderr, "%d Email [%.*s]\n", port, (int) (z - q), q);
                     args[a] = strndup (q, z - q);
                     setenv ("email", args[a], 1);
                     a++;
                     args[a] = 0;
                     if (!fork ())
                     {
                        close (s);
                        if (debug)
                           fprintf (stderr, "Script %s (%d args)\n", recscript, a);
                        execv (recscript, args);
                        err (1, "%s", recscript);
                     }
                     a -= 2;
                  }
                  if (z < e && *z == '>')
                     z++;
                  if (z < e && *z == ';')
                     break;
                  if (z < e && *z == ',')
                     z++;
                  p = z;
               }
            }
         }
      }
   }
   return done;
}

int
main (int argc, const char *argv[])
{
   umask (0);
   int c;
   const char *hostname = NULL;
   const char *portname = "sip";
   const char *dir = NULL;
   int dump = 0;

   poptContext optCon;          // context for parsing command-line options
   const struct poptOption optionsTable[] = {
      {"rec-script", 'r', POPT_ARG_STRING, &recscript, 0, "Recording script", "path"},
      {"save-script", 's', POPT_ARG_STRING, &savescript, 0, "Saved file script", "path"},
      {"bind-host", 'h', POPT_ARG_STRING, &hostname, 0, "Bind host",
       "hostname"},
      {"bind-port", 'p', POPT_ARGFLAG_SHOW_DEFAULT | POPT_ARG_STRING, &portname, 0, "Bind port", "port"},
      {"directory", 'd', POPT_ARG_STRING, &dir, 0, "Directory (wav files)", "path"},
      {"debug", 'v', POPT_ARG_NONE, &debug, 0, "Debug", 0},
      {"dump", 'V', POPT_ARG_NONE, &dump, 0, "Dump packets", 0},
      POPT_AUTOHELP {NULL, 0, 0, NULL, 0}
   };

   optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
   poptSetOtherOptionHelp (optCon, "[script]");

   if ((c = poptGetNextOpt (optCon)) < -1)
      errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));

   if (poptPeekArg (optCon))
   {
      poptPrintUsage (optCon, stderr, 0);
      return -1;
   }

   if (dir && chdir (dir))
      err (1, "Cannot change to %s", dir);

   int s = -1;                  // socket for SIP incomig
   {                            // binding
    const struct addrinfo hints = { ai_flags: AI_PASSIVE, ai_socktype: SOCK_DGRAM, ai_family: AF_INET6, ai_protocol:IPPROTO_UDP
      };
      struct addrinfo *a = 0,
         *t;
      if (getaddrinfo (hostname, portname, &hints, &a) || !a)
         errx (1, "Cannot look up %s", portname);
      for (t = a; t; t = t->ai_next)
      {
         int on = 1;
         s = socket (t->ai_family, t->ai_socktype, t->ai_protocol);
         if (s < 0)
            continue;
         setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on));
         if (bind (s, t->ai_addr, t->ai_addrlen))
         {                      // failed to connect
            close (s);
            s = -1;
            continue;
         }
         break;
      }
      freeaddrinfo (a);
      if (s < 0)
         errx (1, "Cannot bind %s", portname);
   }

   int opt = 1;
   if (setsockopt (s, IPPROTO_IP, IP_PKTINFO, &opt, sizeof (opt)))
      err (1, "IP sockopt");
   if (setsockopt (s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt, sizeof (opt)))
      err (1, "IP6 sockopt");

   // Child process pick up
   void babysit (int s)
   {
      while (waitpid (-1, NULL, WNOHANG) > 0);
      signal (SIGCHLD, &babysit);
   }
   signal (SIGCHLD, &babysit);

   openlog ("voip-answer", LOG_CONS | LOG_PID, LOG_LOCAL7);

   // Main loop - accepting SIP messages
   while (1)
   {
      int len = 0;
      ui8 rx[2000] = { },       // TODO len?
         tx[1500];

      // This is complicated as we want to get the receive side IP address information here
      union
      {
         char cmsg[CMSG_SPACE (sizeof (struct in_pktinfo))];
         char cmsg6[CMSG_SPACE (sizeof (struct in6_pktinfo))];
      } u;
      struct sockaddr_in6 peeraddr;
      struct iovec io = {
       iov_base:rx,
       iov_len:sizeof (rx),
      };
      struct msghdr mh = {
       msg_name:&peeraddr,
       msg_namelen:sizeof (peeraddr),
       msg_control:&u,
       msg_controllen:sizeof (u),
       msg_iov:&io,
       msg_iovlen:1,
      };
      len = recvmsg (s, &mh, 0);
      if (len < 0)
         err (1, "recvmsg");
      void *addrto = NULL;
      struct cmsghdr *cmsg;
      struct in_pktinfo *pi = NULL;
      struct in6_pktinfo *pi6 = NULL;
      int family = 0;
      for (cmsg = CMSG_FIRSTHDR (&mh); cmsg != NULL; cmsg = CMSG_NXTHDR (&mh, cmsg))
         if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO)
         {
            family = AF_INET;
            pi = (void *) CMSG_DATA (cmsg);
            addrto = &pi->ipi_spec_dst;
            break;
         } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO)
         {
            family = AF_INET6;
            pi6 = (void *) CMSG_DATA (cmsg);
            addrto = &pi6->ipi6_addr;
            break;
         }
      if (!addrto)
      {
         if (debug)
            fprintf (stderr, "No family found\n");
         continue;
      }
      char addr[INET6_ADDRSTRLEN + 1] = "";
      inet_ntop (peeraddr.sin6_family, &peeraddr.sin6_addr, addr, sizeof (addr));
      if (!strncmp (addr, "::ffff:", 7))
         strcpy (addr, addr + 7);
      if (dump)
         fprintf (stderr, "Receive %u bytes from %s:\n%.*s", len, addr, len, rx);
      else if (debug)
         fprintf (stderr, "Receive %u bytes from %s:\n", len, addr);
      if (len <= 4)
         continue;              // ignore
      if (!isalpha (*rx))
         continue;              // ignore
      ui8 *rxe = rx + len;      // rx end
      ui8 *txe = tx + sizeof (tx);      // rx space end
      ui8 *txp = tx;            // tx pointer
      ui8 *p,
       *e;                      // general extracting headers and stuff
      ui8 *me;                  // method end
      for (me = rx; me < rxe && isalpha (*me); me++);
      if (me - rx == 3 && !strncasecmp (rx, "SIP", 3))
         continue;              // Status, ignore
      if (me - rx == 3 && !strncasecmp (rx, "ACK", 3))
         continue;              // we ignore ACK as no reply needed
      int nonanswer = 0;
      int rport = -1;           // response port allocated
      void make_reply (int rev)
      {                         // Copy some key headers
         p = NULL;
         if (rev)
         {
            char temp[1000];
            sprintf (temp, "SIP/2.0/UDP 0.0.0.0:5060"); // dummy Via
            sip_add_header (&txp, txe, "v", temp, NULL);
         } else
            while ((p = sip_find_header (rx, rxe, "Via", "v", &e, p)))
               sip_add_header (&txp, txe, "v", p, e);
         if ((p = sip_find_header (rx, rxe, "From", "f", &e, NULL)))
            sip_add_header (&txp, txe, rev ? "t" : "f", p, e);
         if ((p = sip_find_header (rx, rxe, "To", "t", &e, NULL)))
         {
            sip_add_header (&txp, txe, rev ? "f" : "t", p, e);
            if (rport >= 0)
            {
               char temp[20];
               sprintf (temp, "%u", rport);
               sip_add_extra (&txp, txe, "tag", temp, NULL, ';', 0,0);
            }
         }
         if ((p = sip_find_header (rx, rxe, "Call-ID", "i", &e, NULL)))
            sip_add_header (&txp, txe, "i", p, e);
         if (!rev && (p = sip_find_header (rx, rxe, "CSeq", NULL, &e, NULL)))
            sip_add_header (&txp, txe, "CSeq", p, e);
      }
      void send_reply (int s)
      {                         // Send reply
         if (txp == tx)
            return;
         sendto (s, tx, txp - tx, 0, (struct sockaddr *) &peeraddr, sizeof (peeraddr));
         if (dump)
            fprintf (stderr, "Sent %u bytes to %s:\n%.*s", (int) (txp - tx), addr, (int) (txp - tx), tx);
         else if (debug)
            fprintf (stderr, "Sent %u bytes to %s:\n", (int) (txp - tx), addr);
      }
      // Do we consider this a new call?
      if (me - rx == 6 && !strncasecmp (rx, "INVITE", 6))
      {                         // It is an invite, check there is no tag on the To header, as that would make it a re-invite
         p = sip_find_header (rx, rxe, "To", "t", &e, NULL);
         p = sip_find_semi (p, e, "tag", &e);
         if (!p)
         {                      // Looks like a new INVITE - allocate port and fork
            int a = socket (family, SOCK_DGRAM, IPPROTO_UDP);
            if (a < 0)
               continue;
          struct sockaddr_in6 raddr = { sin6_family:family };
            socklen_t raddrlen = sizeof (raddr);
            if (bind (a, (struct sockaddr *) &raddr, sizeof (raddr)))
               continue;
            if (getsockname (a, (struct sockaddr *) &raddr, &raddrlen))
               continue;        // something wrong
            rport = htons (raddr.sin6_port);
            {                   // Check URI for = or XXX= at start, used to indicate a non-answer call progress response required
               ui8 *e,
                *p = sip_find_request (rx, rxe, &e);
               if (p + 4 < e && !strncasecmp (p, "sip:", 4))
                  p += 4;
               int v = read_unsigned (&p, e);
               if (p < e && *p == '=')
                  nonanswer = v;
            }
            pid_t p = fork ();
            if (p < 0)
               continue;        // fork failed
            if (!p)
            {
               const char *done = audio_in (rport, a, rx, rxe, nonanswer);      // child
               ui8 *e,
                *p = sip_find_header (rx, rxe, "Contact", "m", &e, NULL);
               p = sip_find_uri (p, e, &e);
               if (nonanswer)
               {
                  txp += sprintf (txp, "SIP/2.0 %u Done\r\n", nonanswer);
                  make_reply (0);
               } else if (done && !*done)
               {
                  txp += sprintf (txp, "BYE %.*s SIP/2.0\r\n", (int) (e - p), p);
                  make_reply (1);
                  sip_add_header (&txp, txe, "CSeq", "1 BYE", NULL);
                  sip_add_header (&txp, txe, "l", "0", NULL);
               } else if (done && *done >= ' ')
               {                // refer
                  txp += sprintf (txp, "REFER %.*s SIP/2.0\r\n", (int) (e - p), p);
                  make_reply (1);
                  sip_add_header (&txp, txe, "CSeq", "1 REFER", NULL);
                  sip_add_header (&txp, txe, "l", "0", NULL);
                  while (p < e && *p != '@')
                     p++;
                  char temp[200];
                  snprintf (temp, sizeof (temp), "sip:%s%.*s", done, (int) (e - p), p);
                  sip_add_header (&txp, txe, "Refer-To", temp, NULL);
                  sip_add_header (&txp, txe, "Authorization", "Digest username=\"Voicemail\"", NULL);
               }
               send_reply (s);
               return 0;
            }
            close (a);
         }
      }
      // Construct a simple 200 OK reply.
      if (nonanswer)
         txp += sprintf (txp, "SIP/2.0 183 Call progress\r\n");
      else
         txp += sprintf (txp, "SIP/2.0 200 OK\r\n");
      make_reply (0);
      if (rport >= 0)
      {                         // SDP
         char sdp[1000];
         char temp[50] = "IP6 ";
         inet_ntop (family, addrto, temp + 4, sizeof (temp) - 4);
         if (family == AF_INET)
            temp[2] = '4';
         p = sdp;
         p += sprintf (sdp, "v=0\r\n"   //
                       "o=- %d 1 IN %s\r\n"     //
                       "s=call\r\n"     //
                       "c=IN %s\r\n"    //
                       "t=0 0\r\n"      //
                       "m=audio %u RTP/AVP 8 9 101\r\n" //
                       "a=rtpmap:8 pcma/8000\r\n"       //
                       "a=rtpmap:9 pcma/8000/2\r\n"     //
                       "a=rtpmap:101 telephone-event/8000\r\n"  //
                       "a=fmtp:101 0-16\r\n"    //
                       "a=ptime:20\r\n" //
                       "a=sendrecv\r\n" //
                       , rport, temp, temp, rport);
         *p = 0;
         sprintf (temp, "%u", (int) (p - sdp));
         sip_add_header (&txp, txe, "c", "application/sdp", NULL);
         sip_add_header (&txp, txe, "l", temp, NULL);
         if (txp < txe)
            *txp++ = '\r';
         if (txp < txe)
            *txp++ = '\n';
         if (txp + (p - sdp) < txe)
         {
            strcpy (txp, sdp);
            txp += (p - sdp);
         }
      } else
         sip_add_header (&txp, txe, "l", "0", NULL);    // length
      send_reply (s);
   }
   return 0;
}
