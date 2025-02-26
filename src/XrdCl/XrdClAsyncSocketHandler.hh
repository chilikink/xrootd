//------------------------------------------------------------------------------
// Copyright (c) 2011-2012 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_ASYNC_SOCKET_HANDLER_HH__
#define __XRD_CL_ASYNC_SOCKET_HANDLER_HH__

#include "XrdCl/XrdClSocket.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClPoller.hh"
#include "XrdCl/XrdClPostMasterInterfaces.hh"
#include "XrdCl/XrdClTaskManager.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClAsyncMsgReader.hh"
#include "XrdCl/XrdClAsyncHSReader.hh"
#include "XrdCl/XrdClAsyncMsgWriter.hh"
#include "XrdCl/XrdClAsyncHSWriter.hh"

namespace XrdCl
{
  class Stream;

  //----------------------------------------------------------------------------
  //! Utility class handling asynchronous socket interactions and forwarding
  //! events to the parent stream.
  //----------------------------------------------------------------------------
  class AsyncSocketHandler: public SocketHandler
  {
    public:
      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      AsyncSocketHandler( const URL        &url,
                          Poller           *poller,
                          TransportHandler *transport,
                          AnyObject        *channelData,
                          uint16_t          subStreamNum,
                          Stream           *strm );

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      ~AsyncSocketHandler();

      //------------------------------------------------------------------------
      //! Set address
      //------------------------------------------------------------------------
      void SetAddress( const XrdNetAddr &address )
      {
        pSockAddr = address;
      }

      //------------------------------------------------------------------------
      //! Get the address that the socket is connected to
      //------------------------------------------------------------------------
      const XrdNetAddr &GetAddress() const
      {
        return pSockAddr;
      }

      //------------------------------------------------------------------------
      //! Connect to the currently set address
      //------------------------------------------------------------------------
      XRootDStatus Connect( time_t timeout );

      //------------------------------------------------------------------------
      //! Close the connection
      //------------------------------------------------------------------------
      XRootDStatus Close();

      //------------------------------------------------------------------------
      //! Handle a socket event
      //------------------------------------------------------------------------
      virtual void Event( uint8_t type, XrdCl::Socket */*socket*/ );

      //------------------------------------------------------------------------
      //! Enable uplink
      //------------------------------------------------------------------------
      XRootDStatus EnableUplink()
      {
        if( !pPoller->EnableWriteNotification( pSocket, true, pTimeoutResolution ) )
          return XRootDStatus( stFatal, errPollerError );
        return XRootDStatus();
      }

      //------------------------------------------------------------------------
      //! Disable uplink
      //------------------------------------------------------------------------
      XRootDStatus DisableUplink()
      {
        if( !pPoller->EnableWriteNotification( pSocket, false ) )
          return XRootDStatus( stFatal, errPollerError );
        return XRootDStatus();
      }

      //------------------------------------------------------------------------
      //! Get stream name
      //------------------------------------------------------------------------
      const std::string &GetStreamName()
      {
        return pStreamName;
      }

      //------------------------------------------------------------------------
      //! Get timestamp of last registered socket activity
      //------------------------------------------------------------------------
      time_t GetLastActivity()
      {
        return pLastActivity;
      }

      //------------------------------------------------------------------------
      //! Get the IP stack
      //------------------------------------------------------------------------
      std::string GetIpStack() const;

      //------------------------------------------------------------------------
      //! Get hostname
      //------------------------------------------------------------------------
      std::string GetIpAddr();

    protected:

      //------------------------------------------------------------------------
      //! Convert Stream object and sub-stream number to stream name
      //------------------------------------------------------------------------
      static std::string ToStreamName( const URL &url, uint16_t strmnb );

      //------------------------------------------------------------------------
      // Connect returned
      //------------------------------------------------------------------------
      virtual void OnConnectionReturn();

      //------------------------------------------------------------------------
      // Got a write readiness event
      //------------------------------------------------------------------------
      void OnWrite();

      //------------------------------------------------------------------------
      // Got a write readiness event while handshaking
      //------------------------------------------------------------------------
      void OnWriteWhileHandshaking();

