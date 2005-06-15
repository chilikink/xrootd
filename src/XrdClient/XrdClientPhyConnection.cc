//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientPhyConnection                                               //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// Class handling physical connections to xrootd servers                //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//       $Id$

#include <time.h>
#include <stdlib.h>
#include "XrdClient/XrdClientPhyConnection.hh"
#include "XrdClient/XrdClientDebug.hh"
#include "XrdClient/XrdClientMessage.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "XrdOuc/XrdOucPthread.hh"
#include "XrdClient/XrdClientSid.hh"
#include "XrdSec/XrdSecInterface.hh"
#include <sys/socket.h>


//____________________________________________________________________________
void *SocketReaderThread(void * arg, XrdClientThread *thr)
{
   // This thread is the base for the async capabilities of TXPhyConnection
   // It repeatedly keeps reading from the socket, while feeding the
   // MsqQ with a stream of TXMessages containing what's happening
   // at the socket level

   XrdClientPhyConnection *thisObj;

   Info(XrdClientDebug::kHIDEBUG,
	"SocketReaderThread",
	"Reader Thread starting.");

   thr->SetCancelDeferred();
   thr->SetCancelOn();

   thisObj = (XrdClientPhyConnection *)arg;

   thisObj->StartedReader();

   while (1) {
     thisObj->BuildMessage(TRUE, TRUE);

     if (thisObj->CheckAutoTerm())
	break;
   }

   Info(XrdClientDebug::kHIDEBUG,
        "SocketReaderThread",
        "Reader Thread exiting.");

   return 0;
}

//____________________________________________________________________________
XrdClientPhyConnection::XrdClientPhyConnection(XrdClientAbsUnsolMsgHandler *h):
   fReaderCV(0) {

   // Constructor
   fServerType = kUnknown;

   Touch();

   fServer.Clear();

   SetLogged(kNo);

   fRequestTimeout = EnvGetLong(NAME_REQUESTTIMEOUT);

   UnsolicitedMsgHandler = h;

   fReaderthreadhandler = 0;
   fReaderthreadrunning = FALSE;
}

//____________________________________________________________________________
XrdClientPhyConnection::~XrdClientPhyConnection()
{
   // Destructor
  Info(XrdClientDebug::kHIDEBUG,
       "XrdClientPhyConnection",
       "Destroying. [" << fServer.Host << ":" << fServer.Port << "]");

   Disconnect();

   if (fSocket) {
      delete fSocket;
      fSocket = 0;
   }

   UnlockChannel();

   if (fReaderthreadrunning)
      fReaderthreadhandler->Cancel();

   if (fSecProtocol) {
      // This insures that the right destructor is called
      // (Do not do C++ delete).
      fSecProtocol->Delete();
      fSecProtocol = 0;
   }
}

//____________________________________________________________________________
bool XrdClientPhyConnection::Connect(XrdClientUrlInfo RemoteHost)
{
   // Connect to remote server
   XrdOucMutexHelper l(fMutex);

   Info(XrdClientDebug::kHIDEBUG,
	"Connect",
	"Connecting to [" << RemoteHost.Host << ":" <<	RemoteHost.Port << "]");
  
   fSocket = new XrdClientSock(RemoteHost);

   if(!fSocket) {
      Error("Connect","Unable to create a client socket. Aborting.");
      abort();
   }

   fSocket->TryConnect();

   if (!fSocket->IsConnected()) {
      Error("Connect", 
            "can't open connection to [" <<
	    RemoteHost.Host << ":" <<	RemoteHost.Port << "]");

      Disconnect();

      return FALSE;
   }

   Touch();

   fTTLsec = DATA_TTL;

   Info(XrdClientDebug::kHIDEBUG, "Connect", "Connected to [" <<
	RemoteHost.Host << ":" << RemoteHost.Port << "]");

   fServer = RemoteHost;
   fReaderthreadrunning = FALSE;

   return TRUE;
}

//____________________________________________________________________________
void XrdClientPhyConnection::StartReader() {
   bool running;

   {
      XrdOucMutexHelper l(fMutex);
      running = fReaderthreadrunning;
   }
   // Start reader thread

   // Parametric asynchronous stuff.
   // If we are going Sync, then nothing has to be done,
   // otherwise the reader thread must be started
   if ( !running && EnvGetLong(NAME_GOASYNC) ) {

      Info(XrdClientDebug::kHIDEBUG,
	   "StartReader", "Starting reader thread...");

      // Now we launch  the reader thread
      fReaderthreadhandler = new XrdClientThread(SocketReaderThread);
      if (!fReaderthreadhandler)
	 Error("PhyConnection",
	       "Can't create reader thread: out of system resources");

      fReaderthreadhandler->Run(this);
      
      do {
          {
             XrdOucMutexHelper l(fMutex);
	     fReaderthreadhandler->Detach();
             running = fReaderthreadrunning;
          }

          if (!running) fReaderCV.Wait(100);
      } while (!running);


   }
}


