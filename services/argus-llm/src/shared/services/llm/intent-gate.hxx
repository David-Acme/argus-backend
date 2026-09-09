#pragma once

#include <shared/services/intent/fasttext-classifier.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <shared/services/memory/phrase-catalog.hxx>

#include <string>

// Owns, for the process lifetime, everything the router needs.
class IntentGate
{
public:
  IntentGate();

  const IntentRouter& router() const { return router_; }
  bool isLoaded() const { return classifier_.isLoaded(); }

private:
  PhraseCatalog catalog_;
  FastTextClassifier classifier_;
  IntentRouter router_;
};