      //------------------------------------------------------------------------
      // Got a read readiness event
      //------------------------------------------------------------------------
      void OnRead();

      //------------------------------------------------------------------------
      // Got a read readiness event while handshaking
      //------------------------------------------------------------------------
      void OnReadWhileHandshaking();

      //------------------------------------------------------------------------
      // Handle the handshake message
      //------------------------------------------------------------------------
      void HandleHandShake( std::unique_ptr<Message> msg );

      //------------------------------------------------------------------------
      // Prepare the next step of the hand-shake procedure
      //------------------------------------------------------------------------
      void HandShakeNextStep( bool done );

      //------------------------------------------------------------------------
      // Handle fault
      //------------------------------------------------------------------------
      void OnFault( XRootDStatus st );

      //------------------------------------------------------------------------
      // Handle fault while handshaking
      //------------------------------------------------------------------------
      void OnFaultWhileHandshaking( XRootDStatus st );

      //------------------------------------------------------------------------
      // Handle write timeout event
      //------------------------------------------------------------------------
      void OnWriteTimeout();

      //------------------------------------------------------------------------
      // Handle read timeout event
      //------------------------------------------------------------------------
      void OnReadTimeout();

      //------------------------------------------------------------------------
      // Handle timeout event while handshaking
      //------------------------------------------------------------------------
      void OnTimeoutWhileHandshaking();

      //------------------------------------------------------------------------
      // Handle header corruption in case of kXR_status response
      //------------------------------------------------------------------------
      void OnHeaderCorruption();

      //------------------------------------------------------------------------
      // Carry out the TLS hand-shake
      //
      // The TLS hand-shake is being initiated in HandleHandShake() by calling
      // Socket::TlsHandShake(), however it returns suRetry the TLS hand-shake
      // needs to be followed up by OnTlsHandShake().
      //
      // However, once the TLS connection has been established the server may
      // decide to redo the TLS hand-shake at any time, this operation is handled
      // under the hood by read and write requests and facilitated by
      // Socket::MapEvent()
      //------------------------------------------------------------------------
      XRootDStatus DoTlsHandShake();

      //------------------------------------------------------------------------
      // Handle read/write event if we are in the middle of a TLS hand-shake
      //------------------------------------------------------------------------
      // Handle read/write event if we are in the middle of a TLS hand-shake
      void OnTLSHandShake();

      //------------------------------------------------------------------------
      // Prepare a HS writer for sending and enable uplink
      //------------------------------------------------------------------------
      void SendHSMsg();

      //------------------------------------------------------------------------
      // Extract the value of a wait response
      //
      // @param rsp : the server response
      // @return    : if rsp is a wait response then its value
      //              otherwise -1
      //------------------------------------------------------------------------
      inline kXR_int32 HandleWaitRsp( Message *rsp );

      //------------------------------------------------------------------------
      // Check if HS wait time elapsed
      //------------------------------------------------------------------------
      void CheckHSWait();

      //------------------------------------------------------------------------
      // Data members
      //------------------------------------------------------------------------
      Poller                        *pPoller;
      TransportHandler              *pTransport;
      AnyObject                     *pChannelData;
      uint16_t                       pSubStreamNum;
      Stream                        *pStream;
      std::string                    pStreamName;
      Socket                        *pSocket;
      XrdNetAddr                     pSockAddr;
      std::unique_ptr<HandShakeData> pHandShakeData;
      bool                           pHandShakeDone;
      uint16_t                       pTimeoutResolution;
      time_t                         pConnectionStarted;
      time_t                         pConnectionTimeout;
      time_t                         pLastActivity;
      time_t                         pHSWaitStarted;
      time_t                         pHSWaitSeconds;
      URL                            pUrl;
      bool                           pTlsHandShakeOngoing;

      std::unique_ptr<AsyncHSWriter>  hswriter;
      std::unique_ptr<AsyncMsgReader> rspreader;
      std::unique_ptr<AsyncHSReader>  hsreader;
      std::unique_ptr<AsyncMsgWriter> reqwriter;
  };
}

#endif // __XRD_CL_ASYNC_SOCKET_HANDLER_HH__