//____________________________________________________________________________
void XrdClientPhyConnection::StartedReader() {
   XrdOucMutexHelper l(fMutex);
   fReaderthreadrunning = TRUE;
   fReaderCV.Post();
}

//____________________________________________________________________________
bool XrdClientPhyConnection::ReConnect(XrdClientUrlInfo RemoteHost)
{
   // Re-connection attempt

   Disconnect();
   return Connect(RemoteHost);
}

//____________________________________________________________________________
void XrdClientPhyConnection::Disconnect()
{
   XrdOucMutexHelper l(fMutex);

   // Disconnect from remote server

   if (fSocket) {
      Info(XrdClientDebug::kHIDEBUG,
	   "PhyConnection", "Disconnecting socket...");
      fSocket->Disconnect();
   }

   // We do not destroy the socket here. The socket will be destroyed
   // in CheckAutoTerm or in the ConnMgr
}


//____________________________________________________________________________
bool XrdClientPhyConnection::CheckAutoTerm() {
   bool doexit = FALSE;

  {
   XrdOucMutexHelper l(fMutex);

   // Parametric asynchronous stuff
   // If we are going async, we might be willing to term ourself
   if (!IsValid() && EnvGetLong(NAME_GOASYNC)) {

         Info(XrdClientDebug::kHIDEBUG,
              "CheckAutoTerm", "Self-Cancelling reader thread.");


         fReaderthreadrunning = FALSE;
         fReaderthreadhandler = 0;

         //delete fSocket;
         //fSocket = 0;

         doexit = TRUE;
      }

  }


  if (doexit) {
	UnlockChannel();
        return true;
   }

  return false;
}


//____________________________________________________________________________
void XrdClientPhyConnection::Touch()
{
   // Set last-use-time to present time
   XrdOucMutexHelper l(fMutex);

   time_t t = time(0);

   Info(XrdClientDebug::kDUMPDEBUG,
      "Touch",
      "Setting last use to current time" << t);

   fLastUseTimestamp = t;
}

//____________________________________________________________________________
int XrdClientPhyConnection::ReadRaw(void *buf, int len) {
   // Receive 'len' bytes from the connected server and store them in 'buf'.
   // Return 0 if OK. 

   int res;


   Touch();

   if (IsValid()) {

      Info(XrdClientDebug::kDUMPDEBUG,
	   "ReadRaw",
	   "Reading from " <<
	   fServer.Host << ":" << fServer.Port);

      res = fSocket->RecvRaw(buf, len);

      if (res && (res != TXSOCK_ERR_TIMEOUT) && errno ) {
	 //strerror_r(errno, errbuf, sizeof(buf));

         Info(XrdClientDebug::kHIDEBUG,
	      "ReadRaw", "Read error on " <<
	      fServer.Host << ":" << fServer.Port << ". errno=" << errno );
      }

      // If a socket error comes, then we disconnect
      // but we have not to disconnect in the case of a timeout
      if ((res && (res != TXSOCK_ERR_TIMEOUT)) ||
          (!fSocket->IsConnected())) {

	 Info(XrdClientDebug::kHIDEBUG,
	      "ReadRaw", 
	      "Disconnection reported on" <<
	      fServer.Host << ":" << fServer.Port);

         Disconnect();

      }

      Touch();

      return res;
   }
   else {
      // Socket already destroyed or disconnected
      Info(XrdClientDebug::kUSERDEBUG,
	   "ReadRaw", "Socket is disconnected.");

      return TXSOCK_ERR;
   }

}

//____________________________________________________________________________
XrdClientMessage *XrdClientPhyConnection::ReadMessage(int streamid) {
   // Gets a full loaded XrdClientMessage from this phyconn.
   // May be a pure msg pick from a queue

   Touch();
   return fMsgQ.GetMsg(streamid, fRequestTimeout );

 }

