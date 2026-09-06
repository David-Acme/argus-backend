#include "voice-engine-seam.hxx"

IVoiceStt& voiceStt()
{
  static SingletonVoiceStt adapter;
  return adapter;
}

IVoiceTts& voiceTts()
{
  static SingletonVoiceTts adapter;
  return adapter;
}

IVoiceLlm& voiceLlm()
{
  static SingletonVoiceLlm adapter;
  return adapter;
}
