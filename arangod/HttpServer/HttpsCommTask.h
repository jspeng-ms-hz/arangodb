////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_HTTP_SERVER_HTTPS_COMM_TASK_H
#define ARANGOD_HTTP_SERVER_HTTPS_COMM_TASK_H 1

#include "HttpServer/HttpCommTask.h"

#include <openssl/ssl.h>


namespace triagens {
namespace rest {
class HttpsServer;

////////////////////////////////////////////////////////////////////////////////
/// @brief https communication
////////////////////////////////////////////////////////////////////////////////

class HttpsCommTask : public HttpCommTask {
  HttpsCommTask(HttpsCommTask const&) = delete;
  HttpsCommTask const& operator=(HttpsCommTask const&) = delete;

  
 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief read block size
  ////////////////////////////////////////////////////////////////////////////////

  static size_t const READ_BLOCK_SIZE = 10000;

  
 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a new task with a given socket
  ////////////////////////////////////////////////////////////////////////////////

  HttpsCommTask(HttpsServer*, TRI_socket_t, ConnectionInfo const&,
                double keepAliveTimeout, SSL_CTX* ctx, int verificationMode,
                int (*verificationCallback)(int, X509_STORE_CTX*));

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief destructs a task
  ////////////////////////////////////////////////////////////////////////////////

 protected:
  ~HttpsCommTask();

  
 protected:
  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool setup(Scheduler*, EventLoop) override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool handleEvent(EventToken, EventType) override;

  
  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool fillReadBuffer() override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool handleWrite() override;

  
 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief accepts SSL connection
  ////////////////////////////////////////////////////////////////////////////////

  bool trySSLAccept();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief reads from SSL connection
  ////////////////////////////////////////////////////////////////////////////////

  bool trySSLRead();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief writes from SSL connection
  ////////////////////////////////////////////////////////////////////////////////

  bool trySSLWrite();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief shuts down the SSL connection
  ////////////////////////////////////////////////////////////////////////////////

  void shutdownSsl(bool initShutdown);

  
 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief accepted done
  ////////////////////////////////////////////////////////////////////////////////

  bool _accepted;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief read blocked on write
  ////////////////////////////////////////////////////////////////////////////////

  bool _readBlockedOnWrite;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief write blocked on read
  ////////////////////////////////////////////////////////////////////////////////

  bool _writeBlockedOnRead;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief temporary buffer
  ////////////////////////////////////////////////////////////////////////////////

  char* _tmpReadBuffer;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief ssl
  ////////////////////////////////////////////////////////////////////////////////

  SSL* _ssl;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief context
  ////////////////////////////////////////////////////////////////////////////////

  SSL_CTX* _ctx;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief verification mode
  ////////////////////////////////////////////////////////////////////////////////

  int _verificationMode;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief verification callback
  ////////////////////////////////////////////////////////////////////////////////

  int (*_verificationCallback)(int, X509_STORE_CTX*);
};
}
}

#endif


