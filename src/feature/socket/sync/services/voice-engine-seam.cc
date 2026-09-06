#include "voice-engine-seam.hxx"

#include <shared/services/tts/remote/tts-remote.hxx>

IVoiceStt& voiceStt()
{
  static SingletonVoiceStt adapter;
  return adapter;
}

// Lazily resolved on the first call (after the boot config is loaded): the
// argus-tts HTTP adapter when tts.remote_url is set, the in-process
// singleton otherwise (Ruling BI).
IVoiceTts& voiceTts()
{
  static RemoteVoiceTts remoteAdapter;
  static SingletonVoiceTts localAdapter;
  if (TtsRemoteConfig::resolve().enabled())
    return remoteAdapter;
  return localAdapter;
}

IVoiceLlm& voiceLlm()
{
  static SingletonVoiceLlm adapter;
  return adapter;
}
