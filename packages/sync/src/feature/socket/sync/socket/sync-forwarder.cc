#include "sync-forwarder.hxx"

void sendSocketFrameError(const SocketFrameError& input)
{
  Json::Value envelope;
  envelope["type"] = input.type + "_error";
  envelope["status"] = input.status;
  envelope["error"] = input.error;
  input.conn->sendJson(envelope);
}