//____________________________________________________________________________
XrdClientMessage *XrdClientPhyConnection::BuildMessage(bool IgnoreTimeouts, bool Enqueue)
{
   // Builds an XrdClientMessage, and makes it read its header/data from the socket
   // Also put automatically the msg into the queue

   XrdClientMessage *m;
   bool parallelsid;

   m = new XrdClientMessage();
   if (!m) {
      Error("BuildMessage",
	    "Cannot create a new Message. Aborting.");
      abort();
   }

   m->ReadRaw(this);

   parallelsid = SidManager->GetSidInfo(m->HeaderSID());

   if ( parallelsid || (m->IsAttn()) ) {
      UnsolRespProcResult res;

      // Here we insert the PhyConn-level support for unsolicited responses
      // Some of them will be propagated in some way to the upper levels
      // The path should be
      //  here -> XrdClientConnMgr -> all the involved XrdClientLogConnections ->
      //   -> all the corresponding XrdClient

      res = HandleUnsolicited(m);

      // The purpose of this message ends here
      if ( (parallelsid) && (res != kUNSOL_KEEP) )
	 SidManager->ReleaseSid(m->HeaderSID());

      delete m;
      m = 0;
   }
   else
      if (Enqueue) {
         // If we have to ignore the socket timeouts, then we have not to
         // feed the queue with them. In this case, the newly created XrdClientMessage
         // has to be freed.
	 //if ( !IgnoreTimeouts || !m->IsError() )

         //bool waserror;

         if (IgnoreTimeouts) {

            if (m->GetStatusCode() != XrdClientMessage::kXrdMSC_timeout) {
               //waserror = m->IsError();

               fMsgQ.PutMsg(m);

               //if (waserror)
               //   for (int kk=0; kk < 10; kk++) fMsgQ.PutMsg(0);
            }
            else {
               delete m;
               m = 0;
            }

         } else
            fMsgQ.PutMsg(m);
      }
  
   return m;
}

//____________________________________________________________________________
UnsolRespProcResult XrdClientPhyConnection::HandleUnsolicited(XrdClientMessage *m)
{
   // Local processing of unsolicited responses is done here

   bool ProcessingToGo = TRUE;
   struct ServerResponseBody_Attn *attnbody;

   Touch();

   // Local processing of the unsolicited XrdClientMessage
   attnbody = (struct ServerResponseBody_Attn *)m->GetData();

   if (attnbody) {
    
      switch (attnbody->actnum) {
      case kXR_asyncms:
         // A message arrived from the server. Let's print it.
         Info(XrdClientDebug::kNODEBUG,
	      "HandleUnsolicited",
              "Message from " <<
	      fServer.Host << ":" << fServer.Port << ". '" <<
              attnbody->parms << "'");

         ProcessingToGo = FALSE;
         break;
      }
   }

   // Now we propagate the message to the interested object, if any
   // It could be some sort of upper layer of the architecture
   if (ProcessingToGo)
      return SendUnsolicitedMsg(this, m);
   else 
      return kUNSOL_CONTINUE;
}

//____________________________________________________________________________
int XrdClientPhyConnection::WriteRaw(const void *buf, int len)
{
   // Send 'len' bytes located at 'buf' to the connected server.
   // Return number of bytes sent. 

   int res;

   Touch();

   if (IsValid()) {

      Info(XrdClientDebug::kDUMPDEBUG,
	   "WriteRaw",
	   "Writing to" <<
	   XrdClientDebug::kDUMPDEBUG);
    
      res = fSocket->SendRaw(buf, len);

      if ((res < 0)  && (res != TXSOCK_ERR_TIMEOUT) && errno) {
	 //strerror_r(errno, errbuf, sizeof(buf));

         Info(XrdClientDebug::kHIDEBUG,
	      "WriteRaw", "Write error on " <<
	      fServer.Host << ":" << fServer.Port << ". errno=" << errno );

      }

      // If a socket error comes, then we disconnect (and destroy the fSocket)
      if ((res < 0) || (!fSocket) || (!fSocket->IsConnected())) {

	 Info(XrdClientDebug::kHIDEBUG,
	      "WriteRaw", 
	      "Disconnection reported on" <<
	      fServer.Host << ":" << fServer.Port);

         Disconnect();
      }

      Touch();
      return( res );
   }
   else {
      // Socket already destroyed or disconnected
      Info(XrdClientDebug::kUSERDEBUG,
	   "WriteRaw",
	   "Socket is disconnected.");
      return TXSOCK_ERR;
   }
}


//____________________________________________________________________________
bool XrdClientPhyConnection::ExpiredTTL()
{
   // Check expiration time
   return( (time(0) - fLastUseTimestamp) > fTTLsec );
}

//____________________________________________________________________________
void XrdClientPhyConnection::LockChannel()
{
   // Lock 
   fRwMutex.Lock();
}

//____________________________________________________________________________
void XrdClientPhyConnection::UnlockChannel()
{
   // Unlock
   fRwMutex.UnLock();
}
